/* Control flow functions for trees.
   Copyright (C) 2001, 2002, 2003 Free Software Foundation, Inc.
   Contributed by Diego Novillo <dnovillo@redhat.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "rtl.h"
#include "tm_p.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "output.h"
#include "errors.h"
#include "flags.h"
#include "function.h"
#include "expr.h"
#include "ggc.h"
#include "langhooks.h"
#include "diagnostic.h"
#include "tree-flow.h"
#include "timevar.h"
#include "tree-dump.h"
#include "toplev.h"
#include "except.h"
#include "cfgloop.h"

/* This file contains functions for building the Control Flow Graph (CFG)
   for a function tree.  */

/* Local declarations.  */

/* Initial capacity for the basic block array.  */
static const int initial_cfg_capacity = 20;

/* Dump files and flags.  */
static FILE *dump_file;		/* CFG dump file. */
static int dump_flags;		/* CFG dump flags.  */

/* Mapping of labels to their associated blocks.  This can greatly speed up
   building of the CFG in code with lots of gotos.  */
static varray_type label_to_block_map;

/* CFG statistics.  */
struct cfg_stats_d
{
  long num_merged_labels;
};

static dominance_info pdom_info = NULL;

static struct cfg_stats_d cfg_stats;

static struct obstack block_tree_ann_obstack;
static void *first_block_tree_ann_obj = 0;

/* Nonzero if we found a computed goto while building basic blocks.  */
static bool found_computed_goto;

/* If we found computed gotos, then they are all revectored to this
   location.  We try to unfactor them after we have translated out
   of SSA form.  */
static tree factored_computed_goto_label;

/* The factored computed goto.  We cache this so we can easily recover
   the destination of computed gotos when unfactoring them.  */
static tree factored_computed_goto;

/* The root of statement_lists of basic blocks for the garbage collector.
   This is a hack; we really should GC the entire CFG structure.  */
varray_type tree_bb_root;

/* Basic blocks and flowgraphs.  */
static basic_block create_bb (tree, basic_block);
static void create_blocks_annotations (void);
static void create_block_annotation (basic_block);
static void free_blocks_annotations (void);
static void clear_blocks_annotations (void);
static void make_blocks (tree);
static void factor_computed_gotos (void);
static tree tree_block_label (basic_block bb);

/* Edges.  */
static void make_edges (void);
static void make_ctrl_stmt_edges (basic_block);
static void make_exit_edges (basic_block);
static void make_cond_expr_edges (basic_block);
static void make_switch_expr_edges (basic_block);
static void make_goto_expr_edges (basic_block);
static edge tree_redirect_edge_and_branch_1 (edge, basic_block, bool);
static edge tree_redirect_edge_and_branch (edge, basic_block);

/* Various helpers.  */
static inline bool stmt_starts_bb_p (tree, tree);
static int tree_verify_flow_info (void);
static basic_block tree_make_forwarder_block (basic_block, int, int, edge, int);
static struct loops *tree_loop_optimizer_init (FILE *);
static void tree_loop_optimizer_finalize (struct loops *, FILE *);
static bool thread_jumps (void);
static bool tree_forwarder_block_p (basic_block);
static void bsi_commit_edge_inserts_1 (edge e);

/* Flowgraph optimization and cleanup.  */

static void remove_bb (basic_block);
static bool cleanup_control_flow (void);
static edge find_taken_edge_cond_expr (basic_block, tree);
static edge find_taken_edge_switch_expr (basic_block, tree);
static tree find_case_label_for_value (tree, tree);
static int phi_alternatives_equal (basic_block, edge, edge);

/* Location to track pending stmt for edge insertion.  */
#define PENDING_STMT(e)	((e)->insns.t)

/*---------------------------------------------------------------------------
			      Create basic blocks
---------------------------------------------------------------------------*/

/* Entry point to the CFG builder for trees.  FNBODY is the body of the
   function to process.  */

void
build_tree_cfg (tree *fnbody)
{
  timevar_push (TV_TREE_CFG);

  /* Register specific tree functions.  */
  tree_register_cfg_hooks ();

  /* Initialize the basic block array.  */
  n_basic_blocks = 0;
  last_basic_block = 0;
  VARRAY_BB_INIT (basic_block_info, initial_cfg_capacity, "basic_block_info");
  memset ((void *) &cfg_stats, 0, sizeof (cfg_stats));

  VARRAY_TREE_INIT (tree_bb_root, initial_cfg_capacity, "tree_bb_root");
  VARRAY_TREE_INIT (tree_phi_root, initial_cfg_capacity, "tree_phi_root");

  /* Build a mapping of labels to their associated blocks.  */
  VARRAY_BB_INIT (label_to_block_map, initial_cfg_capacity,
		  "label to block map");

  ENTRY_BLOCK_PTR->next_bb = EXIT_BLOCK_PTR;
  EXIT_BLOCK_PTR->prev_bb = ENTRY_BLOCK_PTR;

  found_computed_goto = 0;
  make_blocks (*fnbody);

  /* Computed gotos are hell to deal with, especially if there are
     lots of them with a large number of destinations.  So we factor
     them to a common computed goto location before we build the
     edge list.  After we convert back to normal form, we will un-factor
     the computed gotos since factoring introduces an unwanted jump.  */
  if (found_computed_goto)
    factor_computed_gotos ();

  if (n_basic_blocks > 0)
    {
      /* Adjust the size of the array.  */
      VARRAY_GROW (basic_block_info, n_basic_blocks);
      VARRAY_GROW (tree_bb_root, n_basic_blocks);
      VARRAY_GROW (tree_phi_root, n_basic_blocks);

      /* Create block annotations.  */
      create_blocks_annotations ();

      /* Create the edges of the flowgraph.  */
      make_edges ();
    }

  timevar_pop (TV_TREE_CFG);

  /* Debugging dumps.  */
  if (n_basic_blocks > 0)
    {
      /* Write the flowgraph to a dot file.  */
      dump_file = dump_begin (TDI_dot, &dump_flags);
      if (dump_file)
	{
	  tree_cfg2dot (dump_file);
	  dump_end (TDI_dot, dump_file);
	}

      /* Dump a textual representation of the flowgraph.  */
      dump_file = dump_begin (TDI_cfg, &dump_flags);
      if (dump_file)
	{
	  dump_tree_cfg (dump_file, dump_flags);
	  dump_end (TDI_cfg, dump_file);
	}
    }
}

/* Search the CFG for any computed gotos.  If found, factor them to a 
   common computed goto site.  Also record the location of that site so
   that we can un-factor the gotos after we have converted back to 
   normal form.  */

static void
factor_computed_gotos (void)
{
  basic_block bb;
  tree factored_label_decl = NULL;
  tree var = NULL;

  /* We know there are one or more computed gotos in this function.
     Examine the last statement in each basic block to see if the block
     ends with a computed goto.  */
	
  FOR_EACH_BB (bb)
    {
      block_stmt_iterator bsi = bsi_last (bb);
      tree last;

      if (bsi_end_p (bsi))
	continue;
      last = bsi_stmt (bsi);

      /* Ignore the computed goto we create when we factor the original
	 computed gotos.  */
      if (last == factored_computed_goto)
	continue;

      /* If the last statement is a compted goto, factor it.  */
      if (computed_goto_p (last))
	{
	  tree assignment;

	  /* The first time we find a computed goto we need to create
	     the factored goto block and the variable each original
	     computed goto will use for their goto destination.  */
	  if (! factored_computed_goto)
	    {
	      basic_block new_bb = create_bb (NULL, bb);
	      block_stmt_iterator new_bsi = bsi_start (new_bb);

	      /* Create the destination of the factored goto.  Each original
		 computed goto will put its desired destination into this
		 variable and jump to the label we create immediately
		 below.  */
	      var = create_tmp_var (ptr_type_node, "gotovar");

	      /* Build a label for the new block which will contain the
		 factored computed goto.  */
	      factored_label_decl = create_artificial_label ();
	      factored_computed_goto_label
		= build1 (LABEL_EXPR, void_type_node, factored_label_decl);
	      bsi_insert_after (&new_bsi, factored_computed_goto_label,
				BSI_NEW_STMT);

	      /* Build our new computed goto.  */
	      factored_computed_goto = build1 (GOTO_EXPR, void_type_node, var);
	      bsi_insert_after (&new_bsi, factored_computed_goto,
				BSI_NEW_STMT);
	    }

	  /* Copy the original computed goto's destination into VAR.  */
          assignment = build (MODIFY_EXPR, ptr_type_node,
			      var, GOTO_DESTINATION (last));
	  bsi_insert_before (&bsi, assignment, BSI_SAME_STMT);

	  /* And re-vector the computed goto to the new destination.  */
          GOTO_DESTINATION (last) = factored_label_decl;
	}
    }
}

/* Create annotations for all the basic blocks.  */

static void create_blocks_annotations (void)
{
  basic_block bb;
  static int initialized;

  if (!initialized)
    {
      gcc_obstack_init (&block_tree_ann_obstack);
      initialized = 1;
    }
  /* Check whether TREE_ANNOTATIONS data are still allocated.  */
  else if (first_block_tree_ann_obj)
    abort ();
  
  first_block_tree_ann_obj = obstack_alloc (&block_tree_ann_obstack, 0);
  
  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    create_block_annotation (bb);
}

/* Create annotations for a single basic block.  */

static void create_block_annotation (basic_block bb)
{
  /* Verify that the tree_annotations field is clear.  */
  if (bb->tree_annotations || !first_block_tree_ann_obj)
    abort ();
  bb->tree_annotations = obstack_alloc (&block_tree_ann_obstack, 
					sizeof (struct bb_ann_d));
  memset (bb->tree_annotations, 0, sizeof (struct bb_ann_d));
}

/* Free the annotations for all the basic blocks.  */

static void free_blocks_annotations (void)
{
  if (!first_block_tree_ann_obj)
    abort ();
  obstack_free (&block_tree_ann_obstack, first_block_tree_ann_obj);
  first_block_tree_ann_obj = NULL;

  clear_blocks_annotations ();  
}

/* Clear the annotations for all the basic blocks.  */

static void
clear_blocks_annotations (void)
{
  basic_block bb;

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, NULL, next_bb)
    bb->tree_annotations = NULL;
}

/* Build a flowgraph for the statement_list STMT_LIST.  */

static void
make_blocks (tree stmt_list)
{
  tree_stmt_iterator i = tsi_start (stmt_list);
  tree stmt = NULL;
  bool start_new_block = true;
  bool first_stmt_of_list = true;
  basic_block bb = ENTRY_BLOCK_PTR;

  while (!tsi_end_p (i))
    {
      tree prev_stmt;

      prev_stmt = stmt;
      stmt = tsi_stmt (i);

      /* If the statement starts a new basic block or if we have determined
	 in a previous pass that we need to create a new block for STMT, do
	 so now.  */
      if (start_new_block || stmt_starts_bb_p (stmt, prev_stmt))
	{
	  if (!first_stmt_of_list)
	    stmt_list = tsi_split_statement_list_before (&i);
	  bb = create_bb (stmt_list, bb);
	  start_new_block = false;
	}

      /* Now add STMT to BB and create the subgraphs for special statement
	 codes.  */
      set_bb_for_stmt (stmt, bb);

      if (computed_goto_p (stmt))
	found_computed_goto = true;

      /* If STMT is a basic block terminator, set START_NEW_BLOCK for the
	 next iteration.  */
      if (stmt_ends_bb_p (stmt))
	start_new_block = true;

      tsi_next (&i);
      first_stmt_of_list = false;
    }
}


/* Create and return a new basic block after bb AFTER.  Use STMT_LIST for
   the body if non-null, otherwise create a new statement list.  */

static basic_block
create_bb (tree stmt_list, basic_block after)
{
  basic_block bb;

  /* Create and initialize a new basic block.  */
  bb = alloc_block ();
  memset (bb, 0, sizeof (*bb));

  bb->index = last_basic_block;
  bb->flags = BB_NEW;
  bb->stmt_list = stmt_list ? stmt_list : alloc_stmt_list ();

  /* Add the new block to the linked list of blocks.  */
  link_block (bb, after);

  /* Grow the basic block array if needed.  */
  if ((size_t) n_basic_blocks == VARRAY_SIZE (basic_block_info))
    {
      size_t new_size = n_basic_blocks + (n_basic_blocks + 3) / 4;
      VARRAY_GROW (basic_block_info, new_size);
      VARRAY_GROW (tree_bb_root, new_size);
      VARRAY_GROW (tree_phi_root, new_size);
    }

  /* Add the newly created block to the array.  */
  BASIC_BLOCK (n_basic_blocks) = bb;
  VARRAY_TREE (tree_bb_root, bb->index) = bb->stmt_list;

  n_basic_blocks++;
  last_basic_block++;

  return bb;
}

/*---------------------------------------------------------------------------
				 Edge creation
---------------------------------------------------------------------------*/

/* Join all the blocks in the flowgraph.  */

static void
make_edges (void)
{
  basic_block bb;
  edge e;

  /* Create an edge from entry to the first block with executable
     statements in it.  */
  make_edge (ENTRY_BLOCK_PTR, BASIC_BLOCK (0), EDGE_FALLTHRU);

  /* Traverse basic block array placing edges.  */
  FOR_EACH_BB (bb)
    {
      tree first = first_stmt (bb);
      tree last = last_stmt (bb);

      if (first)
        {
	  /* Edges for statements that always alter flow control.  */
	  if (is_ctrl_stmt (last))
	    make_ctrl_stmt_edges (bb);

	  /* Edges for statements that sometimes alter flow control.  */
	  if (is_ctrl_altering_stmt (last))
	    make_exit_edges (bb);
	}

      /* Finally, if no edges were created above, this is a regular
	 basic block that only needs a fallthru edge.  */
      if (bb->succ == NULL)
	make_edge (bb, bb->next_bb, EDGE_FALLTHRU);
    }

  /* If there is a fallthru edge to exit out of the last block, transform it
     to a return statement.  */
  for (e = EXIT_BLOCK_PTR->prev_bb->succ; e; e = e->succ_next)
    if (e->flags & EDGE_FALLTHRU)
      break;
  if (e && e->dest == EXIT_BLOCK_PTR)
    {
      block_stmt_iterator bsi;
      tree x;

      /* ??? Can we have multiple outgoing edges here?  COND_EXPR
	 always has two gotos, and I can't think how one would have
	 achived this via EH.  */
      if (e != EXIT_BLOCK_PTR->prev_bb->succ || e->succ_next)
	abort ();
	
      x = build (RETURN_EXPR, void_type_node, NULL_TREE);
      bsi = bsi_last (EXIT_BLOCK_PTR->prev_bb);
      bsi_insert_after (&bsi, x, BSI_NEW_STMT);

      e->flags &= ~EDGE_FALLTHRU;
    }

  /* We do not care about fake edges, so remove any that the CFG
     builder inserted for completeness.  */
  remove_fake_edges ();

  /* Clean up the graph and warn for unreachable code.  */
  cleanup_tree_cfg ();
}

