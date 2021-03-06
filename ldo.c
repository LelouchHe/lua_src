/*
** $Id: ldo.c,v 2.105 2012/06/08 15:14:04 roberto Exp $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/


#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#define ldo_c
#define LUA_CORE

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"
#include "lzio.h"




/*
** {======================================================
** Error-recovery functions
** =======================================================
*/

/*
** LUAI_THROW/LUAI_TRY define how Lua does exception handling. By
** default, Lua handles errors with exceptions when compiling as
** C++ code, with _longjmp/_setjmp when asked to use them, and with
** longjmp/setjmp otherwise.
*/
#if !defined(LUAI_THROW)

#if defined(__cplusplus) && !defined(LUA_USE_LONGJMP)
/* C++ exceptions */
#define LUAI_THROW(L,c)		throw(c)
#define LUAI_TRY(L,c,a) \
	try { a } catch(...) { if ((c)->status == 0) (c)->status = -1; }
#define luai_jmpbuf		int  /* dummy variable */

#elif defined(LUA_USE_ULONGJMP)
/* in Unix, try _longjmp/_setjmp (more efficient) */
#define LUAI_THROW(L,c)		_longjmp((c)->b, 1)
#define LUAI_TRY(L,c,a)		if (_setjmp((c)->b) == 0) { a }
#define luai_jmpbuf		jmp_buf

#else
/* default handling with long jumps */
#define LUAI_THROW(L,c)		longjmp((c)->b, 1)
#define LUAI_TRY(L,c,a)		if (setjmp((c)->b) == 0) { a }
#define luai_jmpbuf		jmp_buf

#endif

#endif



/* chain list of long jump buffers */
struct lua_longjmp {
  struct lua_longjmp *previous;
  luai_jmpbuf b;

  // 在luaD_throw中设置
  volatile int status;  /* error code */
};

// 设置错误信息,丢弃oldtop之上
static void seterrorobj (lua_State *L, int errcode, StkId oldtop) {
  switch (errcode) {
    case LUA_ERRMEM: {  /* memory error? */
      setsvalue2s(L, oldtop, G(L)->memerrmsg); /* reuse preregistered msg. */
      break;
    }
    case LUA_ERRERR: {
      setsvalue2s(L, oldtop, luaS_newliteral(L, "error in error handling"));
      break;
    }
    default: {
      setobjs2s(L, oldtop, L->top - 1);  /* error message on current top */
      break;
    }
  }
  // 恢复top,丢弃错误msg以上的所有内容
  L->top = oldtop + 1;
}


// 本state的错误处理 -> main错误处理 -> panic -> abort
// 错误信息一般在L的栈顶,如果非本L处理,要先移动到main的栈顶
l_noret luaD_throw (lua_State *L, int errcode) {
  if (L->errorJmp) {  /* thread has an error handler? */
	// 如果有的话,是在luaD_rawrunprotected链入的局部jmpbuf
    L->errorJmp->status = errcode;  /* set status */
    LUAI_THROW(L, L->errorJmp);  /* jump to it */
  }
  else {  /* thread has no error handler */
    L->status = cast_byte(errcode);  /* mark it as dead */
    if (G(L)->mainthread->errorJmp) {  /* main thread has a handler? */
      setobjs2s(L, G(L)->mainthread->top++, L->top - 1);  /* copy error obj. */
      luaD_throw(G(L)->mainthread, errcode);  /* re-throw in main thread */
    }
    else {  /* no handler at all; abort */
      if (G(L)->panic) {  /* panic function? */
        lua_unlock(L);
        G(L)->panic(L);  /* call it (last chance to jump out) */
      }
      abort();
    }
  }
}


