#include "hive_socket_lib.h"
#include "socket_poll.h"

#include "lua.h"
#include "lauxlib.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#define MAX_ID 0x7fffffff
#define DEFAULT_SOCKET 128
#define READ_BUFFER 4000
#define MAX_EVENT 32
#define BACKLOG 32

//表示为被使用
#define STATUS_INVALID 0

//想要调用close关闭描述符，但描述符对应的写缓冲区中还要数据，
//为 欲关闭状态
#define STATUS_HALFCLOSE 1

//添加到了struct socket_pool中,并添加到了epoll中
#define STATUS_SUSPEND 2

//描述符 写缓冲区节点
struct write_buffer {
	struct write_buffer * next;
	//缓冲区起始位置
	char *ptr;

	//大小
	size_t sz;

	
	void *buffer;
};

//对描述符的封装
struct socket {
	int fd;			//socket描述符
	int id;			//应用层对应的id标号，确保不会重复

	short status;	//设置状态标记  ,是否被使用，
	
	short listen;	//是否是监听套接字
	struct write_buffer * head;
	struct write_buffer * tail;
};
 
//存放 管理 socket*指针
struct socket_pool {
	//对于epoll来说是int	epoll_create()的返回值
	poll_fd fd;			
	
	struct event ev[MAX_EVENT];		//调用epoll之后得到的活跃描述符
	
	int id;		//应用层对应的id标号，确保不会重复,初始化时为 1

	int count;	 //实际存储数量
	int cap;     //容量
	
	struct socket ** s;		//存储socket *的指针数组,存储指针有利于复制元素
};

//初始化  socket_pool
static int
linit(lua_State *L) {
	struct socket_pool * sp = lua_touserdata(L, lua_upvalueindex(1));
	if (sp->s) {
		return luaL_error(L, "Don't init socket twice");
	}
	//对于epoll来说，就是调用epoll_create
	sp->fd = sp_create();
	
	sp->count = 0;
	sp->cap = DEFAULT_SOCKET;
	sp->id = 1;
	sp->s = malloc(sp->cap * sizeof(struct socket *));
	int i;
	
	for (i=0;i<sp->cap;i++) {
		sp->s[i] = malloc(sizeof(struct socket));
		memset(sp->s[i],0, sizeof(struct socket));
	}
	return 0;
}


//从lua参数中获取struct socket_pool *
static inline struct socket_pool *
get_sp(lua_State *L) {
	struct socket_pool * pool = lua_touserdata(L, lua_upvalueindex(1));
	if (pool->s == NULL) {
		luaL_error(L, "Init socket first");
	}
	return pool;
}

//退出
static int
lexit(lua_State *L) {
	struct socket_pool * pool = lua_touserdata(L, 1);
	int i;
	if (pool->s) {
		for (i=0;i<pool->cap;i++) {
			//关闭有效的描述符
			if (pool->s[i]->status != STATUS_INVALID && pool->s[i]->fd >=0) {
				//调用的close()函数
				closesocket(pool->s[i]->fd);
			}
			free(pool->s[i]);
		}
		free(pool->s);
		pool->s = NULL;
	}
	pool->cap = 0;
	pool->count = 0;
	if (!sp_invalid(pool->fd)) {
					//close()
		pool->fd = sp_release(pool->fd);
	}
	pool->id = 1;

	return 0;
}

//扩大  socket_pool  容量 
static void
expand_pool(struct socket_pool *p) {
	struct socket ** s = malloc(p->cap * 2 * sizeof(struct socket *));
	memset(s, 0, p->cap * 2 * sizeof(struct socket *));
	int i;

	//复制原来的
	for (i=0;i<p->cap;i++) {
		int nid = p->s[i]->id % (p->cap *2);
		assert(s[nid] == NULL);
		s[nid] = p->s[i];
	}
	for (i=0;i<p->cap * 2;i++) {
		if (s[i] == NULL) {
			s[i] = malloc(sizeof(struct socket));
			memset(s[i],0,sizeof(struct socket));
		}
	}
	free(p->s);
	p->s = s;
	p->cap *=2;
}