/* Create edges for control statement at basic block BB.  */

static void
make_ctrl_stmt_edges (basic_block bb)
{
  tree last = last_stmt (bb);
  tree first = first_stmt (bb);

#if defined ENABLE_CHECKING
  if (last == NULL_TREE)
    abort();
#endif

  if (TREE_CODE (first) == LABEL_EXPR
      && NONLOCAL_LABEL (LABEL_EXPR_LABEL (first)))
    make_edge (ENTRY_BLOCK_PTR, bb, EDGE_ABNORMAL);

  switch (TREE_CODE (last))
    {
    case GOTO_EXPR:
      make_goto_expr_edges (bb);
      break;

    case RETURN_EXPR:
      make_edge (bb, EXIT_BLOCK_PTR, 0);
      break;

    case COND_EXPR:
      make_cond_expr_edges (bb);
      break;

    case SWITCH_EXPR:
      make_switch_expr_edges (bb);
      break;

    case RESX_EXPR:
      make_eh_edges (last);
      /* Yet another NORETURN hack.  */
      if (bb->succ == NULL)
	make_edge (bb, EXIT_BLOCK_PTR, EDGE_FAKE);
      break;

    default:
      abort ();
    }
}

/* Create exit edges for statements in block BB that alter the flow of
   control.  Statements that alter the control flow are 'goto', 'return'
   and calls to non-returning functions.  */

static void
make_exit_edges (basic_block bb)
{
  tree last = last_stmt (bb);

  if (last == NULL_TREE)
    abort ();

  switch (TREE_CODE (last))
    {
    case CALL_EXPR:
      /* If this function receives a nonlocal goto, then we need to
	 make edges from this call site to all the nonlocal goto
	 handlers.  */
      if (TREE_SIDE_EFFECTS (last)
	  && FUNCTION_RECEIVES_NONLOCAL_GOTO (current_function_decl))
	make_goto_expr_edges (bb);

      /* If this statement has reachable exception handlers, then
	 create abnormal edges to them.  */
      make_eh_edges (last);

      /* Some calls are known not to return.  For such calls we create
	 a fake edge.

	 We really need to revamp how we build edges so that it's not
	 such a bloody pain to avoid creating edges for this case since
	 all we do is remove these edges when we're done building the
	 CFG.  */
      if (call_expr_flags (last) & (ECF_NORETURN | ECF_LONGJMP))
	{
	  make_edge (bb, EXIT_BLOCK_PTR, EDGE_FAKE);
	  return;
	}

      /* Don't forget the fall-thru edge.  */
      make_edge (bb, bb->next_bb, EDGE_FALLTHRU);
      break;

    case MODIFY_EXPR:
      /* A MODIFY_EXPR may have a CALL_EXPR on its RHS and the CALL_EXPR
	 may have an abnormal edge.  Search the RHS for this case and
	 create any required edges.  */
      if (TREE_CODE (TREE_OPERAND (last, 1)) == CALL_EXPR
	  && TREE_SIDE_EFFECTS (TREE_OPERAND (last, 1))
	  && FUNCTION_RECEIVES_NONLOCAL_GOTO (current_function_decl))
	make_goto_expr_edges (bb);

      make_eh_edges (last);
      make_edge (bb, bb->next_bb, EDGE_FALLTHRU);
      break;

    default:
      abort ();
    }
}

/* Create the edges for a COND_EXPR starting at block BB.
   At this point, both clauses must contain only simple gotos.  */

static void
make_cond_expr_edges (basic_block bb)
{
  tree entry = last_stmt (bb);
  basic_block then_bb, else_bb;
  tree then_label, else_label;

#if defined ENABLE_CHECKING
  if (entry == NULL_TREE || TREE_CODE (entry) != COND_EXPR)
    abort ();
#endif

  /* Entry basic blocks for each component.  */
  then_label = GOTO_DESTINATION (COND_EXPR_THEN (entry));
  else_label = GOTO_DESTINATION (COND_EXPR_ELSE (entry));
  then_bb = label_to_block (then_label);
  else_bb = label_to_block (else_label);

  make_edge (bb, then_bb, EDGE_TRUE_VALUE);
  make_edge (bb, else_bb, EDGE_FALSE_VALUE);
}

/* Create the edges for a SWITCH_EXPR starting at block BB.
   At this point, the switch body has been lowered and the
   SWITCH_LABELS filled in, so this is in effect a multi-way branch.  */

static void
make_switch_expr_edges (basic_block bb)
{
  tree entry = last_stmt (bb);
  size_t i, n;
  tree vec;

  vec = SWITCH_LABELS (entry);
  n = TREE_VEC_LENGTH (vec);

  for (i = 0; i < n; ++i)
    {
      tree lab = CASE_LABEL (TREE_VEC_ELT (vec, i));
      basic_block label_bb = label_to_block (lab);
      make_edge (bb, label_bb, 0);
    }
}

basic_block
label_to_block (tree dest)
{
  return VARRAY_BB (label_to_block_map, LABEL_DECL_UID (dest));
}

/* Create edges for a goto statement at block BB.  */

static void
make_goto_expr_edges (basic_block bb)
{
  tree goto_t, dest;
  basic_block target_bb;
  int for_call;
  block_stmt_iterator last = bsi_last (bb);

  goto_t = bsi_stmt (last);

  /* If the last statement is not a GOTO (i.e., it is a RETURN_EXPR,
     CALL_EXPR or MODIFY_EXPR), then the edge is an abnormal edge resulting
     from a nonlocal goto.  */
  if (TREE_CODE (goto_t) != GOTO_EXPR)
    {
      dest = error_mark_node;
      for_call = 1;
    }
  else
    {
      dest = GOTO_DESTINATION (goto_t);
      for_call = 0;

      /* A GOTO to a local label creates normal edges.  */
      if (simple_goto_p (goto_t))
	{
	  make_edge (bb, label_to_block (dest), EDGE_FALLTHRU);
	  bsi_remove (&last);
	  return;
	}

      /* If this is potentially a nonlocal goto, then this should
	 create an edge to the exit block.   */
      if (nonlocal_goto_p (goto_t))
	make_edge (bb, EXIT_BLOCK_PTR, EDGE_ABNORMAL);

      /* Nothing more to do for nonlocal gotos. */
      if (TREE_CODE (dest) == LABEL_DECL)
	return;

      /* Computed gotos remain.  */
    }

  /* Look for the block starting with the destination label.  In the
     case of a computed goto, make an edge to any label block we find
     in the CFG.  */
  FOR_EACH_BB (target_bb)
    {
      block_stmt_iterator bsi;

      for (bsi = bsi_start (target_bb); !bsi_end_p (bsi); bsi_next (&bsi))
	{
	  tree target = bsi_stmt (bsi);

	  if (TREE_CODE (target) != LABEL_EXPR)
	    break;

	  if (
	      /* Computed GOTOs.  Make an edge to every label block that has
		 been marked as a potential target for a computed goto.  */
	      (FORCED_LABEL (LABEL_EXPR_LABEL (target)) && for_call == 0)
	      /* Nonlocal GOTO target.  Make an edge to every label block
		 that has been marked as a potential target for a nonlocal
		 goto.  */
	      || (NONLOCAL_LABEL (LABEL_EXPR_LABEL (target)) && for_call == 1))
	    {
	      make_edge (bb, target_bb, EDGE_ABNORMAL);
	      break;
	    }
	}
    }
}


/*---------------------------------------------------------------------------
			       Flowgraph analysis
---------------------------------------------------------------------------*/

/* Remove unreachable blocks and other miscellaneous clean up work.  */

void
cleanup_tree_cfg (void)
{
  int orig_n_basic_blocks = n_basic_blocks;
  bool something_changed = true;

  timevar_push (TV_TREE_CLEANUP_CFG);
  pdom_info = NULL;

  /* These three transformations can cascade, so we iterate on them until nothing
     changes.  */
  while (something_changed)
    {
      something_changed = cleanup_control_flow ();
      something_changed |= thread_jumps ();
      something_changed |= remove_unreachable_blocks ();
    }

  if (pdom_info != NULL)
    {
      free_dominance_info (pdom_info);
      pdom_info = NULL;
    }
  compact_blocks ();

  /* If we expunged any basic blocks, then the dominator tree is
     no longer valid.  */
  if (n_basic_blocks != orig_n_basic_blocks)
    {
      basic_block bb;
      
      FOR_ALL_BB (bb)
	clear_dom_children (bb);
    }

#ifdef ENABLE_CHECKING
  verify_flow_info ();
#endif
  timevar_pop (TV_TREE_CLEANUP_CFG);
}

/* Walk the function tree removing unnecessary statements.

     * Empty statement nodes are removed

     * Unnecessary TRY_FINALLY and TRY_CATCH blocks are removed

     * Unnecessary COND_EXPRs are removed

     * Some unnecessary BIND_EXPRs are removed

   Clearly more work could be done.  The trick is doing the analysis
   and removal fast enough to be a net improvement in compile times.

   Note that when we remove a control structure such as a COND_EXPR
   BIND_EXPR, or TRY block, we will need to repeat this optimization pass
   to ensure we eliminate all the useless code.  */

struct rus_data
{
  tree *last_goto;
  bool repeat;
  bool may_throw;
  bool may_branch;
  bool has_label;
};

static void remove_useless_stmts_1 (tree *, struct rus_data *);

static bool
remove_useless_stmts_warn_notreached (tree stmt)
{
  if (EXPR_LOCUS (stmt))
    {
      warning ("%Hwill never be executed", EXPR_LOCUS (stmt));
      return true;
    }

  switch (TREE_CODE (stmt))
    {
    case STATEMENT_LIST:
      {
	tree_stmt_iterator i;
	for (i = tsi_start (stmt); !tsi_end_p (i); tsi_next (&i))
	  if (remove_useless_stmts_warn_notreached (tsi_stmt (i)))
	    return true;
      }
      break;

    case COND_EXPR:
      if (remove_useless_stmts_warn_notreached (COND_EXPR_COND (stmt)))
	return true;
      if (remove_useless_stmts_warn_notreached (COND_EXPR_THEN (stmt)))
	return true;
      if (remove_useless_stmts_warn_notreached (COND_EXPR_ELSE (stmt)))
	return true;
      break;

    case TRY_FINALLY_EXPR:
    case TRY_CATCH_EXPR:
      if (remove_useless_stmts_warn_notreached (TREE_OPERAND (stmt, 0)))
	return true;
      if (remove_useless_stmts_warn_notreached (TREE_OPERAND (stmt, 1)))
	return true;
      break;

    case CATCH_EXPR:
      return remove_useless_stmts_warn_notreached (CATCH_BODY (stmt));
    case EH_FILTER_EXPR:
      return remove_useless_stmts_warn_notreached (EH_FILTER_FAILURE (stmt));
    case BIND_EXPR:
      return remove_useless_stmts_warn_notreached (BIND_EXPR_BLOCK (stmt));

    default:
      /* Not a live container.  */
      break;
    }

  return false;
}

static void
remove_useless_stmts_cond (tree *stmt_p, struct rus_data *data)
{
  tree then_clause, else_clause, cond;
  bool save_has_label, then_has_label, else_has_label;

  save_has_label = data->has_label;
  data->has_label = false;
  data->last_goto = NULL;

  remove_useless_stmts_1 (&COND_EXPR_THEN (*stmt_p), data);

  then_has_label = data->has_label;
  data->has_label = false;
  data->last_goto = NULL;

  remove_useless_stmts_1 (&COND_EXPR_ELSE (*stmt_p), data);

  else_has_label = data->has_label;
  data->has_label = save_has_label | then_has_label | else_has_label;

  then_clause = COND_EXPR_THEN (*stmt_p);
  else_clause = COND_EXPR_ELSE (*stmt_p);
  cond = COND_EXPR_COND (*stmt_p);

  /* If neither arm does anything at all, we can remove the whole IF.  */
  if (!TREE_SIDE_EFFECTS (then_clause) && !TREE_SIDE_EFFECTS (else_clause))
    {
      *stmt_p = build_empty_stmt ();
      data->repeat = true;
    }

  /* If there are no reachable statements in an arm, then we can
     zap the entire conditional.  */
  else if (integer_nonzerop (cond) && !else_has_label)
    {
      if (warn_notreached)
	remove_useless_stmts_warn_notreached (else_clause);
      *stmt_p = then_clause;
      data->repeat = true;
    }
  else if (integer_zerop (cond) && !then_has_label)
    {
      if (warn_notreached)
	remove_useless_stmts_warn_notreached (then_clause);
      *stmt_p = else_clause;
      data->repeat = true;
    }

  /* Check a couple of simple things on then/else with single stmts.  */
  else
    {
      tree then_stmt = expr_only (then_clause);
      tree else_stmt = expr_only (else_clause);

      /* Notice branches to a common destination.  */
      if (then_stmt && else_stmt
	  && TREE_CODE (then_stmt) == GOTO_EXPR
	  && TREE_CODE (else_stmt) == GOTO_EXPR
	  && (GOTO_DESTINATION (then_stmt) == GOTO_DESTINATION (else_stmt)))
        {
	  *stmt_p = then_stmt;
	  data->repeat = true;
	}

      /* If the THEN/ELSE clause merely assigns a value to a variable or
	 parameter which is already known to contain that value, then
	 remove the useless THEN/ELSE clause.  */
      else if (TREE_CODE (cond) == VAR_DECL || TREE_CODE (cond) == PARM_DECL)
	{
	  if (else_stmt
	      && TREE_CODE (else_stmt) == MODIFY_EXPR
	      && TREE_OPERAND (else_stmt, 0) == cond
	      && integer_zerop (TREE_OPERAND (else_stmt, 1)))
	    COND_EXPR_ELSE (*stmt_p) = alloc_stmt_list ();
        }
      else if ((TREE_CODE (cond) == EQ_EXPR || TREE_CODE (cond) == NE_EXPR)
	       && (TREE_CODE (TREE_OPERAND (cond, 0)) == VAR_DECL
		   || TREE_CODE (TREE_OPERAND (cond, 0)) == PARM_DECL)
	       && TREE_CONSTANT (TREE_OPERAND (cond, 1)))
	{
	  tree stmt = (TREE_CODE (cond) == EQ_EXPR
		       ? then_stmt : else_stmt);
          tree *location = (TREE_CODE (cond) == EQ_EXPR
			    ? &COND_EXPR_THEN (*stmt_p)
			    : &COND_EXPR_ELSE (*stmt_p));

	  if (stmt
	      && TREE_CODE (stmt) == MODIFY_EXPR
	      && TREE_OPERAND (stmt, 0) == TREE_OPERAND (cond, 0)
	      && TREE_OPERAND (stmt, 1) == TREE_OPERAND (cond, 1))
	    *location = alloc_stmt_list ();
	}
    }
}

