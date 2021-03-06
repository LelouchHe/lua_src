/*
** $Id: lobject.h,v 2.70 2012/05/11 14:10:50 roberto Exp $
** Type definitions for Lua objects
** See Copyright Notice in lua.h
*/


#ifndef lobject_h
#define lobject_h


#include <stdarg.h>


#include "llimits.h"
#include "lua.h"


/*
** Extra tags for non-values
*/
// lua函数的元信息
#define LUA_TPROTO	LUA_NUMTAGS
// upvalue
#define LUA_TUPVAL	(LUA_NUMTAGS+1)
// FIXME:
#define LUA_TDEADKEY	(LUA_NUMTAGS+2)

/*
** number of all possible tags (including LUA_TNONE but excluding DEADKEY)
*/
#define LUA_TOTALTAGS	(LUA_TUPVAL+2)


/*
** tags for Tagged Values have the following use of bits:
** bits 0-3: actual tag (a LUA_T* value)
** bits 4-5: variant bits
** bit 6: whether value is collectable
*/

// | 4: 类型 | 2: 额外信息(一般是子类型,见下) | 1: 是否可以GC |
#define VARBITS		(3 << 4)


/*
** LUA_TFUNCTION variants:
** 0 - Lua function
** 1 - light C function
** 2 - regular C function (closure)
*/

/* Variant tags for functions */
#define LUA_TLCL	(LUA_TFUNCTION | (0 << 4))  /* Lua closure */
#define LUA_TLCF	(LUA_TFUNCTION | (1 << 4))  /* light C function */
#define LUA_TCCL	(LUA_TFUNCTION | (2 << 4))  /* C closure */


/*
** LUA_TSTRING variants */
#define LUA_TSHRSTR	(LUA_TSTRING | (0 << 4))  /* short strings */
#define LUA_TLNGSTR	(LUA_TSTRING | (1 << 4))  /* long strings */


/* Bit mark for collectable types */
#define BIT_ISCOLLECTABLE	(1 << 6)

/* mark a tag as collectable */
#define ctb(t)			((t) | BIT_ISCOLLECTABLE)


/*
** Union of all collectable objects
*/
typedef union GCObject GCObject;


/*
** Common Header for all collectable objects (in macro form, to be
** included in other objects)
*/
// 链表指针,类型,gc标记
#define CommonHeader	GCObject *next; lu_byte tt; lu_byte marked


/*
** Common header in struct form
*/
typedef struct GCheader {
  CommonHeader;
} GCheader;



/*
** Union of all Lua values
*/
// Value包含GC和非GC对象
typedef union Value Value;


#define numfield	lua_Number n;    /* numbers */



/*
** Tagged Values. This is the basic representation of values in Lua,
** an actual value plus a tag with its type.
*/

// 值,类型
// FIXME: 这里是int,上面是lu_byte,有什么区别
#define TValuefields	Value value_; int tt_

typedef struct lua_TValue TValue;


/* macro defining a nil value */
#define NILCONSTANT	{NULL}, LUA_TNIL


#define val_(o)		((o)->value_)
#define num_(o)		(val_(o).n)

// tag是完整的标识,main_type+var_type+gc
// type是main_type+var_type
// typenv是main_type

/* raw type tag of a TValue */
#define rttype(o)	((o)->tt_)

/* tag with no variants (bits 0-3) */
#define novariant(x)	((x) & 0x0F)

/* type tag of a TValue (bits 0-3 for tags + variant bits 4-5) */
#define ttype(o)	(rttype(o) & 0x3F)

/* type tag of a TValue with no variants (bits 0-3) */
#define ttypenv(o)	(novariant(rttype(o)))


