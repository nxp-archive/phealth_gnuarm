/* Control and data flow functions for trees.
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
#include "expr.h"
#include "diagnostic.h"
#include "basic-block.h"
#include "flags.h"
#include "tree-flow.h"
#include "tree-dump.h"
#include "timevar.h"
#include "function.h"
#include "langhooks.h"
#include "toplev.h"
#include "flags.h"
#include "cgraph.h"
#include "tree-inline.h"
#include "tree-mudflap.h"
#include "ggc.h"
#include "cgraph.h"

static void tree_ssa_finish (tree *);

/* Rewrite a function tree to the SSA form and perform the SSA-based
   optimizations on it.  */

/* Main entry point to the tree SSA transformation routines.  FNDECL is the
   FUNCTION_DECL node for the function to optimize.  */

static void
optimize_function_tree (tree fndecl, tree *chain)
{
  /* Don't bother doing anything if the program has errors.  */
  if (errorcount || sorrycount)
    return;

  /* Build the flowgraph.  */
  init_flow ();

  build_tree_cfg (chain);

  /* Begin analysis and optimization passes.  After the function is
     initially renamed into SSA form, passes are responsible from keeping
     it in SSA form.  If a pass exposes new symbols or invalidates the SSA
     numbering for existing variables, it should add them to the
     VARS_TO_RENAME bitmap and call rewrite_into_ssa() afterwards.  */
  if (n_basic_blocks > 0)
    {
      bitmap vars_to_rename;

#ifdef ENABLE_CHECKING
      verify_stmts ();
#endif

      /* Initialize common SSA structures.  */
      init_tree_ssa ();

      /* Find all the variables referenced in the function.  */
      find_referenced_vars (fndecl);

      /* Compute aliasing information for all the variables referenced in
	 the function.  */
      compute_may_aliases (fndecl);

      /*			BEGIN SSA PASSES

	 IMPORTANT: If you change the order in which these passes are
		    executed, you also need to change the enum
		    TREE_DUMP_INDEX in tree.h and DUMP_FILES in
                    tree-dump.c.  */


      /* Rewrite the function into SSA form.  Initially, request all
	 variables to be renamed.  */
      rewrite_into_ssa (fndecl, NULL, TDI_ssa_1);

#ifdef ENABLE_CHECKING
      verify_ssa ();
#endif

      /* Set up VARS_TO_RENAME to allow passes to inform which variables
	 need to be renamed.  */
      vars_to_rename = BITMAP_XMALLOC ();

      /* Perform dominator optimizations.  */
      if (flag_tree_dom)
	{
	  bitmap_clear (vars_to_rename);
	  tree_ssa_dominator_optimize (fndecl, vars_to_rename, TDI_dom_1);

	  /* If the dominator optimizations exposed new variables, we need
	      to repeat the SSA renaming process for those symbols.  */
	  if (bitmap_first_set_bit (vars_to_rename) >= 0)
	    rewrite_into_ssa (fndecl, vars_to_rename, TDI_ssa_2);

#ifdef ENABLE_CHECKING
	  verify_ssa ();
#endif
	}

      /* Do a first DCE pass prior to must-alias.  This pass will remove
	 dead pointer assignments taking the address of local variables.  */
      if (flag_tree_dce)
	tree_ssa_dce (fndecl, TDI_dce_1);

      ggc_collect ();

#ifdef ENABLE_CHECKING
      verify_ssa ();
#endif

      if (flag_tree_loop)
	{
	  tree_ssa_loop_opt (fndecl, TDI_loop);

#ifdef ENABLE_CHECKING
	  verify_ssa ();
#endif
	}

      /* The must-alias pass removes the aliasing and addressability bits
	 from variables that used to have their address taken.  */
      if (flag_tree_must_alias)
	{
	  bitmap_clear (vars_to_rename);
	  tree_compute_must_alias (fndecl, vars_to_rename, TDI_mustalias);

	  /* Run the SSA pass again if we need to rename new variables.  */
	  if (bitmap_first_set_bit (vars_to_rename) >= 0)
	    rewrite_into_ssa (fndecl, vars_to_rename, TDI_ssa_3);
          ggc_collect ();

#ifdef ENABLE_CHECKING
	  verify_ssa ();
#endif
	}

      /* Eliminate tail recursion calls.  */
      tree_optimize_tail_calls (false, TDI_tail1);

#ifdef ENABLE_CHECKING
      verify_ssa ();
#endif

      /* Scalarize some structure references.  */
      if (flag_tree_sra)
	{
	  bitmap_clear (vars_to_rename);
	  tree_sra (fndecl, vars_to_rename, TDI_sra);

	  /* Run the SSA pass again if we need to rename new variables.  */
	  if (bitmap_first_set_bit (vars_to_rename) >= 0)
	    rewrite_into_ssa (fndecl, vars_to_rename, TDI_ssa_4);
          ggc_collect ();

#ifdef ENABLE_CHECKING
	  verify_ssa ();
#endif
	}

      /* Run SCCP (Sparse Conditional Constant Propagation).  */
      if (flag_tree_ccp)
	{
	  bitmap_clear (vars_to_rename);
	  tree_ssa_ccp (fndecl, vars_to_rename, TDI_ccp);

	  /* Run the SSA pass again if we need to rename new variables.  */
	  if (bitmap_first_set_bit (vars_to_rename) >= 0)
	    rewrite_into_ssa (fndecl, vars_to_rename, TDI_ssa_5);
          ggc_collect ();

#ifdef ENABLE_CHECKING
	  verify_ssa ();
#endif
	}

      /* Run SSA-PRE (Partial Redundancy Elimination).  */
      if (flag_tree_pre)
	{
	  tree_perform_ssapre (fndecl, TDI_pre);
	  ggc_collect ();

#ifdef ENABLE_CHECKING
	  verify_ssa ();
#endif
	}

      /* Perform a second pass of dominator optimizations.  */
      if (flag_tree_dom)
	{
	  bitmap_clear (vars_to_rename);
	  tree_ssa_dominator_optimize (fndecl, vars_to_rename, TDI_dom_2);

	  /* Run the SSA pass again if we need to rename new variables.  */
	  if (bitmap_first_set_bit (vars_to_rename) >= 0)
	    rewrite_into_ssa (fndecl, vars_to_rename, TDI_ssa_6);

#ifdef ENABLE_CHECKING
	  verify_ssa ();
#endif
	}

      /* Do a second DCE pass.  */
      if (flag_tree_dce)
	{
	  tree_ssa_dce (fndecl, TDI_dce_2);
	  ggc_collect ();

#ifdef ENABLE_CHECKING
	  verify_ssa ();
#endif
	}

      /* Eliminate tail recursion calls and discover sibling calls.  */
      tree_optimize_tail_calls (true, TDI_tail2);

#ifdef ENABLE_CHECKING
      verify_ssa ();
#endif

#ifdef ENABLE_CHECKING
      verify_flow_info ();
      verify_stmts ();
      verify_ssa ();
#endif

      /* Rewrite the function out of SSA form.  */
      rewrite_out_of_ssa (fndecl, TDI_optimized);
      ggc_collect ();

      /* Flush out flow graph and SSA data.  */
      BITMAP_XFREE (vars_to_rename);
  
      free_dominance_info (CDI_DOMINATORS);
    }

  tree_ssa_finish (chain);
}

