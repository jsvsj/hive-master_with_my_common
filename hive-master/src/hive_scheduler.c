#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "hive_cell.h"
#include "hive_env.h"
#include "hive_scheduler.h"
#include "hive_system_lib.h"

#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>

#define DEFAULT_THREAD 4
#define MAX_GLOBAL_MQ 0x10000
#define GP(p) ((p) % MAX_GLOBAL_MQ)

//全局的消息队列,存储的是cell指针，cell中有自己的循环消息队列
struct global_queue {
	uint32_t head;		//循环队列头	
	uint32_t tail;		//循环队列尾
	//cell数量
	int total;

	//工作线程数量
	int thread;

	//存储cell指针的指针数组    模拟循环队列
	struct cell * queue[MAX_GLOBAL_MQ];
	//用来标记queue对应的下标是否被使用
	bool flag[MAX_GLOBAL_MQ];
};


//时间模块
struct timer {
	uint32_t current;
	struct cell * sys;
	struct global_queue * mq;
};


//将cell添加到q中
static void 
globalmq_push(struct global_queue *q, struct cell * c) {
	uint32_t tail = GP(__sync_fetch_and_add(&q->tail,1));
	q->queue[tail] = c;
	__sync_synchronize();
	
	q->flag[tail] = true;

	__sync_synchronize();
}

//将头部的cell出对,返回
static struct cell * 
globalmq_pop(struct global_queue *q) {
	uint32_t head =  q->head;
	uint32_t head_ptr = GP(head);
	if (head_ptr == GP(q->tail)) {
		return NULL;
	}

	if(!q->flag[head_ptr]) {
		return NULL;
	}

	struct cell * c = q->queue[head_ptr];
	if (!__sync_bool_compare_and_swap(&q->head, head, head+1)) {
		return NULL;
	}
	q->flag[head_ptr] = false;
	__sync_synchronize();

	return c;
}

//初始化
static void
globalmq_init(struct global_queue *q, int thread) {
	memset(q, 0, sizeof(*q));
	q->thread = thread;		//工作线程数量
}

//将total+1
static inline void
globalmq_inc(struct global_queue *q) {
	__sync_add_and_fetch(&q->total,1);
}


//将total-1,即cell的数量-1
static inline void
globalmq_dec(struct global_queue *q) {
	__sync_sub_and_fetch(&q->total,1);
}


//从一个cell的循环消息队列中选取一个消息，处理，之后将该cell加入到尾部
static int
_message_dispatch(struct global_queue *q) {

	//弹出一个cell
	struct cell *c = globalmq_pop(q);
	if (c == NULL)
		return 1;
	//从c对应的循环消息队列中取出一个消息，调用_dispatch()
	int r =  cell_dispatch_message(c);
	
	switch(r) {
	case CELL_EMPTY:
	case CELL_MESSAGE:
		break;
	//如果cell退出了	
	case CELL_QUIT:
		globalmq_dec(q);
		return 1;
	}
	//又将cell加入到q中管理
	globalmq_push(q, c);
	return r;
}


//获取时间,单位是 0.01秒，即 一个滴答是 0.01秒
static uint32_t
_gettime(void) {
	uint32_t t;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	t = (uint32_t)(tv.tv_sec & 0xffffff) * 100;   //tv_sec 秒

	t += tv.tv_usec / 10000;         //tv_usec  微秒

	return t;
}

//初始化timer结构体
static void
timer_init(struct timer *t, struct cell * sys, struct global_queue *mq) {
	t->current = _gettime();
	t->sys = sys;
	t->mq = mq;
}

//发送滴答
static inline void
send_tick(struct cell * c) {
	cell_send(c, 0, NULL);
}

//更新时间
static void
_updatetime(struct timer * t) {
	uint32_t ct = _gettime();
	if (ct > t->current) {

		//计算时间差值,就是滴答数的差值,相差多少个0.01秒
		int diff = ct-t->current;

		//更新时间
		t->current = ct;
		int i;
		for (i=0;i<diff;i++) {
			//向timer中指向的cell发送滴答消息	
			send_tick(t->sys);
		}
	}
}

//时间事件线程中执行的函数,不断的向timer的cell发送消息
static void *
_timer(void *p) {
	struct timer * t = p;
	for (;;) {
		
		_updatetime(t);
		
		//单位是微秒,睡眠 0.25个滴答
		usleep(2500);
		if (t->mq->total <= 1)
			return NULL;
	}
}


