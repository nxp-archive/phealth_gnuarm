/* Compilation driver using callgraph datastructure
   Copyright (C) 2003, 2004 Free Software Foundation, Inc.
   Contributed by Jan Hubicka

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
Software Foundation, 59 Temple Place - Suite 330, Boston, MA
02111-1307, USA.  */

/* This module implements main driver of compilation process as well as
   few basic intraprocedural optimizers.

   The main scope of this file is to act as an interface in between
   tree based frontends and the backend (and middle end)

   The front-end is supposed to use following functionality:

    - cgraph_finalize_function

      This function is called once front-end has parsed whole body of function
      and it is certain that the function body nor the declaration will change.

      (There is one exception needed for implementing GCC extern inline function.)

    - cgraph_varpool_finalize_variable

      This function has same behavior as the above but is used for static
      variables.

    - cgraph_finalize_compilation_unit

      This function is called once compilation unit is finalized and it will
      no longer change.

      In the unit-at-a-time the call-graph construction and local function
      analysis takes place here.  Bodies of unreachable functions are released
      to conserve memory usage.

      ???  The compilation unit in this point of view should be compilation
      unit as defined by the language - for instance C frontend allows multiple
      compilation units to be parsed at once and it should call function each
      time parsing is done so we save memory.

    - cgraph_optimize

      In this unit-at-a-time compilation the intra procedural analysis takes
      place here.  In particular the static functions whose address is never
      taken are marked as local.  Backend can then use this information to
      modify calling conventions, do better inlining or similar optimizations.

    - cgraph_assemble_pending_functions
    - cgraph_varpool_assemble_pending_variables

      In non-unit-at-a-time mode these functions can be used to force compilation
      of functions or variables that are known to be needed at given stage
      of compilation

    - cgraph_mark_needed_node
    - cgraph_varpool_mark_needed_node

      When function or variable is referenced by some hidden way (for instance
      via assembly code and marked by attribute "used"), the call-graph data structure
      must be updated accordingly by this function.

    - analyze_expr callback

      This function is responsible for lowering tree nodes not understood by
      generic code into understandable ones or alternatively marking
      callgraph and varpool nodes referenced by the as needed.

      ??? On the tree-ssa genericizing should take place here and we will avoid
      need for these hooks (replacing them by genericizing hook)

    - expand_function callback

      This function is used to expand function and pass it into RTL back-end.
      Front-end should not make any assumptions about when this function can be
      called.  In particular cgraph_assemble_pending_functions,
      cgraph_varpool_assemble_pending_variables, cgraph_finalize_function,
      cgraph_varpool_finalize_function, cgraph_optimize can cause arbitrarily
      previously finalized functions to be expanded.

    We implement two compilation modes.

      - unit-at-a-time:  In this mode analyzing of all functions is deferred
	to cgraph_finalize_compilation_unit and expansion into cgraph_optimize.

	In cgraph_finalize_compilation_unit the reachable functions are
	analyzed.  During analysis the call-graph edges from reachable
	functions are constructed and their destinations are marked as
	reachable.  References to functions and variables are discovered too
	and variables found to be needed output to the assembly file.  Via
	mark_referenced call in assemble_variable functions referenced by
	static variables are noticed too.

	The intra-procedural information is produced and its existence
	indicated by global_info_ready.  Once this flag is set it is impossible
	to change function from !reachable to reachable and thus
	assemble_variable no longer call mark_referenced.

	Finally the call-graph is topologically sorted and all reachable functions
	that has not been completely inlined or are not external are output.

	??? It is possible that reference to function or variable is optimized
	out.  We can not deal with this nicely because topological order is not
	suitable for it.  For tree-ssa we may consider another pass doing
	optimization and re-discovering reachable functions.

	??? Reorganize code so variables are output very last and only if they
	really has been referenced by produced code, so we catch more cases
	where reference has been optimized out.

      - non-unit-at-a-time

	All functions are variables are output as early as possible to conserve
	memory consumption.  This may or may not result in less memory used but
	it is still needed for some legacy code that rely on particular ordering
	of things output from the compiler.

	Varpool data structures are not used and variables are output directly.

	Functions are output early using call of
	cgraph_assemble_pending_function from cgraph_finalize_function.  The
	decision on whether function is needed is made more conservative so
	uninlininable static functions are needed too.  During the call-graph
	construction the edge destinations are not marked as reachable and it
	is completely relied upn assemble_variable to mark them.  */


#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "rtl.h"
#include "tree-flow.h"
#include "tree-inline.h"
#include "langhooks.h"
#include "pointer-set.h"
#include "toplev.h"
#include "flags.h"
#include "ggc.h"
#include "debug.h"
#include "target.h"
#include "basic-block.h"
#include "tree-iterator.h"
#include "cgraph.h"
#include "diagnostic.h"
#include "timevar.h"
#include "params.h"
#include "c-common.h"
#include "intl.h"
#include "function.h"
#include "ipa_prop.h"
#include "tree-gimple.h"
#include "output.h"
#include "tree-pass.h"
#include "cfgloop.h"

static void cgraph_expand_all_functions (void);
static void cgraph_mark_functions_to_output (void);
static void cgraph_expand_function (struct cgraph_node *);
static tree record_call_1 (tree *, int *, void *);
static void cgraph_analyze_function (struct cgraph_node *node);

/* Records tree nodes seen in cgraph_create_edges.  Simply using
   walk_tree_without_duplicates doesn't guarantee each node is visited
   once because it gets a new htab upon each recursive call from
   record_calls_1.  */
static struct pointer_set_t *visited_nodes;

static FILE *cgraph_dump_file;

/* Determine if function DECL is needed.  That is, visible to something
   either outside this translation unit, something magic in the system
   configury, or (if not doing unit-at-a-time) to something we havn't
   seen yet.  */

