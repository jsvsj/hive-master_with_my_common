local c = require "hive.core"    	--��Ӧhive.c
	
--��package.path�в���hive.system  ����hive/system.lua
--system_cell���ǲ��ҵ��Ĵ��ڵ��ļ���
local system_cell = assert(package.searchpath("hive.system", package.path),"system cell was not found")

local hive = {}

function hive.start(t)
	----��package.path�в���t.main,��test.lua�У����� t.main����"test.main" 
	--���ܻ��������� /test/main.lua
	local main = assert(package.searchpath(t.main, package.path), "main cell was not found")

	--({thread = 4,main = "test.main",},system.lua,main.lua)
	return c.start(t, system_cell, main)
end

return hive