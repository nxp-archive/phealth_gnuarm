/* Dead code elimination pass for the GNU compiler.
   Copyright (C) 2002, 2004, 2004 Free Software Foundation, Inc.
   Contributed by Ben Elliston <bje@redhat.com>
   and Andrew MacLeod <amacleod@redhat.com>
 
This file is part of GCC.
   
GCC is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 2, or (at your option) any
later version.
   
GCC is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.
   
You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to the Free
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

/* Dead code elimination.

   References:

     Building an Optimizing Compiler,
     Robert Morgan, Butterworth-Heinemann, 1998, Section 8.9.

     Advanced Compiler Design and Implementation,
     Steven Muchnick, Morgan Kaufmann, 1997, Section 18.10.

   Dead-code elimination is the removal of instructions which have no
   impact on the program's output.  "Dead instructions" have no impact
   on the program's output, while "necessary instructions" may have
   impact on the output.

   The algorithm consists of three phases:
   1. Marking as necessary all instructions known to be necessary,
      e.g., function calls, writing a value to memory, etc;
   2. Propagating necessary instructions, e.g., the instructions
      giving values to operands in necessary instructions; and
   3. Removing dead instructions (except replacing dead conditionals
      with unconditional jumps).  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "errors.h"
#include "ggc.h"
#include "tree.h"

/* These RTL headers are needed for basic-block.h.  */
#include "rtl.h"
#include "tm_p.h"
#include "hard-reg-set.h"
#include "basic-block.h"

#include "diagnostic.h"
#include "tree-flow.h"
#include "tree-simple.h"
#include "tree-dump.h"
#include "tree-pass.h"
#include "timevar.h"
#include "flags.h"


static varray_type worklist;

static struct stmt_stats
{
  int total;
  int total_phis;
  int removed;
  int removed_phis;
} stats;

/* Forward function prototypes.  */
static inline bool necessary_p (tree);
static inline void clear_necessary (tree);
static inline void mark_necessary (tree, tree);
static void print_stats (void);
static bool need_to_preserve_store (tree);
static void find_useful_stmts (void);
static bool stmt_useful_p (tree);
static void process_worklist (void);
static void remove_dead_stmts (void);
static bool should_remove_dead_stmt (tree);
static void remove_dead_phis (basic_block);

#define NECESSARY(stmt)	   stmt->common.asm_written_flag

/* vector indicating an SSA name has already been processed and marked
   as necessary.  */
static sbitmap processed;

/* Is a tree necessary?  */

static inline bool
necessary_p (tree t)
{
  return NECESSARY (t);
}

static inline void
clear_necessary (tree t)
{
  NECESSARY (t) = 0;
}

/* Mark a tree as necessary.  */

static inline void
mark_necessary (tree def, tree stmt)
{
  int ver;
#ifdef ENABLE_CHECKING
  if ((def == NULL && stmt == NULL) || stmt == error_mark_node
      || (stmt && DECL_P (stmt)))
    abort ();
#endif

  if (def)
    {
      ver = SSA_NAME_VERSION (def);
      if (TEST_BIT (processed, ver))
	return;
      SET_BIT (processed, ver);
      if (!stmt)
	stmt = SSA_NAME_DEF_STMT (def);
    }

  if (necessary_p (stmt))
    return;

  if (tree_dump_file && (tree_dump_flags & TDF_DETAILS))
    {
      fprintf (tree_dump_file, "Marking useful stmt: ");
      print_generic_stmt (tree_dump_file, stmt, TDF_SLIM);
      fprintf (tree_dump_file, "\n");
    }

  NECESSARY (stmt) = 1;
  VARRAY_PUSH_TREE (worklist, stmt);
}


/* Print out removed statement statistics.  */

