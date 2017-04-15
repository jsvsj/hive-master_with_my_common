local csocket = require "cell.c.socket" --��Ӧhive_socket_lib.c
csocket.init()

local cell = require "cell"
local command = {}
local message = {}

--����Ӧ�����ļ���������Ӧ�ò����
local sockets = {}

--��c�����е��õ��� connect
function command.connect(source,addr,port)

	local fd = csocket.connect(addr, port)
	if fd then
		sockets[fd] = source
		return fd
	end
end

--��c�����е��õ���    socket  bind  listen
function command.listen(source, port)

	local fd = csocket.listen(port)
	if fd then
		sockets[fd] = source
		return fd
	end
end

--����cell�еķ�����Ϣ�ĺ���
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

--�ر����ӣ���c�����е��õ���close�ر��ļ�������
function message.disconnect(fd)
	if sockets[fd] then
		sockets[fd] = nil
		csocket.close(fd)
	end
end

cell.command(command)
cell.message(message)

--����
cell.dispatch {
	id = 6, -- socket
	dispatch = csocket.send, -- fd, sz, msg
	replace = true,
}

--��c�����е��� epoll_wait
function cell.main()
	local result = {}
	while true do
		for i = 1, csocket.poll(result) do
			local v = result[i]
			
			local c = sockets[v[1]]
			if c then
				--����� �����׽���
				if type(v[3]) == "string" then
					-- accept: listen fd, new fd , ip ��Ӧ  v[1], v[2], v[3]
					if not pcall(cell.rawsend,c, 6, v[1], v[2], v[3]) then
						--v[1]�Ƕ�Ӧ���ļ�������
						message.disconnect(v[1])
					else
						--Ϊ�½���������������һ�� {}
						sockets[v[2]] = {}
					end
				elseif type(c) == "table" then
					table.insert(c, {v[1],v[2],v[3]})
					
				else
					--��ʱ������������������Ŀɶ��Ļ�Ծ�¼�
					--�� poll���Ѿ���ȡ�����ݣ���v[]��
					-- forward: fd , size , message  
					--fd:��Ծ�������� size:���ݴ�С message:�����ڴ� ,���ζ�Ӧ v[1], v[2], v[3]
					if not pcall(cell.rawsend,c, 6, v[1], v[2], v[3]) then
						--�ͷ� message�ڴ�
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