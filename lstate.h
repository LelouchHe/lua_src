/*
** $Id: lstate.h,v 2.81 2012/06/08 15:14:04 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include "lua.h"

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"


/*

** Some notes about garbage-collected objects:  All objects in Lua must
** be kept somehow accessible until being freed.
**
** Lua keeps most objects linked in list g->allgc. The link uses field
** 'next' of the CommonHeader.
**
** Strings are kept in several lists headed by the array g->strt.hash.
**
** Open upvalues are not subject to independent garbage collection. They
** are collected together with their respective threads. Lua keeps a
** double-linked list with all open upvalues (g->uvhead) so that it can
** mark objects referred by them. (They are always gray, so they must
** be remarked in the atomic step. Usually their contents would be marked
** when traversing the respective threads, but the thread may already be
** dead, while the upvalue is still accessible through closures.)
**
** Objects with finalizers are kept in the list g->finobj.
**
** The list g->tobefnz links all objects being finalized.

*/


struct lua_longjmp;  /* defined in ldo.c */



/* extra stack space to handle TM calls and some other extras */
#define EXTRA_STACK   5


#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)


/* kinds of Garbage Collection */
#define KGC_NORMAL	0
#define KGC_EMERGENCY	1	/* gc was forced by an allocation failure */
#define KGC_GEN		2	/* generational collection */

// FIXME: 为啥不是TString,而是GCObject
typedef struct stringtable {
  GCObject **hash;
  lu_int32 nuse;  /* number of elements */
  int size;
} stringtable;


/*
** information about a call
*/
// 动态信息
typedef struct CallInfo {
  // func是静态信息
  StkId func;  /* function index in the stack */
  StkId	top;  /* top for this function */
  struct CallInfo *previous, *next;  /* dynamic call link */
  short nresults;  /* expected number of results from this function */
  lu_byte callstatus;
  // func的偏移
  // yield/pcallk中保存
  // resume中恢复
  ptrdiff_t extra;
  union {
    struct {  /* only for Lua functions */
	  // base: 主要是作为固定参数的基址,统一可变参数的访问
      StkId base;  /* base for this function */
      const Instruction *savedpc;
    } l;
    struct {  /* only for C functions */
      int ctx;  /* context info. in case of yields */
      lua_CFunction k;  /* continuation in case of yields */
	  // 保存信息
      ptrdiff_t old_errfunc;
      lu_byte old_allowhook;
      lu_byte status;
    } c;
  } u;
} CallInfo;


/*
** Bits in CallInfo status
*/
#define CIST_LUA	(1<<0)	/* call is running a Lua function */
#define CIST_HOOKED	(1<<1)	/* call is running a debug hook */
#define CIST_REENTRY	(1<<2)	/* call is running on same invocation of
                                   luaV_execute of previous call */
#define CIST_YIELDED	(1<<3)	/* call reentered after suspension */
#define CIST_YPCALL	(1<<4)	/* call is a yieldable protected call */
#define CIST_STAT	(1<<5)	/* call has an error status (pcall) */
#define CIST_TAIL	(1<<6)	/* call was tail called */
#define CIST_HOOKYIELD	(1<<7)	/* last hook called yielded */


#define isLua(ci)	((ci)->callstatus & CIST_LUA)


/*
** `global state', shared by all threads of this state
*/
// lua_newthread返回的是共享G的state
// lua_newstate返回的是一个完全独立的新state
typedef struct global_State {
  // 内存分配
  lua_Alloc frealloc;  /* function to reallocate memory */
  void *ud;         /* auxiliary data to `frealloc' */

  // GC
  lu_mem totalbytes;  /* number of bytes currently allocated - GCdebt */
  l_mem GCdebt;  /* bytes allocated not yet compensated by the collector */
  lu_mem GCmemtrav;  /* memory traversed by the GC */
  lu_mem GCestimate;  /* an estimate of the non-garbage memory in use */

  stringtable strt;  /* hash table for strings */

  // 全局表 LUA_REGISTRYINDEX[LUA_RIDX_GLOBALS]
  TValue l_registry;

  unsigned int seed;  /* randomized seed for hashes */

  // GC
  lu_byte currentwhite;
  lu_byte gcstate;  /* state of garbage collector */
  lu_byte gckind;  /* kind of GC running */
  lu_byte gcrunning;  /* true if GC is running */
  int sweepstrgc;  /* position of sweep in `strt' */
  
  // 单指针的,应该是真正的list
  // 双指针的,应该只是指向list中某项而已
  // 默认obj的嵌入链表
  GCObject *allgc;  /* list of all collectable objects */
  // setmetatable时,带有__gc的对象列表
  GCObject *finobj;  /* list of collectable objects with finalizers */

  // 上面二者的current sweep位置
  GCObject **sweepgc;  /* current position of sweep in list 'allgc' */
  GCObject **sweepfin;  /* current position of sweep in list 'finobj' */

  GCObject *gray;  /* list of gray objects */
  GCObject *grayagain;  /* list of objects to be traversed atomically */
  GCObject *weak;  /* list of tables with weak values */
  GCObject *ephemeron;  /* list of ephemeron tables (weak keys) */
  GCObject *allweak;  /* list of all-weak tables */
  GCObject *tobefnz;  /* list of userdata to be GC */

  // 所有open的upvalue
  UpVal uvhead;  /* head of double-linked list of all open upvalues */

  Mbuffer buff;  /* temporary buffer for string concatenation */

  // GC参数
  int gcpause;  /* size of pause between successive GCs */
  int gcmajorinc;  /* how much to wait for a major GC (only in gen. mode) */
  int gcstepmul;  /* GC `granularity' */

  // luaD_throw在abort之前的处理参数
  lua_CFunction panic;  /* to be called in unprotected errors */

  // lua_newstate创建的state
  struct lua_State *mainthread;

  const lua_Number *version;  /* pointer to version number */

  TString *memerrmsg;  /* memory-error message */

  TString *tmname[TM_N];  /* array with tag-method names */

  // 可见,基本类型的mt只有一个
  struct Table *mt[LUA_NUMTAGS];  /* metatables for basic types */
} global_State;


