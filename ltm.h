/*
** $Id: ltm.h,v 2.11 2011/02/28 17:32:10 roberto Exp $
** Tag methods
** See Copyright Notice in lua.h
*/

#ifndef ltm_h
#define ltm_h


#include "lobject.h"


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER TM"
*/
typedef enum {
  // obj[x]
  TM_INDEX,
  // obj[x] = ??
  TM_NEWINDEX,
  // finalizer,������setmetatableʱָ��(������Բο�lapi�е�ʵ��)
  TM_GC,
  // weak
  TM_MODE,
  // #obj
  TM_LEN,
  TM_EQ,  /* last tag method with `fast' access */
  TM_ADD,
  TM_SUB,
  TM_MUL,
  TM_DIV,
  TM_MOD,
  TM_POW,
  TM_UNM,
  TM_LT,
  TM_LE,
  // a .. b
  TM_CONCAT,
  // a(...)
  TM_CALL,
  TM_N		/* number of elements in the enum */
} TMS;


// ����flags���п����ж�
#define gfasttm(g,et,e) ((et) == NULL ? NULL : \
  ((et)->flags & (1u<<(e))) ? NULL : luaT_gettm(et, e, (g)->tmname[e]))

// ��ȡet���e meta metdod(���߽�tag method)
#define fasttm(l,et,e)	gfasttm(G(l), et, e)

// x��type�ĳ���(lua.h)
#define ttypename(x)	luaT_typenames_[(x) + 1]
#define objtypename(x)	ttypename(ttypenv(x))

LUAI_DDEC const char *const luaT_typenames_[LUA_TOTALTAGS];


LUAI_FUNC const TValue *luaT_gettm (Table *events, TMS event, TString *ename);
LUAI_FUNC const TValue *luaT_gettmbyobj (lua_State *L, const TValue *o,
                                                       TMS event);
LUAI_FUNC void luaT_init (lua_State *L);

#endif