static void
print_stats (void)
{
  if (tree_dump_file && (tree_dump_flags & (TDF_STATS|TDF_DETAILS)))
    {
      float percg;

      percg = ((float) stats.removed / (float) stats.total) * 100;
      fprintf (tree_dump_file, "Removed %d of %d statements (%d%%)\n",
	       stats.removed, stats.total, (int) percg);

      if (stats.total_phis == 0)
	percg = 0;
      else
	percg = ((float) stats.removed_phis / (float) stats.total_phis) * 100;

      fprintf (tree_dump_file, "Removed %d of %d PHI nodes (%d%%)\n",
	       stats.removed_phis, stats.total_phis, (int) percg);
    }
}


/* Return true if a store to a variable needs to be preserved.  */

static bool
need_to_preserve_store (tree var)
{
  tree base_symbol;
  tree sym;

  if (var == NULL)
    return false;

  sym = SSA_NAME_VAR (var);
  base_symbol = get_base_symbol (var);

  /* Store to global variables must be preserved.  */
  if (decl_function_context (base_symbol) != current_function_decl)
    return true;

  /* Static locals must be preserved as well.  */
  if (TREE_STATIC (base_symbol))
    return true;

  /* If SYM may alias global memory, we also need to preserve the store.  */
  if (may_alias_global_mem_p (sym))
    return true;

  return false;
}


/* Find obviously useful instructions.  These are things like function
   calls and stores to file level variables.  */

static void
find_useful_stmts (void)
{
  basic_block bb;
  block_stmt_iterator i;

  FOR_EACH_BB (bb)
    {
      tree phi;

      /* Check any PHI nodes in the block.  */
      for (phi = phi_nodes (bb); phi; phi = TREE_CHAIN (phi))
	{
	  clear_necessary (phi);

	  /* PHIs for virtual variables do not directly affect code
	     generation and need not be considered inherently necessary
	     regardless of the bits set in their decl.

	     Thus, we only need to mark PHIs for real variables which
	     need their result preserved as being inherently necessary.  */
	  if (is_gimple_reg (PHI_RESULT (phi))
	      && need_to_preserve_store (PHI_RESULT (phi)))
	    mark_necessary (PHI_RESULT (phi), phi);
	}

      /* Check all statements in the block.  */
      for (i = bsi_start (bb); !bsi_end_p (i); bsi_next (&i))
	{
	  tree stmt = bsi_stmt (i);

	  clear_necessary (stmt);
	  if (stmt_useful_p (stmt))
	    mark_necessary (NULL_TREE, stmt);
	}
    }
}


/* Return true if STMT is necessary.  */

static bool
stmt_useful_p (tree stmt)
{
  def_optype defs;
  vdef_optype vdefs;
  stmt_ann_t ann;
  size_t i;

  /* Instructions that are implicitly live.  Function calls, asm and return
     statements are required.  Labels and BIND_EXPR nodes are kept because
     they are control flow, and we have no way of knowing whether they can
     be removed.   DCE can eliminate all the other statements in a block,
     and CFG can then remove the block and labels.  */
  switch (TREE_CODE (stmt))
    {
    case ASM_EXPR:
    case RETURN_EXPR:
    case CASE_LABEL_EXPR:
    case LABEL_EXPR:
    case BIND_EXPR:
    case RESX_EXPR:
      return true;
    case CALL_EXPR:
      return TREE_SIDE_EFFECTS (stmt);

    case MODIFY_EXPR:
      if (TREE_CODE (TREE_OPERAND (stmt, 1)) == CALL_EXPR
	  && TREE_SIDE_EFFECTS (TREE_OPERAND (stmt, 1)))
	return true;

      /* These values are mildly magic bits of the EH runtime.  We can't
	 see the entire lifetime of these values until landing pads are
	 generated.  */
      if (TREE_CODE (TREE_OPERAND (stmt, 0)) == EXC_PTR_EXPR)
	return true;
      if (TREE_CODE (TREE_OPERAND (stmt, 0)) == FILTER_EXPR)
	return true;
      break;

    case COND_EXPR:
      /* Check if the dest labels are the same. If they are, the condition
	 is useless.  */
      if (GOTO_DESTINATION (COND_EXPR_THEN (stmt))
	  == GOTO_DESTINATION (COND_EXPR_ELSE (stmt)))
	return false;

    default:
      break;
    }

  if (is_ctrl_stmt (stmt) || is_ctrl_altering_stmt (stmt))
    return true;

  /* If the statement has volatile operands, it needs to be preserved.  */
  ann = stmt_ann (stmt);
  if (ann->has_volatile_ops)
    return true;

  get_stmt_operands (stmt);

  defs = DEF_OPS (ann);
  for (i = 0; i < NUM_DEFS (defs); i++)
    if (need_to_preserve_store (DEF_OP (defs, i)))
      return true;

  vdefs = VDEF_OPS (ann);
  for (i = 0; i < NUM_VDEFS (vdefs); i++)
    if (need_to_preserve_store (VDEF_RESULT (vdefs, i)))
      return true;

  return false;
}