//往socket_pool中添加描述符 sock
static int
new_socket(struct socket_pool *p, int sock) {
	int i;
	//扩容
	if (p->count >= p->cap) {
		expand_pool(p);
	}

	
	for (i=0;i<p->cap;i++) {
		int id = p->id + i;
		int n = id % p->cap;
		struct socket * s = p->s[n];
		
		if (s->status == STATUS_INVALID) {
			
			//添加到epoll管理
			if (sp_add(p->fd, sock, s)) {
				goto _error;
			}

			//修改状态
			s->status = STATUS_SUSPEND;
			
			s->listen = 0; //是否是监听套接字

			//设置描述符非阻塞
			sp_nonblocking(sock);
			
			int keepalive = 1; 
			setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive , sizeof(keepalive));

			s->fd = sock;
			s->id = id;
			p->count++;
			p->id = id + 1;
			
			if (p->id > MAX_ID) {
				p->id = 1;
			}
			assert(s->head == NULL && s->tail == NULL);

			//返回应用层维护的id
			return id;
		}
	}
_error:
	closesocket(sock);
	return -1;
}

//调用socket,connect函数,并将描述符加入到socket_pool中,返回 socket描述符对应的应用层序号
static int
lconnect(lua_State *L) {
	int status;
	
	struct addrinfo ai_hints;
	
	struct addrinfo *ai_list = NULL;
	struct addrinfo *ai_ptr = NULL;

	struct socket_pool * pool = get_sp(L);

	//获取主机
	const char * host = luaL_checkstring(L,1);

	//获取端口
	const char * port = luaL_checkstring(L,2);

	memset( &ai_hints, 0, sizeof( ai_hints ) );
	
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	ai_hints.ai_protocol = IPPROTO_TCP;

	status = getaddrinfo( host, port, &ai_hints, &ai_list );
	
	if ( status != 0 ) {
		return 0;
	}
	int sock= -1;

	
	for	( ai_ptr = ai_list;	ai_ptr != NULL;	ai_ptr = ai_ptr->ai_next ) {
		//调用socket函数,得到文件描述符
		sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
		if ( sock < 0 ) {
			continue;
		}
		//连接
		status = connect( sock,	ai_ptr->ai_addr, ai_ptr->ai_addrlen	);
		if ( status	!= 0 ) {
			close(sock);
			sock = -1;
			continue;
		}
		break;
	}

	freeaddrinfo( ai_list );

	if (sock < 0) {
		return 0;
	}

	//将连接描述符加入到pool中管理，会加入到epoll中
	int fd = new_socket(pool, sock);

	//返回对应的应用层id标号
	lua_pushinteger(L,fd);
	return 1;
}


//从socket_pool *p中移除socket,并且关闭对应的socket描述符

//在客户端主动断开连接，即read返回0的时候调用
static void
force_close(struct socket *s, struct socket_pool *p) {

	struct write_buffer *wb = s->head;
	//销毁写的缓冲区数据
	while (wb) {
		struct write_buffer *tmp = wb;
		wb = wb->next;
		free(tmp->buffer);
		free(tmp);
	}
	s->head = s->tail = NULL;
	
	s->status = STATUS_INVALID; //标记状态为未使用

	if (s->fd >=0 ) {
		//从epoll中移除管理
		sp_del(p->fd, s->fd);

		//调用close()函数
		closesocket(s->fd);

		s->fd = -1;
	}
	--p->count;
}


//关闭描述符
static int
lclose(lua_State *L) {
	//获得lua传递的socket_pool参数
	struct socket_pool * p = get_sp(L);

	//获得应用层id号
	int id = luaL_checkinteger(L,1);

	//获得对应位置
	struct socket * s = p->s[id % p->cap];
	
	if (id != s->id) {
		return luaL_error(L, "Close invalid socket %d", id);
	}
	if (s->status == STATUS_INVALID) {
		return 0;
	}
	
	//如果对应的写缓冲区为空，直接关闭,
	if (s->head == NULL) {
		force_close(s,p);
	} else {
		//设置状态标记
		s->status = STATUS_HALFCLOSE;
	}
	return 0;
}

