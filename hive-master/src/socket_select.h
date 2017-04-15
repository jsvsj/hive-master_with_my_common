#ifndef poll_socket_select_h
#define poll_socket_select_h

#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>

#define _WIN32_WINNT 0x0501

#include <winsock2.h>
#include <ws2tcpip.h>


  
#define DEFAULT_CAP 16     //默认初始容量

struct socket_fd {
	int fd;
	void * ud;
	bool write;
};


struct select_pool {
	int select_n;	//监听的描述符在数组中的起始位置
	int cap;		//socket_fd数组容量
	int socket_n;	//数组中最后一个有效数据的位置

	//存放socket_fd的数组
	struct socket_fd * fd;
};


//扩大容量
static void
_expand(struct select_pool *p) {
	p->fd = realloc(p->fd, p->cap * 2 * sizeof(struct socket_fd));
	p->cap *= 2;
	p->select_n = 0;
	p->socket_n = 0;
}

static bool 
sp_invalid(struct select_pool *sp) {
	return sp == NULL;
}

static struct select_pool *
sp_init() {
	return NULL;
}

//创建struct select_pool 
static struct select_pool *
sp_create() {
	struct select_pool * sp = malloc(sizeof(*sp));
	sp->cap = DEFAULT_CAP;
	sp->fd = malloc(sp->cap * sizeof(struct socket_fd));
	sp->select_n = 0;
	sp->socket_n = 0;

	WSADATA wsaData;
	WSAStartup(MAKEWORD(2,2), &wsaData);

	return sp;
}

//销毁  调用 free
static struct select_pool *
sp_release(struct select_pool *sp) {
	if (sp) {
		free(sp->fd);
		free(sp);
	}
	return NULL;
}

//添加描述符
static int 
sp_add(struct select_pool *sp, int sock, void *ud) {

	//判断容量
	if (sp->socket_n >= sp->cap) {
		_expand(sp);
	}

	//数组有效位置+1
	int n = sp->socket_n++;
	
	sp->fd[n].fd = sock;
	sp->fd[n].ud = ud;

	//是否要监听可写事件
	sp->fd[n].write = false;
	return 0;
}

//除去描述符
static void 
sp_del(struct select_pool *sp, int sock) {
	int i;
	bool move = false;
	for (i=0;i<sp->socket_n;i++) {
		if (move) {
			sp->fd[i-1] = sp->fd[i];
		} else if (sp->fd[i].fd == sock) {
			move = true;
		}
	}
	
	if (move) {
		//数组有效位置-1
		sp->socket_n--;
	}
}

//修改描述符写属性
static void 
sp_write(struct select_pool *sp, int sock, void *ud, bool enable) {
	int i;
	for (i=0;i<sp->socket_n;i++) {
		if (sp->fd[i].fd == sock) {
			sp->fd[i].write = enable;
			return;
		}
	}
}

//调用select函数    n代表每次监听的描述符个数
//struct event *e是返回的活跃描述符的封装
static int 
sp_wait(struct select_pool *sp, struct event *e, int n, int timeout) {
	struct timeval ti;

	//设置select()超时时间
	ti.tv_sec = timeout / 1000;
	ti.tv_usec = (timeout % 1000) * 1000;

	if (n > FD_SETSIZE) {
		n = FD_SETSIZE;
	}

	for (;;) {
		fd_set rd, wt;
		FD_ZERO(&rd);
		FD_ZERO(&wt);

		//每次选取n个描述符监听
		int i;
		for (i=0;i<n;i++) {
			int idx = sp->select_n+i;
			
			if (idx >= sp->socket_n) 
				break;

			//监听可读事件
			FD_SET(sp->fd[idx].fd, &rd);

			//监听可写事件
			if (sp->fd[idx].write) {
				FD_SET(sp->fd[idx].fd, &wt);
			}
		}

		//调用select函数
		int ret = select(i+1, &rd, &wt, NULL, &ti);
		
		if (ret <= 0) {
			ti.tv_sec = 0;
			ti.tv_usec = 0;

			sp->select_n += n;
			if (sp->select_n >= sp->socket_n) {
				sp->select_n = 0;
				return ret;
			}
		} else {
		
			int t = 0;

			//起始位置
			int from = sp->select_n;
			sp->select_n += n;
			if (sp->select_n >= sp->socket_n) {
				sp->select_n = 0;
			}
			
			for (i=0;i<n;i++) {
				int idx = from+i;
				if (idx >= sp->socket_n) 
					break;
				
				int fd = sp->fd[idx].fd;
				bool read_flag = FD_ISSET(fd, &rd);
				bool write_flag = FD_ISSET(fd, &wt);
				
				if (read_flag || write_flag) {

					//返回活跃事件
					e[t].s = sp->fd[idx].ud;
					e[t].read = read_flag;
					e[t].write = write_flag;
					++t;
					if (t == ret)
						break;
				}
			}
			assert(t==ret);
			return t;
		}
	}
}

//设置非阻塞
static void
sp_nonblocking(int sock) {
	unsigned long flag=1; 
	ioctlsocket(sock,FIONBIO,&flag);
}

#endif