// 利用setjmp/longjmp模拟异常抛出
// 异常时仍在跳回这里
// lua_error -> luaG_errormsg -> luaD_throw
int luaD_rawrunprotected (lua_State *L, Pfunc f, void *ud) {
  // FIXME: nCcalls有什么作用?
  // luaD_call会在调用时增加,调用结束后减少
  // lua_yieldk也会减少(因为返回了)
  unsigned short oldnCcalls = L->nCcalls;

  // 局部变量 lj,嵌入到L->errorjmp列表中
  // 由于调用全部在一个函数里,所以是可行的
  // (其实感觉还挺简洁)
  struct lua_longjmp lj;
  lj.status = LUA_OK;
  lj.previous = L->errorJmp;  /* chain new error handler */
  L->errorJmp = &lj;
  LUAI_TRY(L, &lj,
    (*f)(L, ud);
  );
  L->errorJmp = lj.previous;  /* restore old error handler */
  L->nCcalls = oldnCcalls;
  return lj.status;
}

/* }====================================================== */


// 可以看到,如果top是偏移而不是指针,有很多好处(此处的操作就免了)
// 但为什么没有呢? 需要区分指针和偏移的情况
// 此时只是将L->stack指向了新的位置,设置好初值
static void correctstack (lua_State *L, TValue *oldstack) {
  CallInfo *ci;
  GCObject *up;
  // 此处尚没有修正L->top
  // 还有oldstack一样,指向旧stack
  L->top = (L->top - oldstack) + L->stack;
  // 调整原则:偏移是一致的
  // 调整open upv的位置
  for (up = L->openupval; up != NULL; up = up->gch.next)
    gco2uv(up)->v = (gco2uv(up)->v - oldstack) + L->stack;
  // 调整ci位置
  for (ci = L->ci; ci != NULL; ci = ci->previous) {
    ci->top = (ci->top - oldstack) + L->stack;
    ci->func = (ci->func - oldstack) + L->stack;
    if (isLua(ci))
      ci->u.l.base = (ci->u.l.base - oldstack) + L->stack;
  }
}


/* some space for error handling */
// 错误时,分配的堆栈大小
#define ERRORSTACKSIZE	(LUAI_MAXSTACK + 200)


// 重新分配栈空间
// FIXME: newsize == ERRORSTACKSIZE时,会怎么处理?
// 随后会有luaG_runerror的调用
void luaD_reallocstack (lua_State *L, int newsize) {
  TValue *oldstack = L->stack;
  int lim = L->stacksize;
  lua_assert(newsize <= LUAI_MAXSTACK || newsize == ERRORSTACKSIZE);
  lua_assert(L->stack_last - L->stack == L->stacksize - EXTRA_STACK);
  luaM_reallocvector(L, L->stack, L->stacksize, newsize, TValue);

  // 初值清零
  for (; lim < newsize; lim++)
    setnilvalue(L->stack + lim); /* erase new segment */
  L->stacksize = newsize;
  L->stack_last = L->stack + newsize - EXTRA_STACK;

  // 调整top/upv/ci
  // 后两者都是指针
  correctstack(L, oldstack);
}


// 扩展L的栈
// state的stack如下:
// | L->stack | ... | L->top(当前使用的最大值) | ... | L->ci->top(当前调用能用的最大值) | .. | L->stack_last | EXTRA_STACK |
// 整体大小为L->stacksize
void luaD_growstack (lua_State *L, int n) {
  int size = L->stacksize;
  if (size > LUAI_MAXSTACK)  /* error after extra size? */
    luaD_throw(L, LUA_ERRERR);
  else {
	// 只考虑当前使用到的stack大小(top - stack)
    int needed = cast_int(L->top - L->stack) + n + EXTRA_STACK;
    int newsize = 2 * size;

    if (newsize > LUAI_MAXSTACK) newsize = LUAI_MAXSTACK;
    if (newsize < needed) newsize = needed;
    if (newsize > LUAI_MAXSTACK) {  /* stack overflow? */
      luaD_reallocstack(L, ERRORSTACKSIZE);
      luaG_runerror(L, "stack overflow");
    }
    else
      luaD_reallocstack(L, newsize);
  }
}


// ci->top有可能超过top(预先分配的空间没有用尽,就会这样)
// 因为ci不一定是线性的,所以只考察L->ci是不够的
static int stackinuse (lua_State *L) {
  CallInfo *ci;
  StkId lim = L->top;
  // 看来连第一个空ci也计算在内
  for (ci = L->ci; ci != NULL; ci = ci->previous) {
    lua_assert(ci->top <= L->stack_last);
    if (lim < ci->top) lim = ci->top;
  }
  return cast_int(lim - L->stack) + 1;  /* part of stack in use */
}


