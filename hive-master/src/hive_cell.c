#include "lua.h"
#include "lauxlib.h"
#include "hive_env.h"
#include "hive_cell.h"
#include "hive_cell_lib.h"
#include "hive_seri.h"
#include "hive_scheduler.h"
#include "hive_socket_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE 64

//消息
struct message {
	int port;
	void * buffer;
};


//循环消息队列
struct message_queue {
	int cap;
	int head;
	int tail;
	struct message *queue;
};

//含有一个循环消息队列
struct cell {
	int lock;
	
	//引用计数
	int ref;
	lua_State *L;
	struct message_queue mq; //循环消息队列
	bool quit;			//是否退出
	bool close;
};

struct cell_ud {
	struct cell * c;
};

static int __cell =0;
#define CELL_TAG (&__cell)

//获取cell,就是增加引用计数
void
cell_grab(struct cell *c) {
	__sync_add_and_fetch(&c->ref,1);
}

//释放cell,引用计数-1
void
cell_release(struct cell *c) {
	if (__sync_sub_and_fetch(&c->ref,1) == 0) {
		//修改标志
		c->quit = true;
	}
}

//加锁
static inline void
cell_lock(struct cell *c) {
	while (__sync_lock_test_and_set(&c->lock,1)) {}
}


//解锁
static inline void
cell_unlock(struct cell *c) {
	__sync_lock_release(&c->lock);
}


//将cell 内存地址转换成字符串,返回到lua层
static int
ltostring(lua_State *L) {
	char tmp[32];
	//获得lua传递的cell
	struct cell_ud * cud = lua_touserdata(L,1);
	int n = sprintf(tmp,"[cell %p]",cud->c);
	lua_pushlstring(L, tmp,n);
	return 1;
}

//调用cell_release(),释放cell,只是将引用计数-1,标识为quit
static int
lrelease(lua_State *L) {
	//cell_ud 中只有一个属性 cell
	struct cell_ud * cud = lua_touserdata(L,1);
	cell_release(cud->c);
	cud->c = NULL;
	return 0;
}

//将cell转换成userdata
void 
cell_touserdata(lua_State *L, int index, struct cell *c) {

	//将index处的table中的cell对应的值入栈   即 t[*c]入栈
	lua_rawgetp(L, index, c);
	
	if (lua_isuserdata(L, -1)) {
		return;
	}
	
	lua_pop(L,1);

	//这个函数分配一块指定大小的内存块， 把内存块地址作为一个完全用户数据压栈， 并返回这个地址
	struct cell_ud * cud = lua_newuserdata(L, sizeof(*cud));
	cud->c = c;
	cell_grab(c);

	//创建元表 {}
	if (luaL_newmetatable(L, "cell")) {

	   //{} 1
		lua_pushboolean(L,1);
		//{0=1}
		lua_rawsetp(L, -2, CELL_TAG);
		//{0=1} ltostring
		lua_pushcfunction(L, ltostring);
		//{0=1,"__tostring"=ltostring}
		lua_setfield(L, -2, "__tostring");

		//{0=1,"__tostring"=ltostring,"__gc"=lrelease}
		lua_pushcfunction(L, lrelease);
		
		lua_setfield(L, -2, "__gc");
	}

	//table  {0=1,"__tostring"=ltostring,"__gc"=lrelease}
	//设置元表
	lua_setmetatable(L, -2);

	//table
	lua_pushvalue(L, -1);

	//t[index]=*c
	lua_rawsetp(L, index, c);
}

//从栈中获取cell
struct cell * 
cell_fromuserdata(lua_State *L, int index) {
	if (lua_type(L, index) != LUA_TUSERDATA) {
		return NULL;
	}
	if (lua_getmetatable(L, index)) {
		lua_rawgetp(L, -1 , CELL_TAG);
		if (lua_toboolean(L, -1)) {
			lua_pop(L,2);
			
			struct cell_ud * cud = lua_touserdata(L, index);
			return cud->c;
		}
		lua_pop(L,2);
	}
	return NULL;
}


//初始化消息队列
static void
mq_init(struct message_queue *mq) {
	mq->cap = DEFAULT_QUEUE;
	mq->head = 0;
	mq->tail = 0;
	mq->queue = malloc(sizeof(struct message) * DEFAULT_QUEUE);
}