static bool
decide_is_function_needed (struct cgraph_node *node, tree decl)
{
  tree origin;
  if (MAIN_NAME_P (DECL_NAME (decl))
      && TREE_PUBLIC (decl))
    {
      node->local.externally_visible = true;
      return true;
    }

  /* If the user told us it is used, then it must be so.  */
  if (lookup_attribute ("used", DECL_ATTRIBUTES (decl)))
    {
      if (TREE_PUBLIC (decl))
        node->local.externally_visible = true;
      return true;
    }

  /* ??? If the assembler name is set by hand, it is possible to assemble
     the name later after finalizing the function and the fact is noticed
     in assemble_name then.  This is arguably a bug.  */
  if (DECL_ASSEMBLER_NAME_SET_P (decl)
      && TREE_SYMBOL_REFERENCED (DECL_ASSEMBLER_NAME (decl)))
    {
      if (TREE_PUBLIC (decl))
        node->local.externally_visible = true;
      return true;
    }

  /* If we decided it was needed before, but at the time we didn't have
     the body of the function available, then it's still needed.  We have
     to go back and re-check its dependencies now.  */
  if (node->needed)
    return true;

  /* Externally visible functions must be output.  The exception is
     COMDAT functions that must be output only when they are needed.  */
  if ((TREE_PUBLIC (decl) && !flag_whole_program)
      && !DECL_COMDAT (decl) && !DECL_EXTERNAL (decl))
    return true;

  /* Constructors and destructors are reachable from the runtime by
     some mechanism.  */
  if (DECL_STATIC_CONSTRUCTOR (decl) || DECL_STATIC_DESTRUCTOR (decl))
    return true;

  if (flag_unit_at_a_time)
    return false;

  /* If not doing unit at a time, then we'll only defer this function
     if its marked for inlining.  Otherwise we want to emit it now.  */

  /* "extern inline" functions are never output locally.  */
  if (DECL_EXTERNAL (decl))
    return false;
  /* Nested functions of extern inline function shall not be emit unless
     we inlined the origin.  */
  for (origin = decl_function_context (decl); origin;
       origin = decl_function_context (origin))
    if (DECL_EXTERNAL (origin))
      return false;
  /* We want to emit COMDAT functions only when absolutely necessary.  */
  if (DECL_COMDAT (decl))
    return false;
  if (!DECL_INLINE (decl)
      || (!node->local.disregard_inline_limits
	  /* When declared inline, defer even the uninlinable functions.
	     This allows them to be eliminated when unused.  */
	  && !DECL_DECLARED_INLINE_P (decl) 
	  && (!node->local.inlinable || !cgraph_default_inline_p (node))))
    return true;

  return false;
}

/* When not doing unit-at-a-time, output all functions enqueued.
   Return true when such a functions were found.  */

bool
cgraph_assemble_pending_functions (void)
{
  bool output = false;

  if (flag_unit_at_a_time)
    return false;

  while (cgraph_nodes_queue)
    {
      struct cgraph_node *n = cgraph_nodes_queue;

      cgraph_nodes_queue = cgraph_nodes_queue->next_needed;
      n->next_needed = NULL;
      if (!n->global.inlined_to && !DECL_EXTERNAL (n->decl))
	{
	  cgraph_expand_function (n);
	  output = true;
	}
    }

  return output;
}

/* As an GCC extension we allow redefinition of the function.  The
   semantics when both copies of bodies differ is not well defined.
   We replace the old body with new body so in unit at a time mode
   we always use new body, while in normal mode we may end up with
   old body inlined into some functions and new body expanded and
   inlined in others.

   ??? It may make more sense to use one body for inlining and other
   body for expanding the function but this is difficult to do.  */

static void
cgraph_reset_node (struct cgraph_node *node)
{
  /* If node->output is set, then this is a unit-at-a-time compilation
     and we have already begun whole-unit analysis.  This is *not*
     testing for whether we've already emitted the function.  That
     case can be sort-of legitimately seen with real function 
     redefinition errors.  I would argue that the front end should
     never present us with such a case, but don't enforce that for now.  */
  gcc_assert (!node->output);

  /* Reset our data structures so we can analyze the function again.  */
  memset (&node->local, 0, sizeof (node->local));
  memset (&node->global, 0, sizeof (node->global));
  memset (&node->rtl, 0, sizeof (node->rtl));
  /* Requeue the node to be re-analyzed if it has been seen in the other unit
     already.
     FIXME: Currently intermodule optimization never inline extern inline
     function defined in multiple units.  This is very wrong.
     */
  if (node->analyzed && flag_unit_at_a_time)
    {
      node->next_needed = cgraph_nodes_queue;
      cgraph_nodes_queue = node;
    }
  node->analyzed = node->local.finalized = false;
  node->local.redefined_extern_inline = true;
  while (node->callees)
    cgraph_remove_edge (node->callees);
  /* We may need to re-queue the node for assembling in case
     we already proceeded it and ignored as not needed.  */
  if (node->reachable && !flag_unit_at_a_time)
    {
      struct cgraph_node *n;

      for (n = cgraph_nodes_queue; n; n = n->next_needed)
	if (n == node)
	  break;
      if (!n)
	node->reachable = 0;
    }
}

/* DECL has been parsed.  Take it, queue it, compile it at the whim of the
   logic in effect.  If NESTED is true, then our caller cannot stand to have
   the garbage collector run at the moment.  We would need to either create
   a new GC context, or just not compile right now.  */