/* Do the actions required to finish with tree-ssa optimization
   passes.  Return the final chain of statements in CHAIN.  */

static void
tree_ssa_finish (tree *chain)
{
  basic_block bb;

  /* Emit gotos for implicit jumps.  */
  disband_implicit_edges ();

  /* Remove the ssa structures.  Do it here since this includes statement
     annotations that need to be intact during disband_implicit_edges.  */
  delete_tree_ssa ();

  /* Re-chain the statements from the blocks.  */
  *chain = alloc_stmt_list ();
  FOR_EACH_BB (bb)
    {
      append_to_statement_list_force (bb->stmt_list, chain);
    }

  /* And get rid of the cfg.  */
  delete_tree_cfg ();
}

/* Called to move the SAVE_EXPRs for parameter declarations in a
   nested function into the nested function.  DATA is really the
   nested FUNCTION_DECL.  */

static tree
set_save_expr_context (tree *tp,
		       int *walk_subtrees,
		       void *data)
{
  if (TREE_CODE (*tp) == SAVE_EXPR && !SAVE_EXPR_CONTEXT (*tp))
    SAVE_EXPR_CONTEXT (*tp) = (tree) data;
  /* Do not walk back into the SAVE_EXPR_CONTEXT; that will cause
     circularity.  */
  else if (DECL_P (*tp))
    *walk_subtrees = 0;

  return NULL;
}

/* For functions-as-trees languages, this performs all optimization and
   compilation for FNDECL.  */