//accept_result push_result中调用
static inline void
result_n(lua_State *L, int n) {
	//int lua_rawgeti (lua_State *L, int index, lua_Integer n);
	//把 t[n] 的值压栈， 这里的 t 是指给定索引处的表。


	//栈元素  {0={}} {} 

	//开始时  n=1

	//{0={}} {} nil
	lua_rawgeti(L,1,n);
	if (lua_istable(L,-1)) {
		return;
	}
	
	//弹出一个元素
	//{0={}} {}
	lua_pop(L,1);

	//{0={}} {} nil
	lua_rawgeti(L,2,n);
	if (!lua_istable(L,-1)) {
		//{0={}} {}
		lua_pop(L,1);

		//{0={}} {} {}
		lua_newtable(L);
	}

	//{0={}} {} {}<-{}
	lua_pushvalue(L,-1);

	//result[n]={}
	lua_rawseti(L,1, n);
	// {0={},idx={}}  {} {}
}

//移除表中n之后的元素
static inline void
remove_after_n(lua_State *L, int n) {
	//栈元素可能是 {0={},1={},2={}3={} .....}  {}

	//返回给定索引处值的固有“长度”,即获得表的长度
	int t = lua_rawlen(L,1);
	int i;
	
 	for (i=n;i<=t;i++) {
		//{0={},1={},2={},3={} .....}  {}   i={}
		lua_rawgeti(L,1,i);
		if (lua_istable(L,-1)) {
			//{0={},1={},2={},3={} .....}  { i={} }  
			lua_rawseti(L,2,i);
		} else {
			lua_pop(L,1);
		}

		//{0={},1={},2={},3={} .....}  { i={} }  nil
		lua_pushnil(L);
		
		lua_rawseti(L,1,i);
	}
}

//连接描述符可读时 调用此函数处理
static int
push_result(lua_State *L, int idx, struct socket *s, struct socket_pool *p) {
		//{0={}} {}  
		
	// 循环次数,返回
	int ret = 0;


	//一次没读完，会不断的循环读取
	for (;;) {
		
		//读的缓冲区
		char * buffer = malloc(READ_BUFFER);
		int r = 0;

		//读取数据
		for (;;) {

			//读取数据
			r = recv(s->fd, buffer, READ_BUFFER,0);
			if (r == -1) {
				switch(errno) {
				case EAGAIN:
					free(buffer);
					return ret;
					
				case EINTR:
					continue;
				}
				
				r = 0;
				break;
			}
			break;
		}
		
		//如果客户关闭
		if (r == 0) {

			//将客户对应的struct socket从socket_pool中移除，关闭对应的描述符
			force_close(s,p);
			free(buffer);
			buffer = NULL;
		}

		//如果是欲关闭状态，就不再接受数据
		if (s->status == STATUS_HALFCLOSE)
		{
			free(buffer);
		} 
		else 
		{

			//调用之后 {0={},idx={}}  {} {}
			result_n(L, idx);
			
			++ret;
			
			++idx;
			
			lua_pushinteger(L, s->id);

			//t[1]=s->id

			 {0={},idx={1=s->sid}}  {} {1=s->sid}
			lua_rawseti(L, -2, 1);

			//r是读到的数据长度
			lua_pushinteger(L, r);

			//t[2]=r
			// {0={},idx={1=s->sid,2=r}}  {} {1=s->sid,2=r}
			lua_rawseti(L, -2, 2);

			lua_pushlightuserdata(L, buffer);

			//t[3]=buffer
			// {0={},idx={1=s->sid,2=r,3=buffer}}  {} {1=s->sid,2=r,3=buffer}
			lua_rawseti(L, -2, 3);

		// {0={},idx={1=s->sid,2=r,3=buffer}}  {}
			lua_pop(L,1);
		}
		
		//如果读完了,会返回
		if (r < READ_BUFFER)
			return ret;
	}
}