/*
** `per thread' state
*/
// FIXME: 有很多状态 L->status, ci->callstatus, ci->u.c.status
struct lua_State {
  CommonHeader;
  lu_byte status;
  StkId top;  /* first free slot in the stack */

  // 共享表
  global_State *l_G;

  // 当前的ci
  CallInfo *ci;  /* call info for current function */

  const Instruction *oldpc;  /* last pc traced */

  // 本state的stack空间
  StkId stack_last;  /* last free slot in the stack */
  StkId stack;  /* stack base */
  int stacksize;

  unsigned short nny;  /* number of non-yieldable calls in stack */
  unsigned short nCcalls;  /* number of nested C calls */

  // hook
  lu_byte hookmask;
  lu_byte allowhook;
  // 触发debug的count值
  int basehookcount;
  // 按count来debug时,当前剩余count值
  // 当hookcount==0时,触发
  int hookcount;
  lua_Hook hook;

  GCObject *openupval;  /* list of open upvalues in this stack */

  GCObject *gclist;

  // setjmp/longjmp
  struct lua_longjmp *errorJmp;  /* current error recover point */
  ptrdiff_t errfunc;  /* current error handling function (stack index) */

  CallInfo base_ci;  /* CallInfo for first level (C calling Lua) */
};


#define G(L)	(L->l_G)


/*
** Union of all collectable objects
*/
union GCObject {
  GCheader gch;  /* common header */
  union TString ts;
  union Udata u;
  union Closure cl;
  struct Table h;
  struct Proto p;
  struct UpVal uv;
  struct lua_State th;  /* thread */
};


#define gch(o)		(&(o)->gch)

/* macros to convert a GCObject into a specific value */
#define rawgco2ts(o)  \
	check_exp(novariant((o)->gch.tt) == LUA_TSTRING, &((o)->ts))
#define gco2ts(o)	(&rawgco2ts(o)->tsv)
#define rawgco2u(o)	check_exp((o)->gch.tt == LUA_TUSERDATA, &((o)->u))
#define gco2u(o)	(&rawgco2u(o)->uv)
#define gco2lcl(o)	check_exp((o)->gch.tt == LUA_TLCL, &((o)->cl.l))
#define gco2ccl(o)	check_exp((o)->gch.tt == LUA_TCCL, &((o)->cl.c))
#define gco2cl(o)  \
	check_exp(novariant((o)->gch.tt) == LUA_TFUNCTION, &((o)->cl))
#define gco2t(o)	check_exp((o)->gch.tt == LUA_TTABLE, &((o)->h))
#define gco2p(o)	check_exp((o)->gch.tt == LUA_TPROTO, &((o)->p))
#define gco2uv(o)	check_exp((o)->gch.tt == LUA_TUPVAL, &((o)->uv))
#define gco2th(o)	check_exp((o)->gch.tt == LUA_TTHREAD, &((o)->th))

/* macro to convert any Lua object into a GCObject */
#define obj2gco(v)	(cast(GCObject *, (v)))


/* actual number of total bytes allocated */
#define gettotalbytes(g)	((g)->totalbytes + (g)->GCdebt)

LUAI_FUNC void luaE_setdebt (global_State *g, l_mem debt);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);
LUAI_FUNC CallInfo *luaE_extendCI (lua_State *L);
LUAI_FUNC void luaE_freeCI (lua_State *L);


#endif