//将消息放入消息队列
static void
mq_push(struct message_queue *mq, struct message *m) {
	mq->queue[mq->tail] = *m;
	++mq->tail;
	//为了循环
	if (mq->tail >= mq->cap) {
		mq->tail = 0;
	}

	//如果满 扩容
	if (mq->head == mq->tail) {
		struct message * q = malloc(mq->cap * 2 * sizeof(*q));
		int i;
		for (i=0;i<mq->cap;i++) {
			q[i] = mq->queue[(mq->head+i) % mq->cap];
		}
		mq->head = 0;
		mq->tail = mq->cap;
		mq->cap *=2;
		free(mq->queue);
		mq->queue = q; 
	}
}



//从循环消息队列中取出头部消息  m是传出参数
static int
mq_pop(struct message_queue *mq, struct message *m) {
	//如果为空
	if (mq->head == mq->tail)
		return 1;

	*m = mq->queue[mq->head];
	++mq->head;

	//为了循环
	if (mq->head >= mq->cap) {
		mq->head = 0;
	}
	return 0;
}


//创建cell
static struct cell *
cell_create() {
	struct cell *c = malloc(sizeof(*c));
	c->lock = 0;
	c->ref = 0;
	c->L = NULL;
	c->quit = false;
	c->close = false;

	//初始化循环消息队列
	mq_init(&c->mq);

	return c;
}

//销毁cell
static void
cell_destroy(struct cell *c) {
	assert(c->ref == 0);
	free(c->mq.queue);
	assert(c->L == NULL);
	free(c);
}


//将错误信息入栈，返回给lua
static int 
traceback(lua_State *L) {
	const char *msg = lua_tostring(L, 1);
	if (msg)
		luaL_traceback(L, L, msg, 1);
	else {
		lua_pushliteral(L, "(no error message)");
	}
	return 1;
}


//处理消息的函数
static int
lcallback(lua_State *L) {
	int port = lua_tointeger(L,1);
	void *msg = lua_touserdata(L,2);
	int err;

	//清空栈中的元素
	lua_settop(L,0);

	//lua_upvalueindex(int i) 返回当前运行的函数的第 i 个上值的伪索引
	//lua_pushvalue(L,int inxed) 把栈上给定索引处的元素作一个副本压栈
	lua_pushvalue(L, lua_upvalueindex(1));	// traceback

	if (msg == NULL) {
		lua_pushvalue(L, lua_upvalueindex(3));	// dispatcher 3
		lua_pushinteger(L, port);
		err = lua_pcall(L, 1, 0, 1);
	} else {
		lua_pushvalue(L, lua_upvalueindex(3));	// traceback dispatcher 
		lua_pushinteger(L, port);	// traceback dispatcher port
		lua_pushvalue(L, lua_upvalueindex(2));	// traceback dispatcher port data_unpack
		lua_pushlightuserdata(L, msg);	// traceback dispatcher port data_unpack msg
		lua_pushvalue(L, lua_upvalueindex(4));	// traceback dispatcher port data_unpack msg cell_map 

		//调用的是data_unpack函数吗
		err = lua_pcall(L, 2, LUA_MULTRET, 1);	
		
		if (err) {
			printf("Unpack failed : %s\n", lua_tostring(L,-1));
			return 0;
		}
		int n = lua_gettop(L);	// traceback dispatcher ...

		//调用dispatcher
		err = lua_pcall(L, n-2, 0, 1);	// traceback 1
	}
	
	if (err) {
		printf("[cell %p] %s\n", lua_touserdata(L, lua_upvalueindex(5)), lua_tostring(L,-1));
	}
	return 0;
}