//接收连接。调用accept函数
//调用epoll后，若监听描述符可读，调用此函数
static int
accept_result(lua_State *L, int idx, struct socket *s, struct socket_pool *p) {
	int ret = 0;
	for (;;) {
		struct sockaddr_in remote_addr;
		socklen_t len = sizeof(struct sockaddr_in);

		//调用accept函数
		int client_fd = accept(s->fd , (struct sockaddr *)&remote_addr ,  &len);

		//直到此处函数返回
		if (client_fd < 0) {
			
			return ret;
		}

		//将新连接的描述符添加到sock pool   p中，加入到epoll中
		//id是新添加的描述符对应的应用层id
		int id = new_socket(p, client_fd);
		if (id < 0) {
			return ret;
		}
	                 //1      2    3   4
	//调用之后栈元素  {0={},1={}}  {} {}   其中 1的{}和 3是一样的  2的{}和4是一样的，
	//修改后会相互影响
		result_n(L, idx);
		
		++ret;
		++idx;

		//{0={},idx={}}  {} {} id
		lua_pushinteger(L, s->id);

		//调用之后  {0={},1={1=s->id}}  {} {1=s->id}  s->id是产生活跃事件的应用层id
		lua_rawseti(L, -2, 1);

		//调用之后  {0={},1={}}  {} {1=s->id} id
		lua_pushinteger(L, id);    //id是新建的应用层id

		//调用之后  {0={},1={1=s->id,2=id}}  {} {1=s->id,2=id}
		lua_rawseti(L, -2, 2);
		
		lua_pushstring(L, inet_ntoa(remote_addr.sin_addr));

		//调用之后  {0={},1={1=s->id,2=id,3="......."}}  {} {1=s->id,2=id,3="......."}
		lua_rawseti(L, -2, 3);

		// {0={},idx={1=s->id,2=id,3="......."}}  {} 
		lua_pop(L,1);
	}
}


//将描述符对应的发送缓冲区数据全部发送，并且不再监听可写事件
static void
sendout(struct socket_pool *p, struct socket *s) {
	while (s->head) {
		struct write_buffer * tmp = s->head;
		for (;;) {
			int sz = send(s->fd, tmp->ptr, tmp->sz,0);
			if (sz < 0) {
				switch(errno) {
				case EINTR:
					continue;
				case EAGAIN:
					return;
				}
				force_close(s,p);
				return;
			}
			if (sz != tmp->sz) {
				tmp->ptr += sz;
				tmp->sz -= sz;
				return;
			}
			break;
		}
		s->head = tmp->next;
		free(tmp->buffer);
		free(tmp);
	}
	s->tail = NULL;
	//不再关注可写事件
	sp_write(p->fd, s->fd, s, false);
}
 
//调用epoll_wait,并对活跃的描述符处理事件   

//lua层调用时 csocket.poll({})

static int
lpoll(lua_State *L) {
	struct socket_pool * p = get_sp(L);

	//栈元素 {}
	luaL_checktype(L,1,LUA_TTABLE);

	//luaL_optinteger如果2位置参数为空，返回100,即默认超时时间是 100
	int timeout = luaL_optinteger(L,2,100);

	//{}
	lua_settop(L,1);
	//{} nil
	lua_rawgeti(L,1,0);

	if (lua_isnil(L,-1)) {
		//{}
		lua_pop(L,1);

		//{} {}
		lua_newtable(L);

		//{} {}<-{}
		
		//添加副本，对副本的修改会影响到原来的
		lua_pushvalue(L,-1)
		
		//result[0]={}
		//{0={}} {}  
		lua_rawseti(L,1,0);
	} else {
		luaL_checktype(L,2,LUA_TTABLE);
	}

	//调用epoll_wait ,得到活跃事件数组  p->ev
	int n = sp_wait(p->fd, p->ev, MAX_EVENT, timeout); 
	int i;
	
	int t = 1;
	for (i=0;i<n;i++) {
		struct event *e = &p->ev[i];
		//可读
		if (e->read) {
			struct socket * s= e->s;
			
			if (s->listen) {
				//如果是监听描述符可读
				//调用之后  {0={},1={1=s->id,2=id,3="......."}}  {} 
				t += accept_result(L, t, e->s, p);
			} else {

				// {0={},idx={1=s->sid,2=r,3=buffer}}  {}
				t += push_result(L, t, e->s, p);
			}
		}

		//可写
		if (e->write) {
			struct socket *s = e->s;
			//将描述符对应的发送缓冲区数据全部发送，并且不再监听可写事件
			sendout(p, s);

			//如果之前关闭描述符时，因为写的缓冲区中有数据，只是 标记要关闭
 			if (s->status == STATUS_HALFCLOSE && s->head == NULL) {
				//关闭
				force_close(s, p);
			}
		}
	}

	remove_after_n(L,t);

	//返回元素的个数
	lua_pushinteger(L, t-1);
	return 1;
}