static void
remove_useless_stmts_tf (tree *stmt_p, struct rus_data *data)
{
  bool save_may_branch, save_may_throw;
  bool this_may_branch, this_may_throw;

  /* Collect may_branch and may_throw information for the body only.  */
  save_may_branch = data->may_branch;
  save_may_throw = data->may_throw;
  data->may_branch = false;
  data->may_throw = false;
  data->last_goto = NULL;

  remove_useless_stmts_1 (&TREE_OPERAND (*stmt_p, 0), data);

  this_may_branch = data->may_branch;
  this_may_throw = data->may_throw;
  data->may_branch |= save_may_branch;
  data->may_throw |= save_may_throw;
  data->last_goto = NULL;

  remove_useless_stmts_1 (&TREE_OPERAND (*stmt_p, 1), data);

  /* If the body is empty, then we can emit the FINALLY block without
     the enclosing TRY_FINALLY_EXPR.  */
  if (!TREE_SIDE_EFFECTS (TREE_OPERAND (*stmt_p, 0)))
    {
      *stmt_p = TREE_OPERAND (*stmt_p, 1);
      data->repeat = true;
    }

  /* If the handler is empty, then we can emit the TRY block without
     the enclosing TRY_FINALLY_EXPR.  */
  else if (!TREE_SIDE_EFFECTS (TREE_OPERAND (*stmt_p, 1)))
    {
      *stmt_p = TREE_OPERAND (*stmt_p, 0);
      data->repeat = true;
    }

  /* If the body neither throws, nor branches, then we can safely
     string the TRY and FINALLY blocks together.  */
  else if (!this_may_branch && !this_may_throw)
    {
      tree stmt = *stmt_p;
      *stmt_p = TREE_OPERAND (stmt, 0);
      append_to_statement_list (TREE_OPERAND (stmt, 1), stmt_p);
      data->repeat = true;
    }
}

static void
remove_useless_stmts_tc (tree *stmt_p, struct rus_data *data)
{
  bool save_may_throw, this_may_throw;
  tree_stmt_iterator i;
  tree stmt;

  /* Collect may_throw information for the body only.  */
  save_may_throw = data->may_throw;
  data->may_throw = false;
  data->last_goto = NULL;

  remove_useless_stmts_1 (&TREE_OPERAND (*stmt_p, 0), data);

  this_may_throw = data->may_throw;
  data->may_throw = save_may_throw;

  /* If the body cannot throw, then we can drop the entire TRY_CATCH_EXPR.  */
  if (!this_may_throw)
    {
      if (warn_notreached)
	remove_useless_stmts_warn_notreached (TREE_OPERAND (*stmt_p, 1));
      *stmt_p = TREE_OPERAND (*stmt_p, 0);
      data->repeat = true;
      return;
    }

  /* Process the catch clause specially.  We may be able to tell that
     no exceptions propagate past this point.  */

  this_may_throw = true;
  i = tsi_start (TREE_OPERAND (*stmt_p, 1));
  stmt = tsi_stmt (i);
  data->last_goto = NULL;

  switch (TREE_CODE (stmt))
    {
    case CATCH_EXPR:
      for (; !tsi_end_p (i); tsi_next (&i))
	{
	  stmt = tsi_stmt (i);
	  /* If we catch all exceptions, then the body does not
	     propagate exceptions past this point.  */
	  if (CATCH_TYPES (stmt) == NULL)
	    this_may_throw = false;
	  data->last_goto = NULL;
	  remove_useless_stmts_1 (&CATCH_BODY (stmt), data);
	}
      break;

    case EH_FILTER_EXPR:
      if (EH_FILTER_MUST_NOT_THROW (stmt))
	this_may_throw = false;
      else if (EH_FILTER_TYPES (stmt) == NULL)
	this_may_throw = false;
      remove_useless_stmts_1 (&EH_FILTER_FAILURE (stmt), data);
      break;

    default:
      /* Otherwise this is a cleanup.  */
      remove_useless_stmts_1 (&TREE_OPERAND (*stmt_p, 1), data);

      /* If the cleanup is empty, then we can emit the TRY block without
	 the enclosing TRY_CATCH_EXPR.  */
      if (!TREE_SIDE_EFFECTS (TREE_OPERAND (*stmt_p, 1)))
	{
	  *stmt_p = TREE_OPERAND (*stmt_p, 0);
	  data->repeat = true;
	}
      break;
    }
  data->may_throw |= this_may_throw;
}

static void
remove_useless_stmts_bind (tree *stmt_p, struct rus_data *data)
{
  tree block;

  /* First remove anything underneath the BIND_EXPR.  */
  remove_useless_stmts_1 (&BIND_EXPR_BODY (*stmt_p), data);

  /* If the BIND_EXPR has no variables, then we can pull everything
     up one level and remove the BIND_EXPR, unless this is the toplevel
     BIND_EXPR for the current function or an inlined function.

     When this situation occurs we will want to apply this
     optimization again.  */
  block = BIND_EXPR_BLOCK (*stmt_p);
  if (BIND_EXPR_VARS (*stmt_p) == NULL_TREE
      && *stmt_p != DECL_SAVED_TREE (current_function_decl)
      && (! block
	  || ! BLOCK_ABSTRACT_ORIGIN (block)
	  || (TREE_CODE (BLOCK_ABSTRACT_ORIGIN (block))
	      != FUNCTION_DECL)))
    {
      *stmt_p = BIND_EXPR_BODY (*stmt_p);
      data->repeat = true;
    }
}

static void
remove_useless_stmts_goto (tree *stmt_p, struct rus_data *data)
{
  tree dest = GOTO_DESTINATION (*stmt_p);

  data->may_branch = true;
  data->last_goto = NULL;

  /* Record the last goto expr, so that we can delete it if unnecessary.  */
  if (TREE_CODE (dest) == LABEL_DECL)
    data->last_goto = stmt_p;
}

static void
remove_useless_stmts_label (tree *stmt_p, struct rus_data *data)
{
  data->has_label = true;

  if (data->last_goto
      && GOTO_DESTINATION (*data->last_goto) == LABEL_EXPR_LABEL (*stmt_p))
    {
      *data->last_goto = build_empty_stmt ();
      data->repeat = true;
    }

  /* ??? Add something here to delete unused labels.  */
}

/* If the function is "const" or "pure", then clear TREE_SIDE_EFFECTS on its
   decl.  This allows us to eliminate redundant or useless
   calls to "const" functions. 

   Gimplifier already does the same operation, but we may notice functions
   being const and pure once their calls has been gimplified, so we need
   to update the flag.  */
static void
update_call_expr_flags (tree call)
{
  tree decl = get_callee_fndecl (call);
  if (!decl)
    return;
  if (call_expr_flags (call) & (ECF_CONST | ECF_PURE))
    TREE_SIDE_EFFECTS (call) = 0;
  if (TREE_NOTHROW (decl))
    TREE_NOTHROW (call) = 1;
}

/* t is CALL_EXPR.  Set current_function_calls_* flags.  */
void
notice_special_calls (tree t)
{
  int flags = call_expr_flags (t);

  if (flags & ECF_MAY_BE_ALLOCA)
    current_function_calls_alloca = true;
  if (flags & ECF_RETURNS_TWICE)
    current_function_calls_setjmp = true;
}

/* Clear flags set by notice_special_calls.  Used by dead code removal
   to update the flags.  */
void
clear_special_calls (void)
{
  current_function_calls_alloca = false;
  current_function_calls_setjmp = false;
}

static void
remove_useless_stmts_1 (tree *tp, struct rus_data *data)
{
  tree t = *tp;
  switch (TREE_CODE (t))
    {
    case COND_EXPR:
      remove_useless_stmts_cond (tp, data);
      break;

    case TRY_FINALLY_EXPR:
      remove_useless_stmts_tf (tp, data);
      break;

    case TRY_CATCH_EXPR:
      remove_useless_stmts_tc (tp, data);
      break;

    case BIND_EXPR:
      remove_useless_stmts_bind (tp, data);
      break;

    case GOTO_EXPR:
      remove_useless_stmts_goto (tp, data);
      break;

    case LABEL_EXPR:
      remove_useless_stmts_label (tp, data);
      break;

    case RETURN_EXPR:
      data->last_goto = NULL;
      data->may_branch = true;
      break;

    case CALL_EXPR:
      data->last_goto = NULL;
      notice_special_calls (t);
      update_call_expr_flags (t);
      if (tree_could_throw_p (t))
        data->may_throw = true;
      break;

    case MODIFY_EXPR:
      data->last_goto = NULL;
      if (TREE_CODE (TREE_OPERAND (t, 1)) == CALL_EXPR)
	{
	  update_call_expr_flags (TREE_OPERAND (t, 1));
	  notice_special_calls (TREE_OPERAND (t, 1));
	}
      if (tree_could_throw_p (t))
	data->may_throw = true;
      break;

    case STATEMENT_LIST:
      {
        tree_stmt_iterator i = tsi_start (t);
	while (!tsi_end_p (i))
	  {
	    t = tsi_stmt (i);
	    if (IS_EMPTY_STMT (t))
	      {
		tsi_delink (&i);
		continue;
	      }
	    
	    remove_useless_stmts_1 (tsi_stmt_ptr (i), data);

	    t = tsi_stmt (i);
	    if (TREE_CODE (t) == STATEMENT_LIST)
	      {
		tsi_link_before (&i, t, TSI_SAME_STMT);
		tsi_delink (&i);
	      }
	    else
	      tsi_next (&i);
	  }
      }
      break;

    default:
      data->last_goto = NULL;
      break;
    }
}

void
remove_useless_stmts (tree *first_p)
{
  struct rus_data data;

  clear_special_calls ();

  do
    {
      memset (&data, 0, sizeof (data));
      remove_useless_stmts_1 (first_p, &data);
    }
  while (data.repeat);
}

/* Remove obviously useless statements in basic block BB.  */

static void
cfg_remove_useless_stmts_bb (basic_block bb)
{
  block_stmt_iterator bsi;
  tree stmt = NULL_TREE;
  tree cond, var = NULL_TREE, val = NULL_TREE;
  struct var_ann_d *ann;

  /* Check whether we come here from a condition, and if so, get the
     condition.  */
  if (!bb->pred
      || bb->pred->pred_next
      || !(bb->pred->flags & (EDGE_TRUE_VALUE | EDGE_FALSE_VALUE)))
    return;

  cond = COND_EXPR_COND (last_stmt (bb->pred->src));
  if (bb->pred->flags & EDGE_FALSE_VALUE)
    cond = invert_truthvalue (cond);

  if (TREE_CODE (cond) == VAR_DECL
      || TREE_CODE (cond) == PARM_DECL)
    {
      var = cond;
      val = convert (TREE_TYPE (cond), integer_zero_node);
    }
  else if ((TREE_CODE (cond) == EQ_EXPR)
	   && (TREE_CODE (TREE_OPERAND (cond, 0)) == VAR_DECL
	       || TREE_CODE (TREE_OPERAND (cond, 0)) == PARM_DECL)
	   && (TREE_CODE (TREE_OPERAND (cond, 1)) == VAR_DECL
	       || TREE_CODE (TREE_OPERAND (cond, 1)) == PARM_DECL
	       || TREE_CONSTANT (TREE_OPERAND (cond, 1))))
    {
      var = TREE_OPERAND (cond, 0);
      val = TREE_OPERAND (cond, 1);
    }
  else
    return;

  /* Only work for normal local variables.  */
  ann = var_ann (var);
  if (!ann
      || ann->may_aliases
      || TREE_ADDRESSABLE (var))
    return;

  if (! TREE_CONSTANT (val))
    {
      ann = var_ann (val);
      if (!ann
	  || ann->may_aliases
	  || TREE_ADDRESSABLE (val))
	return;
    }

  /* Ignore floating point variables, since comparison behaves weird for
     them.  */
  if (FLOAT_TYPE_P (TREE_TYPE (var)))
    return;

  for (bsi = bsi_start (bb); !bsi_end_p (bsi);)
    {
      stmt = bsi_stmt (bsi);

      /* If the THEN/ELSE clause merely assigns a value to a variable/parameter
	 which is already known to contain that value, then remove the useless
	 THEN/ELSE clause.  */
      if (TREE_CODE (stmt) == MODIFY_EXPR
	  && TREE_OPERAND (stmt, 0) == var
	  && operand_equal_p (val, TREE_OPERAND (stmt, 1), 0))
	{
	  bsi_remove (&bsi);
	  continue;
	}

      /* Invalidate the var if we encounter something that could modify it.  */
      if (TREE_CODE (stmt) == ASM_EXPR
	  || TREE_CODE (stmt) == VA_ARG_EXPR
	  || (TREE_CODE (stmt) == MODIFY_EXPR
	      && (TREE_OPERAND (stmt, 0) == var
		  || TREE_OPERAND (stmt, 0) == val
		  || TREE_CODE (TREE_OPERAND (stmt, 1)) == VA_ARG_EXPR)))
	return;
  
      bsi_next (&bsi);
    }
}

/* A cfg-aware version of remove_useless_stmts_and_vars.  */

void
cfg_remove_useless_stmts (void)
{
  basic_block bb;

#ifdef ENABLE_CHECKING
  verify_flow_info ();
#endif

  FOR_EACH_BB (bb)
    {
      cfg_remove_useless_stmts_bb (bb);
    }
}

/* Delete all unreachable basic blocks.  Return true if any unreachable
   blocks were detected and removed.  */

