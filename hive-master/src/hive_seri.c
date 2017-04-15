#include <stdlib.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "hive_seri.h"
#include "hive_cell.h"

#define TYPE_NIL 0
#define TYPE_BOOLEAN 1
// hibits 0 false 1 true
#define TYPE_NUMBER 2
// hibits 0 : 0 , 1: byte, 2:word, 4: dword, 8 : double
#define TYPE_USERDATA 3
#define TYPE_SHORT_STRING 4
// hibits 0~31 : len
#define TYPE_LONG_STRING 5
#define TYPE_TABLE 6
#define TYPE_CELL 7

#define MAX_COOKIE 32
#define COMBINE_TYPE(t,v) ((t) | (v) << 3)

#define BLOCK_SIZE 128
#define MAX_DEPTH 32

//缓冲区链表的节点
struct block {
	struct block * next;
	
	char buffer[BLOCK_SIZE];	//128  该结点能够存储的内存大小
};

//写的链表
struct write_block {
	struct block * head;	//向第一块缓冲区节点
	int len;				//缓冲区链表中存储的已经写入的所有的数据长度
	struct block * current;	//指向最后一块缓冲区节点
	int ptr;				//最后一块缓冲区节点中的buffer可写的其实位置
};


//读的链表
struct read_block {
	char * buffer;
	struct block * current;		
	int len;	//可读数据的总长度
	int ptr;	//current节点中下一个可以存储数据的起始位置,也就是该结点缓冲区已经存储的数据大小
};

//调用malloc创建struct block节点
inline static struct block *
blk_alloc(void) {
	struct block *b = malloc(sizeof(struct block));
	b->next = NULL;
	return b;
}


//将buf指针所指的大小为 sz的内存加入到write_block链表中
inline static void
wb_push(struct write_block *b, const void *buf, int sz) {
	const char * buffer = buf;

	//如果write_block 当前的缓冲区存满了，就创建一个缓冲区节点
	if (b->ptr == BLOCK_SIZE) {
_again:
		b->current = b->current->next = blk_alloc();
		b->ptr = 0;
	}
	//如果当前缓冲区的节点还能够存储sz大小的数据
	if (b->ptr <= BLOCK_SIZE - sz) {
		memcpy(b->current->buffer + b->ptr, buffer, sz);
		b->ptr+=sz;
		
		b->len+=sz;
	}
	
	else 
	{
	//如果当前缓冲区的节点还能够存储一部分大小的数据
		int copy = BLOCK_SIZE - b->ptr;
		memcpy(b->current->buffer + b->ptr, buffer, copy);
		buffer += copy;
		
		b->len += copy;
		sz -= copy;
		goto _again;
	}
}

//初始化write_block链表,b不为空的时候，作为缓冲区链表的头结点
static void
wb_init(struct write_block *wb , struct block *b) {

	//如果b为空
	if (b==NULL) {
		wb->head = blk_alloc();
		wb->len = 0;	//总数据长度
		wb->current = wb->head;
		wb->ptr = 0;

		//将长度写入头缓冲区
		wb_push(wb, &wb->len, sizeof(wb->len));
	} else {
		//如果b不为空,将b作为缓冲区的头结点
		wb->head = b;

		//获取其中存储的长度,就是数据总长度
		int * plen = (int *)b->buffer;
		int sz = *plen;
		wb->len = sz;

		//如果b还连接了其他节点
		while (b->next) {
			sz -= BLOCK_SIZE;		
			b = b->next;
		}
		//指向最后一个节点
		wb->current = b;
		//最后一块缓冲区节点可写的位置起点
		wb->ptr = sz;
	}
}

//恢复为初始状态,返回头缓冲区节点指针
static struct block *
wb_close(struct write_block *b) {
	b->current = b->head;
	b->ptr = 0;
	wb_push(b, &b->len, sizeof(b->len));
	b->current = NULL;
	return b->head;
}

//销毁write_block链表
static void
wb_free(struct write_block *wb) {
	struct block *blk = wb->head;
	while (blk) {
		struct block * next = blk->next;
		free(blk);
		blk = next;
	}
	wb->head = NULL;
	wb->current = NULL;
	wb->ptr = 0;
	wb->len = 0;
}

