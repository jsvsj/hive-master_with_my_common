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

//ȫ�ֵ���Ϣ����,�洢����cellָ�룬cell�����Լ���ѭ����Ϣ����
struct global_queue {
	uint32_t head;		//ѭ������ͷ	
	uint32_t tail;		//ѭ������β
	//cell����
	int total;

	//�����߳�����
	int thread;

	//�洢cellָ���ָ������    ģ��ѭ������
	struct cell * queue[MAX_GLOBAL_MQ];
	//�������queue��Ӧ���±��Ƿ�ʹ��
	bool flag[MAX_GLOBAL_MQ];
};


//ʱ��ģ��
struct timer {
	uint32_t current;
	struct cell * sys;
	struct global_queue * mq;
};


//��cell��ӵ�q��
static void 
globalmq_push(struct global_queue *q, struct cell * c) {
	uint32_t tail = GP(__sync_fetch_and_add(&q->tail,1));
	q->queue[tail] = c;
	__sync_synchronize();
	
	q->flag[tail] = true;

	__sync_synchronize();
}

//��ͷ����cell����,����
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

//��ʼ��
static void
globalmq_init(struct global_queue *q, int thread) {
	memset(q, 0, sizeof(*q));
	q->thread = thread;		//�����߳�����
}

//��total+1
static inline void
globalmq_inc(struct global_queue *q) {
	__sync_add_and_fetch(&q->total,1);
}


//��total-1,��cell������-1
static inline void
globalmq_dec(struct global_queue *q) {
	__sync_sub_and_fetch(&q->total,1);
}


//��һ��cell��ѭ����Ϣ������ѡȡһ����Ϣ������֮�󽫸�cell���뵽β��
static int
_message_dispatch(struct global_queue *q) {

	//����һ��cell
	struct cell *c = globalmq_pop(q);
	if (c == NULL)
		return 1;
	//��c��Ӧ��ѭ����Ϣ������ȡ��һ����Ϣ������_dispatch()
	int r =  cell_dispatch_message(c);
	
	switch(r) {
	case CELL_EMPTY:
	case CELL_MESSAGE:
		break;
	//���cell�˳���	
	case CELL_QUIT:
		globalmq_dec(q);
		return 1;
	}
	//�ֽ�cell���뵽q�й���
	globalmq_push(q, c);
	return r;
}


//��ȡʱ��,��λ�� 0.01�룬�� һ���δ��� 0.01��
static uint32_t
_gettime(void) {
	uint32_t t;
	struct timeval tv;

	gettimeofday(&tv, NULL);
	t = (uint32_t)(tv.tv_sec & 0xffffff) * 100;   //tv_sec ��

	t += tv.tv_usec / 10000;         //tv_usec  ΢��

	return t;
}

//��ʼ��timer�ṹ��
static void
timer_init(struct timer *t, struct cell * sys, struct global_queue *mq) {
	t->current = _gettime();
	t->sys = sys;
	t->mq = mq;
}

//���͵δ�
static inline void
send_tick(struct cell * c) {
	cell_send(c, 0, NULL);
}

//����ʱ��
static void
_updatetime(struct timer * t) {
	uint32_t ct = _gettime();
	if (ct > t->current) {

		//����ʱ���ֵ,���ǵδ����Ĳ�ֵ,�����ٸ�0.01��
		int diff = ct-t->current;

		//����ʱ��
		t->current = ct;
		int i;
		for (i=0;i<diff;i++) {
			//��timer��ָ���cell���͵δ���Ϣ	
			send_tick(t->sys);
		}
	}
}

//ʱ���¼��߳���ִ�еĺ���,���ϵ���timer��cell������Ϣ
static void *
_timer(void *p) {
	struct timer * t = p;
	for (;;) {
		
		_updatetime(t);
		
		//��λ��΢��,˯�� 0.25���δ�
		usleep(2500);
		if (t->mq->total <= 1)
			return NULL;
	}
}