//工作线程
static void *
_worker(void *p) {
	struct global_queue * mq = p;
	for (;;) {
		int i;
		//cell的数量
		int n = mq->total;
		int ret = 1;
		for (i=0;i<n;i++) {
			
			//从global_queue的一个cell的循环消息队列中选取一个消息，处理，之后将该cell加入到尾部
			ret &= _message_dispatch(mq);
			if (n < mq->total) {
				n = mq->total;
			}
		}
		if (ret) {
			usleep(1000);
			if (mq->total <= 1)
				return NULL;
		} 
	}
	return NULL;
}


//创建时间线程,工作线程
static void
_start(struct global_queue *gmq, struct timer *t) {

	int thread = gmq->thread;
	pthread_t pid[thread+1];
	int i;

	//创建时间事件处理线程
	pthread_create(&pid[0], NULL, _timer, t);

	//创建工作线程
	for (i=1;i<=thread;i++) {
		pthread_create(&pid[i], NULL, _worker, gmq);
	}

	//等待线程
	for (i=0;i<=thread;i++) {
		pthread_join(pid[i], NULL); 
	}
}

//创建lua环境,设置环境变量
lua_State *
scheduler_newtask(lua_State *pL) {
	lua_State *L = luaL_newstate();
	luaL_openlibs(L);

	//在注册表中创建表
	hive_createenv(L);

	//从pL的注册表中的表中拷贝"message_queue"= 到L的注册表,{"message_queue"=mq}
	//指向的是同一个global_queue,拷贝的是指针
	struct global_queue * mq = hive_copyenv(L, pL, "message_queue");
	
	//将mq的total+1
	globalmq_inc(mq);
	
	hive_copyenv(L, pL, "system_pointer");

	lua_newtable(L);
	lua_newtable(L);
	//{} {} "v"
 	lua_pushliteral(L, "v");

	//{"__mode"="v"} {}
	lua_setfield(L, -2, "__mode");

	//{"__mode"="v"  元表}
	lua_setmetatable(L,-2);
	
	hive_setenv(L, "cell_map");

	return L;
}

//销毁lua环境
void
scheduler_deletetask(lua_State *L) {
	lua_close(L);
}

void
scheduler_starttask(lua_State *L) {
	hive_getenv(L, "message_queue");
	struct global_queue * gmq = lua_touserdata(L, -1);
	
	lua_pop(L,1);
	hive_getenv(L, "cell_pointer");
	struct cell * c= lua_touserdata(L, -1);
	lua_pop(L,1);
	//将cell加入到gmq中
	globalmq_push(gmq, c);
}

// lua调用时传递的参数 ({thread = 4,main = "test.main",},system.lua,main.lua)
int 
scheduler_start(lua_State *L) {
	//检查函数第一个参数是否是LUA_TTABLE类型
	luaL_checktype(L,1,LUA_TTABLE);

	//检查函数第2个参数是否是字符串，并返回字符串指针
	//对应hive/system.lua
	const char * system_lua = luaL_checkstring(L,2);

	//对应要执行的文件 test/main.lua
	const char * main_lua = luaL_checkstring(L,3);

	//将t["thread"]入栈
	lua_getfield(L,1, "thread");

	//工作线程数
	int thread = luaL_optinteger(L, -1, DEFAULT_THREAD);

	//弹出一个元素
	lua_pop(L,1);

	//创建环境 在L的注册表中创建了一张表
	hive_createenv(L);

	//创建global_queue
	struct global_queue * gmq = lua_newuserdata(L, sizeof(*gmq));

	//初始化
	globalmq_init(gmq, thread);
	
	//把栈上给定索引处的元素作一个副本压栈
	lua_pushvalue(L,-1);
	//设置环境 t["message_queue"]= 
	//即 在之前在注册表中创建的表中加入元素  "message_queue"=gmq
	hive_setenv(L, "message_queue");

	lua_State *sL;

	//再次创建一个lua_State
	sL = scheduler_newtask(L);

	//调用cell_system_lib ,注册了一些函数
	luaL_requiref(sL, "cell.system", cell_system_lib, 0);
	
	lua_pop(sL,1);

	lua_pushstring(sL, main_lua);
	lua_setglobal(sL, "maincell");  //设置maincell=main_lua

	//cell_new中会执行system_lua对应文件,会调用其中的start()函数  会将消息处理的回调函数入栈
	struct cell * sys = cell_new(sL, system_lua);
	if (sys == NULL) {
		return 0;
	}
	
	scheduler_starttask(sL);

	//创建timer
	struct timer * t = lua_newuserdata(L, sizeof(*t));
	timer_init(t,sys,gmq);

	//创建线程,循环工作
	_start(gmq,t);
	
	cell_close(sys);

	return 0;
}

