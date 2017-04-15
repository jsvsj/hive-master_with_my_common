#include "lua.h"
#include "lauxlib.h"

static int __hive = 0;
#define HIVE_TAG (&__hive)


//创建环境
//在注册表中创建一张key为&__hive的表
void
hive_createenv(lua_State *L) {
	//创建一张空表，并入栈
	//{}
	lua_newtable(L);

	//t是LUA_REGISTRYINDEX索引处的表，就是注册表,HIVE_TAG
	//k 是指针 p 对应的轻量用户数据。 而 v 是栈顶的值

	//就是   在注册表中  注册表[__hive]=创建的表
	//t[0]={} t是注册表   即 { 0={} }
	lua_rawsetp(L, LUA_REGISTRYINDEX, HIVE_TAG);
}


//获取环境中 key对应是值
//获取注册表中的 表HIVE_TAG中的值
void 
hive_getenv(lua_State *L, const char * key) {
//int lua_rawgetp (lua_State *L, int index, const void *p);
//把 t[k] 的值压栈， 这里的 t 是指给定索引处的表， k 是指针 p 对应的轻量用户数据。
//就是将hive_createenv函数中创建的表压栈

	//注册表{0={key=....} ....}
	//调用后栈元素 {key=...}
	lua_rawgetp(L, LUA_REGISTRYINDEX, HIVE_TAG);

//将表中t[key]入栈
	//{key=AAA}  AAA
	lua_getfield(L, -1, key);

//把栈顶元素覆盖到-2的位置，然后移除栈顶
	// AAA
	lua_replace(L, -2);
}

//设置环境
//在表中添加元素
void 
hive_setenv(lua_State *L, const char * key) {

	//{}
	lua_rawgetp(L, LUA_REGISTRYINDEX, HIVE_TAG);

	//把栈顶元素移动到指定的有效索引处， 依次移动这个索引之上的元素
	lua_insert(L, -2);
	
	//t[key]=栈顶元素
	//{key=XXXX}
	lua_setfield(L, -2, key);
	lua_pop(L,1);
}

//将fromL中元素的指针放入到 L的环境中
void *
hive_copyenv(lua_State *L, lua_State *fromL, const char *key) {
	//从from的注册表的表HIVE_TAG 中获取key对应的值,在栈顶
	hive_getenv(fromL, key);
	
	void *p = lua_touserdata(fromL, -1);

	//弹出1个元素
	lua_pop(fromL, 1);

	lua_pushlightuserdata(L, p);
	hive_setenv(L, key);

	return p;
}


