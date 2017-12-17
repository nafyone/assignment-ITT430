/* $OpenBSD: auth2.c,v 1.126 2012/12/02 20:34:09 djm Exp $ */
/*
 * Copyright (c) 2000 Markus Friedl.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <fcntl.h>
#include <pwd.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "atomicio.h"
#include "xmalloc.h"
#include "ssh2.h"
#include "packet.h"
#include "log.h"
#include "sshbuf.h"
#include "servconf.h"
#include "compat.h"
#include "key.h"
#include "hostfile.h"
#include "auth.h"
#include "dispatch.h"
#include "pathnames.h"
#ifdef GSSAPI
#include "ssh-gss.h"
#endif
#include "monitor_wrap.h"
#include "err.h"

/* import */
extern ServerOptions options;
extern u_char *session_id2;
extern u_int session_id2_len;

/* methods */

extern Authmethod method_none;
extern Authmethod method_pubkey;
extern Authmethod method_passwd;
extern Authmethod method_kbdint;
extern Authmethod method_hostbased;
#ifdef GSSAPI
extern Authmethod method_gssapi;
#endif
#ifdef JPAKE
extern Authmethod method_jpake;
#endif

Authmethod *authmethods[] = {
	&method_none,
	&method_pubkey,
#ifdef GSSAPI
	&method_gssapi,
#endif
#ifdef JPAKE
	&method_jpake,
#endif
	&method_passwd,
	&method_kbdint,
	&method_hostbased,
	NULL
};

/* protocol */

static int input_service_request(int, u_int32_t, struct ssh *);
static int input_userauth_request(int, u_int32_t, struct ssh *);

/* helper */
static Authmethod *authmethod_lookup(Authctxt *, const char *);
static char *authmethods_get(Authctxt *authctxt);
static int method_allowed(Authctxt *, const char *);
static int list_starts_with(const char *, const char *);

char *
auth2_read_banner(void)
{
	struct stat st;
	char *banner = NULL;
	size_t len, n;
	int fd;

	if ((fd = open(options.banner, O_RDONLY)) == -1)
		return (NULL);
	if (fstat(fd, &st) == -1) {
		close(fd);
		return (NULL);
	}
	if (st.st_size <= 0 || st.st_size > 1*1024*1024) {
		close(fd);
		return (NULL);
	}

	len = (size_t)st.st_size;		/* truncate */
	banner = xmalloc(len + 1);
	n = atomicio(read, fd, banner, len);
	close(fd);

	if (n != len) {
		xfree(banner);
		return (NULL);
	}
	banner[n] = '\0';

	return (banner);
}

static void
userauth_banner(struct ssh *ssh)
{
	char *banner = NULL;
	int r;

	if (options.banner == NULL ||
	    strcasecmp(options.banner, "none") == 0 ||
	    (ssh->compat & SSH_BUG_BANNER) != 0)
		return;

	if ((banner = PRIVSEP(auth2_read_banner())) == NULL)
		goto done;

	if ((r = sshpkt_start(ssh, SSH2_MSG_USERAUTH_BANNER)) != 0 ||
	    (r = sshpkt_put_cstring(ssh, banner)) != 0 ||
	    (r = sshpkt_put_cstring(ssh, "")) != 0 ||	/* language, unused */
	    (r = sshpkt_send(ssh)) != 0)
		fatal("%s: %s", __func__, ssh_err(r));
	debug("userauth_banner: sent");
done:
	if (banner)
		xfree(banner);
}

/*
 * loop until authctxt->success == TRUE
 */
void
do_authentication2(struct ssh *ssh)
{
	Authctxt *authctxt = ssh->authctxt;
	int r;

	ssh_dispatch_init(ssh, &dispatch_protocol_error);
	ssh_dispatch_set(ssh, SSH2_MSG_SERVICE_REQUEST, &input_service_request);
	if ((r = ssh_dispatch_run(ssh, DISPATCH_BLOCK, &authctxt->success)) != 0)
		fatal("%s: ssh_dispatch_run failed: %s", __func__, ssh_err(r));
}

