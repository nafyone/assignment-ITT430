/* $OpenBSD: mac.c,v 1.21 2012/12/11 22:51:45 sthen Exp $ */
/*
 * Copyright (c) 2001 Markus Friedl.  All rights reserved.
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

#include <openssl/hmac.h>

#include <string.h>
#include <signal.h>

#include "cipher.h"
#include "key.h"
#include "mac.h"
#include "misc.h"
#include "err.h"
#include "sshbuf.h"

#include "umac.h"

#define SSH_EVP		1	/* OpenSSL EVP-based MAC */
#define SSH_UMAC	2	/* UMAC (not integrated with OpenSSL) */
#define SSH_UMAC128	3

struct {
	char		*name;
	int		type;
	const EVP_MD *	(*mdfunc)(void);
	int		truncatebits;	/* truncate digest if != 0 */
	int		key_len;	/* just for UMAC */
	int		len;		/* just for UMAC */
	int		etm;		/* Encrypt-then-MAC */
} macs[] = {
	/* Encrypt-and-MAC (encrypt-and-authenticate) variants */
	{ "hmac-sha1",				SSH_EVP, EVP_sha1, 0, 0, 0, 0 },
	{ "hmac-sha1-96",			SSH_EVP, EVP_sha1, 96, 0, 0, 0 },
	{ "hmac-sha2-256",			SSH_EVP, EVP_sha256, 0, 0, 0, 0 },
	{ "hmac-sha2-512",			SSH_EVP, EVP_sha512, 0, 0, 0, 0 },
	{ "hmac-md5",				SSH_EVP, EVP_md5, 0, 0, 0, 0 },
	{ "hmac-md5-96",			SSH_EVP, EVP_md5, 96, 0, 0, 0 },
	{ "hmac-ripemd160",			SSH_EVP, EVP_ripemd160, 0, 0, 0, 0 },
	{ "hmac-ripemd160@openssh.com",		SSH_EVP, EVP_ripemd160, 0, 0, 0, 0 },
	{ "umac-64@openssh.com",		SSH_UMAC, NULL, 0, 128, 64, 0 },
	{ "umac-128@openssh.com",		SSH_UMAC128, NULL, 0, 128, 128, 0 },

	/* Encrypt-then-MAC variants */
	{ "hmac-sha1-etm@openssh.com",		SSH_EVP, EVP_sha1, 0, 0, 0, 1 },
	{ "hmac-sha1-96-etm@openssh.com",	SSH_EVP, EVP_sha1, 96, 0, 0, 1 },
	{ "hmac-sha2-256-etm@openssh.com",	SSH_EVP, EVP_sha256, 0, 0, 0, 1 },
	{ "hmac-sha2-512-etm@openssh.com",	SSH_EVP, EVP_sha512, 0, 0, 0, 1 },
	{ "hmac-md5-etm@openssh.com",		SSH_EVP, EVP_md5, 0, 0, 0, 1 },
	{ "hmac-md5-96-etm@openssh.com",	SSH_EVP, EVP_md5, 96, 0, 0, 1 },
	{ "hmac-ripemd160-etm@openssh.com",	SSH_EVP, EVP_ripemd160, 0, 0, 0, 1 },
	{ "umac-64-etm@openssh.com",		SSH_UMAC, NULL, 0, 128, 64, 1 },
	{ "umac-128-etm@openssh.com",		SSH_UMAC128, NULL, 0, 128, 128, 1 },

	{ NULL,					0, NULL, 0, 0, 0, 0 }
};

static int
mac_setup_by_id(struct sshmac *mac, int which)
{
	int evp_len;
	mac->type = macs[which].type;
	if (mac->type == SSH_EVP) {
		mac->evp_md = (*macs[which].mdfunc)();
		if ((evp_len = EVP_MD_size(mac->evp_md)) <= 0)
			return SSH_ERR_LIBCRYPTO_ERROR;
		mac->key_len = mac->mac_len = (u_int)evp_len;
	} else {
		mac->mac_len = macs[which].len / 8;
		mac->key_len = macs[which].key_len / 8;
		mac->umac_ctx = NULL;
	}
	if (macs[which].truncatebits != 0)
		mac->mac_len = macs[which].truncatebits / 8;
	mac->etm = macs[which].etm;
	return 0;
}