/* Macros to test type */
// GC: string, table, function(带有upvalue的closure), userdata, thread
#define checktag(o,t)		(rttype(o) == (t))
#define checktype(o,t)		(ttypenv(o) == (t))
#define ttisnumber(o)		checktag((o), LUA_TNUMBER)
#define ttisnil(o)		checktag((o), LUA_TNIL)
#define ttisboolean(o)		checktag((o), LUA_TBOOLEAN)
#define ttislightuserdata(o)	checktag((o), LUA_TLIGHTUSERDATA)
#define ttisstring(o)		checktype((o), LUA_TSTRING)
#define ttisshrstring(o)	checktag((o), ctb(LUA_TSHRSTR))
#define ttislngstring(o)	checktag((o), ctb(LUA_TLNGSTR))
#define ttistable(o)		checktag((o), ctb(LUA_TTABLE))
#define ttisfunction(o)		checktype(o, LUA_TFUNCTION)
#define ttisclosure(o)		((rttype(o) & 0x1F) == LUA_TFUNCTION)
#define ttisCclosure(o)		checktag((o), ctb(LUA_TCCL))
#define ttisLclosure(o)		checktag((o), ctb(LUA_TLCL))
#define ttislcf(o)		checktag((o), LUA_TLCF)
#define ttisuserdata(o)		checktag((o), ctb(LUA_TUSERDATA))
#define ttisthread(o)		checktag((o), ctb(LUA_TTHREAD))
#define ttisdeadkey(o)		checktag((o), LUA_TDEADKEY)

#define ttisequal(o1,o2)	(rttype(o1) == rttype(o2))

/* Macros to access values */
#define nvalue(o)	check_exp(ttisnumber(o), num_(o))
#define gcvalue(o)	check_exp(iscollectable(o), val_(o).gc)
#define pvalue(o)	check_exp(ttislightuserdata(o), val_(o).p)
#define rawtsvalue(o)	check_exp(ttisstring(o), &val_(o).gc->ts)
#define tsvalue(o)	(&rawtsvalue(o)->tsv)
#define rawuvalue(o)	check_exp(ttisuserdata(o), &val_(o).gc->u)
#define uvalue(o)	(&rawuvalue(o)->uv)
#define clvalue(o)	check_exp(ttisclosure(o), &val_(o).gc->cl)
#define clLvalue(o)	check_exp(ttisLclosure(o), &val_(o).gc->cl.l)
#define clCvalue(o)	check_exp(ttisCclosure(o), &val_(o).gc->cl.c)
#define fvalue(o)	check_exp(ttislcf(o), val_(o).f)
#define hvalue(o)	check_exp(ttistable(o), &val_(o).gc->h)
#define bvalue(o)	check_exp(ttisboolean(o), val_(o).b)
#define thvalue(o)	check_exp(ttisthread(o), &val_(o).gc->th)
/* a dead value may get the 'gc' field, but cannot access its contents */
#define deadvalue(o)	check_exp(ttisdeadkey(o), cast(void *, val_(o).gc))

// 确实,nil/false => false
#define l_isfalse(o)	(ttisnil(o) || (ttisboolean(o) && bvalue(o) == 0))


#define iscollectable(o)	(rttype(o) & BIT_ISCOLLECTABLE)


/* Macros for internal tests */
#define righttt(obj)		(ttype(obj) == gcvalue(obj)->gch.tt)

// 非gc对象一直存活
// gc对象要判断gc标识
#define checkliveness(g,obj) \
	lua_longassert(!iscollectable(obj) || \
			(righttt(obj) && !isdead(g,gcvalue(obj))))


/* Macros to set values */
#define settt_(o,t)	((o)->tt_=(t))

#define setnvalue(obj,x) \
  { TValue *io=(obj); num_(io)=(x); settt_(io, LUA_TNUMBER); }

#define changenvalue(o,x)	check_exp(ttisnumber(o), num_(o)=(x))

#define setnilvalue(obj) settt_(obj, LUA_TNIL)

#define setfvalue(obj,x) \
  { TValue *io=(obj); val_(io).f=(x); settt_(io, LUA_TLCF); }

#define setpvalue(obj,x) \
  { TValue *io=(obj); val_(io).p=(x); settt_(io, LUA_TLIGHTUSERDATA); }

#define setbvalue(obj,x) \
  { TValue *io=(obj); val_(io).b=(x); settt_(io, LUA_TBOOLEAN); }

#define setgcovalue(L,obj,x) \
  { TValue *io=(obj); GCObject *i_g=(x); \
    val_(io).gc=i_g; settt_(io, ctb(gch(i_g)->tt)); }

#define setsvalue(L,obj,x) \
  { TValue *io=(obj); \
    TString *x_ = (x); \
    val_(io).gc=cast(GCObject *, x_); settt_(io, ctb(x_->tsv.tt)); \
    checkliveness(G(L),io); }