bool
remove_unreachable_blocks (void)
{
  int i;
  bool ret = false;
  basic_block bb;

  find_unreachable_blocks ();

  for (i = last_basic_block - 1; i >= 0; i--)
    {
      bb = BASIC_BLOCK (i);

      if (bb == NULL)
	continue;

      if (!(bb->flags & BB_REACHABLE))
	{
	  remove_bb (bb);
	  ret = true;
	}
    }

  return ret;
}

/* Remove PHI nodes associated with basic block BB and all edges into
   and out of BB.  */
void
remove_phi_nodes_and_edges_for_unreachable_block (basic_block bb)
{
  /* Remove the edges into and out of this block.  */
  while (bb->pred != NULL)
    {
      tree phi;

      /* Since this block is no longer reachable, we can just delete all
         of its PHI nodes.  */
      phi = phi_nodes (bb);
      while (phi)
        {
	  tree next = TREE_CHAIN (phi);
	  remove_phi_node (phi, NULL_TREE, bb);
	  phi = next;
        }

      remove_edge (bb->pred);
    }

  /* Remove edges to BB's successors.  */
  while (bb->succ != NULL)
    ssa_remove_edge (bb->succ);
}

/* Remove block BB and its statements from the flowgraph.  */

static void
remove_bb (basic_block bb)
{
  block_stmt_iterator i;
  location_t *loc = NULL;

  dump_file = dump_begin (TDI_cfg, &dump_flags);
  if (dump_file)
    {
      fprintf (dump_file, "Removing basic block %d\n", bb->index);
      if (dump_flags & TDF_DETAILS)
	{
	  dump_bb (bb, dump_file, 0);
	  fprintf (dump_file, "\n");
	}
      dump_end (TDI_cfg, dump_file);
      dump_file = NULL;
    }

  /* Remove all the instructions in the block.  */
  for (i = bsi_start (bb); !bsi_end_p (i); bsi_remove (&i))
    {
      tree stmt = bsi_stmt (i);

      set_bb_for_stmt (stmt, NULL);

      /* Don't warn for removed gotos.  Gotos are often removed due to
	 jump threading, thus resulting into bogus warnings.  Not great,
	 since this way we lose warnings for gotos in the original program
	 that are indeed unreachable.  */
      if (TREE_CODE (stmt) != GOTO_EXPR && EXPR_LOCUS (stmt) && !loc)
	loc = EXPR_LOCUS (stmt);
    }

  /* If requested, give a warning that the first statement in the
     block is unreachable.  We walk statements backwards in the
     loop above, so the last statement we process is the first statement
     in the block.  */
  if (warn_notreached && loc)
    warning ("%Hwill never be executed", loc);

  remove_phi_nodes_and_edges_for_unreachable_block (bb);

  /* If we have pdom information, then we must also make sure to
     clean up the dominance information.  */
  if (pdom_info)
    delete_from_dominance_info (pdom_info, bb);

  VARRAY_TREE (tree_bb_root, bb->index) = NULL_TREE;
  VARRAY_TREE (tree_phi_root, bb->index) = NULL_TREE;

  /* Remove the basic block from the array.  */
  expunge_block (bb);
}

/* Examine BB to determine if it is a forwarding block (a block which only
   transfers control to a new destination).  If BB is a forwarding block,
   then return the ultimate destination.  */

basic_block
tree_block_forwards_to (basic_block bb)
{
  block_stmt_iterator bsi;
  bb_ann_t ann = bb_ann (bb);
  tree stmt;

  /* If this block is not forwardable, then avoid useless work.  */
  if (! ann->forwardable)
    return NULL;

  /* Set this block to not be forwardable.  This prevents infinite loops since
     any block currently under examination is considered non-forwardable.  */
  ann->forwardable = 0;

  /* No forwarding is possible if this block is a special block (ENTRY/EXIT),
     this block has more than one successor, this block's single successor is
     reached via an abnormal edge, this block has phi nodes, or this block's
     single successor has phi nodes.  */
  if (bb == EXIT_BLOCK_PTR
      || bb == ENTRY_BLOCK_PTR
      || !bb->succ
      || bb->succ->succ_next
      || bb->succ->dest == EXIT_BLOCK_PTR
      || (bb->succ->flags & EDGE_ABNORMAL) != 0
      || phi_nodes (bb)
      || phi_nodes (bb->succ->dest))
    return NULL;

  /* Walk past any labels at the start of this block.  */
  for (bsi = bsi_start (bb); !bsi_end_p (bsi); bsi_next (&bsi))
    {
      stmt = bsi_stmt (bsi);
      if (TREE_CODE (stmt) != LABEL_EXPR)
	break;
    }

  /* If we reached the end of this block we may be able to optimize this
     case.  */
  if (bsi_end_p (bsi))
    {
      basic_block dest;

      /* Recursive call to pick up chains of forwarding blocks.  */
      dest = tree_block_forwards_to (bb->succ->dest);

      /* If none found, we forward to bb->succ->dest at minimum.  */
      if (!dest)
	dest = bb->succ->dest;

      ann->forwardable = 1;
      return dest;
    }

  /* No forwarding possible.  */
  return NULL;
}

/* Try to remove superfluous control structures.  */

static bool
cleanup_control_flow (void)
{
  basic_block bb;
  block_stmt_iterator bsi;
  bool retval = false;
  tree stmt;

  FOR_EACH_BB (bb)
    {
      bsi = bsi_last (bb);

      if (bsi_end_p (bsi))
	continue;
      
      stmt = bsi_stmt (bsi);
      if (TREE_CODE (stmt) == COND_EXPR
	  || TREE_CODE (stmt) == SWITCH_EXPR)
	retval |= cleanup_control_expr_graph (bb, bsi);
    }
  return retval;
}

/* Disconnect an unreachable block in the control expression starting
   at block BB.  */

bool
cleanup_control_expr_graph (basic_block bb, block_stmt_iterator bsi)
{
  edge taken_edge;
  bool retval = false;
  tree expr = bsi_stmt (bsi), val;

  if (bb->succ->succ_next)
    {
      edge e, next;

      switch (TREE_CODE (expr))
	{
	case COND_EXPR:
	  val = COND_EXPR_COND (expr);
	  break;

	case SWITCH_EXPR:
	  val = SWITCH_COND (expr);
	  if (TREE_CODE (val) != INTEGER_CST)
	    return false;
	  break;

	default:
	  abort ();
	}

      taken_edge = find_taken_edge (bb, val);
      if (!taken_edge)
	return false;

      /* Remove all the edges except the one that is always executed.  */
      for (e = bb->succ; e; e = next)
	{
	  next = e->succ_next;
	  if (e != taken_edge)
	    {
	      ssa_remove_edge (e);
	      retval = true;
	    }
	}
    }
  else
    taken_edge = bb->succ;

  bsi_remove (&bsi);
  taken_edge->flags = EDGE_FALLTHRU;

  return retval;
}

/* Given a control block BB and a constant value VAL, return the edge that
   will be taken out of the block.  If VAL does not match a unique edge,
   NULL is returned.  */

edge
find_taken_edge (basic_block bb, tree val)
{
  tree stmt;

  stmt = last_stmt (bb);

#if defined ENABLE_CHECKING
  if (stmt == NULL_TREE || !is_ctrl_stmt (stmt))
    abort ();
#endif

  /* If VAL is not a constant, we can't determine which edge might
     be taken.  */
  if (val == NULL || !really_constant_p (val))
    return NULL;

  if (TREE_CODE (stmt) == COND_EXPR)
    return find_taken_edge_cond_expr (bb, val);

  if (TREE_CODE (stmt) == SWITCH_EXPR)
    return find_taken_edge_switch_expr (bb, val);

  return bb->succ;
}

/* Given a constant value VAL and the entry block BB to a COND_EXPR
   statement, determine which of the two edges will be taken out of the
   block.  Return NULL if either edge may be taken.  */

static edge
find_taken_edge_cond_expr (basic_block bb, tree val)
{
  bool always_false;
  bool always_true;
  edge e;

  /* Determine which branch of the if() will be taken.  */
  always_false = integer_zerop (val);
  always_true = integer_nonzerop (val);

  /* If VAL is a constant but it can't be reduced to a 0 or a 1, then
     we don't really know which edge will be taken at runtime.  This
     may happen when comparing addresses (e.g., if (&var1 == 4))  */
  if (!always_false && !always_true)
    return NULL;

  for (e = bb->succ; e; e = e->succ_next)
    if (((e->flags & EDGE_TRUE_VALUE) && always_true)
	|| ((e->flags & EDGE_FALSE_VALUE) && always_false))
      return e;

  /* There always should be an edge that is taken.  */
  abort ();
}

/* Given a constant value VAL and the entry block BB to a SWITCH_EXPR
   statement, determine which edge will be taken out of the block.  Return
   NULL if any edge may be taken.  */

static edge
find_taken_edge_switch_expr (basic_block bb, tree val)
{
  tree switch_expr, taken_case;
  basic_block dest_bb;
  edge e;

  if (TREE_CODE (val) != INTEGER_CST)
    return NULL;

  switch_expr = last_stmt (bb);
  taken_case = find_case_label_for_value (switch_expr, val);
  dest_bb = label_to_block (CASE_LABEL (taken_case));

  e = find_edge (bb, dest_bb);
  if (!e)
    abort ();
  return e;
}

/* Return the CASE_LABEL_EXPR that SWITCH_EXPR will take for VAL.  */

static tree
find_case_label_for_value (tree switch_expr, tree val)
{
  tree vec = SWITCH_LABELS (switch_expr);
  size_t i, n = TREE_VEC_LENGTH (vec);
  tree default_case = NULL;

  for (i = 0; i < n; ++i)
    {
      tree t = TREE_VEC_ELT (vec, i);

      if (CASE_LOW (t) == NULL)
	default_case = t;
      else if (CASE_HIGH (t) == NULL)
	{
	  /* A `normal' case label.  */
	  if (simple_cst_equal (CASE_LOW (t), val) == 1)
	    return t;
	}
      else
	{
	  /* A case range.  We can only handle integer ranges.  */
	  if (tree_int_cst_compare (CASE_LOW (t), val) <= 0
	      && tree_int_cst_compare (CASE_HIGH (t), val) >= 0)
	    return t;
	}
    }

  if (!default_case)
    abort ();
  return default_case;
}

/* If all the phi nodes in DEST have alternatives for E1 and E2 and
   those alterantives are equal in each of the PHI nodes, then return
   nonzero, else return zero.  */

static int
phi_alternatives_equal (basic_block dest, edge e1, edge e2)
{
  tree phi, val1, val2;
  int n1, n2;

  for (phi = phi_nodes (dest); phi; phi = TREE_CHAIN (phi))
    {
      n1 = phi_arg_from_edge (phi, e1);
      n2 = phi_arg_from_edge (phi, e2);

#ifdef ENABLE_CHECKING
      if (n1 < 0 || n2 < 0)
	abort ();
#endif

      val1 = PHI_ARG_DEF (phi, n1);
      val2 = PHI_ARG_DEF (phi, n2);

      if (!operand_equal_p (val1, val2, false))
	return false;
    }

  return true;
}

/* Computing the Dominance Frontier:

   As described in Morgan, section 3.5, this may be done simply by
   walking the dominator tree bottom-up, computing the frontier for
   the children before the parent.  When considering a block B,
   there are two cases:

   (1) A flow graph edge leaving B that does not lead to a child
   of B in the dominator tree must be a block that is either equal
   to B or not dominated by B.  Such blocks belong in the frontier
   of B.

   (2) Consider a block X in the frontier of one of the children C
   of B.  If X is not equal to B and is not dominated by B, it
   is in the frontier of B.
*/

static void
compute_dominance_frontiers_1 (bitmap *frontiers, dominance_info idom,
			       int bb, sbitmap done)
{
  basic_block b = BASIC_BLOCK (bb);
  edge e;
  basic_block c;
  unsigned int i;
  bitmap dominated;

  /* Ugh.  This could be called via the tree SSA code or via the
     RTL SSA code.  The former has bb annotations, the latter does
     not.  */
  if (bb_ann (b))
    dominated = dom_children (b);
  else
    {
      basic_block *dominated_array;
      unsigned int n_dominated;

      /* Build a sparse bitmap.  This can be expensive as 
         get_domianted_by allocates an array large enough to
	 hold every basic block.  We should probably either
	 make the RTL SSA code use bb annotations or rip it
	 out.  */
      dominated = BITMAP_XMALLOC ();
      n_dominated = get_dominated_by (idom, b, &dominated_array);
  
      for (i = 0; i < n_dominated; i++)
	bitmap_set_bit (dominated, dominated_array[i]->index);

      free (dominated_array);
    }

  SET_BIT (done, bb);

  /* Do the frontier of the children first.  Not all children in the
     dominator tree (blocks dominated by this one) are children in the
     CFG, so check all blocks.  */
  if (dominated)
    EXECUTE_IF_SET_IN_BITMAP (dominated, 0, i,
      {
        c = BASIC_BLOCK (i);
        if (! TEST_BIT (done, c->index))
          compute_dominance_frontiers_1 (frontiers, idom, c->index, done);
      });
      
  /* Find blocks conforming to rule (1) above.  */
  for (e = b->succ; e; e = e->succ_next)
    {
      if (e->dest == EXIT_BLOCK_PTR)
	continue;
      if (get_immediate_dominator (idom, e->dest)->index != bb)
        bitmap_set_bit (frontiers[bb], e->dest->index);
    }

  /* Find blocks conforming to rule (2).  */
  if (dominated)
    EXECUTE_IF_SET_IN_BITMAP (dominated, 0, i,
      {
        int x;
        c = BASIC_BLOCK (i);

        EXECUTE_IF_SET_IN_BITMAP (frontiers[c->index], 0, x,
	  {
	    if (get_immediate_dominator (idom, BASIC_BLOCK (x))->index != bb)
	      bitmap_set_bit (frontiers[bb], x);
	  });
      });

  /* If we built the dominated bitmap rather than using the
     one in the bb's annotation, then make sure we free it.  */
  if (! bb_ann (b))
    BITMAP_XFREE (dominated);
}

void
compute_dominance_frontiers (bitmap *frontiers, dominance_info idom)
{
  sbitmap done = sbitmap_alloc (last_basic_block);

  timevar_push (TV_DOM_FRONTIERS);

  sbitmap_zero (done);

  compute_dominance_frontiers_1 (frontiers, idom, 0, done);

  sbitmap_free (done);

  timevar_pop (TV_DOM_FRONTIERS);
}

