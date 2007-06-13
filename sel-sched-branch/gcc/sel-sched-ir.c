/* Instruction scheduling pass.
   Copyright (C) 2006, 2007 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 2, or (at your option) any later
   version.

   GCC is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING.  If not, write to the Free
   Software Foundation, 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.  */


/* FIXME: check whether we need all these headers, and check the makefile.  */
#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "toplev.h"
#include "rtl.h"
#include "tm_p.h"
#include "hard-reg-set.h"
#include "regs.h"
#include "function.h"
#include "flags.h"
#include "insn-config.h"
#include "insn-attr.h"
#include "except.h"
#include "toplev.h"
#include "recog.h"
#include "cfglayout.h"
#include "params.h"
#include "target.h"
#include "timevar.h"
#include "tree-pass.h"
#include "sched-rgn.h"
#include "sched-int.h"
#include "sched-deps.h"
#include "cselib.h"
#include "ggc.h"
#include "tree.h"
#include "vec.h"
#include "langhooks.h"
#include "rtlhooks-def.h"

#ifdef INSN_SCHEDULING
#include "sel-sched-ir.h"
/* We don't have to use it except for sel_print_insn.  */
#include "sel-sched-dump.h"

/* A structure used to hold various parameters of insn initialization.  */
struct _insn_init insn_init;

/* A vector holding bb info.  */
VEC (sel_bb_info_def, heap) *sel_bb_info = NULL;

/* The loop nest being pipelined.  */
struct loop *current_loop_nest;

/* LOOP_NESTS is a vector containing the corresponding loop nest for
   each region.  */
static VEC(loop_p, heap) *loop_nests = NULL;

/* Saves blocks already in loop regions, indexed by bb->index.  */
static sbitmap bbs_in_loop_rgns = NULL;

/* A vector holding data for each insn rtx.  */
VEC (sel_insn_rtx_data_def, heap) *s_i_r_d = NULL;

/* This variable is used to ensure that no insns will be emitted by
   outer-world functions like redirect_edge_and_branch ().  */
static bool can_add_insns_p = true;

/* The same as the previous flag except that notes are allowed 
   to be emitted.  
   FIXME: avoid this dependency between files.  */
bool can_add_real_insns_p = true;

/* Redefine RTL hooks so we can catch the moment of creating an insn.  */
static void sel_rtl_insn_added (insn_t);
#undef RTL_HOOKS_INSN_ADDED
#define RTL_HOOKS_INSN_ADDED sel_rtl_insn_added
const struct rtl_hooks sel_rtl_hooks = RTL_HOOKS_INITIALIZER;


/* Array containing reverse topological index of function basic blocks,
   indexed by BB->INDEX.  */
static int *rev_top_order_index = NULL;

/* Length of the above array.  */
static int rev_top_order_index_len = -1;

/* A regset pool structure.  */
static struct
{
  regset *v;
  int n;
  int s;

  /* In VV we accumulate all generated regsets so that, when destucting the
     pool, we can compare it with V and check that every regset was returned
     back to pool.  */
  regset *vv;
  int nn;
  int ss;

  int diff;
} regset_pool = { NULL, 0, 0, NULL, 0, 0, 0 };

/* This represents the nop pool.  */
static struct
{
  insn_t *v;
  int n;
  int s;  
} nop_pool = { NULL, 0, 0 };

/* A NOP pattern used to emit placeholder insns.  */
rtx nop_pattern = NULL_RTX;
/* A special instruction that resides in EXIT_BLOCK.
   EXIT_INSN is successor of the insns that lead to EXIT_BLOCK.  */
rtx exit_insn = NULL_RTX;


/* Forward static declarations.  */
static void fence_init (fence_t, insn_t, state_t, deps_t, void *,
                        rtx, rtx, int, int, bool, bool);
static void fence_clear (fence_t);

static void deps_init_id (idata_t, insn_t, bool);

static void cfg_preds (basic_block, insn_t **, int *);



/* Various list functions.  */

/* Copy an instruction list L.  */
ilist_t
ilist_copy (ilist_t l)
{
  ilist_t head = NULL, *tailp = &head;

  while (l)
    {
      ilist_add (tailp, ILIST_INSN (l));
      tailp = &ILIST_NEXT (*tailp);
      l = ILIST_NEXT (l);
    }

  return head;
}

/* Invert an instruction list L.  */
ilist_t
ilist_invert (ilist_t l)
{
  ilist_t res = NULL;

  while (l)
    {
      ilist_add (&res, ILIST_INSN (l));
      l = ILIST_NEXT (l);
    }

  return res;
}

/* Add a new boundary to the LP list with parameters TO, PTR, and DC.  */
void
blist_add (blist_t *lp, insn_t to, ilist_t ptr, deps_t dc)
{
  bnd_t bnd;

  _list_add (lp);
  bnd = BLIST_BND (*lp);

  BND_TO (bnd) = to;
  BND_PTR (bnd) = ptr;
  BND_AV (bnd) = NULL;
  BND_AV1 (bnd) = NULL;
  BND_DC (bnd) = dc;
}

/* Remove the list note pointed to by LP.  */
void
blist_remove (blist_t *lp)
{
  bnd_t b = BLIST_BND (*lp);

  av_set_clear (&BND_AV (b));
  av_set_clear (&BND_AV1 (b));
  ilist_clear (&BND_PTR (b));

  _list_remove (lp);
}

/* Init a fence tail L.  */
void
flist_tail_init (flist_tail_t l)
{
  FLIST_TAIL_HEAD (l) = NULL;
  FLIST_TAIL_TAILP (l) = &FLIST_TAIL_HEAD (l);
}

/* Try to find fence corresponding to INSN in L.  */
fence_t
flist_lookup (flist_t l, insn_t insn)
{
  while (l)
    {
      if (FENCE_INSN (FLIST_FENCE (l)) == insn)
	return FLIST_FENCE (l);

      l = FLIST_NEXT (l);
    }

  return NULL;
}

/* Add new fence consisting of INSN and STATE to the list pointed to by LP.  */
void
flist_add (flist_t *lp, insn_t insn, state_t state, deps_t dc, void *tc, 
           insn_t last_scheduled_insn, insn_t sched_next, int cycle,
	   int cycle_issued_insns, bool starts_cycle_p, bool after_stall_p)
{
  _list_add (lp);
  fence_init (FLIST_FENCE (*lp), insn, state, dc, tc, last_scheduled_insn,
	      sched_next, cycle, cycle_issued_insns, starts_cycle_p,
	      after_stall_p);
}

/* Remove the head node of the list pointed to by LP.  */
static void
flist_remove (flist_t *lp)
{
  fence_clear (FLIST_FENCE (*lp));
  _list_remove (lp);
}

/* Clear the fence list pointed to by LP.  */
void
flist_clear (flist_t *lp)
{
  while (*lp)
    flist_remove (lp);
}

/* Add ORIGINAL_INSN the def list DL honoring CROSSES_CALL.  */
void
def_list_add (def_list_t *dl, insn_t original_insn, bool crosses_call,
	      bool needs_spec_check_p)
{
  def_t d;
  _list_add (dl);
  d = DEF_LIST_DEF (*dl);

  d->orig_insn = original_insn;
  d->crosses_call = crosses_call;
  d->needs_spec_check_p = needs_spec_check_p;
}


/* Functions to work with target contexts.  */

/* Bulk target context.
   NB: It is convenient for debuging purposes to ensure that there are no
   uninitialized (null) target contexts.  */
static tc_t bulk_tc = (tc_t) 1;

/* Target hooks wrappers.
   Possibly it would be nice to provide some default implementations for
   them. */

/* Allocate a store for the target context.  */
static tc_t
alloc_target_context (void)
{
  return (targetm.sched.alloc_sched_context
	  ? targetm.sched.alloc_sched_context () : bulk_tc);
}

/* Init target context TC.
   If CLEAN_P is true, then make TC as it is beginning of the scheduler.
   Overwise, copy current backend context to TC.  */
static void
init_target_context (tc_t tc, bool clean_p)
{
  if (targetm.sched.init_sched_context)
    targetm.sched.init_sched_context (tc, clean_p);
}

/* Allocate and initialize a target context.  Meaning of CLEAN_P is the same as
   int init_target_context ().  */
tc_t
create_target_context (bool clean_p)
{
  tc_t tc = alloc_target_context ();

  init_target_context (tc, clean_p);
  return tc;
}

/* Copy TC to the current backend context.  */
void
set_target_context (tc_t tc)
{
  if (targetm.sched.set_sched_context)
    targetm.sched.set_sched_context (tc);
}

/* TC is about to be destroyed.  Free any internal data.  */
static void
clear_target_context (tc_t tc)
{
  if (targetm.sched.clear_sched_context)
    targetm.sched.clear_sched_context (tc);
}

/*  Clear and free it.  */
static void
delete_target_context (tc_t tc)
{
  clear_target_context (tc);

  if (targetm.sched.free_sched_context)
    targetm.sched.free_sched_context (tc);
}

/* Make a copy of FROM in TO.
   NB: May be this should be a hook.  */
static void
copy_target_context (tc_t to, tc_t from)
{
  tc_t tmp = create_target_context (false);

  set_target_context (from);
  init_target_context (to, false);

  set_target_context (tmp);
  delete_target_context (tmp);
}

/* Create a copy of TC.  */
static tc_t
create_copy_of_target_context (tc_t tc)
{
  tc_t copy = alloc_target_context ();

  copy_target_context (copy, tc);

  return copy;
}

/* Clear TC and initialize it according to CLEAN_P.  The meaning of CLEAN_P
   is the same as in init_target_context ().  */
void
reset_target_context (tc_t tc, bool clean_p)
{
  clear_target_context (tc);
  init_target_context (tc, clean_p);
}

/* Functions to work with dependence contexts. 
   Dc (aka deps context, aka deps_t, aka struct deps *) is short for dependence
   context.  It accumulates information about processed insns to decide if
   current insn is dependent on the processed ones.  */

/* Make a copy of FROM in TO.  */
static void
copy_deps_context (deps_t to, deps_t from)
{
  init_deps (to);
  deps_join (to, from);
}

/* Allocate store for dep context.  */
static deps_t
alloc_deps_context (void)
{
  return xmalloc (sizeof (struct deps));
}

/* Allocate and initialize dep context.  */
static deps_t
create_deps_context (void)
{
  deps_t dc = alloc_deps_context ();

  init_deps (dc);
  return dc;
}

/* Create a copy of FROM.  */
static deps_t
create_copy_of_deps_context (deps_t from)
{
  deps_t to = alloc_deps_context ();

  copy_deps_context (to, from);
  return to;
}

/* Clean up internal data of DC.  */
static void
clear_deps_context (deps_t dc)
{
  free_deps (dc);
}

/* Clear and free DC.  */
static void
delete_deps_context (deps_t dc)
{
  clear_deps_context (dc);
  free (dc);
}

/* Clear and init DC.  */
static void
reset_deps_context (deps_t dc)
{
  clear_deps_context (dc);
  init_deps (dc);
}

static struct sched_deps_info_def _advance_deps_context_sched_deps_info =
  {
    NULL,

    NULL, /* start_insn */
    NULL, /* finish_insn */
    NULL, /* start_x */
    NULL, /* finish_x */
    NULL, /* start_lhs */
    NULL, /* finish_lhs */
    NULL, /* start_rhs */
    NULL, /* finish_rhs */
    haifa_note_reg_set,
    haifa_note_reg_clobber,
    haifa_note_reg_use,
    NULL, /* note_mem_dep */
    NULL, /* note_dep */

    0, 0, 0
  };

/* Process INSN and add its impact on DC.  */
void
advance_deps_context (deps_t dc, insn_t insn)
{
  sched_deps_info = &_advance_deps_context_sched_deps_info;
  deps_analyze_insn (dc, insn);
}


/* Functions to work with DFA states.  */

/* Allocate store for a DFA state.  */
static state_t
state_alloc (void)
{
  return xmalloc (dfa_state_size);
}

/* Allocate and initialize DFA state.  */
static state_t
state_create (void)
{
  state_t state = state_alloc ();

  state_reset (state);
  return state;
}

/* Free DFA state.  */
static void
state_free (state_t state)
{
  free (state);
}

/* Make a copy of FROM in TO.  */
static void
state_copy (state_t to, state_t from)
{
  memcpy (to, from, dfa_state_size);
}

/* Create a copy of FROM.  */
static state_t
state_create_copy (state_t from)
{
  state_t to = state_alloc ();

  state_copy (to, from);
  return to;
}


/* Functions to work with fences.  */

/* Initialize the fence.  */
static void
fence_init (fence_t f, insn_t insn, state_t state, deps_t dc, void *tc,
	    rtx last_scheduled_insn, rtx sched_next, int cycle,
	    int cycle_issued_insns, bool starts_cycle_p, bool after_stall_p)
{
  FENCE_INSN (f) = insn;

  gcc_assert (state != NULL);
  FENCE_STATE (f) = state;

  FENCE_CYCLE (f) = cycle;
  FENCE_ISSUED_INSNS (f) = cycle_issued_insns;
  FENCE_STARTS_CYCLE_P (f) = starts_cycle_p;
  FENCE_AFTER_STALL_P (f) = after_stall_p;
  
  FENCE_BNDS (f) = NULL;
  FENCE_SCHEDULED (f) = false;
  FENCE_SCHEDULED_SOMETHING (f) = false;

  gcc_assert (dc != NULL);
  FENCE_DC (f) = dc;

  gcc_assert (tc != NULL || targetm.sched.alloc_sched_context == NULL);
  FENCE_TC (f) = tc;

  FENCE_LAST_SCHEDULED_INSN (f) = last_scheduled_insn;
  FENCE_SCHED_NEXT (f) = sched_next;
}

#if 0
/* Copy the FROM fence to TO.  */
static void
fence_copy (fence_t to, fence_t from)
{
  fence_init (to, FENCE_INSN (from), state_create_copy (FENCE_STATE (from)),
	      create_copy_of_deps_context (FENCE_DC (from)),
	      create_copy_of_target_context (FENCE_TC (from)),
	      FENCE_LAST_SCHEDULED_INSN (from), FENCE_SCHED_NEXT (from),
	      FENCE_CYCLE (from), FENCE_ISSUED_INSNS (from),
	      FENCE_STARTS_CYCLE_P (from), FENCE_AFTER_STALL_P (from));
}
#endif

/* Clear the fence.  */
static void
fence_clear (fence_t f)
{
  state_t s = FENCE_STATE (f);
  deps_t dc = FENCE_DC (f);
  void *tc = FENCE_TC (f);

  ilist_clear (&FENCE_BNDS (f));

  gcc_assert ((s != NULL && dc != NULL && tc != NULL)
	      || (s == NULL && dc == NULL && tc == NULL));

  if (s != NULL)
    free (s);

  if (dc != NULL)
    free_deps (dc);

  if (tc != NULL)
    delete_target_context (tc);
}

/* Init a list of fences with the head of BB.  */
void
init_fences (basic_block bb)
{
  int succs_num;
  insn_t *succs;
  int i;

  cfg_succs_1 (bb_note (bb), SUCCS_NORMAL | SUCCS_SKIP_TO_LOOP_EXITS, 
	       &succs, &succs_num);

  gcc_assert (flag_sel_sched_pipelining_outer_loops
	      || succs_num == 1);

  for (i = 0; i < succs_num; i++)
    {
      flist_add (&fences, succs[i],
		 state_create (),
		 create_deps_context () /* dc */,
		 create_target_context (true) /* tc */,
		 NULL_RTX /* last_scheduled_insn */, NULL_RTX /* sched_next */,
		 1 /* cycle */, 0 /* cycle_issued_insns */, 
		 1 /* starts_cycle_p */, 0 /* after_stall_p */);
  
    }
  }

/* Add a new fence to NEW_FENCES list, initializing it from all 
   other parameters.  */