// inuse <= stacksize
void luaD_shrinkstack (lua_State *L) {
  int inuse = stackinuse(L);

  // 这里的goodsize是一个估量值,用于判断是否要减小堆栈
  int goodsize = inuse + (inuse / 8) + 2*EXTRA_STACK;
  if (goodsize > LUAI_MAXSTACK) goodsize = LUAI_MAXSTACK;
  if (inuse > LUAI_MAXSTACK ||  /* handling stack overflow? */
      goodsize >= L->stacksize)  /* would grow instead of shrink? */
    condmovestack(L);  /* don't change stack (change only for debugging) */
  else
    luaD_reallocstack(L, goodsize);  /* shrink it */
}


// 没有改动任何环境
// save -> restore
void luaD_hook (lua_State *L, int event, int line) {
  // FIXME: 这里的hook是哪里设置的呢?
  lua_Hook hook = L->hook;
  if (hook && L->allowhook) {
    CallInfo *ci = L->ci;
    ptrdiff_t top = savestack(L, L->top);
    ptrdiff_t ci_top = savestack(L, ci->top);
    lua_Debug ar;
    ar.event = event;
    ar.currentline = line;
    ar.i_ci = ci;
    luaD_checkstack(L, LUA_MINSTACK);  /* ensure minimum stack size */
    ci->top = L->top + LUA_MINSTACK;
    lua_assert(ci->top <= L->stack_last);
    L->allowhook = 0;  /* cannot call hooks inside a hook */
    ci->callstatus |= CIST_HOOKED;
    lua_unlock(L);
    (*hook)(L, &ar);
    lua_lock(L);
    lua_assert(!L->allowhook);
    L->allowhook = 1;
    ci->top = restorestack(L, ci_top);
    L->top = restorestack(L, top);
    ci->callstatus &= ~CIST_HOOKED;
  }
}

// lua的尾递归需要特别处理,所以使用callhook进行预处理
static void callhook (lua_State *L, CallInfo *ci) {
  int hook = LUA_HOOKCALL;
  ci->u.l.savedpc++;  /* hooks assume 'pc' is already incremented */
  // tailcall会破坏堆栈
  // 这可能是需要特别处理的原因
  if (isLua(ci->previous) &&
      GET_OPCODE(*(ci->previous->u.l.savedpc - 1)) == OP_TAILCALL) {
    ci->callstatus |= CIST_TAIL;
    hook = LUA_HOOKTAILCALL;
  }
  luaD_hook(L, hook, -1);
  ci->u.l.savedpc--;  /* correct 'pc' */
}


// 对于可变参数
// 将固定参数移到原top之上
// 最后的stack如下
// | func | nil | ... | varargs1 | varargs2 | ... | arg1(原top) | arg2 | ... | argn| top
// 可变参数就是返回arg1位置,这样
// base之上是固定参数,用正的偏移取值,
// base之下容量也是有限的(到func为止),用负偏移来取
static StkId adjust_varargs (lua_State *L, Proto *p, int actual) {
  int i;
  int nfixargs = p->numparams;
  StkId base, fixed;
  lua_assert(actual >= nfixargs);
  /* move fixed parameters to final position */
  fixed = L->top - actual;  /* first fixed argument */
  base = L->top;  /* final position of first argument */
  for (i=0; i<nfixargs; i++) {
    setobjs2s(L, L->top++, fixed + i);
    //  清空原值
    setnilvalue(fixed + i);
  }
  return base;
}


