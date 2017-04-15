local c = require "hive.core"    	--对应hive.c
	
--在package.path中查找hive.system  就是hive/system.lua
--system_cell就是查找到的存在的文件名
local system_cell = assert(package.searchpath("hive.system", package.path),"system cell was not found")

local hive = {}

function hive.start(t)
	----在package.path中查找t.main,在test.lua中，就是 t.main就是"test.main" 
	--可能会这样查找 /test/main.lua
	local main = assert(package.searchpath(t.main, package.path), "main cell was not found")

	--({thread = 4,main = "test.main",},system.lua,main.lua)
	return c.start(t, system_cell, main)
end

return hive