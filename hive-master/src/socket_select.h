#ifndef poll_socket_select_h
#define poll_socket_select_h

#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>

#define _WIN32_WINNT 0x0501

#include <winsock2.h>
#include <ws2tcpip.h>


  
#define DEFAULT_CAP 16     //Ĭ�ϳ�ʼ����

struct socket_fd {
	int fd;
	void * ud;
	bool write;
};


struct select_pool {
	int select_n;	//�������������������е���ʼλ��
	int cap;		//socket_fd��������
	int socket_n;	//���������һ����Ч���ݵ�λ��

	//���socket_fd������
	struct socket_fd * fd;
};


//��������
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

//����struct select_pool 
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

//����  ���� free
static struct select_pool *
sp_release(struct select_pool *sp) {
	if (sp) {
		free(sp->fd);
		free(sp);
	}
	return NULL;
}

//���������
static int 
sp_add(struct select_pool *sp, int sock, void *ud) {

	//�ж�����
	if (sp->socket_n >= sp->cap) {
		_expand(sp);
	}

	//������Чλ��+1
	int n = sp->socket_n++;
	
	sp->fd[n].fd = sock;
	sp->fd[n].ud = ud;

	//�Ƿ�Ҫ������д�¼�
	sp->fd[n].write = false;
	return 0;
}

//��ȥ������
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
		//������Чλ��-1
		sp->socket_n--;
	}
}

//�޸�������д����
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

//����select����    n����ÿ�μ���������������
//struct event *e�Ƿ��صĻ�Ծ�������ķ�װ
static int 
sp_wait(struct select_pool *sp, struct event *e, int n, int timeout) {
	struct timeval ti;

	//����select()��ʱʱ��
	ti.tv_sec = timeout / 1000;
	ti.tv_usec = (timeout % 1000) * 1000;

	if (n > FD_SETSIZE) {
		n = FD_SETSIZE;
	}

	for (;;) {
		fd_set rd, wt;
		FD_ZERO(&rd);
		FD_ZERO(&wt);

		//ÿ��ѡȡn������������
		int i;
		for (i=0;i<n;i++) {
			int idx = sp->select_n+i;
			
			if (idx >= sp->socket_n) 
				break;

			//�����ɶ��¼�
			FD_SET(sp->fd[idx].fd, &rd);

			//������д�¼�
			if (sp->fd[idx].write) {
				FD_SET(sp->fd[idx].fd, &wt);
			}
		}

		//����select����
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

			//��ʼλ��
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

					//���ػ�Ծ�¼�
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

//���÷�����
static void
sp_nonblocking(int sock) {
	unsigned long flag=1; 
	ioctlsocket(sock,FIONBIO,&flag);
}

#endif
