/*
** $Id: lapi.c,v 2.164 2012/06/08 15:14:04 roberto Exp $
** Lua API
** See Copyright Notice in lua.h
*/


#include <stdarg.h>
#include <string.h>

#define lapi_c
#define LUA_CORE

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"



const char lua_ident[] =
  "$LuaVersion: " LUA_COPYRIGHT " $"
  "$LuaAuthors: " LUA_AUTHORS " $";


/* value at a non-valid index */
#define NONVALIDVALUE		cast(TValue *, luaO_nilobject)

/* corresponding test */
#define isvalid(o)	((o) != luaO_nilobject)

#define api_checkvalidindex(L, i)  api_check(L, isvalid(i), "invalid index")

// api栈操作规则
// 当参数位于栈顶时,参数出栈,结果入栈
// 或者说
// 参数不在api参数列表中时,参数出栈,结果入栈
// 在api参数列表中,直接结果入栈
//
// lua的api名称取的有些乱


// 从idx索引取得对应的值
// stack 构造
// top指向栈顶之上的位置
// func(0) | 1(-n) | 2(-(n-1)) | ... | n(-1) | top
// LUA_REGISTRYINDEX: 很大的负值(大于最大stack),全局idx
// 比这更小的,就是upvalue,二者的偏差是upvalue的offset
static TValue *index2addr (lua_State *L, int idx) {
  CallInfo *ci = L->ci;
  // idx > 0: 从当前栈(func)栈底开始
  if (idx > 0) {
    TValue *o = ci->func + idx;
    api_check(L, idx <= ci->top - (ci->func + 1), "unacceptable index");
    if (o >= L->top) return NONVALIDVALUE;
    else return o;
  }
  // 0 > idx > LUA_REGISTRYINDEX: 从当前栈栈顶(top)开始
  else if (idx > LUA_REGISTRYINDEX) {
    api_check(L, idx != 0 && -idx <= L->top - (ci->func + 1), "invalid index");
    return L->top + idx;
  }
  // idx == LUA_REGISTRYINDEX: 伪索引
  else if (idx == LUA_REGISTRYINDEX)
    return &G(L)->l_registry;
  // idx < LUA_REGISTRYINDEX: func->upvalue[LUA_REGISTRYINDEX - idx - 1]
  else {  /* upvalues */
    idx = LUA_REGISTRYINDEX - idx;
    api_check(L, idx <= MAXUPVAL + 1, "upvalue index too large");
    if (ttislcf(ci->func))  /* light C function? */
      return NONVALIDVALUE;  /* it has no upvalues */
    else {
      CClosure *func = clCvalue(ci->func);
      return (idx <= func->nupvalues) ? &func->upvalue[idx-1] : NONVALIDVALUE;
    }
  }
}


/*
** to be called by 'lua_checkstack' in protected mode, to grow stack
** capturing memory errors
*/
// FIXME: 参数为何是void *
// growstack是间接调用(luaD_rawrunprotected)..参数限制为void *
static void growstack (lua_State *L, void *ud) {
  int size = *(int *)ud;
  luaD_growstack(L, size);
}

// size是额外请求的stack项数量
LUA_API int lua_checkstack (lua_State *L, int size) {
  int res;
  CallInfo *ci = L->ci;
  lua_lock(L);
  if (L->stack_last - L->top > size)  /* stack large enough? */
    res = 1;  /* yes; check is OK */
  else {  /* no; need to grow stack */
    int inuse = cast_int(L->top - L->stack) + EXTRA_STACK;
    if (inuse > LUAI_MAXSTACK - size)  /* can grow without overflow? */
      res = 0;  /* no */
    else  /* try to grow stack */
      res = (luaD_rawrunprotected(L, &growstack, &size) == LUA_OK);
  }
  if (res && ci->top < L->top + size)
    ci->top = L->top + size;  /* adjust frame top */
  lua_unlock(L);
  return res;
}


LUA_API void lua_xmove (lua_State *from, lua_State *to, int n) {
  int i;
  if (from == to) return;
  lua_lock(to);
  api_checknelems(from, n);
  api_check(from, G(from) == G(to), "moving among independent states");
  api_check(from, to->ci->top - to->top >= n, "not enough elements to move");
  from->top -= n;
  for (i = 0; i < n; i++) {
    setobj2s(to, to->top++, from->top + i);
  }
  lua_unlock(to);
}


LUA_API lua_CFunction lua_atpanic (lua_State *L, lua_CFunction panicf) {
  lua_CFunction old;
  lua_lock(L);
  old = G(L)->panic;
  G(L)->panic = panicf;
  lua_unlock(L);
  return old;
}


LUA_API const lua_Number *lua_version (lua_State *L) {
  static const lua_Number version = LUA_VERSION_NUM;
  if (L == NULL) return &version;
  else return G(L)->version;
}



/*
** basic stack manipulation
*/


/*
** convert an acceptable stack index into an absolute index
*/
// 不同于index2addr, 这个只变换idx
// 重温:
// > 0: 正常栈
// <= LUA_REGISTRYINDEX: 全局表和upvalue(属于特殊index,没必要转换)
// otherwise: 栈的逆值
LUA_API int lua_absindex (lua_State *L, int idx) {
  return (idx > 0 || idx <= LUA_REGISTRYINDEX)
         ? idx
         : cast_int(L->top - L->ci->func + idx);
}