//发送数据,通过应用层描述符(对应连接套接字)发送数据到客户端
static int
lsend(lua_State *L) {

	//获得socket_pool结构体
	struct socket_pool * p = get_sp(L);

	//获得lua传递的struct socket的应用层id
	int id = luaL_checkinteger(L,1);

	//数据大小
	int sz = luaL_checkinteger(L,2);

	//数据
	void * msg = lua_touserdata(L,3);

	//通过id获取到 位置
	struct socket * s = p->s[id % p->cap];

	if (id != s->id) {
		free(msg);
		return luaL_error(L,"Write to invalid socket %d", id);
	}

	//如果描述符是没有添加到epoll中管理,销毁数据
	if (s->status != STATUS_SUSPEND) {
		free(msg);
//		return luaL_error(L,"Write to closed socket %d", id);
		return 0;
	}

	
	//如果描述符对应写缓冲区有数据,将现在要发送的数据添加到缓冲区尾部,直接返回 
	if (s->head) {
		
		struct write_buffer * buf = malloc(sizeof(*buf));
		buf->ptr = msg;
		buf->buffer = msg;
		buf->sz = sz;
		assert(s->tail != NULL);
		assert(s->tail->next == NULL);

		//加入到尾部
		buf->next = s->tail->next;
		s->tail->next = buf;
		s->tail = buf;
		return 0;
	}

	//否则，
	char * ptr = msg;

	for (;;) {
		//直接发送
		int wt = send(s->fd, ptr, sz,0);
		if (wt < 0) {
			switch(errno) {
			case EINTR:
				continue;
			}
			break;
		}
		//如果数据全部发送完毕，就返回
		if (wt == sz) {
			return 0;
		}
		sz-=wt;
		ptr+=wt;

		break;
	}

	//将未发送完的数据保存到缓冲区中
	struct write_buffer * buf = malloc(sizeof(*buf));
	buf->next = NULL;
	buf->ptr = ptr;
	buf->sz = sz;
	buf->buffer = msg;
	s->head = s->tail = buf;

	//关注描述符可写事件
	sp_write(p->fd, s->fd, s, true);

	return 0;
}

// buffer support

struct socket_buffer {
	int size;
	int head;
	int tail;
};

//struct socket_buffer只是该缓冲区的头部  int sz是实际的存储数据的位置

//创建一个socket_buffer  大小为 sz
static struct socket_buffer *
new_buffer(lua_State *L, int sz) {
	//sizeof(*buffer) + sz
	struct socket_buffer * buffer = lua_newuserdata(L, sizeof(*buffer) + sz);
	buffer->size = sz;
	buffer->head = 0;
	buffer->tail = 0;

	return buffer;
}

//复制数据
static void
copy_buffer(struct socket_buffer * dest, struct socket_buffer * src) {

	char * ptr = (char *)(src+1);
	
	if (src->tail >= src->head) {

		//大小
		int sz = src->tail - src->head;

		memcpy(dest+1, ptr+ src->head, sz);

		dest->tail = sz;
	} 
	else 
	{
		char * d = (char *)(dest+1);
		int part = src->size - src->head;
		memcpy(d, ptr + src->head, part);
		memcpy(d + part, ptr , src->tail);
		dest->tail = src->tail + part;
	}
}

//在socket_buffer中添加数据
static void
append_buffer(struct socket_buffer * buffer, char * msg, int sz) {
	char * dst = (char *)(buffer + 1);

	//如果可以容纳添加的数据
	if (sz + buffer->tail < buffer->size) {
		memcpy(dst + buffer->tail , msg, sz);
		buffer->tail += sz;
	} else {
		int part = buffer->size - buffer->tail;
		
		memcpy(dst + buffer->tail , msg, part);

		memcpy(dst, msg + part, sz - part);
		buffer->tail = sz - part;
	}
}