/* Process worklist.  Process the uses on each statement in the worklist,
   and add all feeding statements which contribute to the calculation of 
   this value to the worklist.  */

static void
process_worklist (void)
{
  tree i;

  while (VARRAY_ACTIVE_SIZE (worklist) > 0)
    {
      /* Take `i' from worklist.  */
      i = VARRAY_TOP_TREE (worklist);
      VARRAY_POP (worklist);

      if (tree_dump_file && (tree_dump_flags & TDF_DETAILS))
	{
	  fprintf (tree_dump_file, "processing: ");
	  print_generic_stmt (tree_dump_file, i, TDF_SLIM);
	  fprintf (tree_dump_file, "\n");
	}

      if (TREE_CODE (i) == PHI_NODE)
	{
	  int k;

	  /* All the statements feeding this PHI node's arguments are
	     necessary.  */
	  for (k = 0; k < PHI_NUM_ARGS (i); k++)
	    {
	      tree arg = PHI_ARG_DEF (i, k);
	      if (TREE_CODE (arg) == SSA_NAME)
		mark_necessary (arg, NULL);
	    }
	}
      else
	{
	  /* Examine all the USE, VUSE and VDEF operands in this statement.
	     Mark all the statements which feed this statement's uses as
	     necessary.  */
	  vuse_optype vuses;
	  vdef_optype vdefs;
	  use_optype uses;
	  stmt_ann_t ann;
	  size_t k;

	  get_stmt_operands (i);
	  ann = stmt_ann (i);

	  uses = USE_OPS (ann);
	  for (k = 0; k < NUM_USES (uses); k++)
	    {
	      tree use = USE_OP (uses, k);
	      mark_necessary (use, NULL_TREE);
	    }

	  vuses = VUSE_OPS (ann);
	  for (k = 0; k < NUM_VUSES (vuses); k++)
	    {
	      tree vuse = VUSE_OP (vuses, k);
	      mark_necessary (vuse, NULL_TREE);
	    }

	  /* The operands of VDEF expressions are also needed as they
	     represent potential definitions that may reach this
	     statement (VDEF operands allow us to follow def-def links).  */
	  vdefs = VDEF_OPS (ann);
	  for (k = 0; k < NUM_VDEFS (vdefs); k++)
	    mark_necessary (VDEF_OP (vdefs, k), NULL_TREE);
	}
    }
}


/* Eliminate unnecessary instructions. Any instruction not marked as necessary
   contributes nothing to the program, and can be deleted.  */

static void
remove_dead_stmts (void)
{
  basic_block bb;
  block_stmt_iterator i;

  clear_special_calls ();
  FOR_EACH_BB_REVERSE (bb)
    {
      /* Remove dead PHI nodes.  */
      remove_dead_phis (bb);

      /* Remove dead statements.  */
      for (i = bsi_last (bb); !bsi_end_p (i) ; )
	{
	  tree t = bsi_stmt (i);

	  stats.total++;

	  /* If `i' is not in `necessary' then remove from B.  */
	  if (!necessary_p (t) && should_remove_dead_stmt (t))
	    {
	      block_stmt_iterator j = i;
	      bsi_prev (&i);
	      bsi_remove (&j);
	    }
	  else
	    {
	      if (TREE_CODE (t) == CALL_EXPR)
		notice_special_calls (t);
	      else if (TREE_CODE (t) == MODIFY_EXPR
		       && TREE_CODE (TREE_OPERAND (t, 1)) == CALL_EXPR)
		notice_special_calls (TREE_OPERAND (t, 1));
	      bsi_prev (&i);
	    }
	}
    }
}