void
new_fences_add (flist_tail_t new_fences, insn_t insn,
		state_t state, deps_t dc, void *tc, rtx last_scheduled_insn, 
		rtx sched_next, int cycle, int cycle_issued_insns, 
                bool starts_cycle_p, bool after_stall_p)
{
  fence_t f = flist_lookup (FLIST_TAIL_HEAD (new_fences), insn);

  if (!f)
    {
      flist_add (FLIST_TAIL_TAILP (new_fences), insn, state, dc, tc,
		 last_scheduled_insn, sched_next, cycle, cycle_issued_insns,
		 starts_cycle_p, after_stall_p);

      FLIST_TAIL_TAILP (new_fences)
	= &FLIST_NEXT (*FLIST_TAIL_TAILP (new_fences));
    }
  else
    /* Here we should somehow choose between two DFA states.
       Plain reset for now.  */
    {
      gcc_assert (sel_bb_header_p (FENCE_INSN (f))
		  && !sched_next && !FENCE_SCHED_NEXT (f));

      state_reset (FENCE_STATE (f));
      state_free (state);

      reset_deps_context (FENCE_DC (f));
      delete_deps_context (dc);

      reset_target_context (FENCE_TC (f), true);
      delete_target_context (tc);

      if (cycle > FENCE_CYCLE (f))
        FENCE_CYCLE (f) = cycle;

      if (after_stall_p)
        FENCE_AFTER_STALL_P (f) = 1;

      FENCE_ISSUED_INSNS (f) = 0;
      FENCE_STARTS_CYCLE_P (f) = 1;
      FENCE_LAST_SCHEDULED_INSN (f) = NULL;
      FENCE_SCHED_NEXT (f) = NULL;
    }
}

/* Add a new fence to NEW_FENCES list and initialize most of its data 
   as a clean one.  */
void
new_fences_add_clean (flist_tail_t new_fences, insn_t succ, fence_t fence)
{
  new_fences_add (new_fences,
		  succ, state_create (), create_deps_context (),
		  create_target_context (true),
		  NULL_RTX, NULL_RTX, FENCE_CYCLE (fence) + 1,
		  0, 1, FENCE_AFTER_STALL_P (fence));
}

/* Add a new fence to NEW_FENCES list and initialize all of its data 
   from FENCE and SUCC.  */
void
new_fences_add_dirty (flist_tail_t new_fences, insn_t succ, fence_t fence)
{
  new_fences_add (new_fences,
		  succ, state_create_copy (FENCE_STATE (fence)),
		  create_copy_of_deps_context (FENCE_DC (fence)),
		  create_copy_of_target_context (FENCE_TC (fence)),
		  FENCE_LAST_SCHEDULED_INSN (fence), FENCE_SCHED_NEXT (fence),
		  FENCE_CYCLE (fence),
		  FENCE_ISSUED_INSNS (fence),
		  FENCE_STARTS_CYCLE_P (fence),
		  FENCE_AFTER_STALL_P (fence));
}


/* Functions to work with regset and nop pools.  */

regset
get_regset_from_pool (void)
{
  regset rs;

  if (regset_pool.n != 0)
    rs = regset_pool.v[--regset_pool.n];
  else
    /* We need to create the regset.  */
    {
      rs = ALLOC_REG_SET (&reg_obstack);

      if (regset_pool.nn == regset_pool.ss)
	regset_pool.vv = xrealloc (regset_pool.vv,
				   ((regset_pool.ss = 2 * regset_pool.ss + 1)
				    * sizeof (*regset_pool.vv)));

      regset_pool.vv[regset_pool.nn++] = rs;
    }

  regset_pool.diff++;

  return rs;
}

regset
get_clear_regset_from_pool (void)
{
  regset rs = get_regset_from_pool ();

  CLEAR_REG_SET (rs);
  return rs;
}

void
return_regset_to_pool (regset rs)
{
  regset_pool.diff--;

  if (regset_pool.n == regset_pool.s)
    regset_pool.v = xrealloc (regset_pool.v,
			      ((regset_pool.s = 2 * regset_pool.s + 1)
			       * sizeof (*regset_pool.v)));

  regset_pool.v[regset_pool.n++] = rs;
}

static int
cmp_v_in_regset_pool (const void *x, const void *xx)
{
  return *((const regset *) x) - *((const regset *) xx);
}

void
free_regset_pool (void)
{
  if (ENABLE_SEL_CHECKING)
    {
      regset *v = regset_pool.v;
      int i = 0;
      int n = regset_pool.n;

      regset *vv = regset_pool.vv;
      int ii = 0;
      int nn = regset_pool.nn;

      int diff = 0;

      gcc_assert (n <= nn);

      /* Sort both vectors so it will be possible to compare them.  */
      qsort (v, n, sizeof (*v), cmp_v_in_regset_pool);
      qsort (vv, nn, sizeof (*vv), cmp_v_in_regset_pool);

      while (ii < nn)
	{
	  if (v[i] == vv[ii])
	    i++;
	  else
	    /* VV[II] was lost.  */
	    diff++;

	  ii++;
	}

      gcc_assert (diff == regset_pool.diff);
    }

  /* If not true - we have a memory leak.  */
  gcc_assert (regset_pool.diff == 0);

  while (regset_pool.n)
    {
      --regset_pool.n;
      FREE_REG_SET (regset_pool.v[regset_pool.n]);
    }

  free (regset_pool.v);
  regset_pool.v = NULL;
  regset_pool.s = 0;
  
  free (regset_pool.vv);
  regset_pool.vv = NULL;
  regset_pool.nn = 0;
  regset_pool.ss = 0;

  regset_pool.diff = 0;
}

/* Functions to work with nop pools.  NOP insns are used as temporary 
   placeholders of the insns being scheduled to allow correct update of 
   the data sets.  When update is finished, NOPs are deleted.  */

static void set_insn_init (expr_t, vinsn_t, int);
static void vinsn_attach (vinsn_t);
static void vinsn_detach (vinsn_t);

/* Emit a nop before INSN, taking it from pool.  */
insn_t
get_nop_from_pool (insn_t insn)
{
  insn_t nop;
  bool old_p = nop_pool.n != 0;

  if (old_p)
    nop = nop_pool.v[--nop_pool.n];
  else
    nop = nop_pattern;

  insn_init.what = INSN_INIT_WHAT_INSN;
  nop = emit_insn_after (nop, insn);

  if (old_p)
    {
      vinsn_t vi = GET_VINSN_BY_INSN (nop);

      gcc_assert (vi != NULL);

      GET_VINSN_BY_INSN (nop) = NULL;

      insn_init.todo = INSN_INIT_TODO_SSID;
      set_insn_init (INSN_EXPR (insn), vi, INSN_SEQNO (insn));
    }
  else
    {
      insn_init.todo = INSN_INIT_TODO_LUID | INSN_INIT_TODO_SSID;
      set_insn_init (INSN_EXPR (insn), NULL, INSN_SEQNO (insn));
    }

  sel_init_new_insns ();

  if (!old_p)
    /* One more attach to GET_VINSN_BY_INSN to servive
       sched_sel_remove_insn () in return_nop_to_pool ().  */
    vinsn_attach (INSN_VINSN (nop));

  return nop;
}

/* Remove NOP from the instruction stream and return it to the pool.  */
void
return_nop_to_pool (insn_t nop)
{
  gcc_assert (INSN_VINSN (nop) != NULL);

  GET_VINSN_BY_INSN (nop) = INSN_VINSN (nop);

  gcc_assert (INSN_IN_STREAM_P (nop));
  sched_sel_remove_insn (nop);

  if (nop_pool.n == nop_pool.s)
    nop_pool.v = xrealloc (nop_pool.v, ((nop_pool.s = 2 * nop_pool.s + 1)
					* sizeof (*nop_pool.v)));

  nop_pool.v[nop_pool.n++] = nop;
}

/* Free the nop pool.  */
void
free_nop_pool (void)
{
  while (nop_pool.n)
    {
      insn_t nop = nop_pool.v[--nop_pool.n];
      vinsn_t vi = GET_VINSN_BY_INSN (nop);

      gcc_assert (vi != NULL && VINSN_COUNT (vi) == 1);
      vinsn_detach (vi);

      GET_VINSN_BY_INSN (nop) = NULL;
    }

  nop_pool.s = 0;
  free (nop_pool.v);
  nop_pool.v = NULL;
}


static bool
vinsn_equal_p (vinsn_t vi1, vinsn_t vi2)
{
  if (VINSN_TYPE (vi1) != VINSN_TYPE (vi2))
    return false;

  return (VINSN_UNIQUE_P (vi1)
	  ? VINSN_INSN (vi1) == VINSN_INSN (vi2)
	  : expr_equal_p (VINSN_PATTERN (vi1), VINSN_PATTERN (vi2)));
}

/* Returns LHS and RHS are ok to be scheduled separately.  */
static bool
lhs_and_rhs_separable_p (rtx lhs, rtx rhs)
{
  if (lhs == NULL || rhs == NULL)
    return false;

  /* Do not schedule CONST and CONST_INT as rhs: no point to use reg, 
     where const can be used.  Moreover, scheduling const as rhs may lead
     to modes mismatch cause consts don't have modes but they could be merged
     from branches where the same const used in different modes.  */
  if (GET_CODE (lhs) == CONST || GET_CODE (rhs) == CONST_INT)
    return false;

  /* ??? Do not rename predicate registers to avoid ICEs in bundling.  */
  if (COMPARISON_P (rhs))
      return false;

  /* Do not allow single REG to be an rhs.  */
  if (REG_P (rhs))
    return false;

  /* See comment at find_used_regs_1 (*1) for explanation of this 
     restriction.  */
  /* FIXME: remove this later.  */
  if (MEM_P (lhs))
    return false;

  /* This will filter all tricky things like ZERO_EXTRACT etc.
     For now we don't handle it.  */
  if (!REG_P (lhs) && !MEM_P (lhs))
    return false;

  return true;
}

/* Initialize vinsn VI for INSN.  Only for use from vinsn_create ().  */
static void
vinsn_init (vinsn_t vi, insn_t insn, bool force_unique_p)
{
  idata_t id = xcalloc (1, sizeof (*id));

  VINSN_INSN (vi) = insn;

  vi->cost = -1;

  deps_init_id (id, insn, force_unique_p);
  VINSN_ID (vi) = id;

  VINSN_COUNT (vi) = 0;

  {
    int class = haifa_classify_insn (insn);

    if (class >= 2
	&& (!targetm.sched.get_insn_spec_ds
	    || ((targetm.sched.get_insn_spec_ds (insn) & BEGIN_CONTROL)
		== 0)))
      VINSN_MAY_TRAP_P (vi) = true;
    else
      VINSN_MAY_TRAP_P (vi) = false;
  }
}

/* Indicate that VI has become the part of an rtx object.  */
static void
vinsn_attach (vinsn_t vi)
{
  /* Assert that VI is not pending for deletion.  */
  gcc_assert (VINSN_INSN (vi));

  VINSN_COUNT (vi)++;
}

/* Create and init VI from the INSN.  Initialize VINSN_COUNT (VI) with COUNT 
   and use UNIQUE_P for determining the correct VINSN_TYPE (VI).  */
static vinsn_t
vinsn_create (insn_t insn, bool force_unique_p)
{
  vinsn_t vi = xmalloc (sizeof (*vi));

  vinsn_init (vi, insn, force_unique_p);

  return vi;
}

/* Delete the VI vinsn and free its data.  */
static void
vinsn_delete (vinsn_t vi)
{
  gcc_assert (VINSN_COUNT (vi) == 0);

  return_regset_to_pool (VINSN_REG_SETS (vi));
  return_regset_to_pool (VINSN_REG_USES (vi));

  free (VINSN_ID (vi));

  /* This insn should not be deleted as it may have shared parts.  */
  /*  if (!INSN_IN_STREAM_P (insn))  expr_clear (&insn); */

  free (vi);
}

/* Indicate that VI is no longer a part of some rtx object.  
   Remove VI if it is no longer needed.  */
static void
vinsn_detach (vinsn_t vi)
{
  gcc_assert (VINSN_COUNT (vi) > 0);

  if (--VINSN_COUNT (vi) == 0)
    vinsn_delete (vi);
}

/* Returns TRUE if VI is a branch.  */
bool
vinsn_cond_branch_p (vinsn_t vi)
{
  insn_t insn;

  if (!VINSN_UNIQUE_P (vi))
    return false;

  insn = VINSN_INSN (vi);
  if (BB_END (BLOCK_FOR_INSN (insn)) != insn)
    return false;

  return control_flow_insn_p (insn);
}

/* Return latency of INSN.  */
static int
sel_insn_rtx_cost (rtx insn)
{
  int cost;

  /* A USE insn, or something else we don't need to
     understand.  We can't pass these directly to
     result_ready_cost or insn_default_latency because it will
     trigger a fatal error for unrecognizable insns.  */
  if (recog_memoized (insn) < 0)
    cost = 0;
  else
    {
      cost = insn_default_latency (insn);

      if (cost < 0)
	cost = 0;
    }

  return cost;
}

/* Return the cost of the VI.
   !!! FIXME: Unify with haifa-sched.c: insn_cost ().  */
int
sel_vinsn_cost (vinsn_t vi)
{
  int cost = vi->cost;

  if (cost < 0)
    {
      cost = sel_insn_rtx_cost (VINSN_INSN (vi));
      vi->cost = cost;
    }

  return cost;
}

/* Emit new insn after AFTER based on PATTERN and initialize its data from
   EXPR and SEQNO.  */
insn_t
sel_gen_insn_from_rtx_after (rtx pattern, rhs_t expr, int seqno,
			     insn_t after)
{
  insn_t new_insn;

  insn_init.what = INSN_INIT_WHAT_INSN;
  new_insn = emit_insn_after (pattern, after);

  insn_init.todo = INSN_INIT_TODO_LUID | INSN_INIT_TODO_SSID;
  set_insn_init (expr, NULL, seqno);
  sel_init_new_insns ();

  return new_insn;
}

/* Emit new insn after AFTER based on EXPR and SEQNO.  */
insn_t
sel_gen_insn_from_expr_after (rhs_t expr, int seqno, insn_t after)
{
  insn_t insn = RHS_INSN (expr);

  gcc_assert (!INSN_IN_STREAM_P (insn));

  insn_init.what = INSN_INIT_WHAT_INSN;
  add_insn_after (RHS_INSN (expr), after);

  insn_init.todo = INSN_INIT_TODO_SSID;
  set_insn_init (expr, EXPR_VINSN (expr), seqno);

  if (INSN_LUID (insn) == 0)
    insn_init.todo |= INSN_INIT_TODO_LUID;

  sel_init_new_insns ();

  return insn;
}

/* Functions to work with right-hand sides.  */

/* Compare two vinsns as rhses if possible and as vinsns otherwise.  */
bool
vinsns_correlate_as_rhses_p (vinsn_t x, vinsn_t y)
{
  /* We should have checked earlier for (X == Y).  */
  gcc_assert (x != y);

  if (VINSN_TYPE (x) != VINSN_TYPE (y))
    return false;

  if (VINSN_SEPARABLE_P (x)) 
    {
      /* Compare RHSes of VINSNs.  */
      gcc_assert (VINSN_RHS (x));
      gcc_assert (VINSN_RHS (y));

      return expr_equal_p (VINSN_RHS (x), 
			   VINSN_RHS (y));
    }
  else
    /* Compare whole insns. */
    return vinsn_equal_p (x, y);
}

/* Initialize RHS.  */
static void
init_expr (expr_t expr, vinsn_t vi, int spec, int priority, int sched_times,
	   ds_t spec_done_ds, ds_t spec_to_check_ds)
{
  vinsn_attach (vi);

  EXPR_VINSN (expr) = vi;
  EXPR_SPEC (expr) = spec;
  EXPR_PRIORITY (expr) = priority;
  EXPR_SCHED_TIMES (expr) = sched_times;
  EXPR_SPEC_DONE_DS (expr) = spec_done_ds;
  EXPR_SPEC_TO_CHECK_DS (expr) = spec_to_check_ds;
}

/* Make a copy of the rhs FROM into the rhs TO.  */
void
copy_expr (expr_t to, expr_t from)
{
  init_expr (to, EXPR_VINSN (from), EXPR_SPEC (from), EXPR_PRIORITY (from),
	     EXPR_SCHED_TIMES (from), EXPR_SPEC_DONE_DS (from),
	     EXPR_SPEC_TO_CHECK_DS (from));
}

