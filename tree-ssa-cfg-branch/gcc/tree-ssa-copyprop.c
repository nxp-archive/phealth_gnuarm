/* SSA-Copy propagation for the GNU compiler.
   Copyright (C) 2003 Free Software Foundation, Inc.
   Contributed by Aldy Hernandez <aldy@quesejoda.com>
   and Diego Novillo <dnovillo@redhat.com>
   
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

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "errors.h"
#include "ggc.h"
#include "tree.h"
#include "rtl.h"
#include "tm_p.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "timevar.h"
#include "diagnostic.h"
#include "tree-flow.h"
#include "tree-simple.h"
#include "tree-dump.h"

/* Local variables.  */
static FILE *dump_file;
static int dump_flags;

/* Local functions.  */
static void copyprop_stmt (tree);
static void copyprop_phi (tree);
static inline tree get_original (tree);
static struct block_tree *get_common_scope (struct block_tree *,
					    struct block_tree *);
static void move_var_to_scope (tree, struct block_tree *);


/* Main entry point to the copy propagator.  The algorithm is a simple
   linear scan of the flowgraph.  For every variable X_i used in the
   function, it retrieves its unique reaching definition.  If X_i's
   definition is a copy (i.e., X_i = Y_j), then X_i is replaced with Y_j.  */

void
tree_ssa_copyprop (tree fndecl)
{
  basic_block bb;

  timevar_push (TV_TREE_COPYPROP);
  dump_file = dump_begin (TDI_copyprop, &dump_flags);

  /* Traverse every block in the flowgraph propagating copies in each
     statement.  */
  FOR_EACH_BB (bb)
    {
      block_stmt_iterator si;
      tree phi;

      for (phi = phi_nodes (bb); phi; phi = TREE_CHAIN (phi))
	copyprop_phi (phi);

      for (si = bsi_start (bb); !bsi_end_p (si); bsi_next (&si))
	copyprop_stmt (bsi_stmt (si));
    }

  if (dump_file)
    {
      dump_cfg_function_to_file (fndecl, dump_file, dump_flags);
      dump_end (TDI_copyprop, dump_file);
    }

  timevar_pop (TV_TREE_COPYPROP);
}


/* Propagate copies in statement STMT.  If operand X_i in STMT is defined
   by a statement of the form X_i = Y_j, replace the use of X_i with Y_j.  */

static void
copyprop_stmt (tree stmt)
{
  varray_type uses;
  size_t i;
  bool modified;
  basic_block bb = bb_for_stmt (stmt);

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "\nPropagating in statement: ");
      print_generic_expr (dump_file, stmt, TDF_SLIM);
      fprintf (dump_file, "\n");
    }

  get_stmt_operands (stmt);
  modified = false;

  /* Propagate real uses.  */
  uses = use_ops (stmt);
  for (i = 0; uses && i < VARRAY_ACTIVE_SIZE (uses); i++)
    {
      tree *use_p = (tree *) VARRAY_GENERIC_PTR (uses, i);
      tree orig = get_original (*use_p);

      if (orig && may_propagate_copy (*use_p, orig))
	{
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    {
	      fprintf (dump_file, "\tReplacing ");
	      print_generic_expr (dump_file, *use_p, 0);
	      fprintf (dump_file, " with ");
	      print_generic_expr (dump_file, orig, 0);
	      fprintf (dump_file, "\n");
	    }

	  propagate_copy (bb, use_p, orig);
	  modified = true;
	}
    }

  if (modified)
    modify_stmt (stmt);
}


/* Propagate copies inside PHI node PHI.  If argument X_i of PHI comes from
   a definition of the form X_i = Y_j, replace it with Y_j.  */

static void
copyprop_phi (tree phi)
{
  int i;

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "\nPropagating in PHI node: ");
      print_generic_expr (dump_file, phi, 0);
      fprintf (dump_file, "\n");
    }

  for (i = 0; i < PHI_NUM_ARGS (phi); i++)
    {
      tree arg = PHI_ARG_DEF (phi, i);
      tree orig;

      if (TREE_CODE (arg) != SSA_NAME)
	continue;

      orig = get_original (arg);
      if (orig && may_propagate_copy (arg, orig))
	{
	  if (dump_file && dump_flags & TDF_DETAILS)
	    {
	      fprintf (dump_file, "\tReplacing ");
	      print_generic_expr (dump_file, arg, 0);
	      fprintf (dump_file, " with ");
	      print_generic_expr (dump_file, orig, 0);
	      fprintf (dump_file, "\n");
	    }

	  PHI_ARG_DEF (phi, i) = orig;
	}
    }
}