// top在栈顶之上,指向的是空槽
LUA_API int lua_gettop (lua_State *L) {
  return cast_int(L->top - (L->ci->func + 1));
}

// 可以扩张和缩小
// idx > 0: 表示栈最后的元素数
// idx < 0: 类似,只是以逆表示
// 正/负转换要注意
LUA_API void lua_settop (lua_State *L, int idx) {
  StkId func = L->ci->func;
  lua_lock(L);
  if (idx >= 0) {
    api_check(L, idx <= L->stack_last - (func + 1), "new top too large");
    while (L->top < (func + 1) + idx)
      setnilvalue(L->top++);
    L->top = (func + 1) + idx;
  }
  else {
    api_check(L, -(idx+1) <= (L->top - (func + 1)), "invalid new top");
    L->top += idx+1;  /* `subtract' index (index is negative) */
  }
  lua_unlock(L);
}


// 直接删除idx项
LUA_API void lua_remove (lua_State *L, int idx) {
  StkId p;
  lua_lock(L);
  p = index2addr(L, idx);
  api_checkvalidindex(L, p);
  while (++p < L->top) setobjs2s(L, p-1, p);
  L->top--;
  lua_unlock(L);
}


// top-1 => idx
// 不需要调整top(数量没变)
LUA_API void lua_insert (lua_State *L, int idx) {
  StkId p;
  StkId q;
  lua_lock(L);
  p = index2addr(L, idx);
  api_checkvalidindex(L, p);
  for (q = L->top; q>p; q--) setobjs2s(L, q, q-1);
  setobjs2s(L, p, L->top);
  lua_unlock(L);
}


static void moveto (lua_State *L, TValue *fr, int idx) {
  TValue *to = index2addr(L, idx);
  api_checkvalidindex(L, to);
  setobj(L, to, fr);
  if (idx < LUA_REGISTRYINDEX)  /* function upvalue? */
    luaC_barrier(L, clCvalue(L->ci->func), fr);
  /* LUA_REGISTRYINDEX does not need gc barrier
     (collector revisits it before finishing collection) */
}


// 删除原idx
// top-1 => idx
// 数量减1
LUA_API void lua_replace (lua_State *L, int idx) {
  lua_lock(L);
  api_checknelems(L, 1);
  moveto(L, L->top - 1, idx);
  L->top--;
  lua_unlock(L);
}


LUA_API void lua_copy (lua_State *L, int fromidx, int toidx) {
  TValue *fr;
  lua_lock(L);
  fr = index2addr(L, fromidx);
  api_checkvalidindex(L, fr);
  moveto(L, fr, toidx);
  lua_unlock(L);
}


// 将idx复制到top处
// 数量加1
// 可以看到,除了settop等有check的外,其余L->top++都使用api_incr_top
LUA_API void lua_pushvalue (lua_State *L, int idx) {
  lua_lock(L);
  setobj2s(L, L->top, index2addr(L, idx));
  api_incr_top(L);
  lua_unlock(L);
}



/*
** access functions (stack -> C)
*/

// LUA_TNONE不作为o的一部分,可以节约空间
// nil对象只有一份也是同理
LUA_API int lua_type (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (isvalid(o) ? ttypenv(o) : LUA_TNONE);
}


LUA_API const char *lua_typename (lua_State *L, int t) {
  UNUSED(L);
  return ttypename(t);
}


LUA_API int lua_iscfunction (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (ttislcf(o) || (ttisCclosure(o)));
}

// 个人感觉num/string的转换很不好
// 不如提供接口,而在语言层面上禁止自动转换
// string/num => true
LUA_API int lua_isnumber (lua_State *L, int idx) {
  TValue n;
  const TValue *o = index2addr(L, idx);
  return tonumber(o, &n);
}


// string/num => true
LUA_API int lua_isstring (lua_State *L, int idx) {
  int t = lua_type(L, idx);
  return (t == LUA_TSTRING || t == LUA_TNUMBER);
}


LUA_API int lua_isuserdata (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return (ttisuserdata(o) || ttislightuserdata(o));
}


// 不涉及metamethod的相等比较
LUA_API int lua_rawequal (lua_State *L, int index1, int index2) {
  StkId o1 = index2addr(L, index1);
  StkId o2 = index2addr(L, index2);
  return (isvalid(o1) && isvalid(o2)) ? luaV_rawequalobj(o1, o2) : 0;
}


// 使用fake的目的是简化流程
// 统一视为双参数操作,不用单独处理的逻辑
LUA_API void  lua_arith (lua_State *L, int op) {
  StkId o1;  /* 1st operand */
  StkId o2;  /* 2nd operand */
  lua_lock(L);
  if (op != LUA_OPUNM) /* all other operations expect two operands */
    api_checknelems(L, 2);
  else {  /* for unary minus, add fake 2nd operand */
    api_checknelems(L, 1);
    // param(top-1) => fake_param(top)
    setobjs2s(L, L->top, L->top - 1);
    L->top++;
  }
  o1 = L->top - 2;
  o2 = L->top - 1;
  if (ttisnumber(o1) && ttisnumber(o2)) {
    changenvalue(o1, luaO_arith(op, nvalue(o1), nvalue(o2)));
  }
  // 不是num/string,进行tonumber/metamethod
  else {
    luaV_arith(L, o1, o1, o2, cast(TMS, op - LUA_OPADD + TM_ADD));
  }

  // 有一个结果存在,故只-1
  L->top--;
  lua_unlock(L);
}