/* Remove dead PHI nodes from block BB.  */

static void
remove_dead_phis (basic_block bb)
{
  tree prev, phi;

  prev = NULL_TREE;
  phi = phi_nodes (bb);
  while (phi)
    {
      stats.total_phis++;

      if (!necessary_p (phi))
	{
	  tree next = TREE_CHAIN (phi);

	  if (tree_dump_file && (tree_dump_flags & TDF_DETAILS))
	    {
	      fprintf (tree_dump_file, "Deleting : ");
	      print_generic_stmt (tree_dump_file, phi, TDF_SLIM);
	      fprintf (tree_dump_file, "\n");
	    }

	  remove_phi_node (phi, prev, bb);
	  stats.removed_phis++;
	  phi = next;
	}
      else
	{
	  prev = phi;
	  phi = TREE_CHAIN (phi);
	}
    }
}


/* Remove dead statement pointed by iterator I.  */

static bool
should_remove_dead_stmt (tree t)
{
  if (tree_dump_file && (tree_dump_flags & TDF_DETAILS))
    {
      fprintf (tree_dump_file, "Deleting : ");
      print_generic_stmt (tree_dump_file, t, TDF_SLIM);
      fprintf (tree_dump_file, "\n");
    }

  stats.removed++;

  if (TREE_CODE (t) == COND_EXPR)
    {
      /* A dead COND_EXPR means the condition is dead. We dont change any
         flow, just replace the expression with a constant.  */
      COND_EXPR_COND (t) = integer_zero_node;
      modify_stmt (t);

      if (tree_dump_file && (tree_dump_flags & TDF_DETAILS))
	{
	  fprintf (tree_dump_file, "   by replacing the condition with 0:\n");
	  print_generic_stmt (tree_dump_file, t, TDF_SLIM);
	  fprintf (tree_dump_file, "\n");
	}

      return false;
    }

#ifdef ENABLE_CHECKING
  if (is_ctrl_stmt (t) || is_ctrl_altering_stmt (t))
    abort ();
#endif

  return true;
}

/* Cleanup the dead code, but avoid cfg changes.  */

void
tree_ssa_dce_no_cfg_changes (void)
{
  memset ((void *) &stats, 0, sizeof (stats));

  VARRAY_TREE_INIT (worklist, 64, "work list");

  processed = sbitmap_alloc (highest_ssa_version + 1);
  sbitmap_zero (processed);

  find_useful_stmts ();

  if (tree_dump_file && (tree_dump_flags & TDF_DETAILS))
    fprintf (tree_dump_file, "\nProcessing worklist:\n");

  process_worklist ();

  if (tree_dump_file && (tree_dump_flags & TDF_DETAILS))
    fprintf (tree_dump_file, "\nEliminating unnecessary instructions:\n");

  sbitmap_free (processed);

  remove_dead_stmts ();
}

/* Main routine to eliminate dead code.  */

static void
tree_ssa_dce (void)
{
  tree_ssa_dce_no_cfg_changes ();
  cleanup_tree_cfg ();

  /* Debugging dumps.  */
  if (tree_dump_file)
    {
      dump_function_to_file (current_function_decl,
			     tree_dump_file, tree_dump_flags);
      print_stats ();
    }
}

static bool
gate_dce (void)
{
  return flag_tree_dce != 0;
}

struct tree_opt_pass pass_dce = 
{
  "dce",				/* name */
  gate_dce,				/* gate */
  tree_ssa_dce,				/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_DCE,				/* tv_id */
  PROP_cfg | PROP_ssa,			/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_ggc_collect | TODO_verify_ssa	/* todo_flags_finish */
};