// 将metamethod __call放到类似其他函数调用的栈底
// 此时,对应的func就是__call函数,其上是原先的obj+参数
static StkId tryfuncTM (lua_State *L, StkId func) {
  const TValue *tm = luaT_gettmbyobj(L, func, TM_CALL);
  StkId p;
  ptrdiff_t funcr = savestack(L, func);
  // 可以看到,这里tm如果不是函数的话,就直接出错了
  // 如果不这样的话,就可以实现__call的递归的
  if (!ttisfunction(tm))
    luaG_typeerror(L, func, "call");
  /* Open a hole inside the stack at `func' */
  for (p = L->top; p > func; p--) setobjs2s(L, p, p-1);
  incr_top(L);
  func = restorestack(L, funcr);  /* previous call may change stack */
  setobj2s(L, func, tm);  /* tag method is the new function to be called */
  return func;
}



#define next_ci(L) (L->ci = (L->ci->next ? L->ci->next : luaE_extendCI(L)))


/*
** returns true if function has been executed (C function)
*/

// | func(ci->func) | arg0 | arg1 | ... | top | ...(LUA_MINSTACK) | ci->top
// 调用函数受限于func-ci->top之间(应该可以动态扩展的吧)
// 返回结果也是存在这里
int luaD_precall (lua_State *L, StkId func, int nresults) {
  lua_CFunction f;
  CallInfo *ci;
  int n;  /* number of arguments (Lua) or returns (C) */

  // funcr是当前func距离当前栈底的大小
  // 即偏移
  ptrdiff_t funcr = savestack(L, func);
  switch (ttype(func)) {
    // C函数直接调用
    // 不涉及vm
    case LUA_TLCF:  /* light C function */
      f = fvalue(func);
      goto Cfunc;
    case LUA_TCCL: {  /* C closure */
      f = clCvalue(func)->f;
  Cfunc:
	  // check,当不够时,会自动grow
      luaD_checkstack(L, LUA_MINSTACK);  /* ensure minimum stack size */

      // 将f链入L->ci表
      ci = next_ci(L);  /* now 'enter' new function */
      ci->nresults = nresults;
      // ci->func == func
      // TODO: 为什么不直接赋值呢?
      // luaD_checkstack可能会移动stack,所以只有偏移是合法的
      ci->func = restorestack(L, funcr);
      ci->top = L->top + LUA_MINSTACK;
      lua_assert(ci->top <= L->stack_last);
      ci->callstatus = 0;
      if (L->hookmask & LUA_MASKCALL)
        luaD_hook(L, LUA_HOOKCALL, -1);
      lua_unlock(L);
      // n是返回结果数
	  // 此时stack上的正是f需要的量(func+args)
	  // C调用,f,参数和L->top,ci->top
      n = (*f)(L);  /* do the actual call */
      lua_lock(L);
      api_checknelems(L, n);
      luaD_poscall(L, L->top - n);
      return 1;
    }
    case LUA_TLCL: {  /* Lua function: prepare its call */
      StkId base;
      Proto *p = clLvalue(func)->p;
	  // 这里有可能移stack,所以下面还得用偏移取值
      luaD_checkstack(L, p->maxstacksize);
      func = restorestack(L, funcr);

      // lua对缺少的参数设为nil
      // (嗯嗯..不像C)
      // n是实际参数个数,有可能大于p->numparams
      n = cast_int(L->top - func) - 1;  /* number of real arguments */
      for (; n < p->numparams; n++)
        setnilvalue(L->top++);  /* complete missing arguments */

	  // base永远指向第一个固定参数位置
      base = (!p->is_vararg) ? func + 1 : adjust_varargs(L, p, n);
      ci = next_ci(L);  /* now 'enter' new function */
      ci->nresults = nresults;

	  // func并没有移动,所以可以看到,func之后不一定直接和参数相接(可变参数会移动base)
	  // 但不影响我们的调用,因为我们使用的是base来找参数(从luaV_execute中更能看出来)
      ci->func = func;
      ci->u.l.base = base;
      ci->top = base + p->maxstacksize;
      lua_assert(ci->top <= L->stack_last);
      ci->u.l.savedpc = p->code;  /* starting point */
      ci->callstatus = CIST_LUA;
      L->top = ci->top;
      if (L->hookmask & LUA_MASKCALL)
        callhook(L, ci);
      // 没有实际调用
      // 返回0
      return 0;
    }
    default: {  /* not a function */
      func = tryfuncTM(L, func);  /* retry with 'function' tag method */
      // mock
      // 现在栈底func是一个函数类型了
      return luaD_precall(L, func, nresults);  /* now it must be a function */
    }
  }
}