/*---------------------------------------------------------------------------
			 Code insertion and replacement
---------------------------------------------------------------------------*/

/* Insert basic block NEW_BB before block BB.  */

void
insert_bb_before (basic_block new_bb, basic_block bb)
{
  edge e;

  /* Reconnect BB's predecessors to NEW_BB.  */
  for (e = bb->pred; e; e = e->pred_next)
    redirect_edge_succ (e, new_bb);

  /* Create the edge NEW_BB -> BB.  */
  make_edge (new_bb, bb, 0);
}



/*---------------------------------------------------------------------------
			      Debugging functions
---------------------------------------------------------------------------*/

/* Dump tree-specific information of BB to file OUTF.  */

void
tree_dump_bb (basic_block bb, FILE *outf, int indent)
{
  dump_generic_bb (outf, bb, indent, TDF_VOPS);
}

/* Dump a basic block on stderr.  */

void
debug_tree_bb (basic_block bb)
{
  dump_bb (bb, stderr, 0);
}

/* Dump a basic block N on stderr.  */

basic_block
debug_tree_bb_n (int n)
{
  debug_tree_bb (BASIC_BLOCK (n));
  return BASIC_BLOCK (n);
}	 

/* Dump the CFG on stderr.

   FLAGS are the same used by the tree dumping functions
   (see TDF_* in tree.h).  */

void
debug_tree_cfg (int flags)
{
  dump_tree_cfg (stderr, flags);
}


/* Dump the program showing basic block boundaries on the given FILE.

   FLAGS are the same used by the tree dumping functions (see TDF_* in
   tree.h).  */

void
dump_tree_cfg (FILE *file, int flags)
{
  if (flags & TDF_DETAILS)
    {
      const char *funcname
	= (*lang_hooks.decl_printable_name) (current_function_decl, 2);

      fputc ('\n', file);
      fprintf (file, ";; Function %s\n\n", funcname);
      fprintf (file, ";; \n%d basic blocks, %d edges, last basic block %d.\n\n",
	       n_basic_blocks, n_edges, last_basic_block);

      brief_dump_cfg (file);
      fprintf (file, "\n");
    }

  if (flags & TDF_STATS)
    dump_cfg_stats (file);

  dump_function_to_file (current_function_decl, file, flags | TDF_BLOCKS);
}

/* Dump CFG statistics on FILE.  */

void
dump_cfg_stats (FILE *file)
{
  static long max_num_merged_labels = 0;
  unsigned long size, total = 0;
  long n_edges;
  basic_block bb;
  const char * const fmt_str   = "%-30s%-13s%12s\n";
  const char * const fmt_str_1 = "%-30s%13lu%11lu%c\n";
  const char * const fmt_str_3 = "%-43s%11lu%c\n";
  const char *funcname
    = (*lang_hooks.decl_printable_name) (current_function_decl, 2);


  fprintf (file, "\nCFG Statistics for %s\n\n", funcname);

  fprintf (file, "---------------------------------------------------------\n");
  fprintf (file, fmt_str, "", "  Number of  ", "Memory");
  fprintf (file, fmt_str, "", "  instances  ", "used ");
  fprintf (file, "---------------------------------------------------------\n");

  size = n_basic_blocks * sizeof (struct basic_block_def);
  total += size;
  fprintf (file, fmt_str_1, "Basic blocks", n_basic_blocks, SCALE (size),
	   LABEL (size));

  n_edges = 0;
  FOR_EACH_BB (bb)
    {
      edge e;
      for (e = bb->succ; e; e = e->succ_next)
	n_edges++;
    }
  size = n_edges * sizeof (struct edge_def);
  total += size;
  fprintf (file, fmt_str_1, "Edges", n_edges, SCALE (size), LABEL (size));

  size = n_basic_blocks * sizeof (struct bb_ann_d);
  total += size;
  fprintf (file, fmt_str_1, "Basic block annotations", n_basic_blocks,
	   SCALE (size), LABEL (size));

  fprintf (file, "---------------------------------------------------------\n");
  fprintf (file, fmt_str_3, "Total memory used by CFG data", SCALE (total),
	   LABEL (total));
  fprintf (file, "---------------------------------------------------------\n");
  fprintf (file, "\n");

  if (cfg_stats.num_merged_labels > max_num_merged_labels)
    max_num_merged_labels = cfg_stats.num_merged_labels;

  fprintf (file, "Coalesced label blocks: %ld (Max so far: %ld)\n",
	   cfg_stats.num_merged_labels, max_num_merged_labels);

  fprintf (file, "\n");
}


/* Dump CFG statistics on stderr.  */

void
debug_cfg_stats (void)
{
  dump_cfg_stats (stderr);
}


/* Dump the flowgraph to a .dot FILE.  */

void
tree_cfg2dot (FILE *file)
{
  edge e;
  basic_block bb;
  const char *funcname
    = (*lang_hooks.decl_printable_name) (current_function_decl, 2);

  /* Write the file header.  */
  fprintf (file, "digraph %s\n{\n", funcname);

  /* Write blocks and edges.  */
  for (e = ENTRY_BLOCK_PTR->succ; e; e = e->succ_next)
    {
      fprintf (file, "\tENTRY -> %d", e->dest->index);

      if (e->flags & EDGE_FAKE)
	fprintf (file, " [weight=0, style=dotted]");

      fprintf (file, ";\n");
    }
  fputc ('\n', file);

  FOR_EACH_BB (bb)
    {
      enum tree_code head_code, end_code;
      const char *head_name, *end_name;
      int head_line = 0;
      int end_line = 0;
      tree first = first_stmt (bb);
      tree last = last_stmt (bb);

      if (first)
        {
	  head_code = TREE_CODE (first);
	  head_name = tree_code_name[head_code];
	  head_line = get_lineno (first);
	}
      else
        head_name = "no-statement";

      if (last)
        {
	  end_code = TREE_CODE (last);
	  end_name = tree_code_name[end_code];
	  end_line = get_lineno (last);
	}
      else
        end_name = "no-statement";

      fprintf (file, "\t%d [label=\"#%d\\n%s (%d)\\n%s (%d)\"];\n",
	       bb->index, bb->index, head_name, head_line, end_name,
	       end_line);

      for (e = bb->succ; e; e = e->succ_next)
	{
	  if (e->dest == EXIT_BLOCK_PTR)
	    fprintf (file, "\t%d -> EXIT", bb->index);
	  else
	    fprintf (file, "\t%d -> %d", bb->index, e->dest->index);

	  if (e->flags & EDGE_FAKE)
	    fprintf (file, " [weight=0, style=dotted]");

	  fprintf (file, ";\n");
	}

      if (bb->next_bb != EXIT_BLOCK_PTR)
	fputc ('\n', file);
    }

  fputs ("}\n\n", file);
}



/*---------------------------------------------------------------------------
			     Miscellaneous helpers
---------------------------------------------------------------------------*/

/* Return true if T represents a stmt that always transfers control.  */

bool
is_ctrl_stmt (tree t)
{
  return (TREE_CODE (t) == COND_EXPR
	  || TREE_CODE (t) == SWITCH_EXPR
	  || TREE_CODE (t) == GOTO_EXPR
	  || TREE_CODE (t) == RETURN_EXPR
	  || TREE_CODE (t) == RESX_EXPR);
}

/* Return true if T is a stmt that may or may not alter the flow of control
   (i.e., a call to a non-returning function).  */

bool
is_ctrl_altering_stmt (tree t)
{
  tree call = t;

#if defined ENABLE_CHECKING
  if (t == NULL)
    abort ();
#endif

  switch (TREE_CODE (t))
    {
    case MODIFY_EXPR:
      /* A MODIFY_EXPR with a rhs of a call has the characteristics
	 of the call.  */
      call = TREE_OPERAND (t, 1);
      if (TREE_CODE (call) != CALL_EXPR)
	break;
      /* FALLTHRU */

    case CALL_EXPR:
      /* A non-pure/const CALL_EXPR alters flow control if the current function
         has nonlocal labels.  */
      if (TREE_SIDE_EFFECTS (t)
	  && FUNCTION_RECEIVES_NONLOCAL_GOTO (current_function_decl))
	return true;

      /* A CALL_EXPR also alters flow control if it does not return.  */
      if (call_expr_flags (call) & (ECF_NORETURN | ECF_LONGJMP))
	return true;
      break;

    default:
      return false;
    }

  /* If a statement can throw, it alters control flow.  */
  return tree_can_throw_internal (t);
}

/* Return true if T is a computed goto.  */

bool
computed_goto_p (tree t)
{
  return (TREE_CODE (t) == GOTO_EXPR
          && TREE_CODE (GOTO_DESTINATION (t)) != LABEL_DECL);
}

/* Return true when GOTO is an non-local goto.  */
bool
nonlocal_goto_p (tree stmt)
{
 return ((TREE_CODE (GOTO_DESTINATION (stmt)) == LABEL_DECL
	   && (decl_function_context (GOTO_DESTINATION (stmt))
	       != current_function_decl))
	  || (TREE_CODE (GOTO_DESTINATION (stmt)) != LABEL_DECL
	      && DECL_CONTEXT (current_function_decl)));
}

/* Checks whether EXPR is a simple local goto.  */

bool
simple_goto_p (tree expr)
{
  return  (TREE_CODE (expr) == GOTO_EXPR
	   && TREE_CODE (GOTO_DESTINATION (expr)) == LABEL_DECL
	   && ! NONLOCAL_LABEL (GOTO_DESTINATION (expr))
	   && (decl_function_context (GOTO_DESTINATION (expr))
	       == current_function_decl));
}

/* Return true if T should start a new basic block.  PREV_T is the
   statement preceding T.  It is used when T is a label or a case label.
   Labels should only start a new basic block if their previous statement
   wasn't a label.  Otherwise, sequence of labels would generate
   unnecessary basic blocks that only contain a single label.  */

static inline bool
stmt_starts_bb_p (tree t, tree prev_t)
{
  enum tree_code code;

  if (t == NULL_TREE)
    return false;

  /* LABEL_EXPRs start a new basic block only if the preceding statement wasn't
     a label of the same type.  This prevents the creation of consecutive
     blocks that have nothing but a single label.  */
  code = TREE_CODE (t);
  if (code == LABEL_EXPR)
    {
      /* Nonlocal and computed GOTO targets always start a new block.  */
      if (code == LABEL_EXPR
	  && (NONLOCAL_LABEL (LABEL_EXPR_LABEL (t))
	      || FORCED_LABEL (LABEL_EXPR_LABEL (t))))
	return true;

      if (prev_t && TREE_CODE (prev_t) == code)
	{
	  cfg_stats.num_merged_labels++;

	  return false;
	}
      else
	return true;
    }

  return false;
}

/* Return true if T should end a basic block.  */

bool
stmt_ends_bb_p (tree t)
{
  return is_ctrl_stmt (t) || is_ctrl_altering_stmt (t);
}

/* Add gotos that used to be represented implicitly in cfg.  */

void
disband_implicit_edges (void)
{
  basic_block bb;
  block_stmt_iterator last;
  edge e;
  tree stmt, label;

  FOR_EACH_BB (bb)
    {
      last = bsi_last (bb);
      stmt = last_stmt (bb);

      if (stmt && TREE_CODE (stmt) == COND_EXPR)
	{
	  /* Remove superfluous gotos from COND_EXPR branches.  Moved
	     from cfg_remove_useless_stmts here since it violates the
	     invariants for tree--cfg correspondence and thus fits better
	     here where we do it anyway.  */

	  for (e = bb->succ; e; e = e->succ_next)
	    {
	      if (e->dest != bb->next_bb)
		continue;

	      if (e->flags & EDGE_TRUE_VALUE)
		COND_EXPR_THEN (stmt) = build_empty_stmt ();
	      else if (e->flags & EDGE_FALSE_VALUE)
		COND_EXPR_ELSE (stmt) = build_empty_stmt ();
	      else
		abort ();
	    }

	  continue;
	}

      if (stmt && TREE_CODE (stmt) == RETURN_EXPR)
	{
	  /* Remove the RETURN_EXPR if we may fallthru to the exit
	     instead.  */
	  if (!bb->succ
	      || bb->succ->succ_next
	      || bb->succ->dest != EXIT_BLOCK_PTR)
	    abort ();

	  if (bb->next_bb == EXIT_BLOCK_PTR
	      && !TREE_OPERAND (stmt, 0))
	    {
	      bsi_remove (&last);
	      bb->succ->flags |= EDGE_FALLTHRU;
	    }
	  continue;
	}

      /* There can be no fallthru edge if the last statement is a control
         one.  */
      if (stmt && is_ctrl_stmt (stmt))
	continue;

      /* Find a fallthru edge and emit the goto if neccesary.  */
      for (e = bb->succ; e; e = e->succ_next)
	if (e->flags & EDGE_FALLTHRU)
	  break;

      if (!e
	  || e->dest == bb->next_bb)
	continue;

      if (e->dest == EXIT_BLOCK_PTR)
	abort ();

      label = tree_block_label (e->dest);
      /* ??? Why bother putting this back together when rtl is just
	 about to take it apart again?  */
      if (factored_computed_goto_label
	  && label == LABEL_EXPR_LABEL (factored_computed_goto_label))
	label = GOTO_DESTINATION (factored_computed_goto);

      bsi_insert_after (&last,
			build1 (GOTO_EXPR, void_type_node, label),
			BSI_NEW_STMT);
    }

  factored_computed_goto = NULL;
  factored_computed_goto_label = NULL;
}

/* Remove all the blocks and edges that make up the flowgraph.  */

void
delete_tree_cfg (void)
{
  if (n_basic_blocks > 0)
    free_blocks_annotations ();

  free_basic_block_vars (0);
  tree_bb_root = NULL;
  tree_phi_root = NULL;
  label_to_block_map = NULL;
}

/* Return the first statement in basic block BB, stripped of any NOP
   containers.  */

tree
first_stmt (basic_block bb)
{
  block_stmt_iterator i = bsi_start (bb);
  return !bsi_end_p (i) ? bsi_stmt (i) : NULL_TREE;
}

/* Return the last statement in basic block BB, stripped of any NOP
   containers.  */

tree
last_stmt (basic_block bb)
{
  block_stmt_iterator b = bsi_last (bb);
  return !bsi_end_p (b) ? bsi_stmt (b) : NULL_TREE;
}