/*ARGSUSED*/
static int
input_service_request(int type, u_int32_t seq, struct ssh *ssh)
{
	Authctxt *authctxt = ssh->authctxt;
	char *service = NULL;
	int r, acceptit = 0;

	if ((r = sshpkt_get_cstring(ssh, &service, NULL)) != 0 ||
	    (r = sshpkt_get_end(ssh)) != 0)
		goto out;

	if (authctxt == NULL)
		fatal("input_service_request: no authctxt");

	if (strcmp(service, "ssh-userauth") == 0) {
		if (!authctxt->success) {
			acceptit = 1;
			/* now we can handle user-auth requests */
			ssh_dispatch_set(ssh, SSH2_MSG_USERAUTH_REQUEST,
			    &input_userauth_request);
		}
	}
	/* XXX all other service requests are denied */

	if (acceptit) {
		if ((r = sshpkt_start(ssh, SSH2_MSG_SERVICE_ACCEPT)) != 0 ||
		    (r = sshpkt_put_cstring(ssh, service)) != 0 ||
		    (r = sshpkt_send(ssh)) != 0)
			goto out;
		ssh_packet_write_wait(ssh);
	} else {
		debug("bad service request %s", service);
		ssh_packet_disconnect(ssh, "bad service request %s", service);
	}
	r = 0;
 out:
	free(service);
	return r;
}

/*ARGSUSED*/
static int
input_userauth_request(int type, u_int32_t seq, struct ssh *ssh)
{
	Authctxt *authctxt = ssh->authctxt;
	Authmethod *m = NULL;
	char *user = NULL, *service = NULL, *method = NULL, *style = NULL;
	int r, authenticated = 0;

	if (authctxt == NULL)
		fatal("input_userauth_request: no authctxt");

	if ((r = sshpkt_get_cstring(ssh, &user, NULL)) != 0 ||
	    (r = sshpkt_get_cstring(ssh, &service, NULL)) != 0 ||
	    (r = sshpkt_get_cstring(ssh, &method, NULL)) != 0)
		goto out;
	debug("userauth-request for user %s service %s method %s", user, service, method);
	debug("attempt %d failures %d", authctxt->attempt, authctxt->failures);

	if ((style = strchr(user, ':')) != NULL)
		*style++ = 0;

	if (authctxt->attempt++ == 0) {
		/* setup auth context */
		authctxt->pw = PRIVSEP(getpwnamallow(user));
		if (authctxt->pw && strcmp(service, "ssh-connection")==0) {
			authctxt->valid = 1;
			debug2("input_userauth_request: setting up authctxt for %s", user);
		} else {
			logit("input_userauth_request: invalid user %s", user);
			authctxt->pw = fakepw();
		}
		setproctitle("%s%s", authctxt->valid ? user : "unknown",
		    use_privsep ? " [net]" : "");
		authctxt->user = xstrdup(user);
		authctxt->service = xstrdup(service);
		authctxt->style = style ? xstrdup(style) : NULL;
		if (use_privsep)
			mm_inform_authserv(service, style);
		userauth_banner(ssh);
		if (auth2_setup_methods_lists(authctxt) != 0)
			ssh_packet_disconnect(ssh,
			    "no authentication methods enabled");
	} else if (strcmp(user, authctxt->user) != 0 ||
	    strcmp(service, authctxt->service) != 0) {
		ssh_packet_disconnect(ssh, "Change of username or service not allowed: "
		    "(%s,%s) -> (%s,%s)",
		    authctxt->user, authctxt->service, user, service);
	}
	/* reset state */
	auth2_challenge_stop(ssh);
#ifdef JPAKE
	auth2_jpake_stop(ssh);
#endif

#ifdef GSSAPI
	/* XXX move to auth2_gssapi_stop() */
	ssh_dispatch_set(ssh, SSH2_MSG_USERAUTH_GSSAPI_TOKEN, NULL);
	ssh_dispatch_set(ssh, SSH2_MSG_USERAUTH_GSSAPI_EXCHANGE_COMPLETE, NULL);
#endif

	authctxt->postponed = 0;
	authctxt->server_caused_failure = 0;

	/* try to authenticate user */
	m = authmethod_lookup(authctxt, method);
	if (m != NULL && authctxt->failures < options.max_authtries) {
		debug2("input_userauth_request: try method %s", method);
		authenticated =	m->userauth(ssh);
	}
	userauth_finish(ssh, authenticated, method, NULL);
	r = 0;
 out:
	xfree(service);
	xfree(user);
	xfree(method);
	return r;
}

