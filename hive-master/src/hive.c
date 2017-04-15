#include "lua.h"
#include "lauxlib.h"
#include "hive_scheduler.h"


/*�˺���Ϊc���е����⺯��	
 ͨ��������ע������c���еĺ������������Ǵ洢���ʵ���λ��
 �˺�������������
 1.ʹ��luaopen_��Ϊǰ׺
 2.ǰ׺֮������ֽ���Ϊ"require"�Ĳ���
*/


int
luaopen_hive_core(lua_State *L) { 
	luaL_Reg l[] = {
		{ "start", scheduler_start },
		{ NULL, NULL },
	};
	luaL_checkversion(L);

	//����һ���µ�table,����l�еĺ���ע��Ϊtable����
	luaL_newlib(L,l);

	return 1;
}