void
cgraph_finalize_function (tree decl, bool nested)
{
  struct cgraph_node *node = cgraph_node (decl);

  if (node->local.finalized)
    cgraph_reset_node (node);

  notice_global_symbol (decl);
  node->decl = decl;
  node->local.finalized = true;
  node->lowered = DECL_STRUCT_FUNCTION (decl)->cfg->x_entry_block_ptr != NULL;
  if (node->nested)
    lower_nested_functions (decl);
  gcc_assert (!node->nested);

  /* If not unit at a time, then we need to create the call graph
     now, so that called functions can be queued and emitted now.  */
  if (!flag_unit_at_a_time)
    {
      cgraph_analyze_function (node);
      cgraph_decide_inlining_incrementally (node);
    }

  if (decide_is_function_needed (node, decl))
    cgraph_mark_needed_node (node);

  /* Since we reclaim unrechable nodes at the end of every language
     level unit, we need to be conservative about possible entry points
     there.  */
  if (flag_whole_program
      && (TREE_PUBLIC (decl) && !DECL_COMDAT (decl) && !DECL_EXTERNAL (decl)))
    cgraph_mark_reachable_node (node);

  /* If not unit at a time, go ahead and emit everything we've found
     to be reachable at this time.  */
  if (!nested)
    {
      if (!cgraph_assemble_pending_functions ())
	ggc_collect ();
    }

  /* If we've not yet emitted decl, tell the debug info about it.  */
  if (!TREE_ASM_WRITTEN (decl))
    (*debug_hooks->deferred_inline_function) (decl);

  /* Possibly warn about unused parameters.  */
  if (warn_unused_parameter)
    do_warn_unused_parameter (decl);
}

void
cgraph_lower_function (struct cgraph_node *node)
{
  if (node->lowered)
    return;
  tree_lowering_passes (node->decl);
  node->lowered = true;
}

/* Used only while constructing the callgraph.  */
static basic_block current_basic_block;

/* Walk tree and record all calls.  Called via walk_tree.  */
static tree
record_call_1 (tree *tp, int *walk_subtrees, void *data)
{
  tree t = *tp;

  switch (TREE_CODE (t))
    {
    case VAR_DECL:
      /* ??? Really, we should mark this decl as *potentially* referenced
	 by this function and re-examine whether the decl is actually used
	 after rtl has been generated.  */
      if (TREE_STATIC (t) || DECL_EXTERNAL (t))
	{
	  cgraph_varpool_mark_needed_node (cgraph_varpool_node (t));
	  if (lang_hooks.callgraph.analyze_expr)
	    return lang_hooks.callgraph.analyze_expr (tp, walk_subtrees, 
						      data);
	}
      break;

    case ADDR_EXPR:
      if (flag_unit_at_a_time)
	{
	  /* Record dereferences to the functions.  This makes the
	     functions reachable unconditionally.  */
	  tree decl = TREE_OPERAND (*tp, 0);
	  if (TREE_CODE (decl) == FUNCTION_DECL)
	    cgraph_mark_needed_node (cgraph_node (decl));
	}
      break;

    case CALL_EXPR:
      {
	tree decl = get_callee_fndecl (*tp);
	if (decl && TREE_CODE (decl) == FUNCTION_DECL)
	  {
	    cgraph_create_edge (data, cgraph_node (decl), *tp,
			        current_basic_block->count,
				current_basic_block->loop_depth);

	    /* When we see a function call, we don't want to look at the
	       function reference in the ADDR_EXPR that is hanging from
	       the CALL_EXPR we're examining here, because we would
	       conclude incorrectly that the function's address could be
	       taken by something that is not a function call.  So only
	       walk the function parameter list, skip the other subtrees.  */

	    walk_tree (&TREE_OPERAND (*tp, 1), record_call_1, data,
		       visited_nodes);
	    *walk_subtrees = 0;
	  }
	break;
      }

    case STATEMENT_LIST:
      {
	tree_stmt_iterator tsi;
	/* Track current statement while finding CALL_EXPRs.  */
	for (tsi = tsi_start (*tp); !tsi_end_p (tsi); tsi_next (&tsi))
	  {
	    walk_tree (tsi_stmt_ptr (tsi), record_call_1, data,
		       visited_nodes);
	  }
      }
      break;

    default:
      /* Save some cycles by not walking types and declaration as we
	 won't find anything useful there anyway.  */
      if (IS_TYPE_OR_DECL_P (*tp))
	{
	  *walk_subtrees = 0;
	  break;
	}

      if ((unsigned int) TREE_CODE (t) >= LAST_AND_UNUSED_TREE_CODE)
	return lang_hooks.callgraph.analyze_expr (tp, walk_subtrees, data);
      break;
    }

  return NULL;
}

/* Create cgraph edges for function calls inside BODY from NODE.  */

static void
cgraph_create_edges (struct cgraph_node *node, tree body)
{

  /* The nodes we're interested in are never shared, so walk
     the tree ignoring duplicates.  */
  visited_nodes = pointer_set_create ();
  current_basic_block = NULL;

  if (TREE_CODE (body) == FUNCTION_DECL)
    {
      struct function *this_cfun = DECL_STRUCT_FUNCTION (body);
      basic_block this_block;
      tree step;

      /* Reach the trees by walking over the CFG, and note the 
	 enclosing basic-blocks in the call edges.  */
      FOR_EACH_BB_FN (this_block, this_cfun)
	{
	  current_basic_block = this_block;
	  walk_tree (&this_block->stmt_list, record_call_1, node, visited_nodes);
	}
      current_basic_block = NULL;

      /* Walk over any private statics that may take addresses of functions.  */
      if (TREE_CODE (DECL_INITIAL (body)) == BLOCK)
	{
	  for (step = BLOCK_VARS (DECL_INITIAL (body));
	       step;
	       step = TREE_CHAIN (step))
	    if (DECL_INITIAL (step))
	      walk_tree (&DECL_INITIAL (step), record_call_1, node, visited_nodes);
	}

      /* Also look here for private statics.  */
      if (DECL_STRUCT_FUNCTION (body))
	for (step = DECL_STRUCT_FUNCTION (body)->unexpanded_var_list;
	     step;
	     step = TREE_CHAIN (step))
	  {
	    tree decl = TREE_VALUE (step);
	    if (DECL_INITIAL (decl) && TREE_STATIC (decl))
	      walk_tree (&DECL_INITIAL (decl), record_call_1, node, visited_nodes);
	  }
    }
  else
    walk_tree (&body, record_call_1, node, visited_nodes);
    
  pointer_set_destroy (visited_nodes);
  visited_nodes = NULL;
}