// C的poscall在precall中调用
// lua的poscall在luaV_execute执行RETURN时调用
// 删除ci,设置返回值
// 由于有可变参数,所以函数返回的结果不一定是正好的,有可能有空洞
// 应该说以base为底是正好的
// 此处就是移动到原先的func处
int luaD_poscall (lua_State *L, StkId firstResult) {
  StkId res;
  int wanted, i;
  CallInfo *ci = L->ci;
  if (L->hookmask & (LUA_MASKRET | LUA_MASKLINE)) {
    if (L->hookmask & LUA_MASKRET) {
      ptrdiff_t fr = savestack(L, firstResult);  /* hook may change stack */
      luaD_hook(L, LUA_HOOKRET, -1);
      firstResult = restorestack(L, fr);
    }
    L->oldpc = ci->previous->u.l.savedpc;  /* 'oldpc' for caller function */
  }
  // stack上的func对象也要删除,所以结果从func开始
  res = ci->func;  /* res == final position of 1st result */
  wanted = ci->nresults;
  L->ci = ci = ci->previous;  /* back to caller */
  /* move results to correct place */
  // 位置其实就差1(其栈底的func)
  for (i = wanted; i != 0 && firstResult < L->top; i--)
    setobjs2s(L, res++, firstResult++);
  // 缺少的返回值,默认为nil
  while (i-- > 0)
    setnilvalue(res++);
  L->top = res;
  return (wanted - LUA_MULTRET);  /* 0 iff wanted == LUA_MULTRET */
}


/*
** Call a function (C or Lua). The function to be called is at *func.
** The arguments are on the stack, right after the function.
** When returns, all the results are on the stack, starting at the original
** function position.
*/
void luaD_call (lua_State *L, StkId func, int nResults, int allowyield) {
  if (++L->nCcalls >= LUAI_MAXCCALLS) {
    if (L->nCcalls == LUAI_MAXCCALLS)
      luaG_runerror(L, "C stack overflow");
    // TODO: 为什么是这个含义
    else if (L->nCcalls >= (LUAI_MAXCCALLS + (LUAI_MAXCCALLS>>3)))
      luaD_throw(L, LUA_ERRERR);  /* error while handing stack error */
  }

  // nny表示不允许yield的调用个数,每次luaD_call都会累加,只要不为0,调用就不会构建ctx/k
  // 应该只在resume中清零(新开的thread)
  if (!allowyield) L->nny++;
  if (!luaD_precall(L, func, nResults))  /* is a Lua function? */
    luaV_execute(L);  /* call it */
  if (!allowyield) L->nny--;
  L->nCcalls--;
  luaC_checkGC(L);
}

// lua_resume -> luaD_rawrunprotected -> unroll -> finishCcall
// 之所以会在protected下,因为finishCcall会执行k函数,这个过程还会yield的
static void finishCcall (lua_State *L) {
  CallInfo *ci = L->ci;
  int n;
  lua_assert(ci->u.c.k != NULL);  /* must have a continuation */
  lua_assert(L->nny == 0);
  /* finish 'lua_callk' */
  adjustresults(L, ci->nresults);
  /* call continuation function */
  if (!(ci->callstatus & CIST_STAT))  /* no call status? */
    ci->u.c.status = LUA_YIELD;  /* 'default' status */
  lua_assert(ci->u.c.status != LUA_OK);
  ci->callstatus = (ci->callstatus & ~(CIST_YPCALL | CIST_STAT)) | CIST_YIELDED;
  lua_unlock(L);
// TODO: 这个调用做什么
  // k是xx_callk系列提供的continue函数,用于C在yield+resume后执行的
  n = (*ci->u.c.k)(L);
  lua_lock(L);
  api_checknelems(L, n);
  /* finish 'luaD_precall' */
  luaD_poscall(L, L->top - n);
}