/* Merge bits of FROM rhs to TO rhs.  */
void
merge_expr_data (expr_t to, expr_t from)
{
  /* For now, we just set the spec of resulting rhs to be minimum of the specs
     of merged rhses.  */
  if (RHS_SPEC (to) > RHS_SPEC (from))
    RHS_SPEC (to) = RHS_SPEC (from);

  if (RHS_PRIORITY (to) < RHS_PRIORITY (from))
    RHS_PRIORITY (to) = RHS_PRIORITY (from);

  if (RHS_SCHED_TIMES (to) > RHS_SCHED_TIMES (from))
    RHS_SCHED_TIMES (to) = RHS_SCHED_TIMES (from);

  EXPR_SPEC_DONE_DS (to) = ds_max_merge (EXPR_SPEC_DONE_DS (to),
					  EXPR_SPEC_DONE_DS (from));

  EXPR_SPEC_TO_CHECK_DS (to) |= EXPR_SPEC_TO_CHECK_DS (from);
}

/* Merge bits of FROM rhs to TO rhs.  Vinsns in the rhses should correlate.  */
void
merge_expr (expr_t to, expr_t from)
{
  vinsn_t to_vi = EXPR_VINSN (to);
  vinsn_t from_vi = EXPR_VINSN (from);

  gcc_assert (to_vi == from_vi
	      || vinsns_correlate_as_rhses_p (to_vi, from_vi));

  merge_expr_data (to, from);
}

/* Clear the information of this RHS.  */
void
clear_expr (rhs_t rhs)
{
  vinsn_detach (RHS_VINSN (rhs));
  RHS_VINSN (rhs) = NULL;
}


/* Av set functions.  */

/* Add EXPR to SETP.  */
void
av_set_add (av_set_t *setp, expr_t expr)
{
  _list_add (setp);
  copy_expr (_AV_SET_EXPR (*setp), expr);
}

/* Remove expr pointed to by IP from the av_set.  */
void
av_set_iter_remove (av_set_iterator *ip)
{
  clear_expr (_AV_SET_EXPR (*ip->lp));
  _list_iter_remove (ip);
}

/* Search for an rhs in SET, such that it's equivalent to SOUGHT_VINSN in the
   sense of vinsns_correlate_as_rhses_p function. Return NULL if no such rhs is
   in SET was found.  */
rhs_t
av_set_lookup (av_set_t set, vinsn_t sought_vinsn)
{
  rhs_t rhs;
  av_set_iterator i;

  FOR_EACH_RHS (rhs, i, set)
    {
      vinsn_t rhs_vinsn = RHS_VINSN (rhs);

      if (rhs_vinsn == sought_vinsn
	  || vinsns_correlate_as_rhses_p (rhs_vinsn, sought_vinsn))
	return rhs;
    }

  return NULL;
}

/* Search for an rhs in SET, such that it's equivalent to SOUGHT_VINSN in the
   sense of vinsns_correlate_as_rhses_p function, but not SOUGHT_VINSN itself.
   Returns NULL if no such rhs is in SET was found.  */
rhs_t
av_set_lookup_other_equiv_rhs (av_set_t set, vinsn_t sought_vinsn)
{
  rhs_t rhs;
  av_set_iterator i;

  FOR_EACH_RHS (rhs, i, set)
    {
      vinsn_t rhs_vinsn = RHS_VINSN (rhs);

      if (rhs_vinsn == sought_vinsn)
	continue;

      if (vinsns_correlate_as_rhses_p (rhs_vinsn, sought_vinsn))
	return rhs;
    }

  return NULL;
}

/* Return true if there is an expr that correlates to VI in SET.  */
bool
av_set_is_in_p (av_set_t set, vinsn_t vi)
{
  return av_set_lookup (set, vi) != NULL;
}

/* Return a copy of SET.  */
av_set_t
av_set_copy (av_set_t set)
{
  rhs_t rhs;
  av_set_iterator i;
  av_set_t res = NULL;

  FOR_EACH_RHS (rhs, i, set)
    av_set_add (&res, rhs);

  return res;
}

/* Makes set pointed to by TO to be the union of TO and FROM.  Clear av_set
   pointed to by FROMP afterwards.  */
void
av_set_union_and_clear (av_set_t *top, av_set_t *fromp)
{
  rhs_t rhs1;
  av_set_iterator i;

  /* Delete from TOP all rhses, that present in FROMP.  */
  FOR_EACH_RHS_1 (rhs1, i, top)
    {
      rhs_t rhs2 = av_set_lookup (*fromp, RHS_VINSN (rhs1));

      if (rhs2)
	{
          merge_expr (rhs2, rhs1);
	  av_set_iter_remove (&i);
	}
    }

  /* Connect FROMP to the end of the TOP.  */
  *i.lp = *fromp;
}

/* Clear av_set pointed to by SETP.  */
void
av_set_clear (av_set_t *setp)
{
  rhs_t rhs;
  av_set_iterator i;

  FOR_EACH_RHS_1 (rhs, i, setp)
    av_set_iter_remove (&i);

  gcc_assert (*setp == NULL);
}

/* Remove all the elements of SETP except for the first one.  */
void
av_set_leave_one (av_set_t *setp)
{
  av_set_clear (&_AV_SET_NEXT (*setp));
}

/* Return the N'th element of the SET.  */
rhs_t
av_set_element (av_set_t set, int n)
{
  rhs_t rhs;
  av_set_iterator i;

  FOR_EACH_RHS (rhs, i, set)
    if (n-- == 0)
      return rhs;

  gcc_unreachable ();
  return NULL;
}

/* Deletes all expressions from AVP that are conditional branches (IFs).  */
void
av_set_substract_cond_branches (av_set_t *avp)
{
  av_set_iterator i;
  rhs_t rhs;

  FOR_EACH_RHS_1 (rhs, i, avp)
    if (vinsn_cond_branch_p (RHS_VINSN (rhs)))
      av_set_iter_remove (&i);
}

/* Leave in AVP only those expressions, which are present in AV,
   and return it.  */
void
av_set_intersect (av_set_t *avp, av_set_t av)
{
  av_set_iterator i;
  rhs_t rhs;

  FOR_EACH_RHS_1 (rhs, i, avp)
    if (av_set_lookup (av, RHS_VINSN (rhs)) == NULL)
      av_set_iter_remove (&i);
}


/* Dependence hooks to initialize insn data.  */

static struct
{
  deps_where_t where;
  idata_t id;
  bool force_unique_p;
} deps_init_id_data;

/* Start initializing insn data.  */
static void
deps_init_id_start_insn (insn_t insn)
{
  int type;
  idata_t id;

  gcc_assert (deps_init_id_data.where == DEPS_IN_NOWHERE);

  /* Determine whether INSN could be cloned and return appropriate vinsn type.
     That clonable insns which can be separated into lhs and rhs have type SET.
     Other clonable insns have type USE.  */
  type = GET_CODE (insn);

  /* Only regular insns could be cloned.  */
  if (type == INSN)
    {
      if (!deps_init_id_data.force_unique_p)
	{
	  type = USE;

	  if (enable_schedule_as_rhs_p)
	    type = SET;
	}
    }
  else if (type == JUMP_INSN)
    {
      if (simplejump_p (insn))
	type = PC;
    }

  id = deps_init_id_data.id;

  IDATA_TYPE (id) = type;

  IDATA_REG_SETS (id) = get_clear_regset_from_pool ();
  IDATA_REG_USES (id) = get_clear_regset_from_pool ();

  deps_init_id_data.where = DEPS_IN_INSN;
}

/* Start initializing lhs data.  */
static void
deps_init_id_start_lhs (rtx lhs)
{
  gcc_assert (deps_init_id_data.where == DEPS_IN_INSN);

  gcc_assert (IDATA_LHS (deps_init_id_data.id) == NULL);

  if (IDATA_TYPE (deps_init_id_data.id) == SET)
    {
      IDATA_LHS (deps_init_id_data.id) = lhs;
      deps_init_id_data.where = DEPS_IN_LHS;
    }
}

/* Finish initializing lhs data.  */
static void
deps_init_id_finish_lhs (void)
{
  deps_init_id_data.where = DEPS_IN_INSN;
}

/* Downgrade to USE.  */
static void
deps_init_id_downgrade_to_use (void)
{
  gcc_assert (IDATA_TYPE (deps_init_id_data.id) == SET);

  IDATA_TYPE (deps_init_id_data.id) = USE;

  IDATA_LHS (deps_init_id_data.id) = NULL;
  IDATA_RHS (deps_init_id_data.id) = NULL;

  deps_init_id_data.where = DEPS_IN_INSN;
}

/* Note a set of REGNO.  */
static void
deps_init_id_note_reg_set (int regno)
{
  haifa_note_reg_set (regno);

  if (deps_init_id_data.where == DEPS_IN_RHS)
    deps_init_id_downgrade_to_use ();

  if (IDATA_TYPE (deps_init_id_data.id) != PC)
    SET_REGNO_REG_SET (IDATA_REG_SETS (deps_init_id_data.id), regno);
}

/* Note a clobber of REGNO.  */
static void
deps_init_id_note_reg_clobber (int regno)
{
  haifa_note_reg_clobber (regno);

  if (deps_init_id_data.where == DEPS_IN_RHS)
    deps_init_id_downgrade_to_use ();

  if (IDATA_TYPE (deps_init_id_data.id) != PC)
    SET_REGNO_REG_SET (IDATA_REG_SETS (deps_init_id_data.id), regno);
}

/* Note a use of REGNO.  */
static void
deps_init_id_note_reg_use (int regno)
{
  haifa_note_reg_use (regno);

  if (IDATA_TYPE (deps_init_id_data.id) != PC)
    SET_REGNO_REG_SET (IDATA_REG_USES (deps_init_id_data.id), regno);
}

/* Start initializing rhs data.  */
static void
deps_init_id_start_rhs (rtx rhs)
{
  gcc_assert (deps_init_id_data.where == DEPS_IN_INSN);

  /* And there was no sel_deps_reset_to_insn ().  */
  if (IDATA_LHS (deps_init_id_data.id) != NULL)
    {
      IDATA_RHS (deps_init_id_data.id) = rhs;
      deps_init_id_data.where = DEPS_IN_RHS;
    }
}

/* Finish initializing rhs data.  */
static void
deps_init_id_finish_rhs (void)
{
  gcc_assert (deps_init_id_data.where == DEPS_IN_RHS
	      || deps_init_id_data.where == DEPS_IN_INSN);

  deps_init_id_data.where = DEPS_IN_INSN;
}

/* Finish initializing insn data.  */
static void
deps_init_id_finish_insn (void)
{
  gcc_assert (deps_init_id_data.where == DEPS_IN_INSN);

  if (IDATA_TYPE (deps_init_id_data.id) == SET)
    {
      rtx lhs = IDATA_LHS (deps_init_id_data.id);
      rtx rhs = IDATA_RHS (deps_init_id_data.id);

      if (lhs == NULL || rhs == NULL || !lhs_and_rhs_separable_p (lhs, rhs))
	/* Downgrade to USE.  */
	deps_init_id_downgrade_to_use ();
    }

  deps_init_id_data.where = DEPS_IN_NOWHERE;
}

static const struct sched_deps_info_def const_deps_init_id_sched_deps_info =
  {
    NULL,

    deps_init_id_start_insn,
    deps_init_id_finish_insn,
    NULL, /* start_x */
    NULL, /* finish_x */
    deps_init_id_start_lhs,
    deps_init_id_finish_lhs,
    deps_init_id_start_rhs,
    deps_init_id_finish_rhs,
    deps_init_id_note_reg_set,
    deps_init_id_note_reg_clobber,
    deps_init_id_note_reg_use,
    NULL, /* note_mem_dep */
    NULL, /* note_dep */

    0, /* use_cselib */
    0, /* use_deps_list */
    0 /* generate_spec_deps */
  };

static struct sched_deps_info_def deps_init_id_sched_deps_info;

/* Initialize instruction data for INSN in ID.  */
static void
deps_init_id (idata_t id, insn_t insn, bool force_unique_p)
{
  struct deps _dc, *dc = &_dc;

  deps_init_id_data.where = DEPS_IN_NOWHERE;
  deps_init_id_data.id = id;
  deps_init_id_data.force_unique_p = force_unique_p;

  init_deps (dc);

  memcpy (&deps_init_id_sched_deps_info,
	  &const_deps_init_id_sched_deps_info,
	  sizeof (deps_init_id_sched_deps_info));

  if (spec_info != NULL)
    deps_init_id_sched_deps_info.generate_spec_deps = 1;

  sched_deps_info = &deps_init_id_sched_deps_info;

  deps_analyze_insn (dc, insn);

  free_deps (dc);

  deps_init_id_data.id = NULL;
}



static bool
sel_cfg_note_p (insn_t insn)
{
  return NOTE_INSN_BASIC_BLOCK_P (insn) || LABEL_P (insn);
}

/* Implement hooks for collecting fundamental insn properties like if insn is
   an ASM or is within a SCHED_GROUP.  */

/* Data for global dependency analysis (to initialize CANT_MOVE and
   SCHED_GROUP_P).  */
static struct
{
  /* Previous insn.  */
  insn_t prev_insn;
} init_global_data;

/* Determine if INSN is in the sched_group, is an asm or should not be
   cloned.  After that initialize its expr.  */
static void
init_global_and_expr_for_insn (insn_t insn)
{
  if (sel_cfg_note_p (insn))
    return;

  gcc_assert (INSN_P (insn));

  if (sel_bb_header_p (insn))
    init_global_data.prev_insn = NULL_RTX;

  if (SCHED_GROUP_P (insn))
    /* Setup a sched_group.  */
    {
      insn_t prev_insn = init_global_data.prev_insn;

      if (prev_insn)
	INSN_SCHED_NEXT (prev_insn) = insn;

      init_global_data.prev_insn = insn;
    }
  else
    init_global_data.prev_insn = NULL_RTX;

  if (GET_CODE (PATTERN (insn)) == ASM_INPUT
      || asm_noperands (PATTERN (insn)) >= 0)
    /* Mark INSN as an asm.  */
    INSN_ASM_P (insn) = true;

  {
    bool force_unique_p;
    ds_t spec_done_ds;

    /* Certain instructions cannot be cloned.  */
    if (CANT_MOVE (insn)
	|| INSN_ASM_P (insn)
	|| SCHED_GROUP_P (insn)
	|| prologue_epilogue_contains (insn) 
	/* Exception handling insns are always unique.  */
	|| (flag_non_call_exceptions && can_throw_internal (insn)))
      force_unique_p = true;
    else
      force_unique_p = false;

    if (targetm.sched.get_insn_spec_ds)
      {
	spec_done_ds = targetm.sched.get_insn_spec_ds (insn);
	spec_done_ds = ds_get_max_dep_weak (spec_done_ds);
      }
    else
      spec_done_ds = 0;

    /* Initialize INSN's expr.  */
    init_expr (INSN_EXPR (insn), vinsn_create (insn, force_unique_p), 0,
	       INSN_PRIORITY (insn), 0, spec_done_ds, 0);
  }
}

static void extend_insn (void);

/* Scan the region and initialize instruction data.  */
void
sel_init_global_and_expr (bb_vec_t bbs)
{
  {
    /* ??? It would be nice to implement push / pop scheme for sched_infos.  */
    const struct sched_scan_info_def ssi =
      {
	NULL, /* extend_bb */
	NULL, /* init_bb */
	extend_insn, /* extend_insn */
	init_global_and_expr_for_insn /* init_insn */
      };

    sched_scan (&ssi, bbs, NULL, NULL, NULL);
  }
}

/* Perform stage 1 of finalization of the INSN's data.  */
static void
finish_global_and_expr_insn_1 (insn_t insn)
{
  if (sel_cfg_note_p (insn))
    return;

  gcc_assert (INSN_P (insn));

  if (INSN_LUID (insn) > 0)
    av_set_clear (&AV_SET (insn));
}