static bool error_found;

/* Callback of verify_cgraph_node.  Check that all call_exprs have
   cgraph nodes.  */

static tree
verify_cgraph_node_1 (tree *tp, int *walk_subtrees, void *data)
{
  tree t = *tp;
  tree decl;

  if (TREE_CODE (t) == CALL_EXPR && (decl = get_callee_fndecl (t)))
    {
      struct cgraph_edge *e = cgraph_edge (data, t);
      if (e)
	{
	  if (e->aux)
	    {
	      error ("Shared call_expr:");
	      debug_tree (t);
	      error_found = true;
	    }
	  if (e->callee->decl != cgraph_node (decl)->decl)
	    {
	      error ("Edge points to wrong declaration:");
	      debug_tree (e->callee->decl);
	      fprintf (stderr," Instead of:");
	      debug_tree (decl);
	    }
	  e->aux = (void *)1;
	}
      else
	{
	  error ("Missing callgraph edge for call expr:");
	  debug_tree (t);
	  error_found = true;
	}
    }

  /* Save some cycles by not walking types and declaration as we
     won't find anything useful there anyway.  */
  if (IS_TYPE_OR_DECL_P (*tp))
    *walk_subtrees = 0;

  return NULL_TREE;
}

/* Verify cgraph nodes of given cgraph node.  */
void
verify_cgraph_node (struct cgraph_node *node)
{
  struct cgraph_edge *e;
  struct cgraph_node *main_clone;
  tree decl = node->decl;
  struct function *this_cfun = DECL_STRUCT_FUNCTION (decl);
  basic_block this_block;

  timevar_push (TV_CGRAPH_VERIFY);
  error_found = false;
  for (e = node->callees; e; e = e->next_callee)
    if (e->aux)
      {
	error ("Aux field set for edge %s->%s",
	       cgraph_node_name (e->caller), cgraph_node_name (e->callee));
	error_found = true;
      }
  for (e = node->callers; e; e = e->next_caller)
    {
      if (!e->inline_failed)
	{
	  if (node->global.inlined_to
	      != (e->caller->global.inlined_to
		  ? e->caller->global.inlined_to : e->caller))
	    {
	      error ("Inlined_to pointer is wrong");
	      error_found = true;
	    }
	  if (node->callers->next_caller)
	    {
	      error ("Multiple inline callers");
	      error_found = true;
	    }
	}
      else
	if (node->global.inlined_to)
	  {
	    error ("Inlined_to pointer set for noninline callers");
	    error_found = true;
	  }
    }
  if (!node->callers && node->global.inlined_to)
    {
      error ("Inlined_to pointer is set but no predecesors found");
      error_found = true;
    }
  if (node->global.inlined_to == node)
    {
      error ("Inlined_to pointer reffers to itself");
      error_found = true;
    }

  for (main_clone = cgraph_node (node->decl); main_clone;
       main_clone = main_clone->next_clone)
    if (main_clone == node)
      break;
  if (!node)
    {
      error ("Node not found in DECL_ASSEMBLER_NAME hash");
      error_found = true;
    }
  
  if (node->analyzed
      && DECL_SAVED_TREE (node->decl) && !TREE_ASM_WRITTEN (node->decl)
      && (!DECL_EXTERNAL (node->decl) || node->global.inlined_to))
    {
      if (this_cfun->cfg->x_entry_block_ptr)
	{
	  /* The nodes we're interested in are never shared, so walk
	     the tree ignoring duplicates.  */
	  visited_nodes = pointer_set_create ();
	  /* Reach the trees by walking over the CFG, and note the
	     enclosing basic-blocks in the call edges.  */
	  FOR_EACH_BB_FN (this_block, this_cfun)
	    {
	      walk_tree (&this_block->stmt_list, verify_cgraph_node_1, node, visited_nodes);
	    }
	  pointer_set_destroy (visited_nodes);
	  visited_nodes = NULL;
	}
      else
	/* No CFG available?!  */
	gcc_unreachable ();

      for (e = node->callees; e; e = e->next_callee)
	{
	  if (!e->aux)
	    {
	      error ("Edge %s->%s has no corresponding call_expr",
		     cgraph_node_name (e->caller),
		     cgraph_node_name (e->callee));
	      error_found = true;
	    }
	  e->aux = 0;
	}
    }
  if (error_found)
    {
      dump_cgraph_node (stderr, node);
      internal_error ("verify_cgraph_node failed.");
    }
  timevar_pop (TV_CGRAPH_VERIFY);
}

/* Verify whole cgraph structure.  */
void
verify_cgraph (void)
{
  struct cgraph_node *node;

  if (sorrycount || errorcount)
    return;

  for (node = cgraph_nodes; node; node = node->next)
    verify_cgraph_node (node);
}

/* Walk the decls we marked as neccesary and see if they reference new variables
   or functions and add them into the worklists.  */
static bool
cgraph_varpool_analyze_pending_decls (void)
{
  bool changed = false;
  timevar_push (TV_IPA_ANALYSIS);

  while (cgraph_varpool_first_unanalyzed_node)
    {
      tree decl = cgraph_varpool_first_unanalyzed_node->decl;

      cgraph_varpool_first_unanalyzed_node->analyzed = true;

      /* Some datastructures (such as typeinfos for EH handling) can be output
         late during the RTL compilation.  We need to make these invisible to
	 IPA optimizers or we confuse them badly.  */
      if (cgraph_global_info_ready)
        cgraph_varpool_first_unanalyzed_node->non_ipa = true;
      cgraph_varpool_first_unanalyzed_node = cgraph_varpool_first_unanalyzed_node->next_needed;

      if (DECL_INITIAL (decl))
	cgraph_create_edges (NULL, DECL_INITIAL (decl));
      changed = true;
    }
  timevar_pop (TV_IPA_ANALYSIS);
  return changed;
}

