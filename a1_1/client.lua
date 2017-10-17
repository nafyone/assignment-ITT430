socket = require('socket')
io.write("connect to host ")
server = io.read()
io.write("connect to port ")
port = io.read()
client = socket.connect( server , port)
if client then
	io.write("connection succes\n")
	while true do 

		io.write("msg to server")
		client:send(io.read().. "\n")
		reply = client:receive()
		io.write("message from server" .. reply .. "\n")

		if reply == "end" then break end
	end
end
client:close()