// lua_resume -> resume一个yield的coroutine ->unroll
// lua_resume -> 失败后 recover + unroll
// 可以看到,coroutine在yield之后,会返回调用resume的地方
// 调用链上(L->ci)的所有的
// FIXME: 为啥unroll要调用调用链上的ci呢?这个时候resume的coroutine已经返回了啊
static void unroll (lua_State *L, void *ud) {
  UNUSED(ud);
  for (;;) {
    if (L->ci == &L->base_ci)  /* stack is empty? */
      return;  /* coroutine finished normally */
    if (!isLua(L->ci))  /* C function? */
      finishCcall(L);
    else {  /* Lua function */
      luaV_finishOp(L);  /* finish interrupted instruction */
      luaV_execute(L);  /* execute down to higher C 'boundary' */
    }
  }
}


/*
** check whether thread has a suspended protected call
*/
static CallInfo *findpcall (lua_State *L) {
  CallInfo *ci;
  for (ci = L->ci; ci != NULL; ci = ci->previous) {  /* search for a pcall */
    if (ci->callstatus & CIST_YPCALL)
      return ci;
  }
  return NULL;  /* no pending pcall */
}


// 寻找到pcallk
// 这些信息都在lua_pcallk处设置
static int recover (lua_State *L, int status) {
  StkId oldtop;
  CallInfo *ci = findpcall(L);
  // 找到pcallk处
  if (ci == NULL) return 0;  /* no recovery point */
  /* "finish" luaD_pcall */
  // extra都是之前的函数,在lua_pcallk和lua_yieldk中设置
  oldtop = restorestack(L, ci->extra);
  luaF_close(L, oldtop);
  // 设置错误信息
  seterrorobj(L, status, oldtop);
  L->ci = ci;
  L->allowhook = ci->u.c.old_allowhook;
  L->nny = 0;  /* should be zero to be yieldable */
  // 调整栈
  luaD_shrinkstack(L);
  L->errfunc = ci->u.c.old_errfunc;
  ci->callstatus |= CIST_STAT;  /* call has error status */
  ci->u.c.status = status;  /* (here it is) */
  return 1;  /* continue running the coroutine */
}


/*
** signal an error in the call to 'resume', not in the execution of the
** coroutine itself. (Such errors should not be handled by any coroutine
** error handler and should not kill the coroutine.)
*/
static l_noret resume_error (lua_State *L, const char *msg, StkId firstArg) {
  L->top = firstArg;  /* remove args from the stack */
  setsvalue2s(L, L->top, luaS_new(L, msg));  /* push error message */
  incr_top(L);
  luaD_throw(L, -1);  /* jump back to 'lua_resume' */
}


/*
** do the work for 'lua_resume' in protected mode
*/
static void resume (lua_State *L, void *ud) {
  int nCcalls = L->nCcalls;
  StkId firstArg = cast(StkId, ud);
  CallInfo *ci = L->ci;
  if (nCcalls >= LUAI_MAXCCALLS)
    resume_error(L, "C stack overflow", firstArg);
  // new
  if (L->status == LUA_OK) {  /* may be starting a coroutine */
    // resume一个new的coroutine时,必须是新的state,没有ci
    if (ci != &L->base_ci)  /* not in base level? */
      resume_error(L, "cannot resume non-suspended coroutine", firstArg);
    /* coroutine is in base level; start running it */
    // precall会新建需要的ci的
	if (!luaD_precall(L, firstArg - 1, LUA_MULTRET))  /* Lua function? */
      luaV_execute(L);  /* call it */
  }
  else if (L->status != LUA_YIELD)
    resume_error(L, "cannot resume dead coroutine", firstArg);
  // suspended
  // FIXME: 这个的ci是谁建立的?
  // 应该是上面LUA_OK分支在precall中新建的
  else {  /* resuming from previous yield */
    L->status = LUA_OK;
	// ci->extra保存着yield的函数
    ci->func = restorestack(L, ci->extra);
	// FIXME: isLua为啥是判断hook?
	// 判断的是当前ci的状态,为true表示该resume是在一个lua函数中调用的
	// 唯一的可能则是在hook里
    if (isLua(ci))  /* yielded inside a hook? */
      luaV_execute(L);  /* just continue running Lua code */
    else {  /* 'common' yield */
      if (ci->u.c.k != NULL) {  /* does it have a continuation? */
        int n;
        ci->u.c.status = LUA_YIELD;  /* 'default' status */
        ci->callstatus |= CIST_YIELDED;
        lua_unlock(L);
        n = (*ci->u.c.k)(L);  /* call continuation */
        lua_lock(L);
        api_checknelems(L, n);
        firstArg = L->top - n;  /* yield results come from continuation */
      }

	  // 有没有k,都要pop掉参数的
      luaD_poscall(L, firstArg);  /* finish 'luaD_precall' */
    }
    unroll(L, NULL);
  }
  lua_assert(nCcalls == L->nCcalls);
}