// 可能涉及到meta的比较
LUA_API int lua_compare (lua_State *L, int index1, int index2, int op) {
  StkId o1, o2;
  int i = 0;
  lua_lock(L);  /* may call tag method */
  o1 = index2addr(L, index1);
  o2 = index2addr(L, index2);
  if (isvalid(o1) && isvalid(o2)) {
    switch (op) {
      case LUA_OPEQ: i = equalobj(L, o1, o2); break;
      case LUA_OPLT: i = luaV_lessthan(L, o1, o2); break;
      case LUA_OPLE: i = luaV_lessequal(L, o1, o2); break;
      default: api_check(L, 0, "invalid option");
    }
  }
  lua_unlock(L);
  return i;
}

// x就表示有一个isnum的额外参数
// isnum就是表示是否转换成功的标志(感觉好多余啊)
// string的数字也会进行转换
LUA_API lua_Number lua_tonumberx (lua_State *L, int idx, int *isnum) {
  TValue n;
  const TValue *o = index2addr(L, idx);
  if (tonumber(o, &n)) {
    if (isnum) *isnum = 1;
    return nvalue(o);
  }
  else {
    if (isnum) *isnum = 0;
    return 0;
  }
}


LUA_API lua_Integer lua_tointegerx (lua_State *L, int idx, int *isnum) {
  TValue n;
  const TValue *o = index2addr(L, idx);
  if (tonumber(o, &n)) {
    lua_Integer res;
    lua_Number num = nvalue(o);
    lua_number2integer(res, num);
    if (isnum) *isnum = 1;
    return res;
  }
  else {
    if (isnum) *isnum = 0;
    return 0;
  }
}


LUA_API lua_Unsigned lua_tounsignedx (lua_State *L, int idx, int *isnum) {
  TValue n;
  const TValue *o = index2addr(L, idx);
  if (tonumber(o, &n)) {
    lua_Unsigned res;
    lua_Number num = nvalue(o);
    lua_number2unsigned(res, num);
    if (isnum) *isnum = 1;
    return res;
  }
  else {
    if (isnum) *isnum = 0;
    return 0;
  }
}


// nil/false => false
LUA_API int lua_toboolean (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return !l_isfalse(o);
}


// 可以转换:改变了idx的值和类型
// 不可转换:不变
LUA_API const char *lua_tolstring (lua_State *L, int idx, size_t *len) {
  StkId o = index2addr(L, idx);
  // 没有用lua_isstring
  // 此时num还没有string值
  if (!ttisstring(o)) {
    lua_lock(L);  /* `luaV_tostring' may create a new string */
    if (!luaV_tostring(L, o)) {  /* conversion failed? */
      if (len != NULL) *len = 0;
      lua_unlock(L);
      return NULL;
    }
    luaC_checkGC(L);
    o = index2addr(L, idx);  /* previous call may reallocate the stack */
    lua_unlock(L);
  }
  if (len != NULL) *len = tsvalue(o)->len;
  return svalue(o);
}


LUA_API size_t lua_rawlen (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttypenv(o)) {
    case LUA_TSTRING: return tsvalue(o)->len;
    case LUA_TUSERDATA: return uvalue(o)->len;
    case LUA_TTABLE: return luaH_getn(hvalue(o));
    default: return 0;
  }
}


// idx需要是cfunc
// 这个函数其实不算转换,而是取值,cfunc/nil
LUA_API lua_CFunction lua_tocfunction (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  if (ttislcf(o)) return fvalue(o);
  else if (ttisCclosure(o))
    return clCvalue(o)->f;
  else return NULL;  /* not a C function */
}


LUA_API void *lua_touserdata (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttypenv(o)) {
    // 跳过object的标记结构Udata(主要用于管理)
    // 后面才是真正的userdata内容
    case LUA_TUSERDATA: return (rawuvalue(o) + 1);
    // lightuserdata就是一指针而已
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}


// thread其实就是单独的lua_State
LUA_API lua_State *lua_tothread (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (!ttisthread(o)) ? NULL : thvalue(o);
}

// 这是lua内采用引用/指针传值的几个类型:
// table,function,userdata,thread
LUA_API const void *lua_topointer (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttype(o)) {
    case LUA_TTABLE: return hvalue(o);
    case LUA_TLCL: return clLvalue(o);
    case LUA_TCCL: return clCvalue(o);
    case LUA_TLCF: return cast(void *, cast(size_t, fvalue(o)));
    case LUA_TTHREAD: return thvalue(o);
    case LUA_TUSERDATA:
    case LUA_TLIGHTUSERDATA:
      return lua_touserdata(L, idx);
    default: return NULL;
  }
}

// 可见,除了tostring/tonumber真正改变idx的值之外
// 其他都是取值而不改变
// tostring/tonumber体现了string/number的互通
// 不过这也仅在操作中,在相等判断时,不做转型


/*
** push functions (C -> stack)
*/