/* Output all variables enqueued to be assembled.  */
bool
cgraph_varpool_assemble_pending_decls (void)
{
  bool changed = false;

  if (errorcount || sorrycount)
    return false;
 
  /* EH might mark decls as needed during expansion.  This should be safe since
     we don't create references to new function, but it should not be used
     elsewhere.  */
  cgraph_varpool_analyze_pending_decls ();

  while (cgraph_varpool_nodes_queue)
    {
      tree decl = cgraph_varpool_nodes_queue->decl;
      struct cgraph_varpool_node *node = cgraph_varpool_nodes_queue;

      cgraph_varpool_nodes_queue = cgraph_varpool_nodes_queue->next_needed;
      if (!TREE_ASM_WRITTEN (decl) && !DECL_EXTERNAL (decl))
	{
	  if (!node->non_ipa)
            ipa_modify_variable (node);
	  assemble_variable (decl, 0, 1, 0);
	  changed = true;
	}
      node->next_needed = NULL;
    }
  return changed;
}

/* Analyze the function scheduled to be output.  */
static void
cgraph_analyze_function (struct cgraph_node *node)
{
  tree decl = node->decl;
  struct loops loops;

  timevar_push (TV_IPA_ANALYSIS);
  push_cfun (DECL_STRUCT_FUNCTION (decl));
  current_function_decl = decl;

  cgraph_lower_function (node);
  if (flag_unit_at_a_time)
    tree_early_local_passes (decl);

  node->count = ENTRY_BLOCK_PTR->count;

  if (optimize)
    flow_loops_find (&loops, LOOP_TREE);
  cgraph_create_edges (node, decl);
  if (optimize)
    flow_loops_free (&loops);
  free_dominance_info (CDI_DOMINATORS);

  /* Only optimization we do in non-unit-at-a-time mode is inlining.  We don't
     use the passmanager then and instead call it directly.  Since we probably
     don't want to add more passes like this, it should be OK.  */
  if (!flag_unit_at_a_time)
    cgraph_analyze_function_inlinability (node);

  node->analyzed = true;
  current_function_decl = NULL;
  pop_cfun ();
  timevar_pop (TV_IPA_ANALYSIS);
}

/* Analyze the whole (source level) compilation unit once it is parsed
   completely.  For frontends supporting multiple compilation units to
   be parsed at once this function shall be called for each of them so
   unreachable static functions are elliminated early.  */

void
cgraph_finalize_compilation_unit (void)
{
  struct cgraph_node *node;
  /* Keep track of already processed nodes when called multiple times for
     intermodule optmization.  */
  static struct cgraph_node *first_analyzed;

  if (!flag_unit_at_a_time)
    {
      cgraph_assemble_pending_functions ();
      return;
    }

  if (!quiet_flag)
    {
      fprintf (stderr, "\nAnalyzing compilation unit");
      fflush (stderr);
    }

  timevar_push (TV_CGRAPH);
  cgraph_varpool_analyze_pending_decls ();
  if (cgraph_dump_file)
    {
      fprintf (cgraph_dump_file, "Initial entry points:");
      for (node = cgraph_nodes; node != first_analyzed; node = node->next)
	if (node->needed && DECL_SAVED_TREE (node->decl))
	  fprintf (cgraph_dump_file, " %s", cgraph_node_name (node));
      fprintf (cgraph_dump_file, "\n");
    }

  /* Propagate reachability flag and lower representation of all reachable
     functions.  In the future, lowering will introduce new functions and
     new entry points on the way (by template instantiation and virtual
     method table generation for instance).  */
  while (cgraph_nodes_queue)
    {
      struct cgraph_edge *edge;
      tree decl = cgraph_nodes_queue->decl;

      node = cgraph_nodes_queue;
      cgraph_nodes_queue = cgraph_nodes_queue->next_needed;
      node->next_needed = NULL;

      /* ??? It is possible to create extern inline function and later using
	 weak alias attribute to kill its body. See
	 gcc.c-torture/compile/20011119-1.c  */
      if (!DECL_SAVED_TREE (decl))
	{
	  cgraph_reset_node (node);
	  continue;
	}

      gcc_assert (!node->analyzed && node->reachable);
      gcc_assert (DECL_SAVED_TREE (decl));

      cgraph_analyze_function (node);

      for (edge = node->callees; edge; edge = edge->next_callee)
	if (!edge->callee->reachable)
	  cgraph_mark_reachable_node (edge->callee);

      cgraph_varpool_analyze_pending_decls ();
    }

  /* Collect entry points to the unit.  */

  if (cgraph_dump_file)
    {
      fprintf (cgraph_dump_file, "Unit entry points:");
      for (node = cgraph_nodes; node != first_analyzed; node = node->next)
	if (node->needed && DECL_SAVED_TREE (node->decl))
	  fprintf (cgraph_dump_file, " %s", cgraph_node_name (node));
    }

  if (cgraph_dump_file)
    fprintf (cgraph_dump_file, "\nReclaiming functions:");

  for (node = cgraph_nodes; node != first_analyzed; node = node->next)
    {
      tree decl = node->decl;

      if (node->local.finalized && !DECL_SAVED_TREE (decl))
        cgraph_reset_node (node);

      if (!node->reachable && node->local.finalized)
	{
	  if (cgraph_dump_file)
	    fprintf (cgraph_dump_file, " %s", cgraph_node_name (node));
	  cgraph_remove_node (node);
	  continue;
	}
      else
	{
	  node->next_needed = NULL;
	  if (!node->local.finalized)
	    DECL_SAVED_TREE (decl) = NULL;
	}
      gcc_assert (!node->local.finalized || DECL_SAVED_TREE (decl));
      gcc_assert (node->analyzed == node->local.finalized);
    }
  first_analyzed = cgraph_nodes;
  if (!quiet_flag)
    fprintf (stderr, "\n\n");
  ggc_collect ();
  timevar_pop (TV_CGRAPH);
}
/* Figure out what functions we want to assemble.  */