/* Return a pointer to the last statement in block BB.  */

tree *
last_stmt_ptr (basic_block bb)
{
  block_stmt_iterator last = bsi_last (bb);
  return !bsi_end_p (last) ? bsi_stmt_ptr (last) : NULL;
}

/* Return the last statement of an otherwise empty block.  Return NULL
   if the block is totally empty, or if it contains more than one stmt.  */

tree
last_and_only_stmt (basic_block bb)
{
  block_stmt_iterator i = bsi_last (bb);
  tree last, prev;

  if (bsi_end_p (i))
    return NULL_TREE;

  last = bsi_stmt (i);
  bsi_prev (&i);
  if (bsi_end_p (i))
    return last;

  /* Empty statements should no longer appear in the instruction stream.
     Everything that might have appeared before should be deleted by
     remove_useless_stmts, and the optimizers should just bsi_remove
     instead of smashing with build_empty_stmt.

     Thus the only thing that should appear here in a block containing
     one executable statement is a label.  */
    
  prev = bsi_stmt (i);
  if (TREE_CODE (prev) == LABEL_EXPR)
    return last;
  else
    return NULL_TREE;
}

/* Insert statement T into basic block BB.  */

void
set_bb_for_stmt (tree t, basic_block bb)
{
  if (TREE_CODE (t) == STATEMENT_LIST)
    {
      tree_stmt_iterator i;
      for (i = tsi_start (t); !tsi_end_p (i); tsi_next (&i))
	set_bb_for_stmt (tsi_stmt (i), bb);
    }
  else
    {
      stmt_ann_t ann = get_stmt_ann (t);
      ann->bb = bb;

      /* If the statement is a label, add the label to block-to-labels map
	 so that we can speed up edge creation for GOTO_EXPRs.  */
      if (TREE_CODE (t) == LABEL_EXPR)
	{
	  int uid;

	  t = LABEL_EXPR_LABEL (t);
	  uid = LABEL_DECL_UID (t);
	  if (uid == -1)
	    {
	      LABEL_DECL_UID (t) = uid = cfun->last_label_uid++;
	      if (VARRAY_SIZE (label_to_block_map) <= (unsigned) uid)
		VARRAY_GROW (label_to_block_map, 3 * uid / 2);
	    }
	  else
	    {
#ifdef ENABLE_CHECKING
	      /* We're moving an existing label.  Make sure that we've
		 removed it from the old block.  */
	      if (bb && VARRAY_BB (label_to_block_map, uid))
		abort ();
#endif
	    }
	  VARRAY_BB (label_to_block_map, uid) = bb;
	}
    }
}

/* Insert a statement, or statement list, before the given pointer.  */

void
bsi_insert_before (block_stmt_iterator *i, tree t, enum bsi_iterator_update m)
{
  set_bb_for_stmt (t, i->bb);
  modify_stmt (t);
  tsi_link_before (&i->tsi, t, m);
}

/* Insert a statement, or statement list, after the given pointer.  */

void
bsi_insert_after (block_stmt_iterator *i, tree t, enum bsi_iterator_update m)
{
  set_bb_for_stmt (t, i->bb);
  modify_stmt (t);
  tsi_link_after (&i->tsi, t, m);
}

/* Remove the statement at the given pointer.  The pointer is updated
   to the next statement.  */

void
bsi_remove (block_stmt_iterator *i)
{
  tree t = bsi_stmt (*i);
  set_bb_for_stmt (t, NULL);
  modify_stmt (t);
  tsi_delink (&i->tsi);
}

/* Move the statement at FROM so it comes right after the statement at TO.  */

void 
bsi_move_after (block_stmt_iterator *from, block_stmt_iterator *to)
{
  tree stmt = bsi_stmt (*from);
  bsi_remove (from);
  bsi_insert_after (to, stmt, BSI_SAME_STMT);
} 

/* Move the statement at FROM so it comes right before the statement at TO.  */

void 
bsi_move_before (block_stmt_iterator *from, block_stmt_iterator *to)
{
  tree stmt = bsi_stmt (*from);
  bsi_remove (from);
  bsi_insert_before (to, stmt, BSI_SAME_STMT);
}

/* Move the statement at FROM to the end of basic block BB.  */

void
bsi_move_to_bb_end (block_stmt_iterator *from, basic_block bb)
{
  block_stmt_iterator last = bsi_last (bb);
  
  /* Have to check bsi_end_p because it could be an empty block.  */
  if (!bsi_end_p (last) && is_ctrl_stmt (bsi_stmt (last)))
    bsi_move_before (from, &last);
  else
    bsi_move_after (from, &last);
}

/* Replace the contents of a stmt with another.  */

void
bsi_replace (const block_stmt_iterator *bsi, tree stmt, bool preserve_eh_info)
{
  int eh_region;
  tree orig_stmt = bsi_stmt (*bsi);

  SET_EXPR_LOCUS (stmt, EXPR_LOCUS (orig_stmt));
  set_bb_for_stmt (stmt, bsi->bb);

  /* Preserve EH region information from the original statement, if
     requested by the caller.  */
  if (preserve_eh_info)
    {
      eh_region = lookup_stmt_eh_region (orig_stmt);
      if (eh_region >= 0)
	add_stmt_to_eh_region (stmt, eh_region);
    }

  *bsi_stmt_ptr (*bsi) = stmt;
  modify_stmt (stmt);
}

/* This routine locates a place to insert a statement on an edge.  Every
   attempt is made to place the stmt in an existing basic block, but
   sometimes that isn't possible.  When it isn't possible, the edge is
   split and the stmt is added to the new block.

   In all cases, the returned *BSI points to the correct location.  The
   return value is true if insertion should be done after the location,
   or false if before the location.  */

static bool
tree_find_edge_insert_loc (edge e, block_stmt_iterator *bsi)
{
  basic_block dest, src;
  tree tmp;

  dest = e->dest;
 restart:

  /* If the destination has one predecessor, insert there.  Except
     for the exit block.  */
  if (dest->pred->pred_next == NULL && dest != EXIT_BLOCK_PTR)
    {
      *bsi = bsi_start (dest);
      if (bsi_end_p (*bsi))
	return true;

      /* Make sure we insert after any leading labels.  */
      tmp = bsi_stmt (*bsi);
      while (TREE_CODE (tmp) == LABEL_EXPR)
	{
	  bsi_next (bsi);
	  if (bsi_end_p (*bsi))
	    break;
	  tmp = bsi_stmt (*bsi);
	}

      if (bsi_end_p (*bsi))
	{
	  *bsi = bsi_last (dest);
	  return true;
	}
      else
	return false;
    }

  /* If the source has one successor, the edge is not abnormal and
     the last statement does not end a basic block, insert there.
     Except for the entry block.  */
  src = e->src;
  if ((e->flags & EDGE_ABNORMAL) == 0
      && src->succ->succ_next == NULL
      && src != ENTRY_BLOCK_PTR)
    {
      *bsi = bsi_last (src);
      if (bsi_end_p (*bsi))
	return true;

      tmp = bsi_stmt (*bsi);
      if (!stmt_ends_bb_p (tmp))
	return true;
    }

  /* Otherwise, create a new basic block, and split this edge.  */
  dest = tree_split_edge (e);
  e = dest->pred;
  goto restart;
}

/* This routine will commit all pending edge insertions, creating any new
   basic blocks which are necessary.

   If UPDATE_ANNOTATIONS is true, then new bitmaps are created for the
   dominator children, and they are updated.  If specified, NEW_BLOCKS
   returns a count of the number of new basic blocks which were created.  */

void
bsi_commit_edge_inserts (bool update_annotations, int *new_blocks)
{
  basic_block bb;
  edge e;
  int blocks;

  blocks = n_basic_blocks;

  bsi_commit_edge_inserts_1 (ENTRY_BLOCK_PTR->succ);

  FOR_EACH_BB (bb)
    for (e = bb->succ; e; e = e->succ_next)
      bsi_commit_edge_inserts_1 (e);

  if (new_blocks)
    *new_blocks = n_basic_blocks - blocks;

  /* Expand arrays if we created new blocks and need to update them.  */
  if (update_annotations && blocks != n_basic_blocks)
    {
      /* TODO. Unimplemented at the moment.  */
      abort ();
    }
}


/* Commit insertions pending at edge E.  */

static void
bsi_commit_edge_inserts_1 (edge e)
{
  if (PENDING_STMT (e))
    {
      block_stmt_iterator bsi;
      tree stmt = PENDING_STMT (e);

      PENDING_STMT (e) = NULL_TREE;

      if (tree_find_edge_insert_loc (e, &bsi))
	bsi_insert_after (&bsi, stmt, BSI_NEW_STMT);
      else
	bsi_insert_before (&bsi, stmt, BSI_NEW_STMT);
    }
}


/* This routine adds a stmt to the pending list on an edge. No actual
   insertion is made until a call to bsi_commit_edge_inserts () is made.  */

void
bsi_insert_on_edge (edge e, tree stmt)
{
  append_to_statement_list (stmt, &PENDING_STMT (e));
}

/* Similar to bsi_insert_on_edge+bsi_commit_edge_inserts.  */
/* ??? Why in the world do we need this?  Only PRE uses it.  */

void
bsi_insert_on_edge_immediate (edge e, tree stmt)
{
  block_stmt_iterator bsi;

  if (PENDING_STMT (e))
    abort ();

  if (tree_find_edge_insert_loc (e, &bsi))
    bsi_insert_after (&bsi, stmt, BSI_NEW_STMT);
  else
    bsi_insert_before (&bsi, stmt, BSI_NEW_STMT);
}

/*---------------------------------------------------------------------------
	    Tree specific functions for the cfg loop optimizer
---------------------------------------------------------------------------*/

/* Split a (typically critical) edge.  Return the new block.
   Abort on abnormal edges.  */

basic_block
tree_split_edge (edge edge_in)
{
  basic_block new_bb, after_bb, dest;
  edge new_edge, e;
  tree phi;
  int i, num_elem;

  /* Abnormal edges cannot be split.  */
  if (edge_in->flags & EDGE_ABNORMAL)
    abort ();

  dest = edge_in->dest;

  /* Place the new block in the block list.  Try to keep the new block
     near its "logical" location.  This is of most help to humans looking
     at debugging dumps.  */

  for (e = dest->pred; e; e = e->pred_next)
    if (e->src->next_bb == dest)
      break;
  if (!e)
    after_bb = dest->prev_bb;
  else
    after_bb = edge_in->src;

  new_bb = create_bb (NULL, after_bb);
  create_block_annotation (new_bb);
  new_edge = make_edge (new_bb, dest, EDGE_FALLTHRU);

  if (!tree_redirect_edge_and_branch_1 (edge_in, new_bb, true))
    abort ();

  /* Find all the PHI arguments on the original edge, and change them to
     the new edge.  */
  for (phi = phi_nodes (dest); phi; phi = TREE_CHAIN (phi))
    {
      num_elem = PHI_NUM_ARGS (phi);
      for (i = 0; i < num_elem; i++)
	if (PHI_ARG_EDGE (phi, i) == edge_in)
	  {
	    PHI_ARG_EDGE (phi, i) = new_edge;
	    break;
	  }
    }

  return new_bb;
}

/* Return true when BB has label LABEL in it.  */
static bool
has_label_p (basic_block bb, tree label)
{
  block_stmt_iterator bsi;

  for (bsi = bsi_start (bb); !bsi_end_p (bsi); bsi_next (&bsi))
    {
      tree stmt = bsi_stmt (bsi);

      if (TREE_CODE (stmt) != LABEL_EXPR)
	return false;
      if (LABEL_EXPR_LABEL (stmt) == label)
	return true;
    }
  return false;
}

/* Callback for walk_tree, check that all elements with address taken are
   properly noticed as such.  */

static tree
verify_addr_expr (tree *tp, int *walk_subtrees ATTRIBUTE_UNUSED,
		  void *data ATTRIBUTE_UNUSED)
{
  if (TREE_CODE (*tp) == ADDR_EXPR)
    {
      tree x = TREE_OPERAND (*tp, 0);
      while (TREE_CODE (x) == ARRAY_REF
	     || TREE_CODE (x) == COMPONENT_REF
	     || TREE_CODE (x) == REALPART_EXPR
	     || TREE_CODE (x) == IMAGPART_EXPR)
	x = TREE_OPERAND (x, 0);
      if (TREE_CODE (x) != VAR_DECL && TREE_CODE (x) != PARM_DECL)
	return NULL;
      if (!TREE_ADDRESSABLE (x))
        return x;
    }
  return NULL;
}


/* Verify the STMT, return true if STMT is missformed.
   Always keep global so it can be called via GDB. 

   TODO: Implement type checking.  */

bool
verify_stmt (tree stmt)
{
  tree addr;

  if (!is_gimple_stmt (stmt))
    {
      error ("Is not valid gimple statement.");
      debug_generic_stmt (stmt);
      return true;
    }
  addr = walk_tree (&stmt, verify_addr_expr, NULL, NULL);
  if (addr)
    {
      error ("Address taken, but ADDRESABLE bit not set");
      debug_generic_stmt (addr);
      return true;
    }
  return false;
}

/* Return true when the T can be shared.  */
static bool
tree_node_shared_p (tree t)
{
  if (TYPE_P (t) || DECL_P (t)
      || is_gimple_min_invariant (t)
      || TREE_CODE (t) == SSA_NAME)
    return true;
  while ((TREE_CODE (t) == ARRAY_REF
          && is_gimple_min_invariant (TREE_OPERAND (t, 1)))
	 || (TREE_CODE (t) == COMPONENT_REF
	     || TREE_CODE (t) == REALPART_EXPR
	     || TREE_CODE (t) == IMAGPART_EXPR))
    t = TREE_OPERAND (t, 0);
  if (DECL_P (t))
    return true;
  return false;
}

/* Called via walk_trees.  Verify tree sharing.  */
static tree
verify_node_sharing (tree * tp, int *walk_subtrees, void *data)
{
  htab_t htab = (htab_t) data;
  void **slot;

  if (tree_node_shared_p (*tp))
    {
      *walk_subtrees = false;
      return NULL;
    }
  slot = htab_find_slot (htab, *tp, INSERT);
  if (*slot)
    return *slot;
  *slot = *tp;
  return NULL;
}


/* Verify the GIMPLE statement chain.  */