LUA_API void lua_pushnil (lua_State *L) {
  lua_lock(L);
  setnilvalue(L->top);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushnumber (lua_State *L, lua_Number n) {
  lua_lock(L);
  setnvalue(L->top, n);
  luai_checknum(L, L->top,
    luaG_runerror(L, "C API - attempt to push a signaling NaN"));
  api_incr_top(L);
  lua_unlock(L);
}

// 可以看到,lua内部是不区分符号和类型的
// 这两个接口应该主要是为了方便使用
LUA_API void lua_pushinteger (lua_State *L, lua_Integer n) {
  lua_lock(L);
  setnvalue(L->top, cast_num(n));
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushunsigned (lua_State *L, lua_Unsigned u) {
  lua_Number n;
  lua_lock(L);
  n = lua_unsigned2number(u);
  setnvalue(L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}

// 内部会进行复制,并返回内部表示
// 所以参数可以外部处理,但返回不能
LUA_API const char *lua_pushlstring (lua_State *L, const char *s, size_t len) {
  TString *ts;
  lua_lock(L);
  luaC_checkGC(L);
  ts = luaS_newlstr(L, s, len);
  setsvalue2s(L, L->top, ts);
  api_incr_top(L);
  lua_unlock(L);
  return getstr(ts);
}


LUA_API const char *lua_pushstring (lua_State *L, const char *s) {
  if (s == NULL) {
    lua_pushnil(L);
    return NULL;
  }
  else {
    TString *ts;
    lua_lock(L);
    luaC_checkGC(L);
    ts = luaS_new(L, s);
    setsvalue2s(L, L->top, ts);
    api_incr_top(L);
    lua_unlock(L);
    return getstr(ts);
  }
}


LUA_API const char *lua_pushvfstring (lua_State *L, const char *fmt,
                                      va_list argp) {
  const char *ret;
  lua_lock(L);
  luaC_checkGC(L);
  ret = luaO_pushvfstring(L, fmt, argp);
  lua_unlock(L);
  return ret;
}


LUA_API const char *lua_pushfstring (lua_State *L, const char *fmt, ...) {
  const char *ret;
  va_list argp;
  lua_lock(L);
  luaC_checkGC(L);
  va_start(argp, fmt);
  ret = luaO_pushvfstring(L, fmt, argp);
  va_end(argp);
  lua_unlock(L);
  return ret;
}

// upvalue的顺序也是正常入栈的
// 最后upvalue全出栈,closure入栈置顶
LUA_API void lua_pushcclosure (lua_State *L, lua_CFunction fn, int n) {
  lua_lock(L);
  if (n == 0) {
    setfvalue(L->top, fn);
  }
  else {
    Closure *cl;
    api_checknelems(L, n);
    api_check(L, n <= MAXUPVAL, "upvalue index too large");
    luaC_checkGC(L);
    cl = luaF_newCclosure(L, n);
    cl->c.f = fn;

    // 类似弹出n个参数
	// upvalue的顺序和push到stack的顺序是一样的
	// 不知道为什么不能更直接些,要避免这个情况
    L->top -= n;
    while (n--)
      setobj2n(L, &cl->c.upvalue[n], L->top + n);
    setclCvalue(L, L->top, cl);
  }
  api_incr_top(L);
  lua_unlock(L);
}

// b != 0在输入时就简化之后的判断
LUA_API void lua_pushboolean (lua_State *L, int b) {
  lua_lock(L);
  setbvalue(L->top, (b != 0));  /* ensure that true is 1 */
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushlightuserdata (lua_State *L, void *p) {
  lua_lock(L);
  setpvalue(L->top, p);
  api_incr_top(L);
  lua_unlock(L);
}


// 还可以把别的L推进来么?
// 可能是G是全局的,不同coroutine都能取到,但不是每个coroutine都是mainthread
// G是全局共享的
LUA_API int lua_pushthread (lua_State *L) {
  lua_lock(L);
  setthvalue(L, L->top, L);
  api_incr_top(L);
  lua_unlock(L);
  return (G(L)->mainthread == L);
}



/*
** get functions (Lua -> stack)
*/


// push _G[var]
// 感觉这个和idx=LUA_REGISTRYINDEX的取table值相同
// 有些许不同:
// LUA_REGISTRYINDEX: 全局表,除了全局变量之外还有很多
// LUA_RIDX_GLOBALS: 只是全局表中的全局变量表而已
// 而且使用的接口也不一样
LUA_API void lua_getglobal (lua_State *L, const char *var) {
  Table *reg = hvalue(&G(L)->l_registry);
  const TValue *gt;  /* global table */
  lua_lock(L);
  gt = luaH_getint(reg, LUA_RIDX_GLOBALS);
  setsvalue2s(L, L->top++, luaS_new(L, var));
  luaV_gettable(L, gt, L->top - 1, L->top - 1);
  lua_unlock(L);
}


// 名字可能不好,应该是lua_tableget,即从idx指的table取值
// top-1 = idx[top-1]
LUA_API void lua_gettable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  api_checkvalidindex(L, t);
  luaV_gettable(L, t, L->top - 1, L->top - 1);
  lua_unlock(L);
}

// 带field/i的操作,都是为更方便使用准备的,即key不用入栈,直接作为参数(很像php中的idx/string表)
// 带raw的,一般是纯table操作,不涉及meta调用

// push idx[k]
LUA_API void lua_getfield (lua_State *L, int idx, const char *k) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  api_checkvalidindex(L, t);
  setsvalue2s(L, L->top, luaS_new(L, k));
  api_incr_top(L);
  luaV_gettable(L, t, L->top - 1, L->top - 1);
  lua_unlock(L);
}

// 同lua_gettable的区别,可以从调用上看
// luaV_gettable一看前缀就知道设计vm操作,即meta方法的调用
// luaH_get就只是table的操作(H for hashtable)
// top-1 = idx[top-1]
LUA_API void lua_rawget (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setobj2s(L, L->top - 1, luaH_get(hvalue(t), L->top - 1));
  lua_unlock(L);
}


// push idx[n]
LUA_API void lua_rawgeti (lua_State *L, int idx, int n) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setobj2s(L, L->top, luaH_getint(hvalue(t), n));
  api_incr_top(L);
  lua_unlock(L);
}


// push idx[p]
LUA_API void lua_rawgetp (lua_State *L, int idx, const void *p) {
  StkId t;
  TValue k;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setpvalue(&k, cast(void *, p));
  setobj2s(L, L->top, luaH_get(hvalue(t), &k));
  api_incr_top(L);
  lua_unlock(L);
}


// push table[narray, nrec]
LUA_API void lua_createtable (lua_State *L, int narray, int nrec) {
  Table *t;
  lua_lock(L);
  luaC_checkGC(L);
  t = luaH_new(L);
  sethvalue(L, L->top, t);
  api_incr_top(L);
  if (narray > 0 || nrec > 0)
    luaH_resize(L, t, narray, nrec);
  lua_unlock(L);
}


// push objindex.mt
// or nothing
// 注意返回值,1:有mt;0:无
LUA_API int lua_getmetatable (lua_State *L, int objindex) {
  const TValue *obj;
  Table *mt = NULL;
  int res;
  lua_lock(L);
  obj = index2addr(L, objindex);

  // table/userdata可以有自己的meta
  // 其他类型是统一的(在G(L)->mt中)
  switch (ttypenv(obj)) {
    case LUA_TTABLE:
      mt = hvalue(obj)->metatable;
      break;
    case LUA_TUSERDATA:
      mt = uvalue(obj)->metatable;
      break;
    default:
      mt = G(L)->mt[ttypenv(obj)];
      break;
  }
  if (mt == NULL)
    res = 0;
  else {
    sethvalue(L, L->top, mt);
    api_incr_top(L);
    res = 1;
  }
  lua_unlock(L);
  return res;
}


// push idx->env or nil
// uservalue本来就是userdata.env(实际是一个table)
LUA_API void lua_getuservalue (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  o = index2addr(L, idx);
  api_checkvalidindex(L, o);
  api_check(L, ttisuserdata(o), "userdata expected");
  if (uvalue(o)->env) {
    sethvalue(L, L->top, uvalue(o)->env);
  } else
    setnilvalue(L->top);
  api_incr_top(L);
  lua_unlock(L);
}


/*
** set functions (stack -> Lua)
*/


// _G[var] = top-1
// pop 1
LUA_API void lua_setglobal (lua_State *L, const char *var) {
  Table *reg = hvalue(&G(L)->l_registry);
  const TValue *gt;  /* global table */
  lua_lock(L);
  api_checknelems(L, 1);
  gt = luaH_getint(reg, LUA_RIDX_GLOBALS);
  setsvalue2s(L, L->top++, luaS_new(L, var));
  luaV_settable(L, gt, L->top - 1, L->top - 2);
  // 前面把var转为string入栈,所以此处pop2次
  L->top -= 2;  /* pop value and key */
  lua_unlock(L);
}


// 先key,后value
// idx[top-2] = top-1
// pop 2
LUA_API void lua_settable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 2);
  t = index2addr(L, idx);
  api_checkvalidindex(L, t);
  luaV_settable(L, t, L->top - 2, L->top - 1);
  L->top -= 2;  /* pop index and value */
  lua_unlock(L);
}