/* Perform stage 2 of finalization of the INSN's data.  */
static void
finish_global_and_expr_insn_2 (insn_t insn)
{
  if (sel_cfg_note_p (insn))
    return;

  gcc_assert (INSN_P (insn));

  if (INSN_LUID (insn) > 0)
    {
      gcc_assert (VINSN_COUNT (INSN_VINSN (insn)) == 1);

      clear_expr (INSN_EXPR (insn));
    }
}

static void finish_insn (void);

/* Finalize per instruction data for the whole region.  */
void
sel_finish_global_and_expr (void)
{
  {
    bb_vec_t bbs;
    int i;

    bbs = VEC_alloc (basic_block, heap, current_nr_blocks);

    for (i = 0; i < current_nr_blocks; i++)
      VEC_quick_push (basic_block, bbs, BASIC_BLOCK (BB_TO_BLOCK (i)));

    /* Before cleaning up insns exprs we first must clean all the cached
       av_set.  */

    /* Clear INSN_AVs.  */
    {
      const struct sched_scan_info_def ssi =
	{
	  NULL, /* extend_bb */
	  NULL, /* init_bb */
	  NULL, /* extend_insn */
	  finish_global_and_expr_insn_1 /* init_insn */
	};

      sched_scan (&ssi, bbs, NULL, NULL, NULL);
    }

    /* Clear INSN_EXPRs.  */
    {
      const struct sched_scan_info_def ssi =
	{
	  NULL, /* extend_bb */
	  NULL, /* init_bb */
	  NULL, /* extend_insn */
	  finish_global_and_expr_insn_2 /* init_insn */
	};

      sched_scan (&ssi, bbs, NULL, NULL, NULL);
    }

    VEC_free (basic_block, heap, bbs);
  }

  finish_insn ();
}

/* In the below hooks, we merely calculate whether or not a dependence 
   exists, and in what part of insn.  However, we will need more data 
   when we'll start caching dependence requests.  */

/* Container to hold information for dependency analysis.  */
static struct _has_dependence_data
{
  deps_t dc;

  /* A variable to track which part of rtx we are scanning in
     sched-deps.c: sched_analyze_insn ().  */
  deps_where_t where;

  /* Current producer.  */
  insn_t pro;

  /* Current consumer.  */
  vinsn_t con;

  /* Is SEL_DEPS_HAS_DEP_P[DEPS_IN_X] is true, then X has a dependence.
     X is from { INSN, LHS, RHS }.  */
  ds_t has_dep_p[DEPS_IN_NOWHERE];
} has_dependence_data;

/* Start analyzing dependencies of INSN.  */
static void
has_dependence_start_insn (insn_t insn ATTRIBUTE_UNUSED)
{
  gcc_assert (has_dependence_data.where == DEPS_IN_NOWHERE);

  has_dependence_data.where = DEPS_IN_INSN;
}

/* Finish analyzing dependencies of an insn.  */
static void
has_dependence_finish_insn (void)
{
  gcc_assert (has_dependence_data.where == DEPS_IN_INSN);

  has_dependence_data.where = DEPS_IN_NOWHERE;
}

/* Start analyzing dependencies of LHS.  */
static void
has_dependence_start_lhs (rtx lhs ATTRIBUTE_UNUSED)
{
  gcc_assert (has_dependence_data.where == DEPS_IN_INSN);

  if (VINSN_LHS (has_dependence_data.con) != NULL)
    has_dependence_data.where = DEPS_IN_LHS;
}

/* Finish analyzing dependencies of an lhs.  */
static void
has_dependence_finish_lhs (void)
{
  has_dependence_data.where = DEPS_IN_INSN;
}

/* Start analyzing dependencies of RHS.  */
static void
has_dependence_start_rhs (rtx rhs ATTRIBUTE_UNUSED)
{
  gcc_assert (has_dependence_data.where == DEPS_IN_INSN);

  if (VINSN_RHS (has_dependence_data.con) != NULL)
    has_dependence_data.where = DEPS_IN_RHS;
}

/* Start analyzing dependencies of an rhs.  */
static void
has_dependence_finish_rhs (void)
{
  gcc_assert (has_dependence_data.where == DEPS_IN_RHS
	      || has_dependence_data.where == DEPS_IN_INSN);

  has_dependence_data.where = DEPS_IN_INSN;
}

/* Note a set of REGNO.  */
static void
has_dependence_note_reg_set (int regno)
{
  struct deps_reg *reg_last = &has_dependence_data.dc->reg_last[regno];

  if (!sched_insns_conditions_mutex_p (has_dependence_data.pro,
				       VINSN_INSN
				       (has_dependence_data.con)))
    {
      ds_t *dsp = &has_dependence_data.has_dep_p[has_dependence_data.where];

      if (reg_last->sets != NULL
	  || reg_last->clobbers != NULL)
	*dsp = (*dsp & ~SPECULATIVE) | DEP_OUTPUT;

      if (reg_last->uses)
	*dsp = (*dsp & ~SPECULATIVE) | DEP_ANTI;
    }
}

/* Note a clobber of REGNO.  */
static void
has_dependence_note_reg_clobber (int regno)
{
  struct deps_reg *reg_last = &has_dependence_data.dc->reg_last[regno];

  if (!sched_insns_conditions_mutex_p (has_dependence_data.pro,
				       VINSN_INSN
				       (has_dependence_data.con)))
    {
      ds_t *dsp = &has_dependence_data.has_dep_p[has_dependence_data.where];

      if (reg_last->sets)
	*dsp = (*dsp & ~SPECULATIVE) | DEP_OUTPUT;
	
      if (reg_last->uses)
	*dsp = (*dsp & ~SPECULATIVE) | DEP_ANTI;
    }
}

/* Note a use of REGNO.  */
static void
has_dependence_note_reg_use (int regno)
{
  struct deps_reg *reg_last = &has_dependence_data.dc->reg_last[regno];

  if (!sched_insns_conditions_mutex_p (has_dependence_data.pro,
				       VINSN_INSN
				       (has_dependence_data.con)))
    {
      ds_t *dsp = &has_dependence_data.has_dep_p[has_dependence_data.where];

      if (reg_last->sets)
	*dsp = (*dsp & ~SPECULATIVE) | DEP_TRUE;

      if (reg_last->clobbers)
	*dsp = (*dsp & ~SPECULATIVE) | DEP_ANTI;
    }
}

/* Note a memory dependence.  */
static void
has_dependence_note_mem_dep (rtx mem ATTRIBUTE_UNUSED,
			     rtx pending_mem ATTRIBUTE_UNUSED,
			     insn_t pending_insn ATTRIBUTE_UNUSED,
			     ds_t ds ATTRIBUTE_UNUSED)
{
  if (!sched_insns_conditions_mutex_p (has_dependence_data.pro,
				       VINSN_INSN (has_dependence_data.con)))
    {
      ds_t *dsp = &has_dependence_data.has_dep_p[has_dependence_data.where];

      *dsp = ds_full_merge (ds, *dsp, pending_mem, mem);
    }
}

/* Note a dependence.  */
static void
has_dependence_note_dep (insn_t pro ATTRIBUTE_UNUSED,
			 ds_t ds ATTRIBUTE_UNUSED)
{
  if (!sched_insns_conditions_mutex_p (has_dependence_data.pro,
				       VINSN_INSN (has_dependence_data.con)))
    {
      ds_t *dsp = &has_dependence_data.has_dep_p[has_dependence_data.where];

      *dsp = ds_full_merge (ds, *dsp, NULL_RTX, NULL_RTX);
    }
}

static const struct sched_deps_info_def const_has_dependence_sched_deps_info =
  {
    NULL,

    has_dependence_start_insn,
    has_dependence_finish_insn,
    NULL, /* start_x */
    NULL, /* finish_x */
    has_dependence_start_lhs,
    has_dependence_finish_lhs,
    has_dependence_start_rhs,
    has_dependence_finish_rhs,
    has_dependence_note_reg_set,
    has_dependence_note_reg_clobber,
    has_dependence_note_reg_use,
    has_dependence_note_mem_dep,
    has_dependence_note_dep,

    0, /* use_cselib */
    0, /* use_deps_list */
    0 /* generate_spec_deps */
  };

static struct sched_deps_info_def has_dependence_sched_deps_info;

static void
setup_has_dependence_sched_deps_info (void)
{
  memcpy (&has_dependence_sched_deps_info,
	  &const_has_dependence_sched_deps_info,
	  sizeof (has_dependence_sched_deps_info));

  if (spec_info != NULL)
    has_dependence_sched_deps_info.generate_spec_deps = 1;

  sched_deps_info = &has_dependence_sched_deps_info;
}

void
sel_clear_has_dependence (void)
{
  int i;

  for (i = 0; i < DEPS_IN_NOWHERE; i++)
    has_dependence_data.has_dep_p[i] = 0;
}

/* Return nonzero if RHS has is dependent upon PRED.  */
ds_t
has_dependence_p (rhs_t rhs, insn_t pred, ds_t **has_dep_pp)
{
  struct deps _dc;
  int i;
  ds_t ds;

  if (INSN_SIMPLEJUMP_P (pred))
    /* Unconditional jump is just a transfer of control flow.
       Ignore it.  */
    return false;

  has_dependence_data.dc = &_dc;
  init_deps (has_dependence_data.dc);

  /* Initialize empty dep context with information about PRED.  */
  advance_deps_context (has_dependence_data.dc, pred);

  has_dependence_data.where = DEPS_IN_NOWHERE;

  has_dependence_data.pro = pred;

  has_dependence_data.con = RHS_VINSN (rhs);

  sel_clear_has_dependence ();

  /* Now catch all dependencies that would be generated between PRED and
     INSN.  */
  setup_has_dependence_sched_deps_info ();
  deps_analyze_insn (has_dependence_data.dc, RHS_INSN (rhs));

  free_deps (has_dependence_data.dc);

  *has_dep_pp = has_dependence_data.has_dep_p;

  ds = 0;

  for (i = 0; i < DEPS_IN_NOWHERE; i++)
    ds = ds_full_merge (ds, has_dependence_data.has_dep_p[i],
			NULL_RTX, NULL_RTX);

  return ds;
}

/* Dependence hooks implementation that checks dependence latency constraints 
   on the insns being scheduled.  The entry point for these routines is 
   tick_check_p predicate.  */ 

static struct
{
  /* An rhs we are currently checking.  */
  rhs_t rhs;

  /* A minimal cycle for its scheduling.  */
  int cycle;

  /* Whether we have seen a true dependence while checking.  */
  bool seen_true_dep_p;
} tick_check_data;

/* Update minimal scheduling cycle for tick_check_insn given that it depends
   on PRO with status DS and weight DW.  */
static void
tick_check_dep_with_dw (insn_t pro_insn, ds_t ds, dw_t dw)
{
  rhs_t con_rhs = tick_check_data.rhs;
  insn_t con_insn = RHS_INSN (con_rhs);

  if (con_insn != pro_insn)
    {
      enum reg_note dt;
      int tick;

      if (/* PROducer was removed from above due to pipelining.  See PR8.  */
	  !INSN_IN_STREAM_P (pro_insn)
	  /* Or PROducer was originally on the next iteration regarding the
	     CONsumer.  */
	  || (INSN_SCHED_TIMES (pro_insn)
	      - RHS_SCHED_TIMES (con_rhs)) > 1)
	/* Don't count this dependence.  */
	{
	  /* ??? This assert fails on a large testcase.  It is not clear
	     to me if the assert is right so defer debugging until a smaller
	     testcase is avalailable.  */
	  gcc_assert (1 || pipelining_p);

	  return;
	}

      dt = ds_to_dt (ds);
      if (dt == REG_DEP_TRUE)
        tick_check_data.seen_true_dep_p = true;

      gcc_assert (INSN_SCHED_CYCLE (pro_insn) > 0);

      tick = (INSN_SCHED_CYCLE (pro_insn)
	      + dep_cost (pro_insn, dt, dw, con_insn));

      /* When there are several kinds of dependencies between pro and con,
         only REG_DEP_TRUE should be taken into account.  */
      if (tick > tick_check_data.cycle
	  && (dt == REG_DEP_TRUE || !tick_check_data.seen_true_dep_p))
	tick_check_data.cycle = tick;
    }
}

/* An implementation of note_dep hook.  */
static void
tick_check_note_dep (insn_t pro, ds_t ds)
{
  tick_check_dep_with_dw (pro, ds, 0);
}

/* An implementation of note_mem_dep hook.  */
static void
tick_check_note_mem_dep (rtx mem1, rtx mem2, insn_t pro, ds_t ds)
{
  dw_t dw;

  dw = (ds_to_dt (ds) == REG_DEP_TRUE
        ? estimate_dep_weak (mem1, mem2)
        : 0);

  tick_check_dep_with_dw (pro, ds, dw);
}

static struct sched_deps_info_def _tick_check_sched_deps_info =
  {
    NULL,

    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    haifa_note_reg_set,
    haifa_note_reg_clobber,
    haifa_note_reg_use,
    tick_check_note_mem_dep,
    tick_check_note_dep,

    0, 0, 0
  };

/* Returns true when VI's insn can be scheduled on the current cycle of 
   FENCE.  That is, all data from possible producers in DC_ORIG is ready.  */
bool
tick_check_p (rhs_t rhs, deps_t dc_orig, fence_t fence)
{
  struct deps _dc, *dc = &_dc;

  /* Initialize variables.  */
  tick_check_data.rhs = rhs;
  tick_check_data.cycle = 0;
  tick_check_data.seen_true_dep_p = false;

  /* Calculate TICK_CHECK_CYCLE.  */
  copy_deps_context (dc, dc_orig);

  sched_deps_info = &_tick_check_sched_deps_info;
  deps_analyze_insn (dc, RHS_INSN (rhs));

  free_deps (dc);

  return FENCE_CYCLE (fence) >= tick_check_data.cycle;
}



/* Functions to work with insns.  */

/* Returns true if LHS of INSN is a register and it's the same register as
   R2.  */
bool
lhs_of_insn_equals_to_reg_p (insn_t insn, rtx reg)
{
  rtx lhs = INSN_LHS (insn);

  gcc_assert (reg != NULL_RTX);

  if (lhs == NULL)
    return false;

  return REG_P (lhs) && (REGNO (lhs) == REGNO (reg));
}

/* Returns whether INSN_RTX is valid in terms of target architecture.
   Don't use this function inside gcc_assert () because it has side effects:
   e.g. it initializes INSN_CODE (INSN_RTX).  */
bool
insn_rtx_valid (rtx insn_rtx)
{
  /* Reset the INSN_CODE.  After register replacement it might have became
     a different insn.  */
  INSN_CODE (insn_rtx) = -1;

  if (recog_memoized (insn_rtx) >= 0)
    {
      extract_insn (insn_rtx);
      return constrain_operands (reload_completed) != 0;
    }
  else
    return false;
}

/* Returns whether INSN is eligible for substitution, i.e. it's a copy
   operation x := y, and RHS that is moved up through this insn should be
   substituted.  */
bool
insn_eligible_for_subst_p (insn_t insn)
{
  /* Since we've got INSN_LHS and INSN_RHS it should be the SET insn,
     and it's RHS is free of side effects (like AUTO_INC), 
     so we just need to make sure the INSN_RHS consists of only one simple
     REG rtx.  */

  if (INSN_RHS (insn) && INSN_LHS (insn))
    {
      if (REG_P (INSN_RHS (insn)) && REG_P (INSN_LHS (insn)))
	{
	  gcc_assert (GET_MODE (INSN_LHS (insn)) 
		      == GET_MODE (INSN_RHS (insn)));
	}
      if ((REG_P (INSN_RHS (insn)) 
	   && (REG_P (INSN_LHS (insn)) 
	       || GET_CODE (INSN_RHS (insn)) == CONST_INT)))
	  return true;             
    }
  return false;
}

/* Extracts machine mode MODE and destination location DST_LOC 
   for given INSN.  */
void
get_dest_and_mode (rtx insn, rtx *dst_loc, enum machine_mode *mode)
{
  rtx pat = PATTERN (insn);

  gcc_assert (dst_loc);
  gcc_assert (GET_CODE (pat) == SET);

  *dst_loc = SET_DEST (pat);

  gcc_assert (*dst_loc);
  gcc_assert (MEM_P (*dst_loc) || REG_P (*dst_loc));

  if (mode)
    *mode = GET_MODE (*dst_loc);
}