// stack是普通调用的情况,所以在resume调用时,才能执行
// FIXME: 只是借用了from->nCcalls,为什么要这样?
LUA_API int lua_resume (lua_State *L, lua_State *from, int nargs) {
  int status;
  lua_lock(L);
  luai_userstateresume(L, nargs);
  L->nCcalls = (from) ? from->nCcalls + 1 : 1;
  // 给luaD_call的累加提供初始值
  // 也就是说,这里不是清零,而是初始而已,所有的coroutine都需要resume才能运行
  L->nny = 0;  /* allow yields */
  api_checknelems(L, (L->status == LUA_OK) ? nargs + 1 : nargs);

  // 有错误/yield,都会返回
  status = luaD_rawrunprotected(L, resume, L->top - nargs);
  if (status == -1)  /* error calling 'lua_resume'? */
    status = LUA_ERRRUN;
  else {  /* yield or regular error */
    while (status != LUA_OK && status != LUA_YIELD) {  /* error? */
      if (recover(L, status))  /* recover point? */
		// recover成功,找到了pcallk
        // 此时,L->ci就是lua_pcallk调用了
        status = luaD_rawrunprotected(L, unroll, NULL);  /* run continuation */
      else {  /* unrecoverable error */
        L->status = cast_byte(status);  /* mark thread as `dead' */
        seterrorobj(L, status, L->top);
        L->ci->top = L->top;
        break;
      }
    }
    lua_assert(status == L->status);
  }
  L->nny = 1;  /* do not allow yields */
  L->nCcalls--;
  lua_assert(L->nCcalls == ((from) ? from->nCcalls : 0));
  lua_unlock(L);
  return status;
}


LUA_API int lua_yieldk (lua_State *L, int nresults, int ctx, lua_CFunction k) {
  CallInfo *ci = L->ci;
  luai_userstateyield(L, nresults);
  lua_lock(L);
  api_checknelems(L, nresults);
  // nny>0: 不允许yield
  // 由luaD_call的allyield参数进行累加
  if (L->nny > 0) {
    if (L != G(L)->mainthread)
      luaG_runerror(L, "attempt to yield across metamethod/C-call boundary");
    else
      luaG_runerror(L, "attempt to yield from outside a coroutine");
  }
  L->status = LUA_YIELD;
  // extra是func的偏移
  ci->extra = savestack(L, ci->func);  /* save current 'func' */
  // lua中不使用ctx和k
  if (isLua(ci)) {  /* inside a hook? */
    api_check(L, k == NULL, "hooks cannot continue after yielding");
  }
  // C中throw出来
  else {
    if ((ci->u.c.k = k) != NULL)  /* is there a continuation? */
      ci->u.c.ctx = ctx;  /* save context */
    ci->func = L->top - nresults - 1;  /* protect stack below results */
    // TODO: 这个throw去了哪里?
    // 应该是lua_resume的status = luaD_rawrunprotected(L, resume, L->top - nargs);
    // yield是在resume中返回
    luaD_throw(L, LUA_YIELD);
  }
  // 只有isLua为true时,才会走到这里
  lua_assert(ci->callstatus & CIST_HOOKED);  /* must be inside a hook */
  lua_unlock(L);
  return 0;  /* return to 'luaD_hook' */
}


