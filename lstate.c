/*
** $Id: lstate.c,v 2.98 2012/05/30 12:33:44 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/


#include <stddef.h>
#include <string.h>

#define lstate_c
#define LUA_CORE

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


#if !defined(LUAI_GCPAUSE)
#define LUAI_GCPAUSE	200  /* 200% */
#endif

#if !defined(LUAI_GCMAJOR)
#define LUAI_GCMAJOR	200  /* 200% */
#endif

#if !defined(LUAI_GCMUL)
#define LUAI_GCMUL	200 /* GC runs 'twice the speed' of memory allocation */
#endif


#define MEMERRMSG	"not enough memory"


/*
** a macro to help the creation of a unique random seed when a state is
** created; the seed is used to randomize hashes.
*/
#if !defined(luai_makeseed)
#include <time.h>
#define luai_makeseed()		cast(size_t, time(NULL))
#endif



/*
** thread state + extra space
*/
typedef struct LX {
#if defined(LUAI_EXTRASPACE)
  char buff[LUAI_EXTRASPACE];
#endif
  lua_State l;
} LX;


/*
** Main thread combines a thread state and the global state
*/
// 这意味着主线程会创建主线程的lua_State和golbal_State
// 可以理解,main_thread和全局共享信息绑定
typedef struct LG {
  LX l;
  global_State g;
} LG;


// 从lua_State*转型为LX*
#define fromstate(L)	(cast(LX *, cast(lu_byte *, (L)) - offsetof(LX, l)))


/*
** Compute an initial seed as random as possible. In ANSI, rely on
** Address Space Layout Randomization (if present) to increase
** randomness..
*/
#define addbuff(b,p,e) \
  { size_t t = cast(size_t, e); \
    memcpy(buff + p, &t, sizeof(t)); p += sizeof(t); }

static unsigned int makeseed (lua_State *L) {
  char buff[4 * sizeof(size_t)];
  unsigned int h = luai_makeseed();
  int p = 0;
  addbuff(buff, p, L);  /* heap variable */
  addbuff(buff, p, &h);  /* local variable */
  addbuff(buff, p, luaO_nilobject);  /* global variable */
  addbuff(buff, p, &lua_newstate);  /* public function */
  lua_assert(p == sizeof(buff));
  return luaS_hash(buff, p, h);
}


/*
** set GCdebt to a new value keeping the value (totalbytes + GCdebt)
** invariant
*/
// g->totalbytes + g->GCdebt是恒量
void luaE_setdebt (global_State *g, l_mem debt) {
  g->totalbytes -= (debt - g->GCdebt);
  g->GCdebt = debt;
}


// 新建CallInfo,并链接入L->ci表
// 配合next_ci(见ldo.c)(在这个宏里将L->ci指向新ci)
CallInfo *luaE_extendCI (lua_State *L) {
  CallInfo *ci = luaM_new(L, CallInfo);
  lua_assert(L->ci->next == NULL);
  L->ci->next = ci;
  ci->previous = L->ci;
  ci->next = NULL;
  return ci;
}


// 将L->ci全部释放
// 但保留第一个ci
void luaE_freeCI (lua_State *L) {
  CallInfo *ci = L->ci;
  CallInfo *next = ci->next;
  ci->next = NULL;
  while ((ci = next) != NULL) {
    next = ci->next;
    luaM_free(L, ci);
  }
}

// L1是newthread出来的
static void stack_init (lua_State *L1, lua_State *L) {
  int i; CallInfo *ci;
  /* initialize stack array */
  // 初始栈大小:40
  L1->stack = luaM_newvector(L, BASIC_STACK_SIZE, TValue);
  L1->stacksize = BASIC_STACK_SIZE;
  for (i = 0; i < BASIC_STACK_SIZE; i++)
    setnilvalue(L1->stack + i);  /* erase new stack */
  L1->top = L1->stack;
  // 空余EXTRA_STACK备用
  L1->stack_last = L1->stack + L1->stacksize - EXTRA_STACK;
  /* initialize first ci */
  ci = &L1->base_ci;
  ci->next = ci->previous = NULL;
  ci->callstatus = 0;
  ci->func = L1->top;
  // base_ci指向为nil,仅作为哨兵而已
  setnilvalue(L1->top++);  /* 'function' entry for this 'ci' */
  // 惯例,每个ci最小LUA_MINSTACK(20)
  ci->top = L1->top + LUA_MINSTACK;
  L1->ci = ci;
}


static void freestack (lua_State *L) {
  if (L->stack == NULL)
    return;  /* stack not completely built yet */
  L->ci = &L->base_ci;  /* free the entire 'ci' list */
  luaE_freeCI(L);
  luaM_freearray(L, L->stack, L->stacksize);  /* free stack array */
}


/*
** Create registry table and its predefined values
*/
// 全局表+main_thread/globals_table
static void init_registry (lua_State *L, global_State *g) {
  // 能取个别的名字么?我还以为是metatable
  TValue mt;
  /* create registry */
  // 全局表
  Table *registry = luaH_new(L);
  sethvalue(L, &g->l_registry, registry);
  // 大小为LUA_RIDX_LAST(2)的数组,0hash
  luaH_resize(L, registry, LUA_RIDX_LAST, 0);
  /* registry[LUA_RIDX_MAINTHREAD] = L */
  // mt = L,registry[LUA_RIDX_MAINTHREAD]=mt
  setthvalue(L, &mt, L);
  luaH_setint(L, registry, LUA_RIDX_MAINTHREAD, &mt);
  /* registry[LUA_RIDX_GLOBALS] = table of globals */
  // mt = {}, registry[LUA_RIDX_MAINTHREAD]=mt
  sethvalue(L, &mt, luaH_new(L));
  // global表没有值
  luaH_setint(L, registry, LUA_RIDX_GLOBALS, &mt);
}