// idx[k] = top-1
// pop 1
LUA_API void lua_setfield (lua_State *L, int idx, const char *k) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 1);
  t = index2addr(L, idx);
  api_checkvalidindex(L, t);
  setsvalue2s(L, L->top++, luaS_new(L, k));
  luaV_settable(L, t, L->top - 1, L->top - 2);
  L->top -= 2;  /* pop value and key */
  lua_unlock(L);
}


// idx[top-2] = top-1
// pop 2
LUA_API void lua_rawset (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 2);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setobj2t(L, luaH_set(L, hvalue(t), L->top-2), L->top-1);

  // FIXME: 这两句什么意思?
  invalidateTMcache(hvalue(t));
  luaC_barrierback(L, gcvalue(t), L->top-1);

  L->top -= 2;
  lua_unlock(L);
}


// idx[n] = top-1
// pop 1
LUA_API void lua_rawseti (lua_State *L, int idx, int n) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 1);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  luaH_setint(L, hvalue(t), n, L->top - 1);
  luaC_barrierback(L, gcvalue(t), L->top-1);
  L->top--;
  lua_unlock(L);
}


// idx[p] = top-1
// pop 1
LUA_API void lua_rawsetp (lua_State *L, int idx, const void *p) {
  StkId t;
  TValue k;
  lua_lock(L);
  api_checknelems(L, 1);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setpvalue(&k, cast(void *, p));
  setobj2t(L, luaH_set(L, hvalue(t), &k), L->top - 1);
  luaC_barrierback(L, gcvalue(t), L->top - 1);
  L->top--;
  lua_unlock(L);
}