//mainfile对应  system.lua
struct cell *
cell_new(lua_State *L, const char * mainfile) {

	//作用就是调用socket_lib然后把该模块绑定到cell.c.socket模块名字下,会将模块的副本入栈
	luaL_requiref(L, "cell.c.socket", socket_lib, 0);

	
	lua_pop(L,1);

	//获取环境中 cell map对应的值,存放在栈顶
	hive_getenv(L, "cell_map");

	//将一个可接受的索引 idx 转换为绝对索引 （即，一个不依赖栈顶在哪的值)
	int cell_map = lua_absindex(L,-1);	// cell_map

	//调用cell_lib
	luaL_requiref(L, "cell.c", cell_lib, 0);	// cell_map cell_lib

	//创建cell
	struct cell * c = cell_create();
	
	c->L = L;
	cell_touserdata(L, cell_map, c);	// cell_map cell_lib cell_ud

	//t["self"]=栈顶  t是-2出的表
	lua_setfield(L, -2, "self");	// cell_map cell_lib

	hive_getenv(L, "system_pointer");
	struct cell * sys = lua_touserdata(L, -1);	// cell_map cell_lib system_cell
	lua_pop(L, 1);	
	if (sys) {
		cell_touserdata(L, cell_map, sys);
		lua_setfield(L, -2, "system");
	}

	lua_pop(L,2);
	lua_pushlightuserdata(L, c);
	hive_setenv(L, "cell_pointer");

	//加载mainfile中的代码，编译成chunk,压入栈顶 对应文件hive/system.lua
	int err = luaL_loadfile(L, mainfile);
	
	if (err) {
		printf("%d : %s\n", err, lua_tostring(L,-1));
		lua_pop(L,1);
		goto _error;
	}

	//执行加载的代码块
	err = lua_pcall(L, 0, 0, 0);
	if (err) {
		printf("new cell (%s) error %d : %s\n", mainfile, err, lua_tostring(L,-1));
		lua_pop(L,1);
		goto _error;
	}

	//闭包需要惯例的参数
	lua_pushcfunction(L, traceback);	// upvalue 1
	lua_pushcfunction(L, data_unpack); // upvalue 2
	hive_getenv(L, "dispatcher");	// upvalue 3
	
	if (!lua_isfunction(L, -1)) {
		printf("set dispatcher first\n");
		goto _error;
	}
	
	hive_getenv(L, "cell_map");	// upvalue 4
	lua_pushlightuserdata(L, c);            // upvalue 5
  
	//把一个新的 C 闭包压栈 参数 n 告之函数有多少个值需要关联到函数上
	//lcallback是消息处理函数
	lua_pushcclosure(L, lcallback, 5);
	return c;
_error:
	scheduler_deletetask(L);
	c->L = NULL;
	cell_destroy(c);

	return NULL;
}

//关闭cell,就是clock=true
void 
cell_close(struct cell *c) {
	cell_lock(c);
	c->close = true;
	cell_unlock(c);
}

//调用处理消息的函数
static void
_dispatch(lua_State *L, struct message *m) {
	//lua_pushvalue() 把栈上给定索引处的元素作一个副本压栈
	//这里应该是将要调用的函数入栈
	lua_pushvalue(L, 1);	// dup callback
	
	lua_pushinteger(L, m->port);
	lua_pushlightuserdata(L, m->buffer);

	//调用一个函数
	lua_call(L, 2, 0);
}

//将中的 循环消息队列中所有的消息出对，调用_dispatch函数
static void
trash_msg(lua_State *L, struct cell *c) {
	// no new message in , because already set c->close
	// don't need lock c later
	struct message m;
	while (!mq_pop(&c->mq, &m)) {
		_dispatch(L, &m);
	}
	
	// HIVE_PORT 5 : exit 
	// read cell.lua
	//这个消息是一个标志，标志着消息处理完毕
	m.port = 5;
	m.buffer = NULL;

	
	_dispatch(L, &m);
}


//从循环消息队列中 获取消息,调用_dispatch()
int 
cell_dispatch_message(struct cell *c) {
	cell_lock(c);
	lua_State *L = c->L;
	
	//如果cell退出了
	if (c->quit) {
		cell_destroy(c);
		return CELL_QUIT;
	}

	//如果cell标记了退出，将对应的消息队列中的消息全部处理
	if (c->close && L) {
		c->L = NULL;
		cell_grab(c);
		cell_unlock(c);
		
		trash_msg(L,c);
		cell_release(c);
		scheduler_deletetask(L);

		return CELL_EMPTY;
	}

	//从循环消息队列中 获取消息
	struct message m;
	int empty = mq_pop(&c->mq, &m);

	//如果为空
	if (empty || L == NULL) {
		cell_unlock(c);
		return CELL_EMPTY;
	} 
	cell_grab(c);
	
	cell_unlock(c);

	//处理
	_dispatch(L,&m);

	cell_release(c);

	return CELL_MESSAGE;
}

//发送消息，就是将消息添加到cell中的循环消息队列
int 
cell_send(struct cell *c, int port, void *msg) {
	cell_lock(c);
	if (c->quit || c->close) {
		cell_unlock(c);
		return 1;
	}
	struct message m = { port, msg };
	mq_push(&c->mq, &m);
	cell_unlock(c);
	return 0;
}