//初始化read_block
static int
rb_init(struct read_block *rb, struct block *b) {
	rb->buffer = NULL;
	rb->current = b;

	//初始化长度
	memcpy(&(rb->len),b->buffer,sizeof(rb->len));
	
	rb->ptr = sizeof(rb->len);
	rb->len -= rb->ptr;
	return rb->len;
}


//从read_block中读取数据。如果数据正好在一个缓冲区节点中，即就是连续存储的，就直接返回指针，不会拷贝到
//buffer中，如果要读的数据分布在不同的节点中，就将数据拷贝到buffer中，在返回buffer指针
static void *
rb_read(struct read_block *rb, void *buffer, int sz) {
	if (rb->len < sz) {
		return NULL;
	}

	//如果buffer不为空,直接返回buffer中的一段
	if (rb->buffer) {
		int ptr = rb->ptr;
		rb->ptr += sz;
		rb->len -= sz;
		return rb->buffer + ptr;
	}

	//buffer为空

	//ptr==BLOCK_SIZE说明current指向的缓冲区节点已经没有可读的数据，销毁之
	if (rb->ptr == BLOCK_SIZE) {
		struct block * next = rb->current->next;
		free(rb->current);
		rb->current = next;
		rb->ptr = 0;
	}

	//能够拷贝的数据
	int copy = BLOCK_SIZE - rb->ptr;

	
	if (sz <= copy) {
		void * ret = rb->current->buffer + rb->ptr;
		rb->ptr += sz;
		rb->len -= sz;
		return ret;
	}

	//如果sz>copy
	char * tmp = buffer;

	memcpy(tmp, rb->current->buffer + rb->ptr,  copy);
	sz -= copy;
	tmp += copy;
	rb->len -= copy;

	for (;;) {
		struct block * next = rb->current->next;
		//current中的数据已经读完，销毁
		free(rb->current);
		
		rb->current = next;

		if (sz < BLOCK_SIZE) {
			memcpy(tmp, rb->current->buffer, sz);
			rb->ptr = sz;
			rb->len -= sz;
			return buffer;
		}
		
		//sz>BLOCK_SIZE
		
		memcpy(tmp, rb->current->buffer, BLOCK_SIZE);
		sz -= BLOCK_SIZE;
		tmp += BLOCK_SIZE;
		rb->len -= BLOCK_SIZE;
	}
}


//销毁read_block链表
static void
rb_close(struct read_block *rb) {
	while (rb->current) {
		struct block * next = rb->current->next;
		free(rb->current);
		rb->current = next;
	}
	rb->len = 0;
	rb->ptr = 0;
}

//将nil添加到写的缓冲区链表中
static inline void
wb_nil(struct write_block *wb) {
	int n = TYPE_NIL;		//宏 0
	wb_push(wb, &n, 1);
}


//将boolean写入到缓冲区中
static inline void
wb_boolean(struct write_block *wb, int boolean) {
	int n = COMBINE_TYPE(TYPE_BOOLEAN , boolean ? 1 : 0);
	wb_push(wb, &n, 1);
}

//将integer写入到缓冲区中
static inline void
wb_integer(struct write_block *wb, int v) {
	if (v == 0) {
		int n = COMBINE_TYPE(TYPE_NUMBER , 0);
		wb_push(wb, &n, 1);
	} else if (v<0) {
		int n = COMBINE_TYPE(TYPE_NUMBER , 4);
		wb_push(wb, &n, 1);
		wb_push(wb, &v, 4);
	} else if (v<0x100) {
		int n = COMBINE_TYPE(TYPE_NUMBER , 1);
		wb_push(wb, &n, 1);
		uint8_t byte = (uint8_t)v;
		wb_push(wb, &byte, 1);
	} else if (v<0x10000) {
		int n = COMBINE_TYPE(TYPE_NUMBER , 2);
		wb_push(wb, &n, 1);
		uint16_t word = (uint16_t)v;
		wb_push(wb, &word, 2);
	} else {
		int n = COMBINE_TYPE(TYPE_NUMBER , 4);
		wb_push(wb, &n, 1);
		wb_push(wb, &v, 4);
	}
}

