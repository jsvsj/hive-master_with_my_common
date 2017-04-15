#include "lua.h"
#include "lauxlib.h"

static int __hive = 0;
#define HIVE_TAG (&__hive)


//��������
//��ע����д���һ��keyΪ&__hive�ı�
void
hive_createenv(lua_State *L) {
	//����һ�ſձ�����ջ
	//{}
	lua_newtable(L);

	//t��LUA_REGISTRYINDEX�������ı�����ע���,HIVE_TAG
	//k ��ָ�� p ��Ӧ�������û����ݡ� �� v ��ջ����ֵ

	//����   ��ע�����  ע���[__hive]=�����ı�
	//t[0]={} t��ע���   �� { 0={} }
	lua_rawsetp(L, LUA_REGISTRYINDEX, HIVE_TAG);
}


//��ȡ������ key��Ӧ��ֵ
//��ȡע����е� ��HIVE_TAG�е�ֵ
void 
hive_getenv(lua_State *L, const char * key) {
//int lua_rawgetp (lua_State *L, int index, const void *p);
//�� t[k] ��ֵѹջ�� ����� t ��ָ�����������ı� k ��ָ�� p ��Ӧ�������û����ݡ�
//���ǽ�hive_createenv�����д����ı�ѹջ

	//ע���{0={key=....} ....}
	//���ú�ջԪ�� {key=...}
	lua_rawgetp(L, LUA_REGISTRYINDEX, HIVE_TAG);

//������t[key]��ջ
	//{key=AAA}  AAA
	lua_getfield(L, -1, key);

//��ջ��Ԫ�ظ��ǵ�-2��λ�ã�Ȼ���Ƴ�ջ��
	// AAA
	lua_replace(L, -2);
}

//���û���
//�ڱ������Ԫ��
void 
hive_setenv(lua_State *L, const char * key) {

	//{}
	lua_rawgetp(L, LUA_REGISTRYINDEX, HIVE_TAG);

	//��ջ��Ԫ���ƶ���ָ������Ч�������� �����ƶ��������֮�ϵ�Ԫ��
	lua_insert(L, -2);
	
	//t[key]=ջ��Ԫ��
	//{key=XXXX}
	lua_setfield(L, -2, key);
	lua_pop(L,1);
}

//��fromL��Ԫ�ص�ָ����뵽 L�Ļ�����
void *
hive_copyenv(lua_State *L, lua_State *fromL, const char *key) {
	//��from��ע���ı�HIVE_TAG �л�ȡkey��Ӧ��ֵ,��ջ��
	hive_getenv(fromL, key);
	
	void *p = lua_touserdata(fromL, -1);

	//����1��Ԫ��
	lua_pop(fromL, 1);

	lua_pushlightuserdata(L, p);
	hive_setenv(L, key);

	return p;
}