#define setuvalue(L,obj,x) \
  { TValue *io=(obj); \
    val_(io).gc=cast(GCObject *, (x)); settt_(io, ctb(LUA_TUSERDATA)); \
    checkliveness(G(L),io); }

#define setthvalue(L,obj,x) \
  { TValue *io=(obj); \
    val_(io).gc=cast(GCObject *, (x)); settt_(io, ctb(LUA_TTHREAD)); \
    checkliveness(G(L),io); }

#define setclLvalue(L,obj,x) \
  { TValue *io=(obj); \
    val_(io).gc=cast(GCObject *, (x)); settt_(io, ctb(LUA_TLCL)); \
    checkliveness(G(L),io); }

#define setclCvalue(L,obj,x) \
  { TValue *io=(obj); \
    val_(io).gc=cast(GCObject *, (x)); settt_(io, ctb(LUA_TCCL)); \
    checkliveness(G(L),io); }

#define sethvalue(L,obj,x) \
  { TValue *io=(obj); \
    val_(io).gc=cast(GCObject *, (x)); settt_(io, ctb(LUA_TTABLE)); \
    checkliveness(G(L),io); }

#define setdeadvalue(obj)	settt_(obj, LUA_TDEADKEY)



#define setobj(L,obj1,obj2) \
	{ const TValue *io2=(obj2); TValue *io1=(obj1); \
	  io1->value_ = io2->value_; io1->tt_ = io2->tt_; \
	  checkliveness(G(L),io1); }


/*
** different types of assignments, according to destination
*/

/* from stack to (same) stack */
#define setobjs2s	setobj
/* to stack (not from same stack) */
#define setobj2s	setobj
#define setsvalue2s	setsvalue
#define sethvalue2s	sethvalue
#define setptvalue2s	setptvalue
/* from table to same table */
#define setobjt2t	setobj
/* to table */
#define setobj2t	setobj
/* to new object */
#define setobj2n	setobj
#define setsvalue2n	setsvalue


/* check whether a number is valid (useful only for NaN trick) */
#define luai_checknum(L,o,c)	{ /* empty */ }


/*
** {======================================================
** NaN Trick
** =======================================================
*/
#if defined(LUA_NANTRICK)

/*
** numbers are represented in the 'd_' field. All other values have the
** value (NNMARK | tag) in 'tt__'. A number with such pattern would be
** a "signaled NaN", which is never generated by regular operations by
** the CPU (nor by 'strtod')
*/

/* allows for external implementation for part of the trick */
#if !defined(NNMARK)	/* { */


#if !defined(LUA_IEEEENDIAN)
#error option 'LUA_NANTRICK' needs 'LUA_IEEEENDIAN'
#endif


#define NNMARK		0x7FF7A500
#define NNMASK		0x7FFFFF00

#undef TValuefields
#undef NILCONSTANT

#if (LUA_IEEEENDIAN == 0)	/* { */

/* little endian */
#define TValuefields  \
	union { struct { Value v__; int tt__; } i; double d__; } u
#define NILCONSTANT	{{{NULL}, tag2tt(LUA_TNIL)}}
/* field-access macros */
#define v_(o)		((o)->u.i.v__)
#define d_(o)		((o)->u.d__)
#define tt_(o)		((o)->u.i.tt__)

#else				/* }{ */

/* big endian */
#define TValuefields  \
	union { struct { int tt__; Value v__; } i; double d__; } u
#define NILCONSTANT	{{tag2tt(LUA_TNIL), {NULL}}}
/* field-access macros */
#define v_(o)		((o)->u.i.v__)
#define d_(o)		((o)->u.d__)
#define tt_(o)		((o)->u.i.tt__)

#endif				/* } */

#endif			/* } */


/* correspondence with standard representation */
#undef val_
#define val_(o)		v_(o)
#undef num_
#define num_(o)		d_(o)


#undef numfield
#define numfield	/* no such field; numbers are the entire struct */

/* basic check to distinguish numbers from non-numbers */
#undef ttisnumber
#define ttisnumber(o)	((tt_(o) & NNMASK) != NNMARK)

#define tag2tt(t)	(NNMARK | (t))

#undef rttype
#define rttype(o)	(ttisnumber(o) ? LUA_TNUMBER : tt_(o) & 0xff)