//将number写入到缓冲区中
static inline void
wb_number(struct write_block *wb, double v) {
	int n = COMBINE_TYPE(TYPE_NUMBER , 8);
	wb_push(wb, &n, 1);
	wb_push(wb, &v, 8);
}


//将指针写入到
static inline void
wb_pointer(struct write_block *wb, void *v, int type) {
	int n = type;
	wb_push(wb, &n, 1);
	wb_push(wb, &v, sizeof(v));
}

//写入字符串
static inline void
wb_string(struct write_block *wb, const char *str, int len) {
	if (len < MAX_COOKIE) {
		int n = COMBINE_TYPE(TYPE_SHORT_STRING, len);
		wb_push(wb, &n, 1);
		if (len > 0) {
			wb_push(wb, str, len);
		}
	} else {
		int n;
		if (len < 0x10000) {
			n = COMBINE_TYPE(TYPE_LONG_STRING, 2);
			wb_push(wb, &n, 1);
			uint16_t x = (uint16_t) len;
			wb_push(wb, &x, 2);
		} else {
			n = COMBINE_TYPE(TYPE_LONG_STRING, 4);
			wb_push(wb, &n, 1);
			uint32_t x = (uint32_t) len;
			wb_push(wb, &x, 4);
		}
		wb_push(wb, str, len);
	}
}


//将元素根据类型写入缓冲区
static void _pack_one(lua_State *L, struct write_block *b, int index, int depth);


//将table类型写入缓冲区   write_block
static int
wb_table_array(lua_State *L, struct write_block * wb, int index, int depth) {
	//lua_rawlen(L,index)返回index处的元素的长度
	int array_size = lua_rawlen(L,index);
	
	if (array_size >= MAX_COOKIE-1) {    			 //MAX_COOKIE  32
		int n = COMBINE_TYPE(TYPE_TABLE, MAX_COOKIE-1);
		wb_push(wb, &n, 1);				//写入类型
		wb_integer(wb, array_size);		//写入长度
	} else {
		int n = COMBINE_TYPE(TYPE_TABLE, array_size);
		wb_push(wb, &n, 1);
	}

	int i;
	for (i=1;i<=array_size;i++) {
		//将index索引处的table中的第i个元素压入到栈顶
		lua_rawgeti(L,index,i);

		//将得到的元素根据类型写入缓冲区,因为table中可以存放多种类型的数据
		_pack_one(L, wb, -1, depth);

		//将元素出栈
		lua_pop(L,1);
	}

	return array_size;
}


static void
wb_table_hash(lua_State *L, struct write_block * wb, int index, int depth, int array_size) {
	lua_pushnil(L);
	
	//从栈顶弹出一个键， 然后把索引指定的表中的一个键值对压栈 （弹出的键
//之后的 “下一” 对）。 如果表中以无更多元素， 那么 lua_next 将返回 0
	
	while (lua_next(L, index) != 0) {
		if (lua_type(L,-2) == LUA_TNUMBER) {
			lua_Number k = lua_tonumber(L,-2);
			int32_t x = (int32_t)lua_tointeger(L,-2);
			if (k == (lua_Number)x && x>0 && x<=array_size) {
				lua_pop(L,1);
				continue;
			}
		}
		_pack_one(L,wb,-2,depth);
		_pack_one(L,wb,-1,depth);

		
		lua_pop(L, 1);
	}
	wb_nil(wb);
}

static void
wb_table(lua_State *L, struct write_block *wb, int index, int depth) {
	if (index < 0) {
		index = lua_gettop(L) + index + 1;
	}
	int array_size = wb_table_array(L, wb, index, depth);
	
	wb_table_hash(L, wb, index, depth, array_size);
}