//在socket_buffer中添加数据
static int
lpush(lua_State *L) {
	struct socket_buffer * buffer = lua_touserdata(L, 1);

	//计算出socket_buffer中已有数据的大小
	int bytes = 0;
	if (buffer) {
		bytes = buffer->tail - buffer->head;
		if (bytes < 0) {
			bytes += buffer->size;
		}
	}

	//获得lua传递的数据
	void * msg = lua_touserdata(L,2);
	if (msg == NULL) {
		lua_settop(L,1);
		lua_pushinteger(L,bytes);
		return 2;
	}

	//获得数据的大小信息
	int sz = luaL_checkinteger(L,3);

	if (buffer == NULL) {
		struct socket_buffer * nbuf = new_buffer(L, sz * 2);
		append_buffer(nbuf, msg, sz);
	}
	//如果大小不够,创建新的
	else if (sz + bytes >= buffer->size) 
	{
		struct socket_buffer * nbuf = new_buffer(L, (sz + bytes) * 2);
		copy_buffer(nbuf, buffer);
		append_buffer(nbuf, msg, sz);
	}
	//直接添加
	else 
	{
		lua_settop(L,1);
		append_buffer(buffer, msg, sz);
	}
	lua_pushinteger(L, sz + bytes);
	free(msg);
	return 2;
}

//弹出数据  
static int
lpop(lua_State *L) {
	struct socket_buffer * buffer = lua_touserdata(L, 1);
	if (buffer == NULL) {
		return 0;
	}

	//得到要弹出的数据的大小
	int sz = luaL_checkinteger(L, 2);

	//得到现有长度
	int	bytes = buffer->tail - buffer->head;
	if (bytes < 0) {
		bytes += buffer->size;
	}

	if (sz > bytes || bytes == 0) {
		lua_pushnil(L);
		lua_pushinteger(L, bytes);
		return 2;
	}

	if (sz == 0) {
		sz = bytes;
	}

	char * ptr = (char *)(buffer+1);
	if (buffer->size - buffer->head >=sz) {
		lua_pushlstring(L, ptr + buffer->head, sz);
		buffer->head+=sz;
	} else {
		luaL_Buffer b;
		luaL_buffinit(L, &b);
		luaL_addlstring(&b, ptr + buffer->head, buffer->size - buffer->head);
		buffer->head = sz - (buffer->size - buffer->head);
		luaL_addlstring(&b, ptr, buffer->head);
		luaL_pushresult(&b);
	}

	bytes -= sz;

	lua_pushinteger(L,bytes);
	if (bytes == 0) {
		buffer->head = buffer->tail = 0;
	}
	
	return 2;
}

static inline int
check_sep(struct socket_buffer *buffer, int from, const char * sep, int sz) {
	const char * ptr = (const char *)(buffer+1);
	int i;
	for (i=0;i<sz;i++) {
		int index = from + i;
		if (index >= buffer->size) {
			index %= buffer->size;
		}
		
		if (ptr[index] != sep[i]) {
			return 0;
		}
	}
	return 1;
}

//
static int
lreadline(lua_State *L) {
	struct socket_buffer * buffer = lua_touserdata(L, 1);
	if (buffer == NULL) {
		return 0;
	}
	size_t len = 0;
	const char *sep = luaL_checklstring(L,2,&len);
	
	int read = !lua_toboolean(L,3);

	//得到现有长度 
	int	bytes = buffer->tail - buffer->head;

	if (bytes < 0) {
		bytes += buffer->size;
	}


	int i;
	for (i=0;i<=bytes-(int)len;i++) {
		int index = buffer->head + i;
		if (index >= buffer->size) {
			index -= buffer->size;
		}

		if (check_sep(buffer, index, sep, (int)len)) {
			
			if (read == 0) {
				lua_pushboolean(L,1);
			} else {
				if (i==0) {
					lua_pushlstring(L, "", 0);
				} else {
					const char * ptr = (const char *)(buffer+1);
					if (--index < 0) {
						index = buffer->size -1;
					}
					if (index < buffer->head) {
						luaL_Buffer b;
						luaL_buffinit(L, &b);
						luaL_addlstring(&b, ptr + buffer->head, buffer->size-buffer->head);
						luaL_addlstring(&b, ptr, index+1);
						luaL_pushresult(&b);
					} else {
						lua_pushlstring(L, ptr + buffer->head, index-buffer->head+1);
					}
					++index;
				}
				index+=len;
				if (index >= buffer->size) {
					index-=buffer->size;
				}
				buffer->head = index;
			}
			return 1;
		}
	}
	return 0;
}