void
verify_stmts (void)
{
  basic_block bb;
  block_stmt_iterator bsi;
  bool err = false;
  htab_t htab;
  tree addr;

  htab = htab_create (37, htab_hash_pointer, htab_eq_pointer, NULL);

  FOR_EACH_BB (bb)
    {
      tree phi;
      int i;

      for (phi = phi_nodes (bb); phi; phi = TREE_CHAIN (phi))
	{
	  int phi_num_args = PHI_NUM_ARGS (phi);

	  for (i = 0; i < phi_num_args; i++)
	    {
	      tree t = PHI_ARG_DEF (phi, i);
	      tree addr;

	      /* Addressable variables do have SSA_NAMEs but they
	         are not considered gimple values.  */
	      if (TREE_CODE (t) != SSA_NAME
		  && TREE_CODE (t) != FUNCTION_DECL
		  && !is_gimple_val (t))
		{
		  error ("PHI def is not GIMPLE value");
		  debug_generic_stmt (phi);
		  debug_generic_stmt (t);
		  err |= true;
		}

	      addr = walk_tree (&t, verify_addr_expr, NULL, NULL);
	      if (addr)
		{
		  error ("Address taken, but ADDRESABLE bit not set");
		  debug_generic_stmt (addr);
		  err |= true;
		}

	      addr = walk_tree (&t, verify_node_sharing, htab, NULL);
	      if (addr)
		{
		  error ("Wrong sharing of tree nodes");
		  debug_generic_stmt (phi);
		  debug_generic_stmt (addr);
		  err |= true;
		}
	    }
	}

      for (bsi = bsi_start (bb); !bsi_end_p (bsi); bsi_next (&bsi))
	{
	  tree stmt = bsi_stmt (bsi);
	  err |= verify_stmt (stmt);
	  addr = walk_tree (&stmt, verify_node_sharing, htab, NULL);
	  if (addr)
	    {
	      error ("Wrong sharing of tree nodes");
	      debug_generic_stmt (stmt);
	      debug_generic_stmt (addr);
	      err |= true;
	    }
	}
    }

  if (err)
    internal_error ("verify_stmts failed.");

  htab_delete (htab);
}


/* Verifies that the flow information is OK.  */

static int
tree_verify_flow_info (void)
{
  int err = 0;
  basic_block bb;
  block_stmt_iterator bsi;
  tree stmt;
  edge e;

  if (ENTRY_BLOCK_PTR->stmt_list)
    {
      error ("ENTRY_BLOCK has stmt list associated with it\n");
      err = 1;
    }
  if (EXIT_BLOCK_PTR->stmt_list)
    {
      error ("EXIT_BLOCK has stmt list associated with it\n");
      err = 1;
    }

  for (e = EXIT_BLOCK_PTR->pred; e; e = e->pred_next)
    if (e->flags & EDGE_FALLTHRU)
      {
	error ("Fallthru to exit from bb %d\n", e->src->index);
	err = 1;
      }

  FOR_EACH_BB (bb)
    {
      bool found_ctrl_stmt = false;

      /* Skip labels on the start of basic block.  */
      for (bsi = bsi_start (bb); !bsi_end_p (bsi); bsi_next (&bsi))
	{
	  if (TREE_CODE (bsi_stmt (bsi)) != LABEL_EXPR)
	    break;
	  if (label_to_block (LABEL_EXPR_LABEL (bsi_stmt (bsi))) != bb)
	    {
	      error ("Label %s to block does not match in bb %d\n",
		     IDENTIFIER_POINTER (DECL_NAME (bsi_stmt (bsi))),
		     bb->index);
	      err = 1;
	    }
	  if (decl_function_context (LABEL_EXPR_LABEL (bsi_stmt (bsi)))
	      != current_function_decl)
	    {
	      error ("Label %s has incorrect context in bb %d\n",
		     IDENTIFIER_POINTER (DECL_NAME (bsi_stmt (bsi))),
		     bb->index);
	      err = 1;
	    }
	}
      /* Verify that body of basic block is free of control flow.  */
      for (; !bsi_end_p (bsi); bsi_next (&bsi))
	{
	  tree stmt = bsi_stmt (bsi);

	  if (found_ctrl_stmt)
	    {
	      error ("Control flow in the middle of basic block %d\n", bb->index);
	      err = 1;
	    }
	  if (stmt_ends_bb_p (stmt))
	    found_ctrl_stmt = true;
	  if (TREE_CODE (stmt) == LABEL_EXPR)
	    {
	      error ("Label %s in the middle of basic block %d\n",
		     IDENTIFIER_POINTER (DECL_NAME (stmt)),
		     bb->index);
	      err = 1;
	    }
	}
      bsi = bsi_last (bb);
      if (bsi_end_p (bsi))
	continue;

      stmt = bsi_stmt (bsi);

      if (is_ctrl_stmt (stmt))
	{
	  for (e = bb->succ; e; e = e->succ_next)
	    if (e->flags & EDGE_FALLTHRU)
	      {
		error ("Fallthru edge after a control statement in bb %d \n",
		       bb->index);
		err = 1;
	      }
	}

      switch (TREE_CODE (stmt))
	{
	case COND_EXPR:
	  {
	    edge true_edge;
	    edge false_edge;
	    if (TREE_CODE (COND_EXPR_THEN (stmt)) != GOTO_EXPR
		|| TREE_CODE (COND_EXPR_ELSE (stmt)) != GOTO_EXPR)
	      {
		error ("Structured COND_EXPR at end of bb %d\n", bb->index);
		err = 1;
	      }
	    if (bb->succ->flags & EDGE_TRUE_VALUE)
	      true_edge = bb->succ, false_edge = bb->succ->succ_next;
	    else
	      false_edge = bb->succ, true_edge = bb->succ->succ_next;
	    if (!true_edge || !false_edge
		|| !(true_edge->flags & EDGE_TRUE_VALUE)
		|| !(false_edge->flags & EDGE_FALSE_VALUE)
		|| (true_edge->flags & (EDGE_FALLTHRU | EDGE_ABNORMAL))
		|| (false_edge->flags & (EDGE_FALLTHRU | EDGE_ABNORMAL))
		|| bb->succ->succ_next->succ_next)
	      {
		error ("Wrong outgoing edge flags at end of bb %d\n", bb->index);
		err = 1;
	      }
	    if (!has_label_p (true_edge->dest,
			      GOTO_DESTINATION (COND_EXPR_THEN (stmt)))
		|| !has_label_p (false_edge->dest,
				 GOTO_DESTINATION (COND_EXPR_ELSE (stmt))))
	      {
		error ("Label %s does not match edge at end of bb %d\n",
		       IDENTIFIER_POINTER (DECL_NAME (stmt)),
		       bb->index);
		err = 1;
	      }
	  }
	  break;

	case GOTO_EXPR:
	  if (simple_goto_p (stmt))
	    {
	      error ("Explicit goto at end of bb %d\n", bb->index);
    	      err = 1;
	    }
	  else
	    {
	      /* We shall double check that the labels in destination blocks have
	         address taken.  */

	      for (e = bb->succ; e; e = e->succ_next)
		if ((e->flags & (EDGE_FALLTHRU | EDGE_TRUE_VALUE | EDGE_FALSE_VALUE))
		    || !(e->flags & EDGE_ABNORMAL))
		  {
		    error ("Wrong outgoing edge flags at end of bb %d\n", bb->index);
		    err = 1;
		  }
	      if (nonlocal_goto_p (stmt))
		{
	          for (e = bb->succ; e; e = e->succ_next)
		    if (e->dest == EXIT_BLOCK_PTR)
		      break;
		  if (!e)
		    {
		      error ("Missing edge to exit past nonlocal goto bb %d\n", bb->index);
		      err = 1;
		    }
		}
	    }
	  break;

	case RETURN_EXPR:
	  if (!bb->succ || bb->succ->succ_next
	      || (bb->succ->flags & (EDGE_FALLTHRU | EDGE_ABNORMAL
		  		     | EDGE_TRUE_VALUE | EDGE_FALSE_VALUE)))
	    {
	      error ("Wrong outgoing edge flags at end of bb %d\n", bb->index);
	      err = 1;
	    }
	  if (bb->succ->dest != EXIT_BLOCK_PTR)
	    {
	      error ("Return edge does not point to exit in bb %d\n", bb->index);
	      err = 1;
	    }
	  break;

	case SWITCH_EXPR:
	  {
	    edge e;
	    size_t i, n;
	    tree vec;

	    vec = SWITCH_LABELS (stmt);
	    n = TREE_VEC_LENGTH (vec);

	    /* Mark all destination basic block.  */
	    for (i = 0; i < n; ++i)
	      {
		tree lab = CASE_LABEL (TREE_VEC_ELT (vec, i));
		basic_block label_bb = label_to_block (lab);

		if (label_bb->aux && label_bb->aux != (void *)1)
		  abort ();
		label_bb->aux = (void *)1;
	      }

	    for (e = bb->succ; e; e = e->succ_next)
	      {
		if (!e->dest->aux)
		  {
		    error ("Extra outgoing edge %d->%d\n", bb->index, e->dest->index);
		    err = 1;
		  }
		e->dest->aux = (void *)2;
		if ((e->flags & (EDGE_FALLTHRU | EDGE_ABNORMAL
					   | EDGE_TRUE_VALUE | EDGE_FALSE_VALUE)))
		  {
		    error ("Wrong outgoing edge flags at end of bb %d\n", bb->index);
		    err = 1;
		  }
	      }
	    /* Check we do have all of them.  */
	    for (i = 0; i < n; ++i)
	      {
		tree lab = CASE_LABEL (TREE_VEC_ELT (vec, i));
		basic_block label_bb = label_to_block (lab);

		if (label_bb->aux != (void *)2)
		  {
		    error ("Missing edge %i->%i\n", bb->index, label_bb->index);
		    err = 1;
		  }
	      }
	    for (e = bb->succ; e; e = e->succ_next)
	      e->dest->aux = (void *)0;
	  }

	default: ;
	}
    }

  return err;
}

/* Split BB into entry part and rest; if REDIRECT_LATCH, redirect edges
   marked as latch into entry part, analogically for REDIRECT_NONLATCH.
   In both of these cases, ignore edge EXCEPT.  If CONN_LATCH, set edge
   between created entry part and BB as latch one.  Return created entry
   part.  */

static basic_block
tree_make_forwarder_block (basic_block bb, int redirect_latch,
                           int redirect_nonlatch, edge except, int conn_latch)
{
  edge e, next_e, new_e, fallthru;
  basic_block dummy;
  tree phi, new_phi, var, label;
  bool first;
  block_stmt_iterator bsi, bsi_tgt;

  dummy = create_bb (NULL, bb->prev_bb);
  create_block_annotation (dummy);
  dummy->count = bb->count;
  dummy->frequency = bb->frequency;
  dummy->loop_depth = bb->loop_depth;

  /* Redirect the incoming edges.  */
  dummy->pred = bb->pred;
  bb->pred = NULL;
  for (e = dummy->pred; e; e = e->pred_next)
    e->dest = dummy;

  /* Move the phi nodes to the dummy block.  */
  set_phi_nodes (dummy, phi_nodes (bb));
  set_phi_nodes (bb, NULL_TREE);

  /* Move the labels to the new basic block.  */
  for (bsi = bsi_start (bb), bsi_tgt = bsi_start (dummy); !bsi_end_p (bsi); )
    {
      label = bsi_stmt (bsi);
      if (TREE_CODE (label) != LABEL_EXPR)
	break;

      bsi_remove (&bsi);
      bsi_insert_after (&bsi_tgt, label, BSI_NEW_STMT);
    }

  fallthru = make_edge (dummy, bb, EDGE_FALLTHRU);

  alloc_aux_for_block (dummy, sizeof (int));
  HEADER_BLOCK (dummy) = 0;
  HEADER_BLOCK (bb) = 1;

  first = true;

  /* Redirect back edges we want to keep.  */
  for (e = dummy->pred; e; e = next_e)
    {
      next_e = e->pred_next;
      if (e != except
	  && ((redirect_latch && LATCH_EDGE (e))
	      || (redirect_nonlatch && !LATCH_EDGE (e))))
	continue;

      dummy->frequency -= EDGE_FREQUENCY (e);
      dummy->count -= e->count;
      if (dummy->frequency < 0)
	dummy->frequency = 0;
      if (dummy->count < 0)
	dummy->count = 0;

      new_e = tree_redirect_edge_and_branch_1 (e, bb, true);

      if (first)
	{
	  first = false;

	  /* The first time we redirect a branch we must create new phi nodes
	     on the start of bb.  */
	  for (phi = phi_nodes (dummy); phi; phi = TREE_CHAIN (phi))
	    {
	      var = PHI_RESULT (phi);
	      new_phi = create_phi_node (var, bb);
	      SSA_NAME_DEF_STMT (var) = new_phi;
	      PHI_RESULT (phi) = make_ssa_name (SSA_NAME_VAR (var), phi);
	      add_phi_arg (&new_phi, PHI_RESULT (phi), fallthru);
	    }

	  /* Ensure that the phi node chains are in the same order.  */
	  set_phi_nodes (bb, nreverse (phi_nodes (bb)));
	}

      /* Move the argument of the phi node.  */
      for (phi = phi_nodes (dummy), new_phi = phi_nodes (bb);
	   phi;
	   phi = TREE_CHAIN (phi), new_phi = TREE_CHAIN (new_phi))
	{
	  var = PHI_ARG_DEF (phi, phi_arg_from_edge (phi, e));
	  add_phi_arg (&new_phi, var, new_e);
	  remove_phi_arg (phi, e->src);
	}
    }

  alloc_aux_for_edge (fallthru, sizeof (int));
  LATCH_EDGE (fallthru) = conn_latch;

  return dummy;
}

/* Initialization of functions specific to the tree IR.  */

void
tree_register_cfg_hooks (void)
{
  cfg_hooks = &tree_cfg_hooks;
}

/* Initialize loop optimizer.  */

static struct loops *
tree_loop_optimizer_init (FILE *dumpfile)
{
  struct loops *loops = xcalloc (1, sizeof (struct loops));

  /* Find the loops.  */
  if (flow_loops_find (loops, LOOP_TREE) <= 1)
    {
      /* No loops.  */
      flow_loops_free (loops);
      free (loops);
      return NULL;
    }

  /* Not going to update these.  */
  free (loops->cfg.rc_order);
  loops->cfg.rc_order = NULL;
  free (loops->cfg.dfs_order);
  loops->cfg.dfs_order = NULL;

#if 0
  /* Does not work just now.  It will be easier to fix it in the no-gotos
     form.  */
  /* Force all latches to have only single successor.  */
  force_single_succ_latches (loops);
#endif

  /* Mark irreducible loops.  */
  mark_irreducible_loops (loops);

  /* Dump loops.  */
  flow_loops_dump (loops, dumpfile, NULL, 1);

#ifdef ENABLE_CHECKING
  verify_dominators (loops->cfg.dom);
  verify_loop_structure (loops);
#endif

  return loops;
}