static void
cgraph_mark_functions_to_output (void)
{
  struct cgraph_node *node;

  for (node = cgraph_nodes; node; node = node->next)
    {
      tree decl = node->decl;
      struct cgraph_edge *e;
      
      gcc_assert (!node->output);

      for (e = node->callers; e; e = e->next_caller)
	if (e->inline_failed)
	  break;

      /* We need to output all local functions that are used and not
	 always inlined, as well as those that are reachable from
	 outside the current compilation unit.  */
      if (DECL_SAVED_TREE (decl)
	  && !node->global.inlined_to
	  && (node->needed
	      || (e && node->reachable))
	  && !TREE_ASM_WRITTEN (decl)
	  && !DECL_EXTERNAL (decl))
	node->output = 1;
      else
	{
	  /* We should've reclaimed all functions that are not needed.  */
#ifdef ENABLE_CHECKING
	  if (!node->global.inlined_to && DECL_SAVED_TREE (decl)
	      && !DECL_EXTERNAL (decl))
	    {
	      dump_cgraph_node (stderr, node);
	      internal_error ("failed to reclaim unneeded function");
	    }
#endif
	  gcc_assert (node->global.inlined_to || !DECL_SAVED_TREE (decl)
		      || DECL_EXTERNAL (decl));

	}
      
    }
}

/* Expand function specified by NODE.  */

static void
cgraph_expand_function (struct cgraph_node *node)
{
  tree decl = node->decl;

  /* We ought to not compile any inline clones.  */
  gcc_assert (!node->global.inlined_to);

  if (flag_unit_at_a_time)
    announce_function (decl);

  /* Must have a CFG here at this point.  */
  gcc_assert (ENTRY_BLOCK_PTR_FOR_FUNCTION (DECL_STRUCT_FUNCTION (node->decl)));

  if (!flag_unit_at_a_time)
    tree_early_local_passes (decl);
  /* Generate RTL for the body of DECL.  */
  lang_hooks.callgraph.expand_function (decl);

  /* Make sure that BE didn't give up on compiling.  */
  /* ??? Can happen with nested function of extern inline.  */
  gcc_assert (TREE_ASM_WRITTEN (node->decl));

  current_function_decl = NULL;
  if (!cgraph_preserve_function_body_p (node->decl))
    {
      DECL_SAVED_TREE (node->decl) = NULL;
      DECL_STRUCT_FUNCTION (node->decl) = NULL;
      DECL_INITIAL (node->decl) = error_mark_node;
      /* Eliminate all call edges.  This is important so the call_expr no longer
	 points to the dead function body.  */
      while (node->callees)
	cgraph_remove_edge (node->callees);
    }
}

/* Expand all functions that must be output.

   Attempt to topologically sort the nodes so function is output when
   all called functions are already assembled to allow data to be
   propagated across the callgraph.  Use a stack to get smaller distance
   between a function and its callees (later we may choose to use a more
   sophisticated algorithm for function reordering; we will likely want
   to use subsections to make the output functions appear in top-down
   order).  */

static void
cgraph_expand_all_functions (void)
{
  struct cgraph_node *node;
  struct cgraph_node **order =
    xcalloc (cgraph_n_nodes, sizeof (struct cgraph_node *));
  int order_pos = 0, new_order_pos = 0;
  int i;

  order_pos = cgraph_postorder (order);
  gcc_assert (order_pos == cgraph_n_nodes);

  /* Garbage collector may remove inline clones we eliminate during
     optimization.  So we must be sure to not reference them.  */
  for (i = 0; i < order_pos; i++)
    if (order[i]->output)
      order[new_order_pos++] = order[i];

  for (i = new_order_pos - 1; i >= 0; i--)
    {
      node = order[i];
      if (node->output)
	{
	  gcc_assert (node->reachable);
	  node->output = 0;
	  cgraph_expand_function (node);
	}
    }
  free (order);
}

/* Mark visibility of all functions.
   
   A local function is one whose calls can occur only in the current
   compilation unit and all its calls are explicit, so we can change
   its calling convention.  We simply mark all static functions whose
   address is not taken as local.

   We also change the TREE_PUBLIC flag of all declarations that are public
   in language point of view but we want to overwrite this default
   via -fwhole-program for the backend point of view.  */

static void
cgraph_function_and_variable_visibility (void)
{
  struct cgraph_node *node;
  struct cgraph_varpool_node *vnode;

  for (node = cgraph_nodes; node; node = node->next)
    {
      if (node->reachable
	  && (DECL_COMDAT (node->decl)
	      || (TREE_PUBLIC (node->decl) && !DECL_EXTERNAL (node->decl)
		  && !flag_whole_program)))
	node->local.externally_visible = 1;
      if (!node->local.externally_visible && node->analyzed
	  && !DECL_EXTERNAL (node->decl))
	{
	  gcc_assert (flag_whole_program || !TREE_PUBLIC (node->decl));
	  TREE_PUBLIC (node->decl) = 0;
	}
      node->local.local = (!node->needed
			   && node->analyzed
			   && !TREE_PUBLIC (node->decl));
    }
  for (vnode = cgraph_varpool_nodes_queue; vnode; vnode = vnode->next_needed)
    {
      if (vnode->needed
	  && (DECL_COMDAT (vnode->decl)
	      || (TREE_PUBLIC (vnode->decl) && !flag_whole_program)))
	vnode->externally_visible = 1;
      if (!vnode->externally_visible)
	{
	  gcc_assert (flag_whole_program || !TREE_PUBLIC (vnode->decl));
	  TREE_PUBLIC (vnode->decl) = 0;
	}
     gcc_assert (TREE_STATIC (vnode->decl));
    }

  /* Because we have to be conservative on the boundaries of source
     level units, it is possible that we marked some functions in
     reachable just because they might be used later via external
     linkage, but after making them local they are really unreachable
     now.  */
  if (flag_whole_program)
    cgraph_remove_unreachable_nodes (true, cgraph_dump_file);

  if (cgraph_dump_file)
    {
      fprintf (cgraph_dump_file, "\nMarking local functions:");
      for (node = cgraph_nodes; node; node = node->next)
	if (node->local.local)
	  fprintf (cgraph_dump_file, " %s", cgraph_node_name (node));
      fprintf (cgraph_dump_file, "\n\n");
      fprintf (cgraph_dump_file, "\nMarking externally visible functions:");
      for (node = cgraph_nodes; node; node = node->next)
	if (node->local.externally_visible)
	  fprintf (cgraph_dump_file, " %s", cgraph_node_name (node));
      fprintf (cgraph_dump_file, "\n\n");
    }
  cgraph_function_flags_ready = true;
}