int
mac_setup(struct sshmac *mac, char *name)
{
	int i;

	for (i = 0; macs[i].name; i++) {
		if (strcmp(name, macs[i].name) == 0) {
			if (mac != NULL)
				return mac_setup_by_id(mac, i);
			return 0;
		}
	}
	return SSH_ERR_INVALID_ARGUMENT;
}

int
mac_init(struct sshmac *mac)
{
	if (mac->key == NULL)
		return SSH_ERR_INVALID_ARGUMENT;
	switch (mac->type) {
	case SSH_EVP:
		if (mac->evp_md == NULL)
			return SSH_ERR_INVALID_ARGUMENT;
		HMAC_CTX_init(&mac->evp_ctx);
		if (HMAC_Init(&mac->evp_ctx, mac->key, mac->key_len,
		    mac->evp_md) != 1) {
			HMAC_CTX_cleanup(&mac->evp_ctx);
			return SSH_ERR_LIBCRYPTO_ERROR;
		}
		return 0;
	case SSH_UMAC:
		if ((mac->umac_ctx = umac_new(mac->key)) == NULL)
			return SSH_ERR_ALLOC_FAIL;
		return 0;
	case SSH_UMAC128:
		mac->umac_ctx = umac128_new(mac->key);
		return 0;
	default:
		return SSH_ERR_INVALID_ARGUMENT;
	}
}

int
mac_compute(struct sshmac *mac, u_int32_t seqno, const u_char *data, int datalen,
    u_char *digest, size_t dlen)
{
	static u_char m[MAC_DIGEST_LEN_MAX];
	u_char b[4], nonce[8];

	if (mac->mac_len > sizeof(m))
		return SSH_ERR_INTERNAL_ERROR;

	switch (mac->type) {
	case SSH_EVP:
		POKE_U32(b, seqno);
		/* reset HMAC context */
		if (HMAC_Init(&mac->evp_ctx, NULL, 0, NULL) != 1 ||
		    HMAC_Update(&mac->evp_ctx, b, sizeof(b)) != 1 ||
		    HMAC_Update(&mac->evp_ctx, data, datalen) != 1 ||
		    HMAC_Final(&mac->evp_ctx, m, NULL) != 1)
			return SSH_ERR_LIBCRYPTO_ERROR;
		break;
	case SSH_UMAC:
		POKE_U64(nonce, seqno);
		umac_update(mac->umac_ctx, data, datalen);
		umac_final(mac->umac_ctx, m, nonce);
		break;
	case SSH_UMAC128:
		put_u64(nonce, seqno);
		umac128_update(mac->umac_ctx, data, datalen);
		umac128_final(mac->umac_ctx, m, nonce);
		break;
	default:
		return SSH_ERR_INVALID_ARGUMENT;
	}
	if (digest != NULL) {
		if (dlen > mac->mac_len)
			dlen = mac->mac_len;
		memcpy(digest, m, dlen);
	}
	return 0;
}

void
mac_clear(struct sshmac *mac)
{
	if (mac->type == SSH_UMAC) {
		if (mac->umac_ctx != NULL)
			umac_delete(mac->umac_ctx);
	} else if (mac->type == SSH_UMAC128) {
		if (mac->umac_ctx != NULL)
			umac128_delete(mac->umac_ctx);
	} else if (mac->evp_md != NULL)
		HMAC_cleanup(&mac->evp_ctx);
	mac->evp_md = NULL;
	mac->umac_ctx = NULL;
}

/* XXX copied from ciphers_valid */
#define	MAC_SEP	","
int
mac_valid(const char *names)
{
	char *maclist, *cp, *p;

	if (names == NULL || strcmp(names, "") == 0)
		return 0;
	if ((maclist = cp = strdup(names)) == NULL)
		return 0;
	for ((p = strsep(&cp, MAC_SEP)); p && *p != '\0';
	    (p = strsep(&cp, MAC_SEP))) {
		if (mac_setup(NULL, p) < 0) {
			free(maclist);
			return 0;
		}
	}
	free(maclist);
	return 1;
}