//�����߳�
static void *
_worker(void *p) {
	struct global_queue * mq = p;
	for (;;) {
		int i;
		//cell������
		int n = mq->total;
		int ret = 1;
		for (i=0;i<n;i++) {
			
			//��global_queue��һ��cell��ѭ����Ϣ������ѡȡһ����Ϣ������֮�󽫸�cell���뵽β��
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


//����ʱ���߳�,�����߳�
static void
_start(struct global_queue *gmq, struct timer *t) {

	int thread = gmq->thread;
	pthread_t pid[thread+1];
	int i;

	//����ʱ���¼������߳�
	pthread_create(&pid[0], NULL, _timer, t);

	//���������߳�
	for (i=1;i<=thread;i++) {
		pthread_create(&pid[i], NULL, _worker, gmq);
	}

	//�ȴ��߳�
	for (i=0;i<=thread;i++) {
		pthread_join(pid[i], NULL); 
	}
}

//����lua����,���û�������
lua_State *
scheduler_newtask(lua_State *pL) {
	lua_State *L = luaL_newstate();
	luaL_openlibs(L);

	//��ע����д�����
	hive_createenv(L);

	//��pL��ע����еı��п���"message_queue"= ��L��ע���,{"message_queue"=mq}
	//ָ�����ͬһ��global_queue,��������ָ��
	struct global_queue * mq = hive_copyenv(L, pL, "message_queue");
	
	//��mq��total+1
	globalmq_inc(mq);
	
	hive_copyenv(L, pL, "system_pointer");

	lua_newtable(L);
	lua_newtable(L);
	//{} {} "v"
 	lua_pushliteral(L, "v");

	//{"__mode"="v"} {}
	lua_setfield(L, -2, "__mode");

	//{"__mode"="v"  Ԫ��}
	lua_setmetatable(L,-2);
	
	hive_setenv(L, "cell_map");

	return L;
}

//����lua����
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
	//��cell���뵽gmq��
	globalmq_push(gmq, c);
}

// lua����ʱ���ݵĲ��� ({thread = 4,main = "test.main",},system.lua,main.lua)
int 
scheduler_start(lua_State *L) {
	//��麯����һ�������Ƿ���LUA_TTABLE����
	luaL_checktype(L,1,LUA_TTABLE);

	//��麯����2�������Ƿ����ַ������������ַ���ָ��
	//��Ӧhive/system.lua
	const char * system_lua = luaL_checkstring(L,2);

	//��ӦҪִ�е��ļ� test/main.lua
	const char * main_lua = luaL_checkstring(L,3);

	//��t["thread"]��ջ
	lua_getfield(L,1, "thread");

	//�����߳���
	int thread = luaL_optinteger(L, -1, DEFAULT_THREAD);

	//����һ��Ԫ��
	lua_pop(L,1);

	//�������� ��L��ע����д�����һ�ű�
	hive_createenv(L);

	//����global_queue
	struct global_queue * gmq = lua_newuserdata(L, sizeof(*gmq));

	//��ʼ��
	globalmq_init(gmq, thread);
	
	//��ջ�ϸ�����������Ԫ����һ������ѹջ
	lua_pushvalue(L,-1);
	//���û��� t["message_queue"]= 
	//�� ��֮ǰ��ע����д����ı��м���Ԫ��  "message_queue"=gmq
	hive_setenv(L, "message_queue");

	lua_State *sL;

	//�ٴδ���һ��lua_State
	sL = scheduler_newtask(L);

	//����cell_system_lib ,ע����һЩ����
	luaL_requiref(sL, "cell.system", cell_system_lib, 0);
	
	lua_pop(sL,1);

	lua_pushstring(sL, main_lua);
	lua_setglobal(sL, "maincell");  //����maincell=main_lua

	//cell_new�л�ִ��system_lua��Ӧ�ļ�,��������е�start()����  �Ὣ��Ϣ����Ļص�������ջ
	struct cell * sys = cell_new(sL, system_lua);
	if (sys == NULL) {
		return 0;
	}
	
	scheduler_starttask(sL);

	//����timer
	struct timer * t = lua_newuserdata(L, sizeof(*t));
	timer_init(t,sys,gmq);

	//�����߳�,ѭ������
	_start(gmq,t);
	
	cell_close(sys);

	return 0;
}