/* Finalize loop optimizer.  */
static void
tree_loop_optimizer_finalize (struct loops *loops, FILE *dumpfile)
{
  if (loops == NULL)
    return;

  /* Another dump.  */
  flow_loops_dump (loops, dumpfile, NULL, 1);

  /* Clean up.  */
  flow_loops_free (loops);
  free (loops);

  /* Checking.  */
#ifdef ENABLE_CHECKING
  verify_flow_info ();
#endif
}

/* Return true if basic block BB does nothing except pass control
   flow to another block and that we can safely insert a label at
   the start of the successor block.  */

static bool
tree_forwarder_block_p (basic_block bb)
{
  block_stmt_iterator bsi;
  edge e;

  /* If we have already determined this block is not forwardable, then
     no further checks are necessary.  */
  if (! bb_ann (bb)->forwardable)
    return false;

  /* BB must have a single outgoing normal edge.  Otherwise it can not be
     a forwarder block.  */
  if (!bb->succ
      || bb->succ->succ_next
      || bb->succ->dest == EXIT_BLOCK_PTR
      || (bb->succ->flags & EDGE_ABNORMAL)
      || bb == ENTRY_BLOCK_PTR)
    {
      bb_ann (bb)->forwardable = 0;
      return false; 
    }

  /* Successors of the entry block are not forwarders.  */
  for (e = ENTRY_BLOCK_PTR->succ; e; e = e->succ_next)
    if (e->dest == bb)
      {
	bb_ann (bb)->forwardable = 0;
	return false;
      }

  /* BB can not have any PHI nodes.  This could potentially be relaxed
     early in compilation if we re-rewrote the variables appearing in
     any PHI nodes in forwarder blocks.  */
  if (phi_nodes (bb))
    {
      bb_ann (bb)->forwardable = 0;
      return false; 
    }

  /* Now walk through the statements.  We can ignore labels, anything else
     means this is not a forwarder block.  */
  for (bsi = bsi_start (bb); !bsi_end_p (bsi); bsi_next (&bsi))
    {
      tree stmt = bsi_stmt (bsi);
 
      switch (TREE_CODE (stmt))
	{
	case LABEL_EXPR:
	  break;

	default:
	  bb_ann (bb)->forwardable = 0;
	  return false;
	}
    }

  return true;
}

/* Threads jumps over empty statements.

   This code should _not_ thread over obviously equivalent conditions
   as that requires nontrivial updates to the SSA graph.  */
   
static bool
thread_jumps (void)
{
  edge e, next, last, old;
  basic_block bb, dest, tmp;
  tree phi;
  int arg;
  bool retval = false;

  FOR_EACH_BB (bb)
    bb_ann (bb)->forwardable = 1;

  FOR_BB_BETWEEN (bb, ENTRY_BLOCK_PTR, EXIT_BLOCK_PTR, next_bb)
    {
      /* Don't waste time on unreachable blocks.  */
      if (!bb->pred)
	continue;

      /* Nor on forwarders.  */
      if (tree_forwarder_block_p (bb))
	continue;
      
      /* This block is now part of a forwarding path, mark it as not
	 forwardable so that we can detect loops.   This bit will be
	 reset below.  */
      bb_ann (bb)->forwardable = 0;

      /* Examine each of our block's successors to see if it is
	 forwardable.  */
      for (e = bb->succ; e; e = next)
	{
	  next = e->succ_next;

	  /* If the edge is abnormal or its destination is not
	     forwardable, then there's nothing to do.  */
	  if ((e->flags & EDGE_ABNORMAL)
	      || !tree_forwarder_block_p (e->dest))
	    continue;

	  /* Now walk though as many forwarder block as possible to find
	     the ultimate destination we want to thread our jump to.  */
	  last = e->dest->succ;
	  bb_ann (e->dest)->forwardable = 0;
	  for (dest = e->dest->succ->dest;
	       tree_forwarder_block_p (dest);
	       last = dest->succ,
	       dest = dest->succ->dest)
	    {
	      /* An infinite loop detected.  We redirect the edge anyway, so
		 that the loop is shrinked into single basic block.  */
	      if (!bb_ann (dest)->forwardable)
		break;

	      if (dest->succ->dest == EXIT_BLOCK_PTR)
		break;

	      bb_ann (dest)->forwardable = 0;
	    }

	  /* Reset the forwardable marks to 1.  */
	  for (tmp = e->dest;
	       tmp != dest;
	       tmp = tmp->succ->dest)
	    bb_ann (tmp)->forwardable = 1;

	  if (dest == e->dest)
	    continue;
	      
	  old = find_edge (bb, dest);
	  if (old)
	    {
	      /* If there already is an edge, check whether the values
		 in phi nodes differ.  */
	      if (!phi_alternatives_equal (dest, last, old))
		{
		  /* The previous block is forwarder.  Redirect our jump
		     to that target instead since we know it has no PHI
		     nodes that will need updating.  */
		  dest = last->src;
	  
		  /* That might mean that no forwarding at all is possible.  */
		  if (dest == e->dest)
		    continue;

		  old = find_edge (bb, dest);
		}
	    }

	  /* Perform the redirection.  */
	  retval = true;
	  e = tree_redirect_edge_and_branch (e, dest);
	  if (!old)
	    {
	      /* Update phi nodes.   We know that the new argument should
		 have the same value as the argument associated with LAST.
		 Otherwise we would have changed our target block above.  */
	      for (phi = phi_nodes (dest); phi; phi = TREE_CHAIN (phi))
		{
		  arg = phi_arg_from_edge (phi, last);
		  if (arg < 0)
		    abort ();
		  add_phi_arg (&phi, PHI_ARG_DEF (phi, arg), e);
		}
	    }
	}

      /* Reset the forwardable bit on our block since it's no longer in
	 a forwarding chain path.  */
      bb_ann (bb)->forwardable = 1;
    }
  return retval;
}

/* Return a non-special label in the head of basic block BLOCK.
   Create one if it doesn't exist.  */

static tree
tree_block_label (basic_block bb)
{
  block_stmt_iterator i, s = bsi_start (bb);
  bool first = true;
  tree label, stmt;

  for (i = s; !bsi_end_p (i); first = false, bsi_next (&i))
    {
      stmt = bsi_stmt (i);
      if (TREE_CODE (stmt) != LABEL_EXPR)
	break;
      label = LABEL_EXPR_LABEL (stmt);
      if (!NONLOCAL_LABEL (label))
	{
	  if (!first)
	    bsi_move_before (&i, &s);
	  return label;
	}
    }

  label = create_artificial_label ();
  stmt = build1 (LABEL_EXPR, void_type_node, label);
  bsi_insert_before (&s, stmt, BSI_NEW_STMT);
  return label;
}

/* Attempt to perform edge redirection by replacing possibly complex jump
   instruction by goto or removing jump completely.  This can apply only
   if all edges now point to the same block.  The parameters and
   return values are equivalent to redirect_edge_and_branch.  */

static edge
tree_try_redirect_by_replacing_jump (edge e, basic_block target)
{
  basic_block src = e->src;
  edge tmp;
  block_stmt_iterator b;
  tree stmt;

  /* Verify that all targets will be TARGET.  */
  for (tmp = src->succ; tmp; tmp = tmp->succ_next)
    if (tmp->dest != target && tmp != e)
      break;

  if (tmp)
    return NULL;

  b = bsi_last (src);
  if (bsi_end_p (b))
    return NULL;
  stmt = bsi_stmt (b);

  if (TREE_CODE (stmt) == COND_EXPR
      || TREE_CODE (stmt) == SWITCH_EXPR)
    {
      bsi_remove (&b);
      e = ssa_redirect_edge (e, target);
      e->flags = EDGE_FALLTHRU;
      return e;
    }

  return NULL;
}

/* Redirect E to DEST.  Return NULL on failure, edge representing redirected
   branch otherwise.  */

static edge
tree_redirect_edge_and_branch_1 (edge e, basic_block dest, bool splitting)
{
  basic_block bb = e->src;
  block_stmt_iterator bsi;
  edge ret;
  tree label, stmt;
  int flags;

  if (e->flags & (EDGE_ABNORMAL_CALL | EDGE_EH))
    return NULL;

  if (e->src != ENTRY_BLOCK_PTR 
      && (ret = tree_try_redirect_by_replacing_jump (e, dest)))
    return ret;

  if (e->dest == dest)
    return NULL;

  label = tree_block_label (dest);

  bsi = bsi_last (bb);
  stmt = bsi_end_p (bsi) ? NULL : bsi_stmt (bsi);
  flags = 0;

  switch (stmt ? TREE_CODE (stmt) : ERROR_MARK)
    {
    case COND_EXPR:
      stmt = (e->flags & EDGE_TRUE_VALUE
	      ? COND_EXPR_THEN (stmt)
	      : COND_EXPR_ELSE (stmt));
      flags = e->flags;
      GOTO_DESTINATION (stmt) = label;
      break;

    case GOTO_EXPR:
      /* No nonabnormal edges should lead from a non-simple goto, and simple
	 ones should be represented implicitly.  */
      abort ();

    case SWITCH_EXPR:
      {
	tree vec = SWITCH_LABELS (stmt);
	size_t i, n = TREE_VEC_LENGTH (vec);

	for (i = 0; i < n; ++i)
	  {
	    tree elt = TREE_VEC_ELT (vec, i);
	    if (label_to_block (CASE_LABEL (elt)) == e->dest)
	      CASE_LABEL (elt) = label;
	  }
      }
      break;

    default:
      /* Otherwise it must be a fallthru edge, and we don't need to
	 do anything except for redirecting it.  */
      if (!(e->flags & EDGE_FALLTHRU))
	abort ();
      break;
    }

  /* Update/insert PHI nodes as necessary.  */

  /* Now update the edges in the CFG.  When splitting edges, we do not 
     want to remove PHI arguments.  */
  if (splitting)
    redirect_edge_succ (e, dest);
  else
    {
      e = ssa_redirect_edge (e, dest);
      e->flags |= flags;
    }

  return e;
}

static edge
tree_redirect_edge_and_branch (edge e, basic_block dest)
{
  return tree_redirect_edge_and_branch_1 (e, dest, false);
}

/* Simple wrapper as we always can redirect fallthru edges.  */

static basic_block
tree_redirect_edge_and_branch_force (edge e, basic_block dest)
{
  e = tree_redirect_edge_and_branch (e, dest);
  if (!e)
    abort ();

  return NULL;
}

/* Dump FUNCTION_DECL FN to file FILE using FLAGS (see TDF_* in tree.h)  */

void
dump_function_to_file (tree fn, FILE *file, int flags)
{
  tree arg, vars, var;
  bool ignore_topmost_bind = false, any_var = false;
  basic_block bb;
  tree chain;

  fprintf (file, "\n;; Function %s",
	    (*lang_hooks.decl_printable_name) (fn, 2));
  fprintf (file, " (%s)\n",
	    IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (fn)));
  fprintf (file, "\n");

  fprintf (file, "%s (", (*lang_hooks.decl_printable_name) (fn, 2));

  arg = DECL_ARGUMENTS (fn);
  while (arg)
    {
      print_generic_expr (file, arg, 0);
      if (TREE_CHAIN (arg))
	fprintf (file, ", ");
      arg = TREE_CHAIN (arg);
    }
  fprintf (file, ")\n");

  if (flags & TDF_RAW)
    {
      dump_node (fn, TDF_SLIM | flags, file);
      return;
    }

  /* When gimple is lowered, the variables are no longer available in the
     bind_exprs, so display them separately.  */
  if (cfun && cfun->unexpanded_var_list)
    {
      ignore_topmost_bind = true;

      fprintf (file, "{\n");
      for (vars = cfun->unexpanded_var_list; vars; vars = TREE_CHAIN (vars))
	{
	  var = TREE_VALUE (vars);

	  print_generic_decl (file, var, flags);
	  fprintf (file, "\n");

	  any_var = true;
	}
    }

  if (basic_block_info)
    {
      /* Make a cfg based dump.  */
      if (!ignore_topmost_bind)
	fprintf (file, "{\n");

      if (any_var && n_basic_blocks)
	fprintf (file, "\n");

      FOR_EACH_BB (bb)
	{
	  dump_generic_bb (file, bb, 2, flags);
	}
	
      fprintf (file, "}\n");
    }
  else
    {
      int indent;

      /* Make a tree based dump.  */
      chain = DECL_SAVED_TREE (fn);

      if (TREE_CODE (chain) == BIND_EXPR)
	{
	  if (ignore_topmost_bind)
	    {
	      chain = BIND_EXPR_BODY (chain);
	      indent = 2;
	    }
	  else
	    indent = 0;
	}
      else
	{
	  if (!ignore_topmost_bind)
	    fprintf (file, "{\n");
	  indent = 2;
	}

      if (any_var)
	fprintf (file, "\n");

      print_generic_stmt_indented (file, chain, flags, indent);
      if (ignore_topmost_bind)
	fprintf (file, "}\n");
    }

  fprintf (file, "\n\n");
}

/* FIXME These need to be filled in with appropriate pointers.  But this
   implies an ABI change in some functions.  */
struct cfg_hooks tree_cfg_hooks = {
  tree_verify_flow_info,
  tree_dump_bb,			/* dump_bb  */
  NULL,				/* create_basic_block  */
  tree_redirect_edge_and_branch,/* redirect_edge_and_branch  */
  tree_redirect_edge_and_branch_force,/* redirect_edge_and_branch_force  */
  NULL,				/* delete_basic_block  */
  NULL,				/* split_block  */
  NULL,				/* can_merge_blocks_p  */
  NULL,				/* merge_blocks  */
  tree_split_edge,		/* cfgh_split_edge  */
  tree_make_forwarder_block,	/* cfgh_make_forward_block  */
  tree_loop_optimizer_init,     /* cfgh_loop_optimizer_init  */
  tree_loop_optimizer_finalize  /* cfgh_loop_optimizer_finalize  */
};