/* Return true when function body of DECL still needs to be kept around
   for later re-use.  */
bool
cgraph_preserve_function_body_p (tree decl)
{
  struct cgraph_node *node;
  /* Keep the body; we're going to dump it.  */
  if (dump_enabled_p (TDI_tree_all))
    return true;
  if (!cgraph_global_info_ready)
    return (DECL_INLINE (decl) && !flag_really_no_inline);
  /* Look if there is any clone around.  */
  for (node = cgraph_node (decl); node; node = node->next_clone)
    if (node->global.inlined_to)
      return true;
  return false;
}

/* Perform simple optimizations based on callgraph.  */

void
cgraph_optimize (void)
{
  struct cgraph_node *node;
  struct cgraph_varpool_node *vnode;
#ifdef ENABLE_CHECKING
  verify_cgraph ();
#endif
  if (!flag_unit_at_a_time)
    {
      cgraph_varpool_assemble_pending_decls ();
      return;
    }
  timevar_push (TV_IPA_OPT);

  process_pending_assemble_externals ();

  if (!quiet_flag)
    {
      fprintf (stderr, "Performing intraprocedural optimizations");
      fflush (stderr);
    }
  if (cgraph_dump_file)
    {
      fprintf (cgraph_dump_file, "\n\nInitial ");
      dump_cgraph (cgraph_dump_file);
    }

  /* Frontend may output common variables after the unit has been finalized.
     It is safe to deal with them here as they are always zero initialized.  */
  cgraph_varpool_analyze_pending_decls ();

  cgraph_function_and_variable_visibility ();

  for (node = cgraph_nodes; node; node = node->next)
    if (node->analyzed)
      ipa_analyze_function (node);
  for (vnode = cgraph_varpool_nodes_queue; vnode; vnode = vnode->next_needed)
    if (!vnode->non_ipa)
      ipa_analyze_variable (vnode);

  if (cgraph_dump_file)
    {
      fprintf (cgraph_dump_file, "Marked ");
      dump_cgraph (cgraph_dump_file);
      dump_varpool (cgraph_dump_file);
    }

  bitmap_obstack_initialize (NULL);
  ipa_passes ();
  bitmap_obstack_release (NULL);
  /* FIXME: this should be unnecesary if inliner took care of removing dead
     functions.  */
  cgraph_remove_unreachable_nodes (false, dump_file);  
  cgraph_global_info_ready = true;
  if (cgraph_dump_file)
    {
      fprintf (cgraph_dump_file, "Optimized ");
      dump_cgraph (cgraph_dump_file);
      dump_varpool (cgraph_dump_file);
    }
  timevar_pop (TV_IPA_OPT);

  /* Output everything.  */
  if (!quiet_flag)
    fprintf (stderr, "\nAssembling functions:\n");
#ifdef ENABLE_CHECKING
  verify_cgraph ();
#endif
  
  cgraph_mark_functions_to_output ();
  cgraph_expand_all_functions ();

  cgraph_varpool_assemble_pending_decls ();
  if (cgraph_dump_file)
    {
      fprintf (cgraph_dump_file, "\nFinal ");
      dump_cgraph (cgraph_dump_file);
    }
#ifdef ENABLE_CHECKING
  verify_cgraph ();
  /* Double check that all inline clones are gone and that all
     function bodies have been released from memory.  */
  if (flag_unit_at_a_time
      && !dump_enabled_p (TDI_tree_all)
      && !(sorrycount || errorcount))
    {
      struct cgraph_node *node;
      bool error_found = false;

      for (node = cgraph_nodes; node; node = node->next)
	if (node->analyzed
	    && (node->global.inlined_to
	        || DECL_SAVED_TREE (node->decl)))
	  {
	    error_found = true;
	    dump_cgraph_node (stderr, node);
 	  }
      if (error_found)
	internal_error ("Nodes with no released memory found.");
    }
#endif
}

/* Generate and emit a static constructor or destructor.  WHICH must be
   one of 'I' or 'D'.  BODY should be a STATEMENT_LIST containing 
   GENERIC statements.  */