#undef settt_
#define settt_(o,t)	(tt_(o) = tag2tt(t))

#undef setnvalue
#define setnvalue(obj,x) \
	{ TValue *io_=(obj); num_(io_)=(x); lua_assert(ttisnumber(io_)); }

#undef setobj
#define setobj(L,obj1,obj2) \
	{ const TValue *o2_=(obj2); TValue *o1_=(obj1); \
	  o1_->u = o2_->u; \
	  checkliveness(G(L),o1_); }


/*
** these redefinitions are not mandatory, but these forms are more efficient
*/

#undef checktag
#undef checktype
#define checktag(o,t)	(tt_(o) == tag2tt(t))
#define checktype(o,t)	(ctb(tt_(o) | VARBITS) == ctb(tag2tt(t) | VARBITS))

#undef ttisequal
// 这里比较的是类型
// 为什么单独拿出num来比较呢?又没有string/num的转换
// 好吧,这里是NANTRICK,目前不用关心
#define ttisequal(o1,o2)  \
	(ttisnumber(o1) ? ttisnumber(o2) : (tt_(o1) == tt_(o2)))


#undef luai_checknum
#define luai_checknum(L,o,c)	{ if (!ttisnumber(o)) c; }

#endif
/* }====================================================== */



/*
** {======================================================
** types and prototypes
** =======================================================
*/


// p/b/f/n都是基本类型,不需要gc
// 剩余的都是gc
union Value {
  GCObject *gc;    /* collectable objects */
  void *p;         /* light userdata */
  int b;           /* booleans */
  lua_CFunction f; /* light C functions */
  // NANTRICK下没有这项
  numfield         /* numbers */
};

// Value value_; int tt_;
struct lua_TValue {
  TValuefields;
};


typedef TValue *StkId;  /* index to stack elements */




/*
** Header for string value; string bytes follow the end of this structure
*/
// FIXME: 数据结构的对齐,怎么操作?
// 利用编译器分配时,会对齐和填空,开头使用一个预期的align的对象,就使后面的都以这个对象对齐
// 因为每个基本对象都有自己的对齐属性,开头对齐了,后面肯定就沿着这个地址来存放
typedef union TString {
  L_Umaxalign dummy;  /* ensures maximum alignment for strings */
  struct {
    CommonHeader;
	// 长字符串: extra = 1: hash已经计算; 否则,尚无
    // 短字符串: extra > 0: 保留关键字; = 0: 其他普通字符串
    lu_byte extra;  /* reserved words for short strings; "has hash" for longs */
	// 这个hash是统一hash,只有在放到hash时,才会取模,因此只计算一次
    unsigned int hash;
    size_t len;  /* number of characters in string */
  } tsv;
} TString;


/* get the actual string (array of bytes) from a TString */
#define getstr(ts)	cast(const char *, (ts) + 1)

/* get the actual string (array of bytes) from a Lua value */
#define svalue(o)       getstr(rawtsvalue(o))


/*
** Header for userdata; memory area follows the end of this structure
*/
typedef union Udata {
  L_Umaxalign dummy;  /* ensures maximum alignment for `local' udata */
  struct {
    CommonHeader;
    struct Table *metatable;
    struct Table *env; // uservalue
    size_t len;  /* number of bytes */
  } uv;
} Udata;



// name也使用了TString,表明这些资源也是可以回收,而不是永远存活的
/*
** Description of an upvalue for function prototypes
*/
// 这里并没有upvalue的值,而是描述信息
typedef struct Upvaldesc {
  TString *name;  /* upvalue name (for debug information) */
  lu_byte instack;  /* whether it is in stack */
  // 偏移,而非指针,因为stack会realloc
  lu_byte idx;  /* index of upvalue (in stack or in outer function's list) */
} Upvaldesc;


/*
** Description of a local variable for function prototypes
** (used for debug information)
*/
typedef struct LocVar {
  TString *varname;
  // 作用域
  int startpc;  /* first point where variable is active */
  int endpc;    /* first point where variable is dead */
} LocVar;