//将元素根据类型写入缓冲区
static void
_pack_one(lua_State *L, struct write_block *b, int index, int depth) {
	if (depth > MAX_DEPTH) {
		wb_free(b);
		luaL_error(L, "serialize can't pack too depth table");
	}
	//lua_type()返回给定有效索引处值的类型
	int type = lua_type(L,index);
	switch(type) {
	case LUA_TNIL:
		wb_nil(b);
		break;
	case LUA_TNUMBER: {
		int32_t x = (int32_t)lua_tointeger(L,index);
		lua_Number n = lua_tonumber(L,index);
		if ((lua_Number)x==n) {
			wb_integer(b, x);
		} else {
			wb_number(b,n);
		}
		break;
	}
	case LUA_TBOOLEAN: 
		wb_boolean(b, lua_toboolean(L,index));
		break;
	case LUA_TSTRING: {
		size_t sz = 0;
		const char *str = lua_tolstring(L,index,&sz);
		wb_string(b, str, (int)sz);
		break;
	}
	case LUA_TLIGHTUSERDATA:
		wb_pointer(b, lua_touserdata(L,index),TYPE_USERDATA);
		break;
	case LUA_TTABLE:
		wb_table(L, b, index, depth+1);
		break;
	case LUA_TUSERDATA: {
		struct cell *c = cell_fromuserdata(L, index);
		if (c) {
			cell_grab(c);
			wb_pointer(b, c, TYPE_CELL);
			break;
		} 
		// else go through
	}
	default:
		wb_free(b);
		luaL_error(L, "Unsupport type %s to serialize", lua_typename(L, type));
	}
}


//将from到栈顶的元素写入write_block
static void
_pack_from(lua_State *L, struct write_block *b, int from) {
	int n = lua_gettop(L) - from;
	int i;
	for (i=1;i<=n;i++) {
		_pack_one(L, b , from + i, 0);
	}
}


int
data_pack(lua_State *L) {
	struct write_block b;
	wb_init(&b, NULL);
	_pack_from(L,&b,0);

	//将write_block恢复初始状态，得到头缓冲区节点指针
	struct block * ret = wb_close(&b);
	
	lua_pushlightuserdata(L,ret);
	return 1;
}

static inline void
__invalid_stream(lua_State *L, struct read_block *rb, int line) {
	int len = rb->len;
	if (rb->buffer == NULL) {
		//销毁read_block链表
		rb_close(rb);
	}
	luaL_error(L, "Invalid serialize stream %d (line:%d)", len, line);
}

#define _invalid_stream(L,rb) __invalid_stream(L,rb,__LINE__)



//从read_block中读取数值类型，如int float double
static double
_get_number(lua_State *L, struct read_block *rb, int cookie) {
	switch (cookie) {
	case 0:
		return 0;
	case 1: {
		uint8_t n = 0;
		//从read_block中读取数据
		uint8_t * pn = rb_read(rb,&n,1);
		if (pn == NULL)
			_invalid_stream(L,rb);
		return *pn;
	}
	case 2: {
		uint16_t n = 0;
		uint16_t * pn = rb_read(rb,&n,2);
		if (pn == NULL)
			_invalid_stream(L,rb);
		return *pn;
	}
	case 4: {
		int n = 0;
		int * pn = rb_read(rb,&n,4);
		if (pn == NULL)
			_invalid_stream(L,rb);
		return *pn;
	}
	case 8: {
		double n = 0;
		double * pn = rb_read(rb,&n,8);
		if (pn == NULL)
			_invalid_stream(L,rb);
		return *pn;
	}
	default:
		_invalid_stream(L,rb);
		return 0;
	}
}


//从read_block中读取指针类型的数据
static void *
_get_pointer(lua_State *L, struct read_block *rb) {
	void * userdata = 0;
	void ** v = (void **)rb_read(rb,&userdata,sizeof(userdata));
	if (v == NULL) {
		_invalid_stream(L,rb);
	}
	return *v;
}

//从read_block中读取字符串
static void
_get_buffer(lua_State *L, struct read_block *rb, int len) {
	char tmp[len];
	char * p = rb_read(rb,tmp,len);
	lua_pushlstring(L,p,len);
}