/* If the unique definition for VAR comes from an assignment of the form
   VAR = ORIG, return ORIG.  Otherwise, return NULL.  */

static inline tree
get_original (tree var)
{
  tree def_stmt;

  def_stmt = SSA_NAME_DEF_STMT (var);

  /* If VAR is not the LHS of its defining statement, it means that VAR is
     defined by a VDEF node.  This implies aliasing or structure updates.
     For instance,

     		# a_2 = VDEF <a_1>
     		a.b = tmp_3;
		return a_2;

     If we allow tmp_3 to propagate into the 'return' statement, we would
     be changing the return type of the function.  */
  if (TREE_CODE (def_stmt) == MODIFY_EXPR
      && TREE_OPERAND (def_stmt, 0) == var
      && TREE_CODE (TREE_OPERAND (def_stmt, 1)) == SSA_NAME)
    return TREE_OPERAND (def_stmt, 1);

  return NULL_TREE;
}


/* Replace the operand pointed to by OP_P with variable VAR.  If *OP_P is a
   pointer, copy the memory tag used originally by *OP_P into VAR.  This is
   needed in cases where VAR had never been dereferenced in the program.
   The propagation occurs in basic block BB.  */
   
void
propagate_copy (basic_block bb, tree *op_p, tree var)
{
#if defined ENABLE_CHECKING
  if (!may_propagate_copy (*op_p, var))
    abort ();
#endif

  /* If VAR doesn't have a memory tag, copy the one from the original
     operand.  */
  if (POINTER_TYPE_P (TREE_TYPE (*op_p)))
    {
      var_ann_t new_ann = var_ann (SSA_NAME_VAR (var));
      var_ann_t orig_ann = var_ann (SSA_NAME_VAR (*op_p));
      if (new_ann->mem_tag == NULL_TREE)
	new_ann->mem_tag = orig_ann->mem_tag;
    }

  *op_p = var;

  fixup_var_scope (bb, var);
}

/* Moves variable VAR high enough in scope tree so that basic block BB is in
   scope.  */
void
fixup_var_scope (basic_block bb, tree var)
{
  struct block_tree *scope, *old_scope;

  /* Update scope of var.  */
  old_scope = var_ann (SSA_NAME_VAR (var))->scope;
  if (old_scope)
    {
      scope = get_common_scope (bb_ann (bb)->block, old_scope);
      if (scope != old_scope)
	move_var_to_scope (SSA_NAME_VAR (var), scope);
    }
}

/* Finds common scope for S1 and S2.  */
static struct block_tree *
get_common_scope (struct block_tree *s1, struct block_tree *s2)
{
  struct block_tree *tmp;

  if (s1->level > s2->level)
    {
      tmp = s1;
      s1 = s2;
      s2 = tmp;
    }

  while (s1->level < s2->level)
    s2 = s2->outer;

  while (s1 != s2)
    {
      s1 = s1->outer;
      s2 = s2->outer;
    }

  while (s1->type != BT_BIND)
    s1 = s1->outer;

  return s1;
}

/* Moves variable VAR to a SCOPE.  */
static void
move_var_to_scope (tree var, struct block_tree *scope)
{
  struct block_tree *old_scope = var_ann (var)->scope;
  tree avar, prev;
  tree block = BIND_EXPR_BLOCK (old_scope->bind);

  prev = NULL_TREE;
  for (avar = BIND_EXPR_VARS (old_scope->bind);
       avar;
       prev = avar, avar = TREE_CHAIN (avar))
    if (avar == var)
      break;
  if (!avar)
    abort ();

  if (block)
    remove_decl (avar, block);
  else
    remove_decl (avar, DECL_INITIAL (current_function_decl));

  if (prev)
    TREE_CHAIN (prev) = TREE_CHAIN (avar);
  else
    BIND_EXPR_VARS (old_scope->bind) = TREE_CHAIN (avar);

  TREE_CHAIN (var) = BIND_EXPR_VARS (scope->bind);
  BIND_EXPR_VARS (scope->bind) = var;
  var_ann (var)->scope = scope;

  DECL_ABSTRACT_ORIGIN (var) = NULL_TREE;
}
