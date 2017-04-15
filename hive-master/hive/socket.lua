local csocket = require "cell.c.socket" --对应hive_socket_lib.c
csocket.init()

local cell = require "cell"
local command = {}
local message = {}

--键对应的是文件描述符的应用层序号
local sockets = {}

--在c语言中调用的是 connect
function command.connect(source,addr,port)

	local fd = csocket.connect(addr, port)
	if fd then
		sockets[fd] = source
		return fd
	end
end

--在c语言中调用的是    socket  bind  listen
function command.listen(source, port)

	local fd = csocket.listen(port)
	if fd then
		sockets[fd] = source
		return fd
	end
end

--调用cell中的发送消息的函数
function command.forward(fd, addr)
	local data = sockets[fd]
	sockets[fd] = addr
	if type(data) == "table" then
		for i=1,#data do
			local v = data[i]
			if not pcall(cell.rawsend, addr, 6, v[1], v[2], v[3]) then
				csocket.freepack(v[3])
				message.disconnect(v[1])
			end
		end
	end
end

--关闭连接，在c语言中调用的是close关闭文件描述符
function message.disconnect(fd)
	if sockets[fd] then
		sockets[fd] = nil
		csocket.close(fd)
	end
end

cell.command(command)
cell.message(message)

--调度
cell.dispatch {
	id = 6, -- socket
	dispatch = csocket.send, -- fd, sz, msg
	replace = true,
}

--在c语言中调用 epoll_wait
function cell.main()
	local result = {}
	while true do
		for i = 1, csocket.poll(result) do
			local v = result[i]
			
			local c = sockets[v[1]]
			if c then
				--如果是 监听套接字
				if type(v[3]) == "string" then
					-- accept: listen fd, new fd , ip 对应  v[1], v[2], v[3]
					if not pcall(cell.rawsend,c, 6, v[1], v[2], v[3]) then
						--v[1]是对应的文件描述符
						message.disconnect(v[1])
					else
						--为新建的连接描述符绑定一个 {}
						sockets[v[2]] = {}
					end
				elseif type(c) == "table" then
					table.insert(c, {v[1],v[2],v[3]})
					
				else
					--此时处理的是连接描述符的可读的活跃事件
					--在 poll中已经读取了数据，在v[]中
					-- forward: fd , size , message  
					--fd:活跃的描述符 size:数据大小 message:数据内存 ,依次对应 v[1], v[2], v[3]
					if not pcall(cell.rawsend,c, 6, v[1], v[2], v[3]) then
						--释放 message内存
						csocket.freepack(v[3])
						message.disconnect(v[1])
					end
				end
			else
				csocket.freepack(v[3])
			end
		end
		cell.sleep(0)
	end
end