// 不提供ctx和k
// 这是同lua_pcallk的区别
// lua_pcallk对于无需yield的调用,使用这个
// func做跳转,看到有3中情况:1.lua_pcallk中,单纯的调用old_top; 2. luaD_protectedparser; 3. GCTM
int luaD_pcall (lua_State *L, Pfunc func, void *u,
                ptrdiff_t old_top, ptrdiff_t ef) {
  int status;
  CallInfo *old_ci = L->ci;
  lu_byte old_allowhooks = L->allowhook;
  unsigned short old_nny = L->nny;
  ptrdiff_t old_errfunc = L->errfunc;
  L->errfunc = ef;
  status = luaD_rawrunprotected(L, func, u);
  if (status != LUA_OK) {  /* an error occurred? */
    StkId oldtop = restorestack(L, old_top);
    luaF_close(L, oldtop);  /* close possible pending closures */

    // 错误入栈
    seterrorobj(L, status, oldtop);

	// 恢复状态,因为有error,表明从longjmp回来
    L->ci = old_ci;
    L->allowhook = old_allowhooks;
    L->nny = old_nny;
    luaD_shrinkstack(L);
  }
  L->errfunc = old_errfunc;
  return status;
}



/*
** Execute a protected parser.
*/
struct SParser {  /* data to `f_parser' */
  ZIO *z;
  Mbuffer buff;  /* dynamic structure used by the scanner */
  Dyndata dyd;  /* dynamic structures used by the parser */
  const char *mode;
  const char *name;
};

// 看来只检查'b'/'t'而已
static void checkmode (lua_State *L, const char *mode, const char *x) {
  if (mode && strchr(mode, x[0]) == NULL) {
    luaO_pushfstring(L,
       "attempt to load a %s chunk (mode is " LUA_QS ")", x, mode);
    luaD_throw(L, LUA_ERRSYNTAX);
  }
}


static void f_parser (lua_State *L, void *ud) {
  int i;
  Closure *cl;
  struct SParser *p = cast(struct SParser *, ud);
  int c = zgetc(p->z);  /* read first character */

  // parse的最后结果都是一个cl
  // 二进制(有固定格式)
  if (c == LUA_SIGNATURE[0]) {
    checkmode(L, p->mode, "binary");
    cl = luaU_undump(L, p->z, &p->buff, p->name);
  }
  else {
    checkmode(L, p->mode, "text");
    cl = luaY_parser(L, p->z, &p->buff, &p->dyd, p->name, c);
  }
  // meaningless?
  // 这个是动态分配的,初始分配1,当有需要时才扩展
  // 这里用来保证是proto和真正分配出来的是一致的
  lua_assert(cl->l.nupvalues == cl->l.p->sizeupvalues);
  for (i = 0; i < cl->l.nupvalues; i++) {  /* initialize upvalues */
    UpVal *up = luaF_newupval(L);
    cl->l.upvals[i] = up;
    luaC_objbarrier(L, cl, up);
  }
}


int luaD_protectedparser (lua_State *L, ZIO *z, const char *name,
                                        const char *mode) {
  struct SParser p;
  int status;
  L->nny++;  /* cannot yield during parsing */
  p.z = z; p.name = name; p.mode = mode;
  p.dyd.actvar.arr = NULL; p.dyd.actvar.size = 0;
  p.dyd.gt.arr = NULL; p.dyd.gt.size = 0;
  p.dyd.label.arr = NULL; p.dyd.label.size = 0;
  luaZ_initbuffer(L, &p.buff);
  status = luaD_pcall(L, f_parser, &p, savestack(L, L->top), L->errfunc);
  luaZ_freebuffer(L, &p.buff);
  luaM_freearray(L, p.dyd.actvar.arr, p.dyd.actvar.size);
  luaM_freearray(L, p.dyd.gt.arr, p.dyd.gt.size);
  luaM_freearray(L, p.dyd.label.arr, p.dyd.label.size);
  L->nny--;
  return status;
}