/*
** open parts of the state that may cause memory-allocation errors
*/
static void f_luaopen (lua_State *L, void *ud) {
  global_State *g = G(L);
  UNUSED(ud);
  stack_init(L, L);  /* init stack */
  init_registry(L, g);
  // 不是resize L,而是特定的resize L->G->strtab
  // 这个名字很差
  luaS_resize(L, MINSTRTABSIZE);  /* initial size of string table */
  // 初始化G(L)的metamethod名称
  luaT_init(L);
  // 初始化关键字字符串表示
  luaX_init(L);
  /* pre-create memory-error message */
  g->memerrmsg = luaS_newliteral(L, MEMERRMSG);

  // strtab/metamethod/key 都用了下面的方式,标记永远不用回收
  luaS_fix(g->memerrmsg);  /* it should never be collected */
  g->gcrunning = 1;  /* allow gc */
}


/*
** preinitialize a state with consistent values without allocating
** any memory (to avoid errors)
*/
static void preinit_state (lua_State *L, global_State *g) {
  G(L) = g;
  L->stack = NULL;
  L->ci = NULL;
  L->stacksize = 0;
  L->errorJmp = NULL;
  L->nCcalls = 0;
  L->hook = NULL;
  L->hookmask = 0;
  L->basehookcount = 0;
  L->allowhook = 1;
  resethookcount(L);
  L->openupval = NULL;
  L->nny = 1;
  L->status = LUA_OK;
  L->errfunc = 0;
}


static void close_state (lua_State *L) {
  global_State *g = G(L);
  luaF_close(L, L->stack);  /* close all upvalues for this thread */
  luaC_freeallobjects(L);  /* collect all objects */
  luaM_freearray(L, G(L)->strt.hash, G(L)->strt.size);
  luaZ_freebuffer(L, &g->buff);
  freestack(L);
  lua_assert(gettotalbytes(g) == sizeof(LG));
  (*g->frealloc)(g->ud, fromstate(L), sizeof(LG), 0);  /* free main block */
}

// newthread之后的state由GC回收
LUA_API lua_State *lua_newthread (lua_State *L) {
  lua_State *L1;
  lua_lock(L);
  luaC_checkGC(L);

  // FIXME: 没看出来LX的buff的作用
  L1 = &luaC_newobj(L, LUA_TTHREAD, sizeof(LX), NULL, offsetof(LX, l))->th;

  // 返回值就是新的L1
  setthvalue(L, L->top, L1);
  api_incr_top(L);

  preinit_state(L1, G(L));
  L1->hookmask = L->hookmask;
  L1->basehookcount = L->basehookcount;
  L1->hook = L->hook;
  resethookcount(L1);
  luai_userstatethread(L, L1);
  stack_init(L1, L);  /* init stack */
  lua_unlock(L);
  return L1;
}


void luaE_freethread (lua_State *L, lua_State *L1) {
  LX *l = fromstate(L1);
  luaF_close(L1, L1->stack);  /* close all upvalues for this thread */
  lua_assert(L1->openupval == NULL);
  luai_userstatefree(L, L1);
  freestack(L1);
  luaM_free(L, l);
}

// 独立的state
// 没有使用luaC_newobj,主要是因为绑定的G和GC特殊标记
LUA_API lua_State *lua_newstate (lua_Alloc f, void *ud) {
  int i;
  lua_State *L;
  global_State *g;
  // g + L的空间
  LG *l = cast(LG *, (*f)(ud, NULL, LUA_TTHREAD, sizeof(LG)));
  if (l == NULL) return NULL;
  L = &l->l.l;
  g = &l->g;
  L->next = NULL;
  L->tt = LUA_TTHREAD;
  g->currentwhite = bit2mask(WHITE0BIT, FIXEDBIT);

  // GC标记
  L->marked = luaC_white(g);

  g->gckind = KGC_NORMAL;
  
  // L/G绑定
  preinit_state(L, g);

  g->frealloc = f;
  g->ud = ud;
  g->mainthread = L;
  g->seed = makeseed(L);
  g->uvhead.u.l.prev = &g->uvhead;
  g->uvhead.u.l.next = &g->uvhead;
  g->gcrunning = 0;  /* no GC while building state */
  g->GCestimate = 0;
  g->strt.size = 0;
  g->strt.nuse = 0;
  g->strt.hash = NULL;
  setnilvalue(&g->l_registry);
  luaZ_initbuffer(L, &g->buff);
  g->panic = NULL;
  g->version = lua_version(NULL);
  g->gcstate = GCSpause;
  g->allgc = NULL;
  g->finobj = NULL;
  g->tobefnz = NULL;
  g->sweepgc = g->sweepfin = NULL;
  g->gray = g->grayagain = NULL;
  g->weak = g->ephemeron = g->allweak = NULL;
  g->totalbytes = sizeof(LG);
  g->GCdebt = 0;
  g->gcpause = LUAI_GCPAUSE;
  g->gcmajorinc = LUAI_GCMAJOR;
  g->gcstepmul = LUAI_GCMUL;
  for (i=0; i < LUA_NUMTAGS; i++) g->mt[i] = NULL;
  if (luaD_rawrunprotected(L, f_luaopen, NULL) != LUA_OK) {
    /* memory allocation error: free partial state */
    close_state(L);
    L = NULL;
  }
  else
    luai_userstateopen(L);
  return L;
}


LUA_API void lua_close (lua_State *L) {
  L = G(L)->mainthread;  /* only the main thread can be closed */
  lua_lock(L);
  luai_userstateclose(L);
  close_state(L);
}