void
cgraph_build_static_cdtor (char which, tree body, int priority)
{
  static int counter = 0;
  char which_buf[16];
  tree decl, name, resdecl;

  sprintf (which_buf, "%c_%d", which, counter++);
  name = get_file_function_name_long (which_buf);

  decl = build_decl (FUNCTION_DECL, name,
		     build_function_type (void_type_node, void_list_node));
  current_function_decl = decl;

  resdecl = build_decl (RESULT_DECL, NULL_TREE, void_type_node);
  DECL_ARTIFICIAL (resdecl) = 1;
  DECL_IGNORED_P (resdecl) = 1;
  DECL_RESULT (decl) = resdecl;

  allocate_struct_function (decl);

  TREE_STATIC (decl) = 1;
  TREE_USED (decl) = 1;
  DECL_ARTIFICIAL (decl) = 1;
  DECL_IGNORED_P (decl) = 1;
  DECL_NO_INSTRUMENT_FUNCTION_ENTRY_EXIT (decl) = 1;
  DECL_SAVED_TREE (decl) = body;
  TREE_PUBLIC (decl) = ! targetm.have_ctors_dtors;
  DECL_UNINLINABLE (decl) = 1;

  DECL_INITIAL (decl) = make_node (BLOCK);
  TREE_USED (DECL_INITIAL (decl)) = 1;

  DECL_SOURCE_LOCATION (decl) = input_location;
  cfun->function_end_locus = input_location;

  switch (which)
    {
    case 'I':
      DECL_STATIC_CONSTRUCTOR (decl) = 1;
      break;
    case 'D':
      DECL_STATIC_DESTRUCTOR (decl) = 1;
      break;
    default:
      gcc_unreachable ();
    }

  gimplify_function_tree (decl);

  /* ??? We will get called LATE in the compilation process.  */
  if (cgraph_global_info_ready)
    {
      tree_lowering_passes (decl);
      tree_early_local_passes (decl);
      tree_rest_of_compilation (decl);
    }
  else
    cgraph_finalize_function (decl, 0);
  
  if (targetm.have_ctors_dtors)
    {
      void (*fn) (rtx, int);

      if (which == 'I')
	fn = targetm.asm_out.constructor;
      else
	fn = targetm.asm_out.destructor;
      fn (XEXP (DECL_RTL (decl), 0), priority);
    }
}

void
init_cgraph (void)
{
  cgraph_dump_file = dump_begin (TDI_cgraph, NULL);
}

/* Update the CALL_EXPR in NEW_VERSION node callers edges.  */

void
update_call_expr (struct cgraph_node *new_version, 
  		  varray_type redirect_callers)
{
  struct cgraph_edge *e;
  unsigned i;
  
  if (new_version == NULL)
    abort ();
  
  for (i = 0; i < VARRAY_ACTIVE_SIZE (redirect_callers); i++)
    {
      e = VARRAY_GENERIC_PTR (redirect_callers, i);
      
      /* Update the call expr on the edges
	 to the new version. */ 
       TREE_OPERAND (TREE_OPERAND (e->call_expr, 0), 0) = new_version->decl;
    }
  for (e = new_version->callers; e; e = e->next_caller)
    { 
      /* Update the call expr on the edges
          of recursive calls. */
      if (e->caller == new_version)
	TREE_OPERAND (TREE_OPERAND (e->call_expr, 0), 0) = new_version->decl;            
    }           
}
 

/* Create a new cgraph node which is the new version of 
   OLD_VERSION node.  REDIRECT_CALLERS holds the callers
    of OLD_VERSION which should be redirected to point to  
    NEW_VERSION.  ALL the callees edges of OLD_VERSION 
    are cloned to the new version node.  Return the new 
    version node.  */
 
struct cgraph_node *
cgraph_copy_node_for_versioning (struct cgraph_node *old_version,
  				 tree new_decl, varray_type redirect_callers)
 {
   struct cgraph_node *new_version;
   struct cgraph_edge *e;
   struct cgraph_edge *next_callee;
   unsigned i;
   
   if (old_version == NULL)
     abort ();
  
   new_version = cgraph_node (new_decl);
   
   new_version->analyzed = true;
   new_version->local = old_version->local;
   new_version->global = old_version->global;
   new_version->rtl = new_version->rtl;
   new_version->reachable = true;
   new_version->static_vars_info = old_version->static_vars_info;
 
   /* Clone the old node callees.  Recursive calls are
      also cloned.  */
   for (e = old_version->callees;e; e=e->next_callee)
     cgraph_clone_edge (e, new_version, e->call_expr, REG_BR_PROB_BASE, e->loop_nest);
   
   /* Fix recursive calls. 
      If old_version has a recursive call after the 
      previous cloning the new version will have an edge
      pointing to the old version which is wrong;
      so redirect it to point to the new version. */
   for (e = new_version->callees ; e; e = next_callee)
     {
       next_callee = e->next_callee;
       if (e->callee == old_version)
         {
           cgraph_redirect_edge_callee (e, new_version);
         }
       if (!next_callee)
	 break;
     }
   for (i = 0; i < VARRAY_ACTIVE_SIZE (redirect_callers); i++) 
     {
       e = VARRAY_GENERIC_PTR (redirect_callers, i); 
       /* Redirect calls to the old version node
	  to point to it's new version.  */ 
       cgraph_redirect_edge_callee (e, new_version);
     }
   
   allocate_struct_function (new_decl);
   cfun->function_end_locus = DECL_SOURCE_LOCATION (new_decl);
   
   return new_version;
 }

 /* Perform function versioning. 
    Function versioning includes:
    1) Generating a new cgraph node for the new version
    and redirect it's edges respectively. 
    2) Copying the old version tree to the new
    version.  
    The function gets :
    - REDIRECT_CALLERS varray, the edges to be redirected 
      to the new version.
    - tree_map, a mapping of tree nodes we want to replace with
      new ones (according to results of prior analysis)
    - the old version's cgraph node. 
    It returns the new version's cgraph node.  */ 

struct cgraph_node *
cgraph_function_versioning (struct cgraph_node *old_version_node, 
                            varray_type redirect_callers, varray_type tree_map) 
{
  tree old_decl = old_version_node->decl;
  struct cgraph_node *new_version_node = NULL;
  tree new_decl;
  
  if (!tree_versionable_function_p (old_decl))
    return NULL;
  
  /* Make a new FUNCTION_DECL tree node for the
     new version. */
  new_decl = copy_node (old_decl);
  
  /* Create the new version's call-graph node. 
     and update the edges of the new node. */
  new_version_node = 
    cgraph_copy_node_for_versioning (old_version_node, new_decl,
                                     redirect_callers);  
  
  /* Copy the old version's function tree to the new
     version.  */
  tree_function_versioning (old_decl, new_decl, tree_map);
  /* Update the call_expr on the edges
     to the new version node. */
  update_call_expr (new_version_node, redirect_callers);
  return new_version_node;
}


