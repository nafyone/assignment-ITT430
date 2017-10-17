socket = require("socket")
port = 3030
server = socket.bind('*',port)
io.write( "listening to port"..port.."\n")
svr = server:accept()
io.write("Connection success")
while true do
	msg = svr:receive()
	io.write("message from client" .. msg .. "\n")
	io.write("msg to client")
	svr:send(io.read() .. "\n")
	
	if msg == "end" then break end
end
io.read()