// objindex.mt = top-1
// pop 1
LUA_API int lua_setmetatable (lua_State *L, int objindex) {
  TValue *obj;
  Table *mt;
  lua_lock(L);
  api_checknelems(L, 1);
  obj = index2addr(L, objindex);
  api_checkvalidindex(L, obj);
  if (ttisnil(L->top - 1))
    mt = NULL;
  else {
    api_check(L, ttistable(L->top - 1), "table expected");
    mt = hvalue(L->top - 1);
  }
  switch (ttypenv(obj)) {
    case LUA_TTABLE: {
      hvalue(obj)->metatable = mt;
      if (mt)
        luaC_objbarrierback(L, gcvalue(obj), mt);

		// 还记得__gc的finalizer么?在setmetatable时才会检查,之后修改meta不会做处理
        luaC_checkfinalizer(L, gcvalue(obj), mt);
      break;
    }
    case LUA_TUSERDATA: {
      uvalue(obj)->metatable = mt;
      if (mt) {
        luaC_objbarrier(L, rawuvalue(obj), mt);
        luaC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    // 非table/userdata的mt
    // 不能在lua中设置,而且是一个类型共用一个mt
	// 看来在C中一改,就全都改了
    default: {
      G(L)->mt[ttypenv(obj)] = mt;
      break;
    }
  }
  L->top--;
  lua_unlock(L);
  return 1;
}


// idx.env = top-1
// pop 1
// uservalue本来就是userdata.env
// lua内userdata分 管理部分(头)和用户数据(env)
LUA_API void lua_setuservalue (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_checkvalidindex(L, o);
  api_check(L, ttisuserdata(o), "userdata expected");
  if (ttisnil(L->top - 1))
    uvalue(o)->env = NULL;
  else {
    api_check(L, ttistable(L->top - 1), "table expected");
    uvalue(o)->env = hvalue(L->top - 1);
    luaC_objbarrier(L, gcvalue(o), hvalue(L->top - 1));
  }
  L->top--;
  lua_unlock(L);
}


/*
** `load' and `call' functions (run Lua code)
*/

// 主要是保证stack的空间是足够的
// 栈的空位就是L->top到L->ci->top之间(当然还可以grow到L->stack_last)
// LUA_MULTRET就需要被调用方自行grow,否则,就是调用方预留
#define checkresults(L,na,nr) \
     api_check(L, (nr) == LUA_MULTRET || (L->ci->top - L->top >= (nr) - (na)), \
	"results from function overflow current stack size")


// YIELDED: 设置ctx,返回状态
// others : OK
// context和continue函数是C完成yield/resume的方式
LUA_API int lua_getctx (lua_State *L, int *ctx) {
  if (L->ci->callstatus & CIST_YIELDED) {
    if (ctx) *ctx = L->ci->u.c.ctx;
    return L->ci->u.c.status;
  }
  else return LUA_OK;
}


// ctx: 表示调用者传给continue函数的ctx.应该可以是任意值,二者协商一致即可(比如是G的一个key)
// k: 函数yield之后,再次resume时执行的函数.coroutine应该是longjmp,这样原调用stack就全没了,所以只能另开调用栈
//    当然可以是自己本身
LUA_API void lua_callk (lua_State *L, int nargs, int nresults, int ctx,
                        lua_CFunction k) {
  StkId func;
  lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checknelems(L, nargs+1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  func = L->top - (nargs+1);
  // FIXME: L->nny是哪里设置的?
  // luaD_call里设置(通过最后一个allowyield参数),表示当前的调用链是否允许yield,会累加,一个不行,之后都不行
  if (k != NULL && L->nny == 0) {  /* need to prepare continuation? */
    L->ci->u.c.k = k;  /* save continuation */
    L->ci->u.c.ctx = ctx;  /* save context */
    luaD_call(L, func, nresults, 1);  /* do the call */
  }
  else  /* no continuation or no yieldable */
    luaD_call(L, func, nresults, 0);  /* just do the call */
  adjustresults(L, nresults);
  lua_unlock(L);
}



/*
** Execute a protected call.
*/
// 仅仅是为了封装而已
struct CallS {  /* data to `f_call' */
  StkId func;
  int nresults;
};


static void f_call (lua_State *L, void *ud) {
  struct CallS *c = cast(struct CallS *, ud);

  // 像这里的call就被lua_pcallk保护在luaD_rawrunproteced中
  luaD_call(L, c->func, c->nresults, 0);
}


// pcall下所有的异常都会longjmp到pcall内部
// 所以可以捕获
// pcallk -> luaD_pcall -> luaD_rawrunprotected(setjmp)
// errfunc: 错误处理函数的下标
LUA_API int lua_pcallk (lua_State *L, int nargs, int nresults, int errfunc,
                        int ctx, lua_CFunction k) {
  struct CallS c;
  int status;
  ptrdiff_t func;
  lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checknelems(L, nargs+1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  if (errfunc == 0)
    func = 0;
  else {
    StkId o = index2addr(L, errfunc);
    api_checkvalidindex(L, o);

	// func是errfunc的stack偏移(相对于整个L->stack而言)
    func = savestack(L, o);
  }
  c.func = L->top - (nargs+1);  /* function to be called */
  if (k == NULL || L->nny > 0) {  /* no continuation or no yieldable? */
    c.nresults = nresults;  /* do a 'conventional' protected call */
	// 此时没有上层的resume提供保护,所以只能此处自己提供
    status = luaD_pcall(L, f_call, &c, savestack(L, c.func), func);
  }
  else {  /* prepare continuation (call is already protected by 'resume') */
    CallInfo *ci = L->ci;
    ci->u.c.k = k;  /* save continuation */
    ci->u.c.ctx = ctx;  /* save context */
    /* save information for error recovery */
    ci->extra = savestack(L, c.func);
    ci->u.c.old_allowhook = L->allowhook;
    ci->u.c.old_errfunc = L->errfunc;
    L->errfunc = func;
    /* mark that function may do error recovery */
    ci->callstatus |= CIST_YPCALL;

	// luaD_call是不保护现场的,所以有错误时,不一定能从luaD_call返回
	// 保护机制有调用方利用resume提供,此处只保存调用信息,不提供保护
    luaD_call(L, c.func, nresults, 1);  /* do the call */
    ci->callstatus &= ~CIST_YPCALL;
    L->errfunc = ci->u.c.old_errfunc;
    status = LUA_OK;  /* if it is here, there were no errors */
  }
  adjustresults(L, nresults);
  lua_unlock(L);
  return status;
}


// top-1 = lclosure
// data是reader使用的参数
LUA_API int lua_load (lua_State *L, lua_Reader reader, void *data,
                      const char *chunkname, const char *mode) {
  ZIO z;
  int status;
  lua_lock(L);
  if (!chunkname) chunkname = "?";
  luaZ_init(L, &z, reader, data);
  status = luaD_protectedparser(L, &z, chunkname, mode);
  if (status == LUA_OK) {  /* no errors? */
    LClosure *f = clLvalue(L->top - 1);  /* get newly created function */
    // _ENV怎么办?这里直接就是全局_G
	// trunk生成的function->upvals[0]应该就是代表着_ENV,初始值是_G
	// 调用parser之后,function的upvals已经构建好,此处只是填值而已
	// 也可以看到,trunk使用全局变量,会变换为引用upvals[0]的table
	// FIXME: 还可以不使用么?
    if (f->nupvalues == 1) {  /* does it have one upvalue? */
      /* get global table from registry */
      Table *reg = hvalue(&G(L)->l_registry);
      const TValue *gt = luaH_getint(reg, LUA_RIDX_GLOBALS);
      /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
      setobj(L, f->upvals[0]->v, gt);
      luaC_barrier(L, f->upvals[0], gt);
    }
  }
  lua_unlock(L);
  return status;
}


// 类似load,并没有指明dump的去向(这个是writer关心的)
// data仍然是writer使用的参数
// 可以想象成输出编译之后的字节码+元信息
// 在内存中,可以保留各种信息,这些在最后dump到二进制,再load回来时,要完整保留
LUA_API int lua_dump (lua_State *L, lua_Writer writer, void *data) {
  int status;
  TValue *o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = L->top - 1;
  if (isLfunction(o))
    status = luaU_dump(L, getproto(o), writer, data, 0);
  else
    status = 1;
  lua_unlock(L);
  return status;
}


LUA_API int  lua_status (lua_State *L) {
  return L->status;
}


/*
** Garbage-collection function
*/

LUA_API int lua_gc (lua_State *L, int what, int data) {
  int res = 0;
  global_State *g;
  lua_lock(L);
  g = G(L);
  switch (what) {
    case LUA_GCSTOP: {
      g->gcrunning = 0;
      break;
    }
    case LUA_GCRESTART: {
      luaE_setdebt(g, 0);
      g->gcrunning = 1;
      break;
    }
    case LUA_GCCOLLECT: {
      luaC_fullgc(L, 0);
      break;
    }
    case LUA_GCCOUNT: {
      /* GC values are expressed in Kbytes: #bytes/2^10 */
      res = cast_int(gettotalbytes(g) >> 10);
      break;
    }
    case LUA_GCCOUNTB: {
      res = cast_int(gettotalbytes(g) & 0x3ff);
      break;
    }
    case LUA_GCSTEP: {
      if (g->gckind == KGC_GEN) {  /* generational mode? */
        res = (g->GCestimate == 0);  /* true if it will do major collection */
        luaC_forcestep(L);  /* do a single step */
      }
      else {
       lu_mem debt = cast(lu_mem, data) * 1024 - GCSTEPSIZE;
       if (g->gcrunning)
         debt += g->GCdebt;  /* include current debt */
       luaE_setdebt(g, debt);
       luaC_forcestep(L);
       if (g->gcstate == GCSpause)  /* end of cycle? */
         res = 1;  /* signal it */
      }
      break;
    }
    case LUA_GCSETPAUSE: {
      res = g->gcpause;
      g->gcpause = data;
      break;
    }
    case LUA_GCSETMAJORINC: {
      res = g->gcmajorinc;
      g->gcmajorinc = data;
      break;
    }
    case LUA_GCSETSTEPMUL: {
      res = g->gcstepmul;
      g->gcstepmul = data;
      break;
    }
    case LUA_GCISRUNNING: {
      res = g->gcrunning;
      break;
    }
    case LUA_GCGEN: {  /* change collector to generational mode */
      luaC_changemode(L, KGC_GEN);
      break;
    }
    case LUA_GCINC: {  /* change collector to incremental mode */
      luaC_changemode(L, KGC_NORMAL);
      break;
    }
    default: res = -1;  /* invalid option */
  }
  lua_unlock(L);
  return res;
}



/*
** miscellaneous functions
*/


// 除去lua_yieldk之外,会进行longjmp的唯一接口
// msg入栈,longjmp/abort
LUA_API int lua_error (lua_State *L) {
  lua_lock(L);
  api_checknelems(L, 1);
  luaG_errormsg(L);
  lua_unlock(L);
  return 0;  /* to avoid warnings */
}


// in idx.next
// top-1 = key
// top   = value
LUA_API int lua_next (lua_State *L, int idx) {
  StkId t;
  int more;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  more = luaH_next(L, hvalue(t), L->top - 1);
  if (more) {
    // luaH_next设置了top-1 = key, top = value
    api_incr_top(L);
  }
  else  /* no more elements */
    L->top -= 1;  /* remove key */
  lua_unlock(L);
  return more;
}


// pop 待连接str
// push最后结果
LUA_API void lua_concat (lua_State *L, int n) {
  lua_lock(L);
  api_checknelems(L, n);
  if (n >= 2) {
    luaC_checkGC(L);
    luaV_concat(L, n);
  }
  else if (n == 0) {  /* push empty string */
    setsvalue2s(L, L->top, luaS_newlstr(L, "", 0));
    api_incr_top(L);
  }
  /* else n == 1; nothing to do */
  lua_unlock(L);
}


// top = #idx
LUA_API void lua_len (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  luaV_objlen(L, L->top, t);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API lua_Alloc lua_getallocf (lua_State *L, void **ud) {
  lua_Alloc f;
  lua_lock(L);
  if (ud) *ud = G(L)->ud;
  f = G(L)->frealloc;
  lua_unlock(L);
  return f;
}


LUA_API void lua_setallocf (lua_State *L, lua_Alloc f, void *ud) {
  lua_lock(L);
  G(L)->ud = ud;
  G(L)->frealloc = f;
  lua_unlock(L);
}


// push userdata
// 实际分配 head + udata(size大小)
// 返回的是udata指针
LUA_API void *lua_newuserdata (lua_State *L, size_t size) {
  Udata *u;
  lua_lock(L);
  luaC_checkGC(L);
  u = luaS_newudata(L, size, NULL);
  setuvalue(L, L->top, u);
  api_incr_top(L);
  lua_unlock(L);
  return u + 1;
}


// 返回name or NULL
static const char *aux_upvalue (StkId fi, int n, TValue **val,
                                GCObject **owner) {
  switch (ttype(fi)) {
    case LUA_TCCL: {  /* C closure */
      CClosure *f = clCvalue(fi);
      if (!(1 <= n && n <= f->nupvalues)) return NULL;
      *val = &f->upvalue[n-1];
      if (owner) *owner = obj2gco(f);
      return "";
    }
    case LUA_TLCL: {  /* Lua closure */
      LClosure *f = clLvalue(fi);
      TString *name;
      Proto *p = f->p;
      if (!(1 <= n && n <= p->sizeupvalues)) return NULL;
      *val = f->upvals[n-1]->v;
      if (owner) *owner = obj2gco(f->upvals[n - 1]);
      name = p->upvalues[n-1].name;
      return (name == NULL) ? "" : getstr(name);
    }
    default: return NULL;  /* not a closure */
  }
}


// top = funcindex.upv[n]
// n从1开始
LUA_API const char *lua_getupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  /* to avoid warnings */
  lua_lock(L);
  name = aux_upvalue(index2addr(L, funcindex), n, &val, NULL);
  if (name) {
    setobj2s(L, L->top, val);
    api_incr_top(L);
  }
  lua_unlock(L);
  return name;
}


// funcindex.upv[n] = top-1
// pop 1 如果有该upv
LUA_API const char *lua_setupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  /* to avoid warnings */
  GCObject *owner = NULL;  /* to avoid warnings */
  StkId fi;
  lua_lock(L);
  fi = index2addr(L, funcindex);
  api_checknelems(L, 1);
  name = aux_upvalue(fi, n, &val, &owner);
  if (name) {
    L->top--;
    setobj(L, val, L->top);
    luaC_barrier(L, owner, L->top);
  }
  lua_unlock(L);
  return name;
}


static UpVal **getupvalref (lua_State *L, int fidx, int n, LClosure **pf) {
  LClosure *f;
  StkId fi = index2addr(L, fidx);
  api_check(L, ttisLclosure(fi), "Lua function expected");
  f = clLvalue(fi);
  api_check(L, (1 <= n && n <= f->p->sizeupvalues), "invalid upvalue index");
  if (pf) *pf = f;
  return &f->upvals[n - 1];  /* get its upvalue pointer */
}


// = &fidx.upv[n]
LUA_API void *lua_upvalueid (lua_State *L, int fidx, int n) {
  StkId fi = index2addr(L, fidx);
  switch (ttype(fi)) {
    case LUA_TLCL: {  /* lua closure */
      return *getupvalref(L, fidx, n, NULL);
    }
    case LUA_TCCL: {  /* C closure */
      CClosure *f = clCvalue(fi);
      api_check(L, 1 <= n && n <= f->nupvalues, "invalid upvalue index");
      return &f->upvalue[n - 1];
    }
    default: {
      api_check(L, 0, "closure expected");
      return NULL;
    }
  }
}


// fidx1.upv[n1] = fidx2.upv[n2]
LUA_API void lua_upvaluejoin (lua_State *L, int fidx1, int n1,
                                            int fidx2, int n2) {
  LClosure *f1;
  UpVal **up1 = getupvalref(L, fidx1, n1, &f1);
  UpVal **up2 = getupvalref(L, fidx2, n2, NULL);
  *up1 = *up2;
  luaC_objbarrier(L, f1, *up2);
}