static void _unpack_one(lua_State *L, struct read_block *rb, int table_index);

//从read_block中读取table类型。压栈
static void
_unpack_table(lua_State *L, struct read_block *rb, int array_size, int table_index) {
	if (array_size == MAX_COOKIE-1) {
		uint8_t type = 0;
		uint8_t *t = rb_read(rb, &type, 1);
		if (t==NULL || (*t & 7) != TYPE_NUMBER) {
			_invalid_stream(L,rb);
		}
		array_size = (int)_get_number(L,rb,*t >> 3);
	}

	//创建一张新的空表压栈   array_size表示建议的表的大小
	lua_createtable(L,array_size,0);
	
	int i;
	for (i=1;i<=array_size;i++) {
		//从read_block中读取一个字节类型的数据，压栈
		_unpack_one(L,rb, table_index);

		//等价于t[i]=v, v为栈顶的值,t为索引处的表table
		lua_rawseti(L,-2,i);
	}
	
	for (;;) {
		_unpack_one(L,rb, table_index);
		if (lua_isnil(L,-1)) {
			lua_pop(L,1);
			return;
		}
		_unpack_one(L,rb, table_index);
		lua_rawset(L,-3);
	}
}

//从read_block中读取各种类型数据，压栈
static void
_push_value(lua_State *L, struct read_block *rb, int type, int cookie,int table_index) {
	
	switch(type) {
	case TYPE_NIL:
		lua_pushnil(L);
		break;
	case TYPE_BOOLEAN:
		lua_pushboolean(L,cookie);
		break;
	case TYPE_NUMBER:
		//_get_number()从中读取数值类型，如int float double
		//将得到的数据压栈
		lua_pushnumber(L,_get_number(L,rb,cookie));
		break;
	case TYPE_USERDATA:
		lua_pushlightuserdata(L,_get_pointer(L,rb));
		break;
	case TYPE_CELL: 
	{
		struct cell * c = _get_pointer(L,rb);
		cell_touserdata(L, table_index, c);
		cell_release(c);
		break;
	}
	case TYPE_SHORT_STRING:
		_get_buffer(L,rb,cookie);
		break;
	case TYPE_LONG_STRING: {
		uint32_t len;
		if (cookie == 2) {
			uint16_t *plen = rb_read(rb, &len, 2);
			if (plen == NULL) {
				_invalid_stream(L,rb);
			}
			_get_buffer(L,rb,(int)*plen);
		} else {
			if (cookie != 4) {
				_invalid_stream(L,rb);
			}
			uint32_t *plen = rb_read(rb, &len, 4);
			if (plen == NULL) {
				_invalid_stream(L,rb);
			}
			_get_buffer(L,rb,(int)*plen);
		}
		break;
	}
	case TYPE_TABLE: {
		_unpack_table(L,rb,cookie, table_index);
		break;
	}
	}
}

//从read_block中读取一个字节类型的数据，压栈
static void
_unpack_one(lua_State *L, struct read_block *rb, int table_index) {
	uint8_t type = 0;
	uint8_t *t = rb_read(rb, &type, 1);
	if (t==NULL) {
		_invalid_stream(L, rb);
	}
	//压栈
	_push_value(L, rb, *t & 0x7, *t>>3, table_index);
}

int
data_unpack(lua_State *L) {
	struct block * blk = lua_touserdata(L,1);
	if (blk == NULL) {
		return luaL_error(L, "Need a block to unpack");
	}
	luaL_checktype(L, 2, LUA_TTABLE);
	lua_settop(L,2);

	//创建read_block
	struct read_block rb;
	rb_init(&rb, blk);

	int i;
	for (i=0;;i++) {
		if (i%16==15) {
			lua_checkstack(L,i);
		}
		uint8_t type = 0;
		uint8_t *t = rb_read(&rb, &type, 1);
		if (t==NULL)
			break;
		
//从read_block中读取各种类型数据，压栈
		_push_value(L, &rb, *t & 0x7, *t>>3, 2);
	}

	rb_close(&rb);

	return lua_gettop(L) - 2;
}