void
tree_rest_of_compilation (tree fndecl, bool nested_p)
{
  location_t saved_loc;
  struct cgraph_node *saved_node = NULL, *node;

  timevar_push (TV_EXPAND);

  if (flag_unit_at_a_time && !cgraph_global_info_ready)
    abort ();

  /* Initialize the RTL code for the function.  */
  current_function_decl = fndecl;
  saved_loc = input_location;
  input_location = DECL_SOURCE_LOCATION (fndecl);
  init_function_start (fndecl);

  /* This function is being processed in whole-function mode.  */
  cfun->x_whole_function_mode_p = 1;

  /* Even though we're inside a function body, we still don't want to
     call expand_expr to calculate the size of a variable-sized array.
     We haven't necessarily assigned RTL to all variables yet, so it's
     not safe to try to expand expressions involving them.  */
  immediate_size_expand = 0;
  cfun->x_dont_save_pending_sizes_p = 1;

  node = cgraph_node (fndecl);

  /* We might need the body of this function so that we can expand
     it inline somewhere else.  This means not lowering some constructs
     such as exception handling.  */
  if (cgraph_preserve_function_body_p (fndecl))
    {
      if (!flag_unit_at_a_time)
	{
	  struct cgraph_edge *e;

	  saved_node = cgraph_clone_node (node);
	  for (e = saved_node->callees; e; e = e->next_callee)
	    if (!e->inline_failed)
	      {
		e->inline_failed = "";
		cgraph_mark_inline_edge (e);
	      }
	}
      cfun->saved_tree = save_body (fndecl, &cfun->saved_args);
    }

  if (flag_inline_trees)
    {
      struct cgraph_edge *e;
      for (e = node->callees; e; e = e->next_callee)
	if (!e->inline_failed || warn_inline)
	  break;
      if (e)
	{
	  timevar_push (TV_INTEGRATION);
	  optimize_inline_calls (fndecl);
	  timevar_pop (TV_INTEGRATION);
	}
    }

  /* If the function has not already been gimplified, do so now.  */
  if (!lang_hooks.gimple_before_inlining)
    gimplify_function_tree (fndecl);

  /* Debugging dump after gimplification.  */
  dump_function (TDI_gimple, fndecl);

  /* Run a pass over the statements deleting any obviously useless
     statements before we build the CFG.  */
  remove_useless_stmts (&DECL_SAVED_TREE (fndecl));
  dump_function (TDI_useless, fndecl);

  /* Mudflap-instrument any relevant declarations.  */
  if (flag_mudflap)
    mudflap_c_function_decls (fndecl);

  /* Lower the structured statements.  */
  lower_function_body (&DECL_SAVED_TREE (fndecl));

  /* Avoid producing notes for blocks.  */
  cfun->dont_emit_block_notes = 1;
  reset_block_changes ();

  dump_function (TDI_lower, fndecl);

  /* Run a pass to lower magic exception handling constructs into,
     well, less magic though not completely mundane constructs.  */
  lower_eh_constructs (&DECL_SAVED_TREE (fndecl));

  /* Mudflap-instrument any relevant operations.  */
  if (flag_mudflap)
    {
      /* Invoke the SSA tree optimizer.  */
      if (optimize >= 1 && !flag_disable_tree_ssa)
	{
	  /* We cannot allow unssa to un-gimplify trees before we
	     instrument them.  */
	  int save_ter = flag_tree_ter;
	  flag_tree_ter = 0;
          optimize_function_tree (fndecl, &DECL_SAVED_TREE (fndecl));
	  flag_tree_ter = save_ter;
	}

      mudflap_c_function_ops (fndecl);

      /* WIP: set -O4 to re-do optimizations on the mudflapified code.
	 Should work, but perhaps needs more thought.  */
      if (optimize >= 4 && !flag_disable_tree_ssa)
	optimize_function_tree (fndecl, &DECL_SAVED_TREE (fndecl));
    }
  else
    {
      /* Invoke the SSA tree optimizer.  */
      if (optimize >= 1 && !flag_disable_tree_ssa)
	optimize_function_tree (fndecl, &DECL_SAVED_TREE (fndecl));
    }

  DECL_SAVED_TREE (fndecl) = build (BIND_EXPR, void_type_node, NULL_TREE,
				    DECL_SAVED_TREE (fndecl), NULL_TREE);

  /* If the function has a variably modified type, there may be
     SAVE_EXPRs in the parameter types.  Their context must be set to
     refer to this function; they cannot be expanded in the containing
     function.  */
  if (decl_function_context (fndecl) == current_function_decl
      && variably_modified_type_p (TREE_TYPE (fndecl)))
    walk_tree (&TREE_TYPE (fndecl), set_save_expr_context, fndecl,
	       NULL);

  /* Set up parameters and prepare for return, for the function.  */
  expand_function_start (fndecl, 0);

  /* Expand the variables recorded during gimple lowering.  */
  expand_used_vars ();

  /* Allow language dialects to perform special processing.  */
  (*lang_hooks.rtl_expand.start) ();

  /* If this function is `main', emit a call to `__main'
     to run global initializers, etc.  */
  if (DECL_NAME (fndecl)
      && MAIN_NAME_P (DECL_NAME (fndecl))
      && DECL_FILE_SCOPE_P (fndecl))
    expand_main_function ();

  /* Generate the RTL for this function.  */
  (*lang_hooks.rtl_expand.stmt) (DECL_SAVED_TREE (fndecl));

  /* We hard-wired immediate_size_expand to zero above.
     expand_function_end will decrement this variable.  So, we set the
     variable to one here, so that after the decrement it will remain
     zero.  */
  immediate_size_expand = 1;

  /* Make sure the locus is set to the end of the function, so that 
     epilogue line numbers and warnings are set properly.  */
  if (cfun->function_end_locus.file)
    input_location = cfun->function_end_locus;

  /* The following insns belong to the top scope.  */
  record_block_change (DECL_INITIAL (current_function_decl));
  
  /* Allow language dialects to perform special processing.  */
  (*lang_hooks.rtl_expand.end) ();

  /* Generate rtl for function exit.  */
  expand_function_end ();

  /* If this is a nested function, protect the local variables in the stack
     above us from being collected while we're compiling this function.  */
  if (nested_p)
    ggc_push_context ();

  /* There's no need to defer outputting this function any more; we
     know we want to output it.  */
  DECL_DEFER_OUTPUT (fndecl) = 0;

  /* Run the optimizers and output the assembler code for this function.  */
  rest_of_compilation (fndecl);

  /* Restore original body if still needed.  */
  if (cfun->saved_tree)
    {
      DECL_SAVED_TREE (fndecl) = cfun->saved_tree;
      DECL_ARGUMENTS (fndecl) = cfun->saved_args;

      /* When not in unit-at-a-time mode, we must preserve out of line copy
	 representing node before inlining.  Restore original ougoing edges
	 using clone we created earlier.  */
      if (!flag_unit_at_a_time)
	{
	  struct cgraph_edge *e;
	  while (node->callees)
	    cgraph_remove_edge (node->callees);
	  node->callees = saved_node->callees;
	  saved_node->callees = NULL;
	  for (e = saved_node->callees; e; e = e->next_callee)
	    e->caller = node;
	  cgraph_remove_node (saved_node);
	}
    }
  else
    DECL_SAVED_TREE (fndecl) = NULL;
  cfun = 0;
  DECL_SAVED_INSNS (fndecl) = 0;

  /* If requested, warn about function definitions where the function will
     return a value (usually of some struct or union type) which itself will
     take up a lot of stack space.  */
  if (warn_larger_than && !DECL_EXTERNAL (fndecl) && TREE_TYPE (fndecl))
    {
      tree ret_type = TREE_TYPE (TREE_TYPE (fndecl));

      if (ret_type && TYPE_SIZE_UNIT (ret_type)
	  && TREE_CODE (TYPE_SIZE_UNIT (ret_type)) == INTEGER_CST
	  && 0 < compare_tree_int (TYPE_SIZE_UNIT (ret_type),
				   larger_than_size))
	{
	  unsigned int size_as_int
	    = TREE_INT_CST_LOW (TYPE_SIZE_UNIT (ret_type));

	  if (compare_tree_int (TYPE_SIZE_UNIT (ret_type), size_as_int) == 0)
	    warning ("%Jsize of return value of '%D' is %u bytes",
                     fndecl, fndecl, size_as_int);
	  else
	    warning ("%Jsize of return value of '%D' is larger than %wd bytes",
                     fndecl, fndecl, larger_than_size);
	}
    }

  if (!nested_p && !flag_inline_trees)
    {
      /* Stop pointing to the local nodes about to be freed.
	 But DECL_INITIAL must remain nonzero so we know this
	 was an actual function definition.
	 For a nested function, this is done in c_pop_function_context.
	 If rest_of_compilation set this to 0, leave it 0.  */
      if (DECL_INITIAL (fndecl) != 0)
	DECL_INITIAL (fndecl) = error_mark_node;

      DECL_ARGUMENTS (fndecl) = 0;
    }

  input_location = saved_loc;

  ggc_collect ();

  /* Undo the GC context switch.  */
  if (nested_p)
    ggc_pop_context ();
  timevar_pop (TV_EXPAND);
}
