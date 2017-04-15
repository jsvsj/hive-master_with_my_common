#include "lua.h"
#include "lauxlib.h"
#include "hive_scheduler.h"


/*此函数为c库中的特殊函数	
 通过调用他注册所有c库中的函数，并将他们存储在适当的位置
 此函数的命名规则
 1.使用luaopen_作为前缀
 2.前缀之后的名字将作为"require"的参数
*/


int
luaopen_hive_core(lua_State *L) { 
	luaL_Reg l[] = {
		{ "start", scheduler_start },
		{ NULL, NULL },
	};
	luaL_checkversion(L);

	//创建一个新的table,并将l中的函数注册为table的域
	luaL_newlib(L,l);

	return 1;
}