/* Returns true when moving through JUMP will result in bookkeeping 
   creation.  */
bool
bookkeeping_can_be_created_if_moved_through_p (insn_t jump)
{
  insn_t succ;
  succ_iterator si;

  if (BB_END (BLOCK_FOR_INSN (jump)) != jump
      || !control_flow_insn_p (jump))
    /* Exit early.  */
    return false;

  FOR_EACH_SUCC (succ, si, jump)
    if (num_preds_gt_1 (succ))
      return true;

  return false;
}

/* Rip-off INSN from the insn stream.  */
void
sched_sel_remove_insn (insn_t insn)
{
  gcc_assert (AV_SET (insn) == NULL && !INSN_AV_VALID_P (insn)
	      && !LV_SET_VALID_P (insn));

  if (INSN_IN_STREAM_P (insn))
    remove_insn (insn);

  /* It is necessary to null this fields before calling add_insn ().  */
  PREV_INSN (insn) = NULL_RTX;
  NEXT_INSN (insn) = NULL_RTX;

  clear_expr (INSN_EXPR (insn));
}

/* Transfer av and lv sets from FROM to TO.  */
void
transfer_data_sets (insn_t to, insn_t from)
{
  /* We used to assert !INSN_AV_VALID_P here, but this is wrong when 
     during previous compute_av_set the window size was reached 
     exactly at TO.  In this case, AV_SET (to) would be NULL.  */
  gcc_assert (AV_SET (to) == NULL && !LV_SET_VALID_P (to));

  AV_SET (to) = AV_SET (from);
  AV_SET (from) = NULL;

  AV_LEVEL (to) = AV_LEVEL (from);
  AV_LEVEL (from) = 0;

  LV_SET (to) = LV_SET (from);
  LV_SET (from) =  NULL;
}

/* Estimate number of the insns in BB.  */
static int
sel_estimate_number_of_insns (basic_block bb)
{
  int res = 0;
  insn_t insn = NEXT_INSN (BB_HEAD (bb)), next_tail = NEXT_INSN (BB_END (bb));

  for (; insn != next_tail; insn = NEXT_INSN (insn))
    if (INSN_P (insn))
      res++;

  return res;
}

/* We don't need separate luids for notes or labels.  */
static int
sel_luid_for_non_insn (rtx x)
{
  gcc_assert (NOTE_P (x) || LABEL_P (x));

  return -1;
}

/* Extend data structures that are indexed by INSN_UID.  */
void
sel_extend_insn_rtx_data (void)
{
  sched_extend_target ();
  sched_deps_local_init (false);

  {
    int new_size = get_max_uid () + 1;

    VEC_safe_grow_cleared (sel_insn_rtx_data_def, heap, s_i_r_d, new_size);
  }
}

/* Finalize data structures that are indexed by INSN_UID.  */
void
sel_finish_insn_rtx_data (void)
{
  sched_deps_local_finish ();
  VEC_free (sel_insn_rtx_data_def, heap, s_i_r_d);

  /* Target will finalize its data structures in
     targetm.sched.md_global_finish ().  */
}

/* Return seqno of the only predecessor of INSN.  */
static int
get_seqno_of_a_pred (insn_t insn)
{
  int seqno;

  gcc_assert (INSN_SIMPLEJUMP_P (insn));

  if (!sel_bb_header_p (insn))
    seqno = INSN_SEQNO (PREV_INSN (insn));
  else
    {
      basic_block bb = BLOCK_FOR_INSN (insn);

      if (single_pred_p (bb)
	  && !in_current_region_p (single_pred (bb)))
	/* We can have preds outside a region when splitting edges
	   for pipelining of an outer loop.  Use succ instead.  */
	{
	  insn_t succ;

	  gcc_assert (flag_sel_sched_pipelining_outer_loops
		      && current_loop_nest);

	  succ = cfg_succ_1 (insn, (SUCCS_NORMAL | SUCCS_SKIP_TO_LOOP_EXITS));
	  gcc_assert (succ != NULL);

	  seqno = INSN_SEQNO (succ);
	}
      else
	{
	  insn_t *preds;
	  int n;

	  cfg_preds (BLOCK_FOR_INSN (insn), &preds, &n);
	  gcc_assert (n == 1);

	  seqno = INSN_SEQNO (preds[0]);
              
	  free (preds);
	}
    }

#ifdef ENABLE_CHECKING
  {
    insn_t succ = cfg_succ (insn);

    gcc_assert ((succ != NULL && seqno <= INSN_SEQNO (succ))
		|| (succ == NULL && flag_sel_sched_pipelining_outer_loops));
  }
#endif

  return seqno;
}

/* Data for each insn in current region.  */
VEC (sel_insn_data_def, heap) *s_i_d = NULL;

/* Extend data structures for insns from current region.  */
static void
extend_insn (void)
{
  /* Extend data structures that are indexed by INSN_UID.  */
  sel_extend_insn_rtx_data ();

  /* Extend data structures for insns from current region.  */
  VEC_safe_grow_cleared (sel_insn_data_def, heap, s_i_d,
			 sched_max_luid);
}

/* Finalize data structures for insns from current region.  */
static void
finish_insn (void)
{
  VEC_free (sel_insn_data_def, heap, s_i_d);
  deps_finish_d_i_d ();
}

static insn_vec_t new_insns = NULL;

/* An implementation of RTL_HOOKS_INSN_ADDED hook.  The hook is used for 
   initializing data structures when new insn is emitted.
   This hook remembers all relevant instuctions which can be initialized later
   with the call to sel_init_new_insns ().  */
static void
sel_rtl_insn_added (insn_t insn)
{
  gcc_assert (can_add_insns_p
	      && (!INSN_P (insn) || can_add_real_insns_p));

  if (!INSN_P (insn) || insn_init.what == INSN_INIT_WHAT_INSN_RTX)
    return;

  gcc_assert (BLOCK_FOR_INSN (insn) == NULL
	      || (VEC_length (sel_bb_info_def, sel_bb_info)
		  <= (unsigned) BLOCK_NUM (insn))
	      || (CONTAINING_RGN (BB_TO_BLOCK (0)) 
		  == CONTAINING_RGN (BLOCK_NUM (insn))));

  /* Initialize a bit later because something (e.g. CFG) is not
     consistent yet.  These insns will be initialized when
     sel_init_new_insns () is called.  */
  VEC_safe_push (rtx, heap, new_insns, insn);
}

/* A proxy to pass initialization data to init_insn ().  */
static sel_insn_data_def _insn_init_ssid;
static sel_insn_data_t insn_init_ssid = &_insn_init_ssid;

/* A dummy variable used in set_insn_init () and init_insn ().  */
static vinsn_t empty_vinsn = NULL;

/* Set all necessary data for initialization of the new insn[s].  */
static void
set_insn_init (expr_t expr, vinsn_t vi, int seqno)
{
  expr_t x = &insn_init_ssid->_expr;

  copy_expr (x, expr);

  if (vi != NULL)
    change_vinsn_in_expr (x, vi);
  else
    change_vinsn_in_expr (x, empty_vinsn);

  insn_init_ssid->seqno = seqno;
}

/* Init data for INSN.  */
static void
init_insn (insn_t insn)
{
  expr_t expr;
  expr_t x;
  sel_insn_data_t ssid = insn_init_ssid;

  /* The fields mentioned below are special and hence are not being
     propagated to the new insns.  */
  gcc_assert (!ssid->asm_p && ssid->sched_next == NULL
	      && ssid->av_level == 0 && ssid->av == NULL
	      && !ssid->after_stall_p && ssid->sched_cycle == 0);

  gcc_assert (INSN_P (insn) && INSN_LUID (insn) > 0);

  expr = INSN_EXPR (insn);
  x = &ssid->_expr;

  copy_expr (expr, x);

  if (EXPR_VINSN (x) == empty_vinsn)
    change_vinsn_in_expr (expr, vinsn_create (insn, false));

  INSN_SEQNO (insn) = ssid->seqno;
}

/* This is used to initialize spurious jumps generated by
   sel_split_block () / sel_redirect_edge ().  */
static void
init_simplejump (insn_t insn)
{
  rtx succ = cfg_succ_1 (insn, SUCCS_ALL);

  gcc_assert (LV_SET (insn) == NULL);

  if (sel_bb_header_p (insn))
    {
      LV_SET (insn) = get_regset_from_pool ();
      COPY_REG_SET (LV_SET (insn), LV_SET (succ));
    }

  init_expr (INSN_EXPR (insn), vinsn_create (insn, false), 0, 0, 0, 0, 0);

  INSN_SEQNO (insn) = get_seqno_of_a_pred (insn);
}

/* This is used to move lv_sets to the first insn of basic block if that
   insn was emitted by the target.  */
static void
insn_init_move_lv_set_if_bb_header (insn_t insn)
{
  if (sel_bb_header_p (insn))
    {
      insn_t next = NEXT_INSN (insn);

      gcc_assert (INSN_LUID (insn) == 0);

      /* Find the insn that used to be a bb_header.  */
      while (INSN_LUID (next) == 0)
	{
	  gcc_assert (!sel_bb_end_p (next));
	  next = NEXT_INSN (next);
	}

      gcc_assert (LV_SET_VALID_P (next));

      LV_SET (insn) = LV_SET (next);
      LV_SET (next) = NULL;
    }
}

/* Perform deferred initialization of insns.  This is used to process 
   a new jump that may be created by redirect_edge.  */
void
sel_init_new_insns (void)
{
  int todo = insn_init.todo;

  if (todo & INSN_INIT_TODO_LUID)
    sched_init_luids (NULL, NULL, new_insns, NULL);

  if (todo & INSN_INIT_TODO_SSID)
    {
      const struct sched_scan_info_def ssi =
	{
	  NULL, /* extend_bb */
	  NULL, /* init_bb */
	  extend_insn, /* extend_insn */
	  init_insn /* init_insn */
	};

      sched_scan (&ssi, NULL, NULL, new_insns, NULL);

      clear_expr (&insn_init_ssid->_expr);
    }

  if (todo & INSN_INIT_TODO_SIMPLEJUMP)
    {
      const struct sched_scan_info_def ssi =
	{
	  NULL, /* extend_bb */
	  NULL, /* init_bb */
	  extend_insn, /* extend_insn */
	  init_simplejump /* init_insn */
	};

      sched_scan (&ssi, NULL, NULL, new_insns, NULL);
    }
  
  if (todo & INSN_INIT_TODO_MOVE_LV_SET_IF_BB_HEADER)
    {
      const struct sched_scan_info_def ssi =
	{
	  NULL, /* extend_bb */
	  NULL, /* init_bb */
	  sel_extend_insn_rtx_data, /* extend_insn */
	  insn_init_move_lv_set_if_bb_header /* init_insn */
	};

      sched_scan (&ssi, NULL, NULL, new_insns, NULL);
    }

  VEC_truncate (rtx, new_insns, 0);
}

/* Finalize new_insns data.  */
void
sel_finish_new_insns (void)
{
  gcc_assert (VEC_empty (rtx, new_insns));

  VEC_free (rtx, heap, new_insns);
}

/* Return the cost of VINSN as estimated by DFA.  This function properly
   handles ASMs, USEs etc.  */
int
vinsn_dfa_cost (vinsn_t vinsn, fence_t fence)
{
  rtx insn = VINSN_INSN (vinsn);

  if (recog_memoized (insn) < 0)
    {
      if (!FENCE_STARTS_CYCLE_P (fence) && VINSN_UNIQUE_P (vinsn)
	  && INSN_ASM_P (insn))
	/* This is asm insn which is tryed to be issued on the
	   cycle not first.  Issue it on the next cycle.  */
	return 1;
      else
	/* A USE insn, or something else we don't need to
	   understand.  We can't pass these directly to
	   state_transition because it will trigger a
	   fatal error for unrecognizable insns.  */
	return 0;
    }
  else
    {
      int cost;
      state_t temp_state = alloca (dfa_state_size);

      state_copy (temp_state, FENCE_STATE (fence));

      cost = state_transition (temp_state, insn);

      if (cost < 0)
	return 0;
      else if (cost == 0)
	return 1;

      return cost;
    }
}


/* Functions to init/finish work with lv sets.  */

/* Init LV_SET of INSN from a global_live_at_start set of BB.
   NOTE: We do need to detach register live info from bb because we
   use those regsets as LV_SETs.  */
static void
init_lv_set_for_insn (insn_t insn, basic_block bb)
{
  LV_SET (insn) = get_regset_from_pool ();
  COPY_REG_SET (LV_SET (insn), glat_start[bb->index]);
}

/* Initialize lv set of all bb headers.  */
void
init_lv_sets (void)
{
  basic_block bb;

  /* Initialization of the LV sets.  */
  FOR_EACH_BB (bb)
    {
      insn_t head;
      insn_t tail;

      get_ebb_head_tail (bb, bb, &head, &tail);

      if (/* BB has at least one insn.  */
	  INSN_P (head))
	init_lv_set_for_insn (head, bb);
    }

  /* Don't forget EXIT_INSN.  */
  init_lv_set_for_insn (exit_insn, EXIT_BLOCK_PTR);
}

/* Release lv set of HEAD.  */
static void
release_lv_set_for_insn (rtx head)
{
  int uid = INSN_UID (head);
  
  if (((unsigned) uid) < VEC_length (sel_insn_rtx_data_def, s_i_r_d))
    {
      regset lv = LV_SET (head);

      if (lv != NULL)
	{
	  return_regset_to_pool (lv);
	  LV_SET (head) = NULL;
	}
    }
}

/* Finalize lv sets of all bb headers.  */
void
free_lv_sets (void)
{
  basic_block bb;

  gcc_assert (LV_SET_VALID_P (exit_insn));
  release_lv_set_for_insn (exit_insn);

  /* !!! FIXME: Walk through bb_headers only.  */
  FOR_EACH_BB (bb)
    {
      insn_t head;
      insn_t next_tail;

      get_ebb_head_tail (bb, bb, &head, &next_tail);
      next_tail = NEXT_INSN (next_tail);

      /* We should scan through all the insns because bundling could
	 have emitted new insns at the bb headers.  */
      while (head != next_tail)
	{
          release_lv_set_for_insn (head);
	  head = NEXT_INSN (head);
	}
    }
}


/* Variables to work with control-flow graph.  */

/* The basic block that already has been processed by the sched_data_update (),
   but hasn't been in sel_add_or_remove_bb () yet.  */
static VEC (basic_block, heap) *last_added_blocks = NULL;

/* Functions to work with control-flow graph.  */

/* Return the first real insn of BB.  If STRICT_P is true, then assume
   that BB is current region and hence has no unrelevant notes in it.  */
static insn_t
sel_bb_header_1 (basic_block bb, bool strict_p)
{
  insn_t header;

  if (bb == EXIT_BLOCK_PTR)
    {
      gcc_assert (exit_insn != NULL_RTX);
      header = exit_insn;
    }
  else
    {
      if (strict_p)
	{
	  rtx note = bb_note (bb);

	  if (note != BB_END (bb))
	    header = NEXT_INSN (note);
	  else
	    header = NULL_RTX;
	}
      else
	{
	  rtx head, tail;

	  get_ebb_head_tail (bb, bb, &head, &tail);

	  if (INSN_P (head))
	    header = head;
	  else
	    header = NULL_RTX;
	}
    }

  return header;
}

/* Return the first real insn of BB.  */
insn_t
sel_bb_header (basic_block bb)
{
  insn_t header = sel_bb_header_1 (bb, true);

  gcc_assert (header == NULL_RTX || INSN_P (header));

  return header;
}

/* Return true if INSN is a basic block header.  */
bool
sel_bb_header_p (insn_t insn)
{
  gcc_assert (insn != NULL_RTX && INSN_P (insn));

  return insn == sel_bb_header (BLOCK_FOR_INSN (insn));
}

/* Return true if BB has no real insns.  If STRICT_P is true, then assume
   that BB is current region and hence has no unrelevant notes in it.  */
bool
sel_bb_empty_p_1 (basic_block bb, bool strict_p)
{
  return sel_bb_header_1 (bb, strict_p) == NULL_RTX;
}