void
userauth_finish(struct ssh *ssh, int authenticated, const char *method,
    const char *submethod)
{
	Authctxt *authctxt = ssh->authctxt;
	char *methods;
	int r, partial = 0;

	if (!authctxt->valid && authenticated)
		fatal("INTERNAL ERROR: authenticated invalid user %s",
		    authctxt->user);
	if (authenticated && authctxt->postponed)
		fatal("INTERNAL ERROR: authenticated and postponed");

	/* Special handling for root */
	if (authenticated && authctxt->pw->pw_uid == 0 &&
	    !auth_root_allowed(method))
		authenticated = 0;

	if (authenticated && options.num_auth_methods != 0) {
		if (!auth2_update_methods_lists(authctxt, method)) {
			authenticated = 0;
			partial = 1;
		}
	}

	/* Log before sending the reply */
	auth_log(authctxt, authenticated, partial, method, submethod, " ssh2");

	if (authctxt->postponed)
		return;

	if (authenticated == 1) {
		/* turn off userauth */
		ssh_dispatch_set(ssh, SSH2_MSG_USERAUTH_REQUEST,
		    &dispatch_protocol_ignore);
		if ((r = sshpkt_start(ssh, SSH2_MSG_USERAUTH_SUCCESS)) != 0 ||
		    (r = sshpkt_send(ssh)) != 0)
			fatal("%s: %s", __func__, ssh_err(r));
		ssh_packet_write_wait(ssh);
		/* now we can break out */
		authctxt->success = 1;
	} else {
		/* Allow initial try of "none" auth without failure penalty */
		if (!authctxt->server_caused_failure &&
		    (authctxt->attempt > 1 || strcmp(method, "none") != 0))
			authctxt->failures++;
		if (authctxt->failures >= options.max_authtries)
			ssh_packet_disconnect(ssh, AUTH_FAIL_MSG,
			    authctxt->user);
		methods = authmethods_get(authctxt);
		debug3("%s: failure partial=%d next methods=\"%s\"", __func__,
		    partial, methods);
		if ((r = sshpkt_start(ssh, SSH2_MSG_USERAUTH_FAILURE)) != 0 ||
		    (r = sshpkt_put_cstring(ssh, methods)) != 0 ||
		    (r = sshpkt_put_u8(ssh, partial)) != 0 ||
		    (r = sshpkt_send(ssh)) != 0)
			fatal("%s: %s", __func__, ssh_err(r));
		ssh_packet_write_wait(ssh);
		xfree(methods);
	}
}

/*
 * Checks whether method is allowed by at least one AuthenticationMethods
 * methods list. Returns 1 if allowed, or no methods lists configured.
 * 0 otherwise.
 */
static int
method_allowed(Authctxt *authctxt, const char *method)
{
	u_int i;

	/*
	 * NB. authctxt->num_auth_methods might be zero as a result of
	 * auth2_setup_methods_lists(), so check the configuration.
	 */
	if (options.num_auth_methods == 0)
		return 1;
	for (i = 0; i < authctxt->num_auth_methods; i++) {
		if (list_starts_with(authctxt->auth_methods[i], method))
			return 1;
	}
	return 0;
}

static char *
authmethods_get(Authctxt *authctxt)
{
	struct sshbuf *b;
	char *list;
	int i, r;

	if ((b = sshbuf_new()) == NULL)
		fatal("%s: sshbuf_new failed", __func__);
	for (i = 0; authmethods[i] != NULL; i++) {
		if (strcmp(authmethods[i]->name, "none") == 0)
			continue;
		if (authmethods[i]->enabled == NULL ||
		    *(authmethods[i]->enabled) == 0)
			continue;
		if (!method_allowed(authctxt, authmethods[i]->name))
			continue;
		if ((r = sshbuf_putf(b, "%s%s", sshbuf_len(b) ? "," : "",
		    authmethods[i]->name)) != 0)
			fatal("%s: buffer error: %s", __func__, ssh_err(r));
	}
	if ((r = sshbuf_put_u8(b, 0)) != 0)
		fatal("%s: buffer error: %s", __func__, ssh_err(r));
	list = xstrdup(sshbuf_ptr(b));
	sshbuf_free(b);
	return list;
}

static Authmethod *
authmethod_lookup(Authctxt *authctxt, const char *name)
{
	int i;

	if (name != NULL)
		for (i = 0; authmethods[i] != NULL; i++)
			if (authmethods[i]->enabled != NULL &&
			    *(authmethods[i]->enabled) != 0 &&
			    strcmp(name, authmethods[i]->name) == 0 &&
			    method_allowed(authctxt, authmethods[i]->name))
				return authmethods[i];
	debug2("Unrecognized authentication method name: %s",
	    name ? name : "NULL");
	return NULL;
}