//分配msg内存
static int
lsendpack(lua_State *L) {
	size_t len = 0;
	const char * str = luaL_checklstring(L, 1, &len);
	lua_pushinteger(L, (int)len);
	void * msg = malloc(len);
	memcpy(msg, str, len);
	lua_pushlightuserdata(L, msg);

	return 2;
}

//销毁msg内存
static int
lfreepack(lua_State *L) {
	void * msg = lua_touserdata(L,1);
	free(msg);
	return 0;
}

// server support
//调用 socket  bind  listen,加入到epoll,struct socket_pool中
///返回监听套接字对应的应用层序号
static int
llisten(lua_State *L) {
	struct socket_pool * p = get_sp(L);
	// only support ipv4
	int port = 0;
	size_t len = 0;
	const char * name = luaL_checklstring(L, 1, &len);
	char binding[len+1];
	memcpy(binding, name, len+1);
	char * portstr = strchr(binding,':');
	uint32_t addr = INADDR_ANY;
	if (portstr == NULL) {
		port = strtol(binding, NULL, 10);
		if (port <= 0) {
			return luaL_error(L, "Invalid address %s", name);
		}
	} else {
		//端口
		port = strtol(portstr + 1, NULL, 10);
		if (port <= 0) {
			return luaL_error(L, "Invalid address %s", name);
		}
		portstr[0] = '\0';
		addr=inet_addr(binding);
	}

	//调用socket函数
	int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

	//加入到socket_pool中，会加入到epoll中管理
	int id = new_socket(p, listen_fd);
	if (id < 0) {
		return luaL_error(L, "Create socket %s failed", name);
	}
	
	struct socket * s = p->s[id % p->cap];


	//标记为监听套接字
	s->listen = 1;

	//设置地址重复利用
	int reuse = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(int));

	struct sockaddr_in my_addr;
	memset(&my_addr, 0, sizeof(struct sockaddr_in));
	my_addr.sin_family = AF_INET;
	my_addr.sin_port = htons(port);
	my_addr.sin_addr.s_addr = addr;

	//绑定
	if (bind(listen_fd, (struct sockaddr *)&my_addr, sizeof(struct sockaddr)) == -1) {
		force_close(s,p);
		return luaL_error(L, "Bind %s failed", name);
	}

	//设置为被动
	if (listen(listen_fd, BACKLOG) == -1) {
		force_close(s,p);
		return luaL_error(L, "Listen %s failed", name);
	}

	lua_pushinteger(L, id);

	return 1;
}

int 
socket_lib(lua_State *L) {
	luaL_checkversion(L);
	luaL_Reg l[] = {
		{ "init", linit },
		{ "connect", lconnect },
		{ "close", lclose },
		{ "poll", lpoll },
		{ "send", lsend },
		{ "sendpack", lsendpack },
		{ "freepack", lfreepack },
		{ "push", lpush },
		{ "pop", lpop },
		{ "readline", lreadline },
		{ "listen", llisten },
		{ NULL, NULL },
	};

	//创建一张表，足够容纳数组l，但并不填充,与luaL_setfuncs配合
	luaL_newlibtable(L,l);

	//创建struct socket_pool
	struct socket_pool *sp = lua_newuserdata(L, sizeof(*sp));
	
	memset(sp, 0, sizeof(*sp));

	//调用epoll_create().初始化sp->fd
	sp->fd = sp_init();

	//创建一张空表 
	lua_newtable(L);
	
	lua_pushcfunction(L, lexit);

	//t['__gc']=lexit
	lua_setfield(L, -2, "__gc");

	//设置为struct socket_pool的元表
	lua_setmetatable(L, -2);

	luaL_setfuncs(L,l,1);
	return 1;
}