/* Return true if BB has no real insns.  If STRICT_P is true, then assume
   that BB is current region and hence has no unrelevant notes in it.  */
bool
sel_bb_empty_p (basic_block bb)
{
  return sel_bb_empty_p_1 (bb, true);
}

/* Return last insn of BB.  */
insn_t
sel_bb_end (basic_block bb)
{
  gcc_assert (!sel_bb_empty_p (bb));

  return BB_END (bb);
}

/* Return true if INSN is the last insn in its basic block.  */
bool
sel_bb_end_p (insn_t insn)
{
  return insn == sel_bb_end (BLOCK_FOR_INSN (insn));
}

/* True when BB belongs to the current scheduling region.  */
bool
in_current_region_p (basic_block bb)
{
  if (bb->index < NUM_FIXED_BLOCKS)
    return false;

  return CONTAINING_RGN (bb->index) == CONTAINING_RGN (BB_TO_BLOCK (0));
}

/* Extend per bb data structures.  */
static void
extend_bb (void)
{
  VEC_safe_grow_cleared (sel_bb_info_def, heap, sel_bb_info, last_basic_block);
}

/* Remove all notes from BB.  */
static void
init_bb (basic_block bb)
{
  remove_notes (bb_note (bb), BB_END (bb));
  BB_NOTE_LIST (bb) = note_list;
}

void
sel_init_bbs (bb_vec_t bbs, basic_block bb)
{
  const struct sched_scan_info_def ssi =
    {
      extend_bb, /* extend_bb */
      init_bb, /* init_bb */
      NULL, /* extend_insn */
      NULL /* init_insn */
    };

  sched_scan (&ssi, bbs, bb, new_insns, NULL);
}

/* Restore other notes for the whole region.  */
static void
sel_restore_other_notes (void)
{
  int bb;

  for (bb = 0; bb < current_nr_blocks; bb++)
    {
      basic_block first, last;

      first = EBB_FIRST_BB (bb);
      last = EBB_LAST_BB (bb)->next_bb;

      do
	{
	  note_list = BB_NOTE_LIST (first);
	  restore_other_notes (NULL, first);
	  BB_NOTE_LIST (first) = NULL_RTX;

          first = first->next_bb;
	}
      while (first != last);
    }
}

static void sel_remove_loop_preheader (void);

/* Free per-bb data structures.  */
void
sel_finish_bbs (void)
{
  sel_restore_other_notes ();

  /* Remove current loop preheader from this loop.  */
  if (flag_sel_sched_pipelining_outer_loops && current_loop_nest)
    sel_remove_loop_preheader ();

  VEC_free (sel_bb_info_def, heap, sel_bb_info);
}

/* Return a number of INSN's successors honoring FLAGS.  */
int
cfg_succs_n (insn_t insn, int flags)
{
  int n = 0;
  succ_iterator si;
  insn_t succ;

  FOR_EACH_SUCC_1 (succ, si, insn, flags)
    n++;

  return n;
}

/* Return true if INSN has a single successor of type FLAGS.  */
bool
sel_insn_has_single_succ_p (insn_t insn, int flags)
{
  insn_t succ;
  succ_iterator si;
  bool first_p = true;

  FOR_EACH_SUCC_1 (succ, si, insn, flags)
    {
      if (first_p)
	first_p = false;
      else
	return false;
    }

  return true;
}

/* Return the successors of INSN in SUCCSP and their number in NP, 
   honoring FLAGS.  Empty blocks are skipped.  */
void
cfg_succs_1 (insn_t insn, int flags, insn_t **succsp, int *np)
{
  int n;
  succ_iterator si;
  insn_t succ;

  n = *np = cfg_succs_n (insn, flags);

  *succsp = xmalloc (n * sizeof (**succsp));

  FOR_EACH_SUCC_1 (succ, si, insn, flags)
    (*succsp)[--n] = succ;
}

/* Find all successors of INSN and record them in SUCCSP and their number 
   in NP.  Empty blocks are skipped, and only normal (forward in-region) 
   edges are processed.  */
void
cfg_succs (insn_t insn, insn_t **succsp, int *np)
{
  cfg_succs_1 (insn, SUCCS_NORMAL, succsp, np);
}

/* Return the only successor of INSN, honoring FLAGS.  */
insn_t
cfg_succ_1 (insn_t insn, int flags)
{
  insn_t succ;
  succ_iterator si;
  bool b = true;

  FOR_EACH_SUCC_1 (succ, si, insn, flags)
    {
      gcc_assert (b);
      b = false;
    }

  return succ;
}

/* Return the only successor of INSN.  Only normal edges are processed.  */
insn_t
cfg_succ (insn_t insn)
{
  return cfg_succ_1 (insn, SUCCS_NORMAL);
}

/* Return the predecessors of BB in PREDS and their number in N. 
   Empty blocks are skipped.  SIZE is used to allocate PREDS.  */
static void
cfg_preds_1 (basic_block bb, insn_t **preds, int *n, int *size)
{
  edge e;
  edge_iterator ei;

  gcc_assert (BLOCK_TO_BB (bb->index) != 0);

  FOR_EACH_EDGE (e, ei, bb->preds)
    {
      basic_block pred_bb = e->src;
      insn_t bb_end = BB_END (pred_bb);

      /* ??? This code is not supposed to walk out of a region.  */
      gcc_assert (in_current_region_p (pred_bb));

      if (sel_bb_empty_p (pred_bb))
	cfg_preds_1 (pred_bb, preds, n, size);
      else
	{
	  if (*n == *size)
	    *preds = xrealloc (*preds, ((*size = 2 * *size + 1)
					* sizeof (**preds)));

	  (*preds)[(*n)++] = bb_end;
	}
    }

  gcc_assert (*n != 0);
}

/* Find all predecessors of BB and record them in PREDS and their number 
   in N.  Empty blocks are skipped, and only normal (forward in-region) 
   edges are processed.  */
static void
cfg_preds (basic_block bb, insn_t **preds, int *n)
{
  int size = 0;

  *preds = NULL;
  *n = 0;
  cfg_preds_1 (bb, preds, n, &size);
}

/* Returns true if we are moving INSN through join point.
   !!! Rewrite me: this should use cfg_preds ().  */
bool
num_preds_gt_1 (insn_t insn)
{
  basic_block bb;

  if (!sel_bb_header_p (insn) || INSN_BB (insn) == 0)
    return false;

  bb = BLOCK_FOR_INSN (insn);

  while (1)
    {
      if (EDGE_COUNT (bb->preds) > 1)
	{
	  if (ENABLE_SEL_CHECKING)
	    {
	      edge e;
	      edge_iterator ei;

	      FOR_EACH_EDGE (e, ei, bb->preds)
		{
		  basic_block pred = e->src;

		  gcc_assert (in_current_region_p (pred));
		}
	    }

	  return true;
	}

      gcc_assert (EDGE_PRED (bb, 0)->dest == bb);
      bb = EDGE_PRED (bb, 0)->src;

      if (!sel_bb_empty_p (bb))
	break;
    }

  return false;
}

/* Returns true if INSN is not a downward continuation of the given path P in 
   the current stage.  */
bool
is_ineligible_successor (insn_t insn, ilist_t p)
{
  insn_t prev_insn;

  /* Check if insn is not deleted.  */
  if (PREV_INSN (insn) && NEXT_INSN (PREV_INSN (insn)) != insn)
    gcc_unreachable ();
  else if (NEXT_INSN (insn) && PREV_INSN (NEXT_INSN (insn)) != insn)
    gcc_unreachable ();

  /* If it's the first insn visited, then the successor is ok.  */
  if (!p)
    return false;

  prev_insn = ILIST_INSN (p);

  if (/* a backward edge.  */
      INSN_SEQNO (insn) < INSN_SEQNO (prev_insn)
      /* is already visited.  */
      || (INSN_SEQNO (insn) == INSN_SEQNO (prev_insn)
	  && (ilist_is_in_p (p, insn)
              /* We can reach another fence here and still seqno of insn 
                 would be equal to seqno of prev_insn.  This is possible 
                 when prev_insn is a previously created bookkeeping copy.
                 In that case it'd get a seqno of insn.  Thus, check here
                 whether insn is in current fence too.  */
              || IN_CURRENT_FENCE_P (insn)))
      /* Was already scheduled on this round.  */
      || (INSN_SEQNO (insn) > INSN_SEQNO (prev_insn)
	  && IN_CURRENT_FENCE_P (insn))
      /* An insn from another fence could also be 
	 scheduled earlier even if this insn is not in 
	 a fence list right now.  Check INSN_SCHED_CYCLE instead.  */
      || (!pipelining_p && INSN_SCHED_TIMES (insn) > 0))
    return true;
  else
    return false;
}

/* Returns true when BB should be the end of an ebb.  Adapted from the 
   code in sched-ebb.c.  */
bool
bb_ends_ebb_p (basic_block bb)
{
  basic_block next_bb = bb_next_bb (bb);
  edge e;
  edge_iterator ei;
  
  if (next_bb == EXIT_BLOCK_PTR
      || bitmap_bit_p (forced_ebb_heads, next_bb->index)
      || (LABEL_P (BB_HEAD (next_bb))
	  /* NB: LABEL_NUSES () is not maintained outside of jump.c .
	     Work around that.  */
	  && !single_pred_p (next_bb)))
    return true;

  if (!in_current_region_p (next_bb))
    return true;

  FOR_EACH_EDGE (e, ei, bb->succs)
    if ((e->flags & EDGE_FALLTHRU) != 0)
      {
	gcc_assert (e->dest == next_bb);

	return false;
      }

  return true;
}

/* Returns true when INSN and SUCC are in the same EBB, given that SUCC is a
   successor of INSN.  */
bool
in_same_ebb_p (insn_t insn, insn_t succ)
{
  basic_block ptr = BLOCK_FOR_INSN (insn);

  for(;;)
    {
      if (ptr == BLOCK_FOR_INSN (succ))
        return true;
    
      if (bb_ends_ebb_p (ptr))
        return false;

      ptr = bb_next_bb (ptr);
    }

  gcc_unreachable ();
  return false;
}

/* An implementation of create_basic_block hook, which additionally updates 
   per-bb data structures.  */
basic_block
sel_create_basic_block (void *headp, void *endp, basic_block after)
{
  basic_block new_bb;
  
  gcc_assert (flag_sel_sched_pipelining_outer_loops 
              || last_added_blocks == NULL);

  new_bb = old_create_basic_block (headp, endp, after);
  VEC_safe_push (basic_block, heap, last_added_blocks, new_bb);

  return new_bb;
}

/* Recomputes the reverse topological order for the function and
   saves it in REV_TOP_ORDER_INDEX.  REV_TOP_ORDER_INDEX_LEN is also
   modified appropriately.  */
static void
recompute_rev_top_order (void)
{
  int *postorder;
  int n_blocks, i;

  if (!rev_top_order_index || rev_top_order_index_len < last_basic_block)
    {
      rev_top_order_index_len = last_basic_block; 
      rev_top_order_index = XRESIZEVEC (int, rev_top_order_index,
                                        rev_top_order_index_len);
    }

  postorder = XNEWVEC (int, n_basic_blocks);

  n_blocks = post_order_compute (postorder, true);
  gcc_assert (n_basic_blocks == n_blocks);

  /* Build reverse function: for each basic block with BB->INDEX == K
     rev_top_order_index[K] is it's reverse topological sort number.  */
  for (i = 0; i < n_blocks; i++)
    {
      gcc_assert (postorder[i] < rev_top_order_index_len);
      rev_top_order_index[postorder[i]] = i;
    }

  free (postorder);
}

/* Clear all flags from insns in BB that could spoil its rescheduling.  */
void
clear_outdated_rtx_info (basic_block bb)
{
  rtx insn;

  FOR_BB_INSNS (bb, insn)
    if (INSN_P (insn) && SCHED_GROUP_P (insn))
      SCHED_GROUP_P (insn) = 0;
}

/* Returns a position in RGN where BB can be inserted retaining 
   topological order.  */
static int
find_place_to_insert_bb (basic_block bb, int rgn)
{
  int i, bbi = bb->index, cur_bbi;
  
  for (i = RGN_NR_BLOCKS (rgn) - 1; i >= 0; i--)
    {
      cur_bbi = BB_TO_BLOCK (i);
      if (rev_top_order_index[bbi] 
          < rev_top_order_index[cur_bbi])
        break;
    }
              
  /* We skipped the right block, so we increase i.  We accomodate
     it for increasing by step later, so we decrease i.  */
  return (i + 1) - 1;
}

/* Add (or remove depending on ADD) BB to (from) the current region 
   and update sched-rgn.c data.  */
static void
sel_add_or_remove_bb_1 (basic_block bb, int add)
{
  int i, pos, bbi = -2, rgn;
  int step = (add > 0) ? 1 : 0;

  rgn = CONTAINING_RGN (BB_TO_BLOCK (0));

  if (step)
    {
      bool has_preds_outside_rgn = false;
      edge e;
      edge_iterator ei;

      /* Find whether we have preds outside the region.  */
      FOR_EACH_EDGE (e, ei, bb->preds)
        if (!in_current_region_p (e->src))
          {
            has_preds_outside_rgn = true;
            break;
          }

      /* Recompute the top order -- needed when we have > 1 pred
         and in case we don't have preds outside.  */
      if (flag_sel_sched_pipelining_outer_loops
          && (has_preds_outside_rgn || EDGE_COUNT (bb->preds) > 1))
        {
          recompute_rev_top_order ();
          bbi = find_place_to_insert_bb (bb, rgn);
        }
      else if (has_preds_outside_rgn)
        {
          /* This is the case when we generate an extra empty block
             to serve as region head during pipelining.  */
          e = EDGE_SUCC (bb, 0);
          gcc_assert (EDGE_COUNT (bb->succs) == 1
                      && in_current_region_p (EDGE_SUCC (bb, 0)->dest)
                      && (BLOCK_TO_BB (e->dest->index) == 0));
                  
          bbi = -1;
        }
      else
        {
          if (EDGE_COUNT (bb->succs) > 0)
	    /* We don't have preds outside the region.  We should have
	       the only pred, because the multiple preds case comes from
	       the pipelining of outer loops, and that is handled above.
	       Just take the bbi of this single pred.  */
            {
              int pred_bbi;

              gcc_assert (EDGE_COUNT (bb->preds) == 1);

              pred_bbi = EDGE_PRED (bb, 0)->src->index;
              bbi = BLOCK_TO_BB (pred_bbi);
            }
          else
            /* BB has no successors.  It is safe to put it in the end.  */
            bbi = current_nr_blocks - 1;
        }
    }
  else
    bbi = BLOCK_TO_BB (bb->index);
  
  /* Assert that we've found a proper place.  */
  gcc_assert (bbi != -2);

  bbi += step;
  pos = RGN_BLOCKS (rgn) + bbi;

  gcc_assert (RGN_HAS_REAL_EBB (rgn) == 0
              && ebb_head[bbi] == pos);

  /* First of all, we free outdated info:
     Nothing to be done here.  */

  if (step)
    {
      /* Second, we make a place for the new block.  */
      extend_regions ();

      for (i = RGN_BLOCKS (rgn + 1) - 1; i >= pos; i--)
	/* We better not use EBB_HEAD here, as it has region-scope.  */
	BLOCK_TO_BB (rgn_bb_table[i])++;
    }
  else
    {
      for (i = RGN_BLOCKS (rgn + 1) - 1; i >= pos; i--)
	BLOCK_TO_BB (rgn_bb_table[i])--;
    }

  memmove (rgn_bb_table + pos + step,
           rgn_bb_table + pos + 1 - step,
           (RGN_BLOCKS (nr_regions) - pos) * sizeof (*rgn_bb_table));

  if (step)
    {
      /* Third, we initialize data for BB.  */
      rgn_bb_table[pos] = bb->index;
      BLOCK_TO_BB (bb->index) = bbi;
      CONTAINING_RGN (bb->index) = rgn;

      RGN_NR_BLOCKS (rgn)++;

      for (i = rgn + 1; i <= nr_regions; i++)
	RGN_BLOCKS (i)++;
    }
  else
    {
      RGN_NR_BLOCKS (rgn)--;
      for (i = rgn + 1; i <= nr_regions; i++)
	RGN_BLOCKS (i)--;
    }
}