/*
 * Check a comma-separated list of methods for validity. Is need_enable is
 * non-zero, then also require that the methods are enabled.
 * Returns 0 on success or -1 if the methods list is invalid.
 */
int
auth2_methods_valid(const char *_methods, int need_enable)
{
	char *methods, *omethods, *method;
	u_int i, found;
	int ret = -1;

	if (*_methods == '\0') {
		error("empty authentication method list");
		return -1;
	}
	omethods = methods = xstrdup(_methods);
	while ((method = strsep(&methods, ",")) != NULL) {
		for (found = i = 0; !found && authmethods[i] != NULL; i++) {
			if (strcmp(method, authmethods[i]->name) != 0)
				continue;
			if (need_enable) {
				if (authmethods[i]->enabled == NULL ||
				    *(authmethods[i]->enabled) == 0) {
					error("Disabled method \"%s\" in "
					    "AuthenticationMethods list \"%s\"",
					    method, _methods);
					goto out;
				}
			}
			found = 1;
			break;
		}
		if (!found) {
			error("Unknown authentication method \"%s\" in list",
			    method);
			goto out;
		}
	}
	ret = 0;
 out:
	free(omethods);
	return ret;
}

/*
 * Prune the AuthenticationMethods supplied in the configuration, removing
 * any methods lists that include disabled methods. Note that this might
 * leave authctxt->num_auth_methods == 0, even when multiple required auth
 * has been requested. For this reason, all tests for whether multiple is
 * enabled should consult options.num_auth_methods directly.
 */
int
auth2_setup_methods_lists(Authctxt *authctxt)
{
	u_int i;

	if (options.num_auth_methods == 0)
		return 0;
	debug3("%s: checking methods", __func__);
	authctxt->auth_methods = xcalloc(options.num_auth_methods,
	    sizeof(*authctxt->auth_methods));
	authctxt->num_auth_methods = 0;
	for (i = 0; i < options.num_auth_methods; i++) {
		if (auth2_methods_valid(options.auth_methods[i], 1) != 0) {
			logit("Authentication methods list \"%s\" contains "
			    "disabled method, skipping",
			    options.auth_methods[i]);
			continue;
		}
		debug("authentication methods list %d: %s",
		    authctxt->num_auth_methods, options.auth_methods[i]);
		authctxt->auth_methods[authctxt->num_auth_methods++] =
		    xstrdup(options.auth_methods[i]);
	}
	if (authctxt->num_auth_methods == 0) {
		error("No AuthenticationMethods left after eliminating "
		    "disabled methods");
		return -1;
	}
	return 0;
}

static int
list_starts_with(const char *methods, const char *method)
{
	size_t l = strlen(method);

	if (strncmp(methods, method, l) != 0)
		return 0;
	if (methods[l] != ',' && methods[l] != '\0')
		return 0;
	return 1;
}

/*
 * Remove method from the start of a comma-separated list of methods.
 * Returns 0 if the list of methods did not start with that method or 1
 * if it did.
 */
static int
remove_method(char **methods, const char *method)
{
	char *omethods = *methods;
	size_t l = strlen(method);

	if (!list_starts_with(omethods, method))
		return 0;
	*methods = xstrdup(omethods + l + (omethods[l] == ',' ? 1 : 0));
	free(omethods);
	return 1;
}

/*
 * Called after successful authentication. Will remove the successful method
 * from the start of each list in which it occurs. If it was the last method
 * in any list, then authentication is deemed successful.
 * Returns 1 if the method completed any authentication list or 0 otherwise.
 */
int
auth2_update_methods_lists(Authctxt *authctxt, const char *method)
{
	u_int i, found = 0;

	debug3("%s: updating methods list after \"%s\"", __func__, method);
	for (i = 0; i < authctxt->num_auth_methods; i++) {
		if (!remove_method(&(authctxt->auth_methods[i]), method))
			continue;
		found = 1;
		if (*authctxt->auth_methods[i] == '\0') {
			debug2("authentication methods list %d complete", i);
			return 1;
		}
		debug3("authentication methods list %d remaining: \"%s\"",
		    i, authctxt->auth_methods[i]);
	}
	/* This should not happen, but would be bad if it did */
	if (!found)
		fatal("%s: method not in AuthenticationMethods", __func__);
	return 0;
}