/*
** Function Prototypes
*/
// 这个也只是描述信息而已
// Proto也是可GC的
// FIXME: 这个就不需要align?
typedef struct Proto {
  CommonHeader;
  TValue *k;  /* constants used by the function */
  Instruction *code;
  struct Proto **p;  /* functions defined inside the function */
  // opcode -> line
  // 会从指令数映射到行数
  // debug时用
  int *lineinfo;  /* map from opcodes to source lines (debug information) */
  LocVar *locvars;  /* information about local variables (debug information) */
  Upvaldesc *upvalues;  /* upvalue information */
  union Closure *cache;  /* last created closure with this prototype */
  TString  *source;  /* used for debug information */
  int sizeupvalues;  /* size of 'upvalues' */
  int sizek;  /* size of `k' */
  int sizecode;
  int sizelineinfo;
  int sizep;  /* size of `p' */
  int sizelocvars;
  int linedefined;
  int lastlinedefined;
  // FIXME: gclist是做什么的?
  GCObject *gclist;
  // see?? 这个是固定参数个数
  // 真实参数个数需要从实际调用中来看
  lu_byte numparams;  /* number of fixed parameters */
  lu_byte is_vararg; // 从定义就能看到函数参数是否可变
  lu_byte maxstacksize;  /* maximum stack used by this function */
} Proto;



/*
** Lua Upvalues
*/
// 有open/closed之分,好像是luaF_xx系列操作的
typedef struct UpVal {
  CommonHeader;
  TValue *v;  /* points to stack or to its own value */
  union {
    TValue value;  /* the value (when closed) */
    struct {  /* double linked list (when open) */
      struct UpVal *prev;
      struct UpVal *next;
    } l;
  } u;
} UpVal;


/*
** Closures
*/

#define ClosureHeader \
	CommonHeader; lu_byte nupvalues; GCObject *gclist

typedef struct CClosure {
  ClosureHeader;
  lua_CFunction f;
  // c.upvalue是value实体
  TValue upvalue[1];  /* list of upvalues */
} CClosure;


typedef struct LClosure {
  ClosureHeader;
  struct Proto *p; //描述
  // l.upvalue只是指针
  UpVal *upvals[1];  /* list of upvalues */
} LClosure;

// 二者保存的upvalue方式不同
typedef union Closure {
  CClosure c;
  LClosure l;
} Closure;


#define isLfunction(o)	ttisLclosure(o)

#define getproto(o)	(clLvalue(o)->p)


/*
** Tables
*/

typedef union TKey {
  struct {
    TValuefields;
    struct Node *next;  /* for chaining */
  } nk;
  TValue tvk;
} TKey;


typedef struct Node {
  TValue i_val;
  TKey i_key;
} Node;


typedef struct Table {
  CommonHeader;
  lu_byte flags;  /* 1<<p means tagmethod(p) is not present */
  lu_byte lsizenode;  /* log2 of size of `node' array */
  struct Table *metatable;
  TValue *array;  /* array part */
  // 看来node是一维数组,hash的碰撞解决很tricky
  Node *node; // hash表
  // 一个hint,每次查找都会从lastfree向前
  // 直到全空,此时就要rehash
  Node *lastfree;  /* any free position is before this position */
  GCObject *gclist;
  int sizearray;  /* size of `array' array */
} Table;



/*
** `module' operation for hashing (size is always a power of 2)
*/
#define lmod(s,size) \
	(check_exp((size&(size-1))==0, (cast(int, (s) & ((size)-1)))))


#define twoto(x)	(1<<(x))
#define sizenode(t)	(twoto((t)->lsizenode))


/*
** (address of) a fixed nil value
*/
#define luaO_nilobject		(&luaO_nilobject_)


LUAI_DDEC const TValue luaO_nilobject_;


LUAI_FUNC int luaO_int2fb (unsigned int x);
LUAI_FUNC int luaO_fb2int (int x);
LUAI_FUNC int luaO_ceillog2 (unsigned int x);
LUAI_FUNC lua_Number luaO_arith (int op, lua_Number v1, lua_Number v2);
LUAI_FUNC int luaO_str2d (const char *s, size_t len, lua_Number *result);
LUAI_FUNC int luaO_hexavalue (int c);
LUAI_FUNC const char *luaO_pushvfstring (lua_State *L, const char *fmt,
                                                       va_list argp);
LUAI_FUNC const char *luaO_pushfstring (lua_State *L, const char *fmt, ...);
LUAI_FUNC void luaO_chunkid (char *out, const char *source, size_t len);


#endif