/* Add (remove depending on ADD) BB to (from) the current region 
   and update all data.  If BB is NULL, add all blocks from 
   last_added_blocks vector.  */
void
sel_add_or_remove_bb (basic_block bb, int add)
{
  if (add > 0)
    {
      /* Extend luids so that new notes will recieve zero luids.  */
      sched_init_luids (NULL, NULL, NULL, NULL);
      sched_init_bbs (last_added_blocks, NULL);
      sel_init_bbs (last_added_blocks, NULL);

      /* When bb is passed explicitly, the vector should contain 
         the only element that equals to bb; otherwise, the vector
         should not be NULL.  */
      gcc_assert (last_added_blocks != NULL);

      if (bb != NULL)
        {
          gcc_assert (VEC_length (basic_block, last_added_blocks) == 1
                      && VEC_index (basic_block, 
                                    last_added_blocks, 0) == bb);

	  /* Free the vector.  */
          VEC_free (basic_block, heap, last_added_blocks);
        }
    }
  else
    {
      gcc_assert (bb != NULL && BB_NOTE_LIST (bb) == NULL_RTX);

      if (glat_start[bb->index] != NULL)
	FREE_REG_SET (glat_start[bb->index]);
      if (glat_end[bb->index] != NULL)
	FREE_REG_SET (glat_end[bb->index]);
    }

  if (bb != NULL)
    {
      sel_add_or_remove_bb_1 (bb, add);

      if (add < 0)
	delete_basic_block (bb);
    }
  else
    /* BB is NULL - process LAST_ADDED_BLOCKS instead.  */
    {
      int i;
      basic_block temp_bb = NULL;

      gcc_assert (add > 0);

      for (i = 0; 
           VEC_iterate (basic_block, last_added_blocks, i, bb); i++)
        {
          sel_add_or_remove_bb_1 (bb, add);
          temp_bb = bb;
        }

      /* We need to fetch at least one bb so we know the region 
         to update.  */
      gcc_assert (temp_bb != NULL);
      bb = temp_bb;

      VEC_free (basic_block, heap, last_added_blocks);
    }

  rgn_setup_region (CONTAINING_RGN (bb->index));
}

/* A wrapper for create_basic_block_before, which also extends per-bb 
   data structures.  Returns the newly created bb.  */
basic_block
sel_create_basic_block_before (basic_block before)
{
  basic_block prev_bb;
  basic_block bb;
  edge e;

  gcc_assert (in_current_region_p (before));

  prev_bb = before->prev_bb;

  e = find_fallthru_edge (prev_bb);
  gcc_assert (e != NULL);

  /* This code is taken from cfghooks.c: split_block ().  */
  bb = create_basic_block (BB_HEAD (before), NULL_RTX, prev_bb);
  bb->count = prev_bb->count;
  bb->frequency = prev_bb->frequency;
  bb->loop_depth = prev_bb->loop_depth;
  make_single_succ_edge (bb, before, EDGE_FALLTHRU);

  redirect_edge_succ (e, bb);

  sel_add_or_remove_bb (bb, 1);

  return bb;
}

/* Remove an empty basic block EMPTY_BB.  When MERGE_UP_P is true, we put 
   EMPTY_BB's note lists into its predecessor instead of putting them 
   into the successor.  */
void
sel_remove_empty_bb (basic_block empty_bb, bool merge_up_p,
		     bool remove_from_cfg_p)
{
  basic_block merge_bb;

  if (merge_up_p)
    {
      merge_bb = empty_bb->prev_bb;

      gcc_assert (EDGE_COUNT (empty_bb->preds) == 1
		  && EDGE_PRED (empty_bb, 0)->src == merge_bb);
    }
  else
    {
      merge_bb = bb_next_bb (empty_bb);

      gcc_assert (EDGE_COUNT (empty_bb->succs) == 1
		  && EDGE_SUCC (empty_bb, 0)->dest == merge_bb);
    }

  gcc_assert (in_current_region_p (merge_bb));

  concat_note_lists (BB_NOTE_LIST (empty_bb), 
		     &BB_NOTE_LIST (merge_bb));
  BB_NOTE_LIST (empty_bb) = NULL_RTX;

  /* Fixup CFG.  */

  gcc_assert (/* The BB contains just a bb note ...  */
	      BB_HEAD (empty_bb) == BB_END (empty_bb)
	      /* ... or an unused label.  */
	      || (LABEL_P (BB_HEAD (empty_bb))
		  /* This guarantees that the only pred edge is a fallthru
		     one.

		     NB: We can't use LABEL_NUSES because it is not maintained
		     outside jump.c .  We check that the only pred edge is
		     fallthru one below.  */
		  && true));

  /* If basic block has predecessors or successors, redirect them.  */
  if (remove_from_cfg_p
      && (EDGE_COUNT (empty_bb->preds) > 0
	  || EDGE_COUNT (empty_bb->succs) > 0))
    {
      basic_block pred;
      basic_block succ;

      /* We need to init PRED and SUCC before redirecting edges.  */
      if (EDGE_COUNT (empty_bb->preds) > 0)
	{
	  edge e;

	  gcc_assert (EDGE_COUNT (empty_bb->preds) == 1);

	  e = EDGE_PRED (empty_bb, 0);

	  gcc_assert (e->src == empty_bb->prev_bb
		      && (e->flags & EDGE_FALLTHRU));

	  pred = empty_bb->prev_bb;
	}
      else
	pred = NULL;

      if (EDGE_COUNT (empty_bb->succs) > 0)
	{
	  edge e;

	  gcc_assert (EDGE_COUNT (empty_bb->succs) == 1);

	  e = EDGE_SUCC (empty_bb, 0);

	  gcc_assert (e->flags & EDGE_FALLTHRU);

	  succ = e->dest;
	}
      else
	succ = NULL;

      if (EDGE_COUNT (empty_bb->preds) > 0 && succ != NULL)
	redirect_edge_succ_nodup (EDGE_PRED (empty_bb, 0), succ);

      if (EDGE_COUNT (empty_bb->succs) > 0 && pred != NULL)
	{
	  edge e = EDGE_SUCC (empty_bb, 0);

	  if (find_edge (pred, e->dest) == NULL)
	    redirect_edge_pred (e, pred);
	}
    }

  /* Finish removing.  */
  sel_add_or_remove_bb (empty_bb, remove_from_cfg_p ? -1 : 0);
}

/* Update the latch when we've splitted or merged it.
   This should be checked for all outer loops, too.  */
static void
change_loops_latches (basic_block from, basic_block to)
{
  gcc_assert (from != to);

  if (flag_sel_sched_pipelining_outer_loops 
      && current_loop_nest)
    {
      struct loop *loop;

      for (loop = current_loop_nest; loop; loop = loop->outer)
        if (considered_for_pipelining_p (loop) && loop->latch == from)
          {
            gcc_assert (loop == current_loop_nest);
            loop->latch = to;
            gcc_assert (loop_latch_edge (loop));
          }
    }
}

/* Splits BB on two basic blocks, adding it to the region and extending 
   per-bb data structures.  Returns the newly created bb.  */
basic_block
sel_split_block (basic_block bb, insn_t after)
{
  basic_block new_bb;

  can_add_real_insns_p = false;
  new_bb = split_block (bb, after)->dest;
  can_add_real_insns_p = true;

  change_loops_latches (bb, new_bb);

  sel_add_or_remove_bb (new_bb, 1);

  gcc_assert (after != NULL || sel_bb_empty_p (bb));

  return new_bb;
}

/* Splits E and adds the newly created basic block to the current region.
   Returns this basic block.  */
basic_block
sel_split_edge (edge e)
{
  basic_block new_bb;
  
  /* We don't need to split edges inside a region.  */
  gcc_assert (!in_current_region_p (e->src)
              && in_current_region_p (e->dest));

  insn_init.what = INSN_INIT_WHAT_INSN;

  new_bb = split_edge (e);

  if (flag_sel_sched_pipelining_outer_loops 
      && current_loop_nest)
    {
      int i;
      basic_block bb;

      /* Some of the basic blocks might not have been added to the loop.  
         Add them here, until this is fixed in force_fallthru.  */
      for (i = 0; 
           VEC_iterate (basic_block, last_added_blocks, i, bb); i++)
        if (!bb->loop_father)
          add_bb_to_loop (bb, e->dest->loop_father);
    }

  /* Add all last_added_blocks to the region.  */
  sel_add_or_remove_bb (NULL, true);

  /* Now the CFG has been updated, and we can init data for the newly 
     created insns.  */
  insn_init.todo = (INSN_INIT_TODO_LUID | INSN_INIT_TODO_SIMPLEJUMP);
  sel_init_new_insns ();

  return new_bb;
}

/* Merge basic block B into basic block A.  */
void
sel_merge_blocks (basic_block a, basic_block b)
{
  gcc_assert (can_merge_blocks_p (a, b));

  sel_remove_empty_bb (b, true, false);
  merge_blocks (a, b);

  change_loops_latches (b, a);
}

/* A wrapper for redirect_edge_and_branch_force, which also initializes
   data structures for possibly created bb and insns.  Returns the newly
   added bb or NULL, when a bb was not needed.  */
basic_block
sel_redirect_edge_force (edge e, basic_block to)
{
  basic_block jump_bb;

  gcc_assert (!sel_bb_empty_p (e->src));

  jump_bb = redirect_edge_and_branch_force (e, to);

  if (jump_bb)
    sel_add_or_remove_bb (jump_bb, 1);

  /* This function could not be used to spoil the loop structure by now,
     thus we don't care to update anything.  But check it to be sure.  */
  if (flag_sel_sched_pipelining_outer_loops && current_loop_nest)
    gcc_assert (loop_latch_edge (current_loop_nest));

  /* Now the CFG has been updated, and we can init data for the newly 
     created insns.  */
  insn_init.todo = (INSN_INIT_TODO_LUID | INSN_INIT_TODO_SIMPLEJUMP);
  sel_init_new_insns ();

  return jump_bb;
}

/* A wrapper for redirect_edge_and_branch.  */
edge
sel_redirect_edge_and_branch (edge e, basic_block to)
{
  edge ee;
  bool latch_edge_p;

  latch_edge_p = (flag_sel_sched_pipelining_outer_loops 
                  && current_loop_nest
                  && e == loop_latch_edge (current_loop_nest));

  ee = redirect_edge_and_branch (e, to);

  /* When we've redirected a latch edge, update the header.  */
  if (latch_edge_p)
    {
      current_loop_nest->header = to;
      gcc_assert (loop_latch_edge (current_loop_nest));
    }

  gcc_assert (ee == e && last_added_blocks == NULL);

  /* Now the CFG has been updated, and we can init data for the newly 
     created insns.  */
  insn_init.todo = (INSN_INIT_TODO_LUID | INSN_INIT_TODO_SIMPLEJUMP);
  sel_init_new_insns ();

  return ee;
}



/* Emit an insn rtx based on PATTERN.  */
static rtx
create_insn_rtx_from_pattern_1 (rtx pattern)
{
  rtx insn_rtx;

  gcc_assert (!INSN_P (pattern));

  start_sequence ();
  insn_init.what = INSN_INIT_WHAT_INSN_RTX;
  insn_rtx = emit_insn (pattern);
  end_sequence ();

  sched_init_luids (NULL, NULL, NULL, NULL);
  sel_extend_insn_rtx_data ();

  return insn_rtx;
}

/* Emit an insn rtx based on PATTERN and ICE if the result is not a valid
   insn.  */
rtx
create_insn_rtx_from_pattern (rtx pattern)
{
  rtx insn_rtx = create_insn_rtx_from_pattern_1 (pattern);

  if (!insn_rtx_valid (insn_rtx))
    gcc_unreachable ();

  return insn_rtx;
}

/* Create a new vinsn for INSN_RTX.  */
vinsn_t
create_vinsn_from_insn_rtx (rtx insn_rtx)
{
  gcc_assert (INSN_P (insn_rtx) && !INSN_IN_STREAM_P (insn_rtx));

  return vinsn_create (insn_rtx, false);
}

/* Create a copy of INSN_RTX.  */
rtx
create_copy_of_insn_rtx (rtx insn_rtx)
{
  bool orig_is_valid_p;
  rtx res;

  gcc_assert (INSN_P (insn_rtx));

  orig_is_valid_p = insn_rtx_valid (insn_rtx);

  res = create_insn_rtx_from_pattern_1 (copy_rtx (PATTERN (insn_rtx)));

  if (insn_rtx_valid (res))
    gcc_assert (orig_is_valid_p);
  else
    gcc_assert (!orig_is_valid_p);

  return res;
}

/* Change vinsn field of EXPR to hold NEW_VINSN.  */
void
change_vinsn_in_expr (expr_t expr, vinsn_t new_vinsn)
{
  vinsn_detach (EXPR_VINSN (expr));

  EXPR_VINSN (expr) = new_vinsn;
  vinsn_attach (new_vinsn);
}

/* Helpers for global init.  */
/* This structure is used to be able to call existing bundling mechanism
   and calculate insn priorities.  */
static struct haifa_sched_info sched_sel_haifa_sched_info = 
{
  NULL, /* init_ready_list */
  NULL, /* can_schedule_ready_p */
  NULL, /* schedule_more_p */
  NULL, /* new_ready */
  NULL, /* rgn_rank */
  sel_print_insn, /* rgn_print_insn */
  contributes_to_priority,

  NULL, NULL,
  NULL, NULL,
  0, 0,

  NULL, /* add_remove_insn */
  NULL, /* begin_schedule_ready */
  NULL, /* advance_target_bb */
};

/* Setup special insns used in the scheduler.  */
void 
setup_nop_and_exit_insns (void)
{
  if (nop_pattern == NULL_RTX)
    nop_pattern = gen_nop ();

  if (exit_insn == NULL_RTX)
    {
      start_sequence ();
      insn_init.what = INSN_INIT_WHAT_INSN_RTX;
      emit_insn (nop_pattern);
      exit_insn = get_insns ();
      end_sequence ();
    }

  set_block_for_insn (exit_insn, EXIT_BLOCK_PTR);
}

/* Free special insns used in the scheduler.  */
void
free_nop_and_exit_insns (void)
{
  exit_insn = NULL_RTX;
  nop_pattern = NULL_RTX;
}

/* Setup a special vinsn used in new insns initialization.  */
void
setup_empty_vinsn (void)
{
  empty_vinsn = vinsn_create (exit_insn, false);
  vinsn_attach (empty_vinsn);
}

/* Free a special vinsn used in new insns initialization.  */
void
free_empty_vinsn (void)
{
  gcc_assert (VINSN_COUNT (empty_vinsn) == 1);
  vinsn_detach (empty_vinsn);
  empty_vinsn = NULL;
}

/* Data structure to describe interaction with the generic scheduler utils.  */
static struct common_sched_info_def sel_common_sched_info;

/* Setup common_sched_info.  */
void
sel_setup_common_sched_info (void)
{
  rgn_setup_common_sched_info ();

  memcpy (&sel_common_sched_info, common_sched_info,
	  sizeof (sel_common_sched_info));

  sel_common_sched_info.fix_recovery_cfg = NULL;
  sel_common_sched_info.add_block = NULL;
  sel_common_sched_info.estimate_number_of_insns
    = sel_estimate_number_of_insns;
  sel_common_sched_info.luid_for_non_insn = sel_luid_for_non_insn;
  sel_common_sched_info.detach_life_info = 1;
  sel_common_sched_info.sched_pass_id = SCHED_SEL_PASS;

  common_sched_info = &sel_common_sched_info;
}

/* Setup pointers to global sched info structures.  */
void
sel_setup_sched_infos (void)
{
  current_sched_info = &sched_sel_haifa_sched_info;
}

/* Adds basic block BB to region RGN at the position *BB_ORD_INDEX,
   *BB_ORD_INDEX after that is increased.  */
static void
sel_add_block_to_region (basic_block bb, int *bb_ord_index, int rgn)
{
  RGN_NR_BLOCKS (rgn) += 1;
  RGN_DONT_CALC_DEPS (rgn) = 0;
  RGN_HAS_REAL_EBB (rgn) = 0;
  RGN_HAS_RENAMING_P (nr_regions) = 0;
  RGN_WAS_PIPELINED_P (nr_regions) = 0;
  RGN_NEEDS_GLOBAL_LIVE_UPDATE (nr_regions) = 0;
  CONTAINING_RGN (bb->index) = rgn;
  BLOCK_TO_BB (bb->index) = *bb_ord_index;
  rgn_bb_table[RGN_BLOCKS (rgn) + *bb_ord_index] = bb->index;
  (*bb_ord_index)++;

  /* FIXME: it is true only when not scheduling ebbs.  */
  RGN_BLOCKS (rgn + 1) = RGN_BLOCKS (rgn) + RGN_NR_BLOCKS (rgn);
}

/* Functions to support pipelining of outer loops.  */

/* Creates a new empty region and returns it's number.  */
static int
sel_create_new_region (void)
{
  int new_rgn_number = nr_regions;

  RGN_NR_BLOCKS (new_rgn_number) = 0;

  /* FIXME: This will work only when EBBs are not created.  */
  if (new_rgn_number != 0)
    RGN_BLOCKS (new_rgn_number) = RGN_BLOCKS (new_rgn_number - 1) + 
      RGN_NR_BLOCKS (new_rgn_number - 1);
  else
    RGN_BLOCKS (new_rgn_number) = 0;

  /* Set the blocks of the next region so the other functions may
     calculate the number of blocks in the region.  */
  RGN_BLOCKS (new_rgn_number + 1) = RGN_BLOCKS (new_rgn_number) + 
    RGN_NR_BLOCKS (new_rgn_number);

  nr_regions++;

  return new_rgn_number;
}

/* If BB1 has a smaller topological sort number than BB2, returns -1;
   if greater, returns 1.  */
static int
bb_top_order_comparator (const void *x, const void *y)
{
  basic_block bb1 = *(const basic_block *) x;
  basic_block bb2 = *(const basic_block *) y;

  gcc_assert (bb1 == bb2 
	      || rev_top_order_index[bb1->index] 
		 != rev_top_order_index[bb2->index]);

  /* It's a reverse topological order in REV_TOP_ORDER_INDEX, so
     bbs with greater number should go earlier.  */
  if (rev_top_order_index[bb1->index] > rev_top_order_index[bb2->index])
    return -1;
  else
    return 1;
}

/* Create a region for LOOP and return its number.  If we don't want 
   to pipeline LOOP, return -1.  */
static int
make_region_from_loop (struct loop *loop)
{
  unsigned int i;
  int num_insns;
  int new_rgn_number = -1;
  struct loop *inner;

  /* Basic block index, to be assigned to BLOCK_TO_BB.  */
  int bb_ord_index = 0;
  basic_block *loop_blocks;
  basic_block preheader_block;

  if (loop->num_nodes 
      > (unsigned) PARAM_VALUE (PARAM_MAX_PIPELINE_REGION_BLOCKS))
    return -1;
  
  /* Don't pipeline loops whose latch belongs to some of its inner loops.  */
  for (inner = loop->inner; inner; inner = inner->inner)
    if (flow_bb_inside_loop_p (inner, loop->latch))
      return -1;

  num_insns = 0;
  loop_blocks = get_loop_body_in_custom_order (loop, bb_top_order_comparator);

  for (i = 0; i < loop->num_nodes; i++)
    {
      num_insns += (common_sched_info->estimate_number_of_insns
                    (loop_blocks [i]));

      if ((loop_blocks[i]->flags & BB_IRREDUCIBLE_LOOP)
          || num_insns > PARAM_VALUE (PARAM_MAX_PIPELINE_REGION_INSNS))
        {
          free (loop_blocks);
          return -1;
        }
      
    }

  preheader_block = loop_preheader_edge (loop)->src;
  gcc_assert (preheader_block);
  gcc_assert (loop_blocks[0] == loop->header);

  new_rgn_number = sel_create_new_region ();

  sel_add_block_to_region (preheader_block, &bb_ord_index, new_rgn_number);
  SET_BIT (bbs_in_loop_rgns, preheader_block->index);

  for (i = 0; i < loop->num_nodes; i++)
    {
      /* Add only those blocks that haven't been scheduled in the inner loop.
	 The exception is the basic blocks with bookkeeping code - they should
	 be added to the region (and they actually don't belong to the loop 
	 body, but to the region containing that loop body).  */

      gcc_assert (new_rgn_number >= 0);

      if (! TEST_BIT (bbs_in_loop_rgns, loop_blocks[i]->index))
	{
	  sel_add_block_to_region (loop_blocks[i], &bb_ord_index, 
                                   new_rgn_number);
	  SET_BIT (bbs_in_loop_rgns, loop_blocks[i]->index);
	}
    }

  free (loop_blocks);
  MARK_LOOP_FOR_PIPELINING (loop);

  return new_rgn_number;
}

/* Create a new region from preheader blocks LOOP_BLOCKS.  */
void
make_region_from_loop_preheader (VEC(basic_block, heap) **loop_blocks)
{
  unsigned int i;
  int new_rgn_number = -1;
  basic_block bb;

  /* Basic block index, to be assigned to BLOCK_TO_BB.  */
  int bb_ord_index = 0;

  new_rgn_number = sel_create_new_region ();

  for (i = 0; VEC_iterate (basic_block, *loop_blocks, i, bb); i++)
    {
      gcc_assert (new_rgn_number >= 0);

      sel_add_block_to_region (bb, &bb_ord_index, new_rgn_number);
    }

  VEC_free (basic_block, heap, *loop_blocks);
  gcc_assert (*loop_blocks == NULL);
}


/* Create region(s) from loop nest LOOP, such that inner loops will be
   pipelined before outer loops.  Returns true when a region for LOOP 
   is created.  */
static bool
make_regions_from_loop_nest (struct loop *loop)
{   
  struct loop *cur_loop;
  int rgn_number;

  /* Traverse all inner nodes of the loop.  */
  for (cur_loop = loop->inner; cur_loop; cur_loop = cur_loop->next)
    if (! TEST_BIT (bbs_in_loop_rgns, cur_loop->header->index)
        && ! make_regions_from_loop_nest (cur_loop))
      return false;

  /* At this moment all regular inner loops should have been pipelined.
     Try to create a region from this loop.  */
  rgn_number = make_region_from_loop (loop);

  if (rgn_number < 0)
    return false;

  VEC_safe_push (loop_p, heap, loop_nests, loop);
  return true;
}

/* Initalize data structures needed.  */
void
pipeline_outer_loops_init (void)
{
  /* Collect loop information to be used in outer loops pipelining.  */
  loop_optimizer_init (LOOPS_HAVE_PREHEADERS
                       | LOOPS_HAVE_FALLTHRU_PREHEADERS
		       | LOOPS_HAVE_RECORDED_EXITS
		       | LOOPS_HAVE_MARKED_IRREDUCIBLE_REGIONS);
  current_loop_nest = NULL;

  bbs_in_loop_rgns = sbitmap_alloc (last_basic_block);
  sbitmap_zero (bbs_in_loop_rgns);

  recompute_rev_top_order ();
}

/* Returns a struct loop for region RGN.  */
loop_p
get_loop_nest_for_rgn (unsigned int rgn)
{
  /* Regions created with extend_rgns don't have corresponding loop nests,
     because they don't represent loops.  */
  if (rgn < VEC_length (loop_p, loop_nests))
    return VEC_index (loop_p, loop_nests, rgn);
  else
    return NULL;
}

/* True when LOOP was included into pipelining regions.   */
bool
considered_for_pipelining_p (struct loop *loop)
{
  if (loop->depth == 0)
    return false;

  /* Now, the loop could be too large or irreducible.  Check whether its 
     region is in LOOP_NESTS.  
     We determine the region number of LOOP as the region number of its 
     latch.  We can't use header here, because this header could be 
     just removed preheader and it will give us the wrong region number.
     Latch can't be used because it could be in the inner loop too.  */
  if (LOOP_MARKED_FOR_PIPELINING_P (loop))
    {
      int rgn = CONTAINING_RGN (loop->latch->index);

      gcc_assert ((unsigned) rgn < VEC_length (loop_p, loop_nests));
      return true;
    }
  
  return false;
}

/* Makes regions from the rest of the blocks, after loops are chosen 
   for pipelining.  */
static void
make_regions_from_the_rest (void)
{
  int cur_rgn_blocks;
  int *loop_hdr;
  int i;

  basic_block bb;
  edge e;
  edge_iterator ei;
  int *degree;
  int new_regions;

  /* Index in rgn_bb_table where to start allocating new regions.  */
  cur_rgn_blocks = nr_regions ? RGN_BLOCKS (nr_regions) : 0;
  new_regions = nr_regions;

  /* Make regions from all the rest basic blocks - those that don't belong to 
     any loop or belong to irreducible loops.  Prepare the data structures
     for extend_rgns.  */

  /* LOOP_HDR[I] == -1 if I-th bb doesn't belong to any loop,
     LOOP_HDR[I] == LOOP_HDR[J] iff basic blocks I and J reside within the same
     loop.  */
  loop_hdr = XNEWVEC (int, last_basic_block);
  degree = XCNEWVEC (int, last_basic_block);


  /* For each basic block that belongs to some loop assign the number
     of innermost loop it belongs to.  */
  for (i = 0; i < last_basic_block; i++)
    loop_hdr[i] = -1;

  FOR_EACH_BB (bb)
    {
      if (bb->loop_father && !bb->loop_father->num == 0
	  && !(bb->flags & BB_IRREDUCIBLE_LOOP))
	loop_hdr[bb->index] = bb->loop_father->num;
    }

  /* For each basic block degree is calculated as the number of incoming 
     edges, that are going out of bbs that are not yet scheduled.
     The basic blocks that are scheduled have degree value of zero.  */
  FOR_EACH_BB (bb) 
    {
      degree[bb->index] = 0;

      if (!TEST_BIT (bbs_in_loop_rgns, bb->index))
	{
	  FOR_EACH_EDGE (e, ei, bb->preds)
	    if (!TEST_BIT (bbs_in_loop_rgns, e->src->index))
	      degree[bb->index]++;
	}
      else
	degree[bb->index] = -1;
    }

  extend_rgns (degree, &cur_rgn_blocks, bbs_in_loop_rgns, loop_hdr);

  /* Any block that did not end up in a region is placed into a region
     by itself.  */
  FOR_EACH_BB (bb)
    if (degree[bb->index] >= 0)
      {
	rgn_bb_table[cur_rgn_blocks] = bb->index;
	RGN_NR_BLOCKS (nr_regions) = 1;
	RGN_BLOCKS (nr_regions) = cur_rgn_blocks++;
        RGN_DONT_CALC_DEPS (nr_regions) = 0;
	RGN_HAS_REAL_EBB (nr_regions) = 0;
	RGN_HAS_RENAMING_P (nr_regions) = 0;
	RGN_WAS_PIPELINED_P (nr_regions) = 0;
	RGN_NEEDS_GLOBAL_LIVE_UPDATE (nr_regions) = 0;
	CONTAINING_RGN (bb->index) = nr_regions++;
	BLOCK_TO_BB (bb->index) = 0;
      }

  free (degree);
  free (loop_hdr);
}

/* Free data structures used in pipelining of outer loops.  */
void pipeline_outer_loops_finish (void)
{
  loop_iterator li;
  struct loop *loop;

  /* Release aux fields so we don't free them later by mistake.  */
  FOR_EACH_LOOP (li, loop, 0)
    loop->aux = NULL;

  loop_optimizer_finalize ();
  free_dominance_info (CDI_DOMINATORS);

  VEC_free (loop_p, heap, loop_nests);

  free (rev_top_order_index);
  rev_top_order_index = NULL;
}

/* This function replaces the find_rgns when 
   FLAG_SEL_SCHED_PIPELINING_OUTER_LOOPS is set.  */
void 
sel_find_rgns (void)
{
  struct loop *loop;

  if (current_loops)
    /* Start traversing from the root node.  */
    for (loop = VEC_index (loop_p, current_loops->larray, 0)->inner; 
	 loop; loop = loop->next)
      make_regions_from_loop_nest (loop);

  /* Make regions from all the rest basic blocks and schedule them.
     These blocks include blocks that don't belong to any loop or belong  
     to irreducible loops.  */
  make_regions_from_the_rest ();

  /* We don't need bbs_in_loop_rgns anymore.  */
  sbitmap_free (bbs_in_loop_rgns);
  bbs_in_loop_rgns = NULL;
}

/* Adds the preheader blocks from previous loop to current region taking 
   it from LOOP_PREHEADER_BLOCKS (current_loop_nest).  
   This function is only used with -fsel-sched-pipelining-outer-loops.  */
void
sel_add_loop_preheader (void)
{
  int i;
  basic_block bb;
  int rgn = CONTAINING_RGN (BB_TO_BLOCK (0));
  VEC(basic_block, heap) *preheader_blocks 
    = LOOP_PREHEADER_BLOCKS (current_loop_nest);

  for (i = 0; VEC_iterate (basic_block, 
			   LOOP_PREHEADER_BLOCKS (current_loop_nest), i, bb); i++)
    {
      
      sel_add_or_remove_bb_1 (bb, true);
      
      /* Set variables for the current region.  */
      rgn_setup_region (rgn);
    }
  
  VEC_free (basic_block, heap, preheader_blocks);
  MARK_LOOP_FOR_PIPELINING (current_loop_nest);
}

/* While pipelining outer loops, returns TRUE if BB is a loop preheader.  */
bool
sel_is_loop_preheader_p (basic_block bb)
{
  /* A preheader may even have the loop depth equal to the depth of 
     the current loop, when it came from it.  Use topological sorting
     to get the right information.  */
  if (flag_sel_sched_pipelining_outer_loops 
      && current_loop_nest)
    {
      struct loop *outer;
      /* BB is placed before the header, so, it is a preheader block.  */
      if (BLOCK_TO_BB (bb->index) 
          < BLOCK_TO_BB (current_loop_nest->header->index))
        return true;

      /* Support the situation when the latch block of outer loop
         could be from here.  */
      for (outer = current_loop_nest->outer; outer; outer = outer->outer)
        if (considered_for_pipelining_p (outer) && outer->latch == bb)
          gcc_unreachable ();
    }
  return false;
}

/* Removes the loop preheader from the current region and saves it in
   PREHEADER_BLOCKS of the father loop, so they will be added later to 
   region that represents an outer loop.  
   This function is only used with -fsel-sched-pipelining-outer-loops.  */
static void
sel_remove_loop_preheader (void)
{
  int i, old_len;
  int cur_rgn = CONTAINING_RGN (BB_TO_BLOCK (0));
  basic_block bb;
  VEC(basic_block, heap) *preheader_blocks 
    = LOOP_PREHEADER_BLOCKS (current_loop_nest->outer);

  gcc_assert (flag_sel_sched_pipelining_outer_loops && current_loop_nest);
  old_len = VEC_length (basic_block, preheader_blocks);

  /* Add blocks that aren't within the current loop to PREHEADER_BLOCKS.  */
  for (i = 0; i < RGN_NR_BLOCKS (cur_rgn); i++)
    {
      bb = BASIC_BLOCK (BB_TO_BLOCK (i));

      /* If the basic block belongs to region, but doesn't belong to 
	 corresponding loop, then it should be a preheader.  */
      if (sel_is_loop_preheader_p (bb))
        VEC_safe_push (basic_block, heap, preheader_blocks, bb);
    }
  
  /* Remove these blocks only after iterating over the whole region.  */
  for (i = VEC_length (basic_block, preheader_blocks) - 1;
       i >= old_len;
       i--)
    {
       bb =  VEC_index (basic_block, preheader_blocks, i); 

       sel_add_or_remove_bb (bb, 0);
    }

  if (!considered_for_pipelining_p (current_loop_nest->outer))
    /* Immediately create new region from preheader.  */
    make_region_from_loop_preheader (&preheader_blocks);
  else
    /* Store preheader within the father's loop structure.  */
    SET_LOOP_PREHEADER_BLOCKS (current_loop_nest->outer, preheader_blocks);
}

#endif
