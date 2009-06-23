/* Interprocedural constant propagation
   Copyright (C) 2008 Free Software Foundation, Inc.
   Contributed by Martin Jambor <mjambor@suse.cz>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/* This file implements early interprocedural SRA and (both early and late)
   intraprocedural SRA.  All of these passes roughly operate in four stages:
   First, they analyze types and declaration properties of variables and/or
   parameters whether they are aggregates which can be reduced or otherwise
   processed, IPA-SRA also needs to make sure the function prototype can be
   in-place modified.  Second, they traverse all statements in the function
   body and collect information about how these variables or parameters are
   accessed (see struct access).  Third, they reorganize and analyze this
   information in order to determine what modifications should be performed, if
   any.  IPA-SRA also needs to check the callers, whether they can be adjusted
   as required.  Finally, the function (and its callers in IPA-SRA) are
   modified as planned in the previous step.

   When doing IPA-SRA, the pass proceeds in the following way:

   1. All parameters are checked whether they are aggregates or pointer to
      aggregates and for other required properties (such as non-volatility).
      Those suitable for reducing into components are marked by setting
      corresponding bits in candidate_bitmap.  The optimization continues even
      if none were found so that unused scalars can be removed and (later on)
      scalars passed by reference which could be passed by value are passed
      that way.

   2. The function body is scanned and all accesses to memory are examined and
      if they access any of the candidates, an access structure is created to
      mark the offset and size of the access.  If an access precludes us from
      reducing any of the candidates (for example when the size or the offset
      cannot bed determined or are not compile time constants), the candidate
      is removed from the bitmap.

   3. The pass sorts all accesses for a particular parameter and searches for
      any overlaps (a pair of accesses which both cover a part of an aggregate
      but at least one also covers a part not covered by the other).  If there
      are any, the parameter is also disqualified.  Otherwise, the pass finds a
      representative access for each combination of offset and size and creates
      a linked list out of these representatives.  In IPA-SRA, accesses are not
      organized into trees since no overlaps are allowed anyway.  If there are
      any representatives of parameters which are passed by reference but which
      are not written to, the optimization walks the function again, trying to
      prove that no side effects can modified these accesses and that
      associated parameters are always dereferenced when the function is run.
      Then decisions are made as to what parameters are to be split into what
      components and this decision is represented in form of vector of struct
      ipa_parm_note.  Each structure describes one parameter of the function
      after the function is modified (and how it relates to original
      parameters) but may also represent a decision to remove a parameter
      altogether.  Finally, we check that all callers can be modified to pass
      the intended new set of parameters.  If they are not, the optimization of
      this function is aborted.

   4. The pass then modifies the parameters in both declaration and the type of
      the current function.  Afterwards it traverses the function again,
      replacing all references to components of the reduced parameters by the
      new parameters, possibly removing an indirect_ref and so on.  Finally, it
      converts all callers so that they pass the new required parameters and
      makes sure the function is private (ie. not COMDAT).


   Most of the steps are different when doing intraprocedural SRA:

   1. The selection of candidates checks all referenced aggregates but is much
      stricter, specifically it does not allow any TREE_ADDRESSABLE
      declarations, let alone pointers to aggregates.  Results are also
      recorded to candidate_bitmap but processing of a function terminates if
      no candidates are found.

   2. This step is entirely the same as in IPA-SRA.  Access structures are
      gathered by scanning the function body.

   3. The optimization then also sorts all accesses for a particular candidate
      and also searches for overlaps but is less strict now.  It only disallows
      partial overlaps, i.e. a pair of accesses covering some common part of
      the base aggregate but _both_ also covering some part that is not covered
      by the other.  If such a partial overlap is found, the aggregate is no
      longer considered for scalarization.  Subsequently, representatives for
      the same combinations of offset and sizes are identified and linked
      together like in IPA-SRA.

      However, that is not the end of access reorganization.  The optimization
      builds a list of tree structures out of them.  In each tree, every parent
      covers all parts of the aggregate that are covered by all its children.
      The roots of the trees are linked together in a linked list.  When
      building the tree, the optimizations instantiates scalar replacements for
      scalar leaves of the tree that have no scalar (grand)parents.

   4. In the modification phase, the pass traverses the function body, looking
      for references to scalarized aggregates.  If such a reference is found
      and it relates to an access representative that has an instantiated
      replacement, the expression is replaced with the reference, possibly with
      some required typecasts.  Moreover, if such an expression relates to a
      non-leaf representative, all the leaves in its subtree that are
      scalarized must be copied in or out of the original aggregate.  There is
      an exception when processing an assignment of two reduced aggregates, in
      that case we try to load the scalarized components of the left hand side
      from those of the aggregate on the right hand side and resort to copying
      through the original aggregates.  Finally, all scalar reductions of
      function parameters are initialized from the parameters themselves at the
      very beginning of the function.

 */




#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "alloc-pool.h"
#include "tm.h"
#include "tree.h"
#include "gimple.h"
#include "cgraph.h"
#include "tree-inline.h"
#include "tree-flow.h"
#include "diagnostic.h"
#include "tree-dump.h"
#include "tree-pass.h"
#include "ipa-prop.h"
#include "timevar.h"
#include "params.h"
#include "target.h"
#include "ipa-utils.h"
#include "flags.h"


/* Enumeration of all aggregate reductions we can do.  */
enum sra_mode {SRA_MODE_EARLY_IPA,
	       SRA_MODE_EARLY_INTRA,
	       SRA_MODE_INTRA};

/* Global variable describing which aggregate reduction we are performing at
   the moment.  */
static enum sra_mode sra_mode;

struct assign_link;

/* ACCESS represents each access to an aggregate variable (base or component).
   It can also represent a group of accesses that refer to the same fragment of
   an aggregate (i.e. those that have exactly the same offset and size).  Such
   representatives for a single aggregate, once determined, are linked in a
   linked list and have the group fields set.

   Moreover, when doing intraprocedural SSA, a tree is built from those
   representatives (by the means of first_child and next_sibling pointers), in
   which all items in a subtree are "within" the root, i.e. their offset is
   greater or equal to offset of the root and offset+size is smaller or equal
   to offset+size of the root.  Children of an access are sorted by offset.
*/

struct access
{
  /* Values returned by `get_ref_base_and_extent' for each COMPONENT_REF
     If EXPR isn't a COMPONENT_REF just set `BASE = EXPR', `OFFSET = 0',
     `SIZE = TREE_SIZE (TREE_TYPE (expr))'.  */
  HOST_WIDE_INT offset;
  HOST_WIDE_INT size;
  tree base;

  /* Expression.  */
  tree expr;
  /* Type.  */
  tree type;

  /* The basic block of this access.  */
  basic_block bb;

  /* Next group representative for this aggregate. */
  struct access *next_grp;

  /* Pointer to the group representative.  Pointer to itself if the struct is
     the representative.  */
  struct access *group_representative;

  /* If this access has any children (in terms of the definition above), this
     points to the first one.  */
  struct access *first_child;

  /* Pointer to the next sibling in the access tree as described above.  */
  struct access *next_sibling;

  /* Pointers to the first and last element in the linked list of assign
     links.  */
  struct assign_link *first_link, *last_link;
  /* Pointer to the next access in the work queue.  */
  struct access *next_queued;

  /* Replacement variable for this access "region."  Never to be accessed
     directly, always only by the means of get_access_replacement() and only
     when to_be_replaced flag is set.  */
  tree replacement_decl;

  /* Last statement ID when access was done or -1 if it was not done in safe
     block.  For a group representative, this is the maximum stmt_no of the
     whole group. */
  int stmt_no;

  /* Is this particular access write access? */
  unsigned write : 1;
  /* in IPA-SRA, is it guaranteed that an access to this or bigger offset is
     always performed when the function is run? */
  unsigned always_safe : 1;

  /* Is this acess currently in the work queue?  */
  unsigned grp_queued : 1;
  /* Does this group contain a write access?  This flag is propagated down the
     access tree.  */
  unsigned grp_write : 1;
  /* Does this group contain a read access?  This flag is propagated down the
     access tree.  */
  unsigned grp_read : 1;
  /* Is the subtree rooted in this access fully covered by scalar
     replacements?  */
  unsigned grp_covered : 1;
  /* If set to true, this access and all below it in an access tree must not be
     scalarized.  */
  unsigned grp_unscalarizable_region : 1;
  /* Whether data have been written to parts of the aggregate covered by this
     access which is not to be scalarized.  This flag is propagated up in the
     access tree.  */
  unsigned grp_unscalarized_data : 1;
  /* Does this access and/or group contain a write access through a
     BIT_FIELD_REF?  */
  unsigned grp_bfr_lhs : 1;

  /* Is it possible that the group refers to data which might be (directly or
     otherwise) modified?  */
  unsigned grp_maybe_modified : 1;
  /* Set when a scalar replacement should be created for this variable.  We do
     the decision and creation at different places because make_rename_temp
     cannot be called from within FOR_EACH_REFERENCED_VAR. */
  unsigned to_be_replaced : 1;
  /* Set when this is a representative of a pointer to scalar (i.e. by
     reference) parameter which we consider for turning into a plain scalar
     (i.e. a by value parameter).  */
  unsigned grp_scalar_ptr : 1;
};

typedef struct access *access_p;

DEF_VEC_P (access_p);
DEF_VEC_ALLOC_P (access_p, heap);

/* Alloc pool for allocating access structures.  */
static alloc_pool access_pool;

/* A structure linking lhs and rhs accesses from an aggregate assignment.  They
   are then used to propagate subaccesses from rhs to lhs as long as they don't
   conflict with what is already there.  */
struct assign_link
{
  struct access *lacc, *racc;
  struct assign_link *next;
};

/* Alloc pool for allocating assign link structures.  */
static alloc_pool link_pool;

/* Base (tree) -> Vector (VEC(access_p,heap) *) map.  */
static struct pointer_map_t *base_access_vec;

/* Bitmap of bases (candidates).  */
static bitmap candidate_bitmap;
/* Obstack for creation of fancy names.  */
static struct obstack name_obstack;


/* Number of parameters of the analyzed function when doing early ipa SRA.  */
static int func_param_count;

/* We employ very simplistic control flow sensitivity in our early ipa SRA
   analysis.  SAFE_BB is very first basic block of function if there is no loop
   edge reaching it.  STMT_NO is number of statement in this BB or -1.  This
   way we can scan if memory write must happen after last read of argument.  */
static basic_block safe_bb;
static int stmt_no;
/* Current BB when executing within scan_function().  */
static basic_block current_bb;
/* scan_function sets the following to true if it encounters a call to
   __builtin_va_start.  */
static bool encountered_va_start;
/* scan_function sets the following to true whenever it encounters a statement
   that can throw externally.  */
static bool encountered_external_throw;

/* Representative of no accesses at all. */
static struct access no_accesses_representant;

/* Head of a linked list of accesses that need to have its subaccesses
   propagated to their assignment counterparts. */
static struct access *work_queue_head;

/* Predicate to test the special value.  */

static inline bool
no_accesses_p (struct access *access)
{
  return access == &no_accesses_representant;
}

/* Dump contents of ACCESS to dump_file in a human friendly way.  If GRP is
   true, representative fields are dumped, otherwise those which only describe
   the individual access are.  */

static void
dump_access (struct access *access, bool grp)
{
  fprintf (dump_file, "access { ");
  fprintf (dump_file, "base = (%d)'", DECL_UID (access->base));
  print_generic_expr (dump_file, access->base, 0);
  fprintf (dump_file, "', offset = %d", (int) access->offset);
  fprintf (dump_file, ", size = %d", (int) access->size);
  fprintf (dump_file, ", expr = ");
  print_generic_expr (dump_file, access->expr, 0);
  fprintf (dump_file, ", type = '");
  print_generic_expr (dump_file, access->type, 0);
  if (grp)
    fprintf (dump_file, ", grp_write = %d, grp_read = %d, grp_covered = %d, "
	     "grp_unscalarizable_region = %d, grp_unscalarized_data = %d, "
	     "grp_maybe_modified = %d, to_be_replaced = %d, "
	     "stmt_no = %d, always_safe = %d'\n",
	     access->grp_write, access->grp_read, access->grp_covered,
	     access->grp_unscalarizable_region, access->grp_unscalarized_data,
	     access->grp_maybe_modified, access->to_be_replaced,
	     access->stmt_no, access->always_safe);
  else
    fprintf (dump_file, ", write = %d, stmt_no = %d'\n", access->write,
	     access->stmt_no);
}

/* Return a vector of pointers to accesses for the variable given in BASE or
   NULL if there is none.  */

static VEC (access_p, heap) *
get_base_access_vector (tree base)
{
  void **slot;

  slot = pointer_map_contains (base_access_vec, base);
  if (!slot)
    return NULL;
  else
    return *(VEC (access_p, heap) **) slot;
}

/* Find an access with required OFFSET and SIZE in a subtree of accesses rooted
   in ACCESS.  Return NULL if it cannot be found.  */

static struct access *
find_access_in_subtree (struct access *access, HOST_WIDE_INT offset,
			HOST_WIDE_INT size)
{
  while (access && (access->offset != offset || access->size != size))
    {
      struct access *child = access->first_child;

      while (child && (child->offset + child->size <= offset))
	child = child->next_sibling;
      access = child;
    }

  return access;
}

/* Return the first group representative for DECL or NULL if none exists.  */

static struct access *
get_first_repr_for_decl (tree base)
{
  VEC (access_p, heap) *access_vec;

  access_vec = get_base_access_vector (base);
  if (!access_vec)
    return NULL;

  return VEC_index (access_p, access_vec, 0);
}

/* Find an access representative for the variable BASE and given OFFSET and
   SIZE.  Requires that access trees have already been built.  Return NULL if
   it cannot be found.  */

static struct access *
get_var_base_offset_size_access (tree base, HOST_WIDE_INT offset,
				 HOST_WIDE_INT size)
{
  struct access *access;

  access = get_first_repr_for_decl (base);
  while (access && (access->offset + access->size <= offset))
    access = access->next_grp;
  if (!access)
    return NULL;

  return find_access_in_subtree (access, offset, size);
}

/* Add LINK to the linked list of assign links of RACC.  */
static void
add_link_to_rhs (struct access *racc, struct assign_link *link)
{
  gcc_assert (link->racc == racc);

  if (!racc->first_link)
    {
      gcc_assert (!racc->last_link);
      racc->first_link = link;
    }
  else
    racc->last_link->next = link;

  racc->last_link = link;
  link->next = NULL;
}

/* Move all link structures in their linked list in OLD_RACC to the linked list
   in NEW_RACC.  */
static void
relink_to_new_repr (struct access *new_racc, struct access *old_racc)
{
  if (!old_racc->first_link)
    {
      gcc_assert (!old_racc->last_link);
      return;
    }

  if (new_racc->first_link)
    {
      gcc_assert (!new_racc->last_link->next);
      gcc_assert (!old_racc->last_link || !old_racc->last_link->next);

      new_racc->last_link->next = old_racc->first_link;
      new_racc->last_link = old_racc->last_link;
    }
  else
    {
      gcc_assert (!new_racc->last_link);

      new_racc->first_link = old_racc->first_link;
      new_racc->last_link = old_racc->last_link;
    }
  old_racc->first_link = old_racc->last_link = NULL;
}

/* Add ACCESS to the work queue (which is actually a stack).  */

static void
add_access_to_work_queue (struct access *access)
{
  if (!access->grp_queued)
    {
      gcc_assert (!access->next_queued);
      access->next_queued = work_queue_head;
      access->grp_queued = 1;
      work_queue_head = access;
    }
}

/* Pop an acess from the work queue, and return it, assuming there is one.  */

static struct access *
pop_access_from_work_queue (void)
{
  struct access *access = work_queue_head;

  work_queue_head = access->next_queued;
  access->next_queued = NULL;
  access->grp_queued = 0;
  return access;
}

/* Mark all representatives (pointed to by REPRESENTATIVES and those accessible
   from them by next_grp linked list) as potentially modified unless it can be
   proved some of them may be not.  Hopefully the declaration DECL and TYPE
   being changed can help us too later on (when better aliasing info is
   available early).  Both DECL and TYPE can be NULL, in that case, nothing can
   be assumed about them.  */

static void
invalidate_by_type_or_decl (VEC (access_p, heap) *representatives, tree decl,
			    tree type ATTRIBUTE_UNUSED, char const * reason)
{
  int i;
  struct access *repr;
  tree parm = DECL_ARGUMENTS (current_function_decl);

  if (dump_file)
    {
      fprintf(dump_file, "  Invalidating, reason: %s", reason);
      if (decl)
        {
          fprintf (dump_file, "  decl: ");
	  print_generic_expr (dump_file, decl, 0);
	}
      if (type)
        {
          fprintf (dump_file, "  type: ");
	  print_generic_expr (dump_file, type, 0);
	}
      if (stmt_no != -1)
	fprintf (dump_file, "  stmt_no: %d", stmt_no);
      fprintf(dump_file, "\n");
    }

  for (i = 0; i < func_param_count; i++, parm = TREE_CHAIN (parm))
    {
      repr = VEC_index (access_p, representatives, i);

      gcc_assert (parm);
      if (no_accesses_p (repr))
	continue;

      for (; repr; repr = repr->next_grp)
	{
	  if (!repr->grp_maybe_modified)
	    {
	      bool invalidate = true;

	      if (repr->stmt_no != -1
		  && (stmt_no == -1 || repr->stmt_no <= stmt_no))
		{
		  if (dump_file)
		    {
		      fprintf (dump_file, "    Not invalidating ");
		      print_generic_expr (dump_file, parm, 0);
		      fprintf (dump_file, " all reads are already done\n");
		    }
		  invalidate = false;
		}

	      /* FIXME: Try to use some alias information so that we can be
		 less conservative.  */

	      if (invalidate)
		{
		  if (dump_file)
		    {
		      fprintf (dump_file, "    Invalidated ");
		      print_generic_expr (dump_file, parm, 0);
		      if (type)
			{
			  fprintf (dump_file, "type: ");
			  print_generic_expr (dump_file, repr->type, 0);
			  fprintf (dump_file, "\n");
			}
		    }
		  repr->grp_maybe_modified = true;
		}
	    }
	}
    }
}

/* Mark all representatives (pointed to by REPRESENTATIVES and those accessible
   from them by next_grp linked list) as potentially modified unless it can be
   proved some of them may be not.  */

static void
invalidate_all (VEC (access_p, heap) *representatives, char const * reason)
{
  invalidate_by_type_or_decl (representatives, NULL, NULL, reason);
}

/* Mark all representatives (pointed to by REPRESENTATIVES and those accessible
   from them by next_grp linked list) as potentially modified if a write to
   expression T can modify them.  */


static inline void
check_op_modifications (VEC (access_p, heap) *representatives, tree t)
{
  while (t && handled_component_p (t))
    t = TREE_OPERAND (t, 0);
  if (!t)
    return;
  if (TREE_CODE (t) == VAR_DECL && (TREE_STATIC (t) || DECL_EXTERNAL (t))
      && TREE_ADDRESSABLE (t))
    invalidate_by_type_or_decl (representatives, t, TREE_TYPE (t),
				"static variable write");
  if (INDIRECT_REF_P (t) || TREE_CODE (t) == TARGET_MEM_REF)
    invalidate_by_type_or_decl (representatives, NULL, TREE_TYPE (t),
				"indirect reference");
}

/* Check whether any representative (in a linked list pointed to by
   REPRESENTATIVES) is potentially modified by a call statement and mark it so
   if it is.  Note: LHS of the statement is not checked because that is
   recorded automatically then the corresponding access is created.  */

static inline void
check_call (VEC (access_p, heap) *representatives, gimple call)
{
  int flags = gimple_call_flags (call);
  tree callee_t = gimple_call_fndecl (call);

  if (flags & (ECF_CONST | ECF_PURE))
    return;
  /* Recursive calls are safe.  */
  if (callee_t == current_function_decl)
    return;
  invalidate_all (representatives, "non-pure call");
}

/* Look into pointer pointed to by GSIP and figure out what interesting side
   effects it have, particularly if any representative (reachable from a linked
   lists pointed to by REPRESENTATIVES) can by modified b any of them.  */

static void
check_stmt_modifications (gimple_stmt_iterator *gsip,
			  VEC (access_p, heap) *representatives)
{
  gimple stmt = gsi_stmt (*gsip);
  unsigned int i;

  if (dump_file)
    {
      fprintf (dump_file, "  scanning for references: ");
      print_gimple_stmt (dump_file, stmt, 0, 0);
    }

  switch (gimple_code (stmt))
    {
    case GIMPLE_ASSIGN:
      check_op_modifications (representatives, gimple_assign_lhs (stmt));
      break;
    case GIMPLE_CALL:
      check_op_modifications (representatives, gimple_call_lhs (stmt));
      check_call (representatives, stmt);
      break;
    case GIMPLE_ASM:
      for (i = 0; i < gimple_asm_noutputs (stmt); i++)
         check_op_modifications (representatives,
				 TREE_VALUE (gimple_asm_output_op (stmt, i)));
      for (i = 0; i < gimple_asm_nclobbers (stmt); i++)
	{
	  tree op = gimple_asm_clobber_op (stmt, i);
	  if (simple_cst_equal(TREE_VALUE (op), memory_identifier_string) == 1)
	    invalidate_all (representatives, "asm memory clobber");
	}
    default:
      break;
    }
}


/* Analyze what representatives (in linked lists accessible from
   REPRESENTATIVES) can be modified by side effects of statements in the
   current function.  */

static void
analyze_modified_params (VEC (access_p, heap) *representatives)
{
  basic_block this_block;
  FOR_EACH_BB (this_block)
  {
    gimple_stmt_iterator gsi;
    struct walk_stmt_info wi;

    stmt_no = 0;
    memset (&wi, 0, sizeof (wi));
    for (gsi = gsi_start_bb (this_block); !gsi_end_p (gsi); gsi_next (&gsi))
      {
	if (this_block == safe_bb)
	  stmt_no++;
	else
	  stmt_no = -1;
	check_stmt_modifications (&gsi, representatives);
      }
    stmt_no = -1;
  }
}

/* Process BB which is a dominator of EXIT for parameter PARM by searching for
   an access to parm that dereference it and if there is one, marking all
   accesses to that or smaller offset as possible to dereference.  */

static void
process_dominator_bb (tree parm, basic_block bb)
{
  int i, access_count;
  VEC (access_p, heap) *access_vec;
  bool hit = false;
  HOST_WIDE_INT offset = 0;

  access_vec = get_base_access_vector (parm);
  if (!access_vec)
    return;
  access_count = VEC_length (access_p, access_vec);

  for (i = 0; i < access_count; i++)
    {
      struct access *access = VEC_index (access_p, access_vec, i);

      if (access->bb != bb)
	continue;

      hit = true;
      if (access->offset > offset)
	offset = access->offset;
    }

  if (!hit)
    return;

  for (i = 0; i < access_count; i++)
    {
      struct access *access = VEC_index (access_p, access_vec, i);

      if (access->offset <= offset)
	access->always_safe = 1;
    }
  return;
}

/* Determine whether we would need to add fake edges in order to guarantee
   dereference legality in callers.  See the fixme in a comment in
   analyze_caller_dereference_legality for some insight why we do not actually
   add the edges. */
static bool
fake_edges_required_p (void)
{
  basic_block bb;

  if (encountered_external_throw)
    return true;

  FOR_EACH_BB (bb)
  {
    edge_iterator ei;
    edge e;

    FOR_EACH_EDGE (e, ei, bb->succs)
      {
	if (e->flags & EDGE_DFS_BACK)
	  return true;
      }
  }
  return false;
}

/* Determine what reduced parameters passed by reference are definitely
   dereferenced so that the dereferencing can be safely moved to the caller. */

static void
analyze_caller_dereference_legality (void)
{
  basic_block entry = ENTRY_BLOCK_PTR_FOR_FUNCTION (cfun);
  basic_block bb = EXIT_BLOCK_PTR_FOR_FUNCTION (cfun);

  /* FIXME: Dominance does not work for the EXIT block.  Until this is fixed,
     we can use instead it's only predecessor If it has only one.  In other
     cases, we'll just check the first basic block.

     Moreover, when there are statements which can throw externally or loops
     (which might just never terminate) we would normally need to add a fake
     edge from such block to the exit block.  That would, however, make the
     exit block have multiple predecessors and so in such cases, we also just
     check the first basic block.
  */
  if (!single_pred_p (bb) || fake_edges_required_p ())
    {
      tree parm;
      for (parm = DECL_ARGUMENTS (current_function_decl);
	   parm;
	   parm = TREE_CHAIN (parm))
	{
	  if (bitmap_bit_p (candidate_bitmap, DECL_UID (parm)))
	    process_dominator_bb (parm, single_succ (entry));
	}

      return;
    }

  bb = single_pred (bb);
  while (bb && bb != entry)
    {
      tree parm;
      for (parm = DECL_ARGUMENTS (current_function_decl);
	   parm;
	   parm = TREE_CHAIN (parm))
	{
	  if (bitmap_bit_p (candidate_bitmap, DECL_UID (parm)))
	    process_dominator_bb (parm, bb);
	}

      bb = get_immediate_dominator (CDI_DOMINATORS, bb);
    }

  return;
}

/* Allocate necessary structures.  */

static void
sra_initialize (void)
{
  safe_bb = single_succ_edge (ENTRY_BLOCK_PTR)->dest;
  if (!single_pred_p (safe_bb))
    safe_bb = NULL;
  candidate_bitmap = BITMAP_ALLOC (NULL);
  gcc_obstack_init (&name_obstack);
  access_pool = create_alloc_pool ("SRA accesses", sizeof (struct access), 16);
  link_pool = create_alloc_pool ("SRA links", sizeof (struct assign_link), 16);
  base_access_vec = pointer_map_create ();
  encountered_va_start = false;
  encountered_external_throw = false;
}

/* Hook fed to pointer_map_traverse, deallocate stored vectors.  */

static bool
delete_base_accesses (const void *key ATTRIBUTE_UNUSED, void **value,
		     void *data ATTRIBUTE_UNUSED)
{
  VEC (access_p, heap) *access_vec;
  access_vec = (VEC (access_p, heap) *) *value;
  VEC_free (access_p, heap, access_vec);

  return true;
}


/* Deallocate all general structures.  */

static void
sra_deinitialize (void)
{
  BITMAP_FREE (candidate_bitmap);
  free_alloc_pool (access_pool);
  free_alloc_pool (link_pool);
  obstack_free (&name_obstack, NULL);

  pointer_map_traverse (base_access_vec, delete_base_accesses, NULL);
  pointer_map_destroy (base_access_vec);
}

/* Remove DECL from candidates for SRA and write REASON to the dump file if
   there is one.  */
static void
disqualify_candidate (tree decl, const char *reason)
{
  bitmap_clear_bit (candidate_bitmap, DECL_UID (decl));

  if (dump_file)
    {
      fprintf (dump_file, "! Disqualifying ");
      print_generic_expr (dump_file, decl, 0);
      fprintf (dump_file, " - %s\n", reason);
    }
}


/* Return true iff the type contains a field or element type which does not
   allow scalarization.  */

static bool
type_internals_preclude_sra_p (tree type)
{
  tree fld;
  tree et;

  switch (TREE_CODE (type))
    {
    case RECORD_TYPE:
    case UNION_TYPE:
    case QUAL_UNION_TYPE:
      for (fld = TYPE_FIELDS (type); fld; fld = TREE_CHAIN (fld))
	if (TREE_CODE (fld) == FIELD_DECL)
	  {
	    tree ft = TREE_TYPE (fld);

	    if (TREE_THIS_VOLATILE (fld)
		|| !DECL_FIELD_OFFSET (fld) || !DECL_SIZE (fld)
		|| !host_integerp (DECL_FIELD_OFFSET (fld), 1)
		|| !host_integerp (DECL_SIZE (fld), 1))
	      return true;

	    if (AGGREGATE_TYPE_P (ft)
		&& type_internals_preclude_sra_p (ft))
	      return true;
	  }

      return false;

    case ARRAY_TYPE:
      et = TREE_TYPE (type);

      if (AGGREGATE_TYPE_P (et))
	return type_internals_preclude_sra_p (et);
      else
	return false;

    default:
      return false;
    }
}

/* Identify candidates for reduction for IPA-SRA based on their type and mark
   them in candidate_bitmap.  Note that these do not necessarily include
   parameter which are unused and thus can be removed.  Return true iff any
   such candidate has been found.  */

static bool
find_param_candidates (void)
{
  tree parm;
  int count = 0;
  bool ret = false;

  for (parm = DECL_ARGUMENTS (current_function_decl);
       parm;
       parm = TREE_CHAIN (parm))
    {
      tree type;

      count++;
      if (TREE_THIS_VOLATILE (parm))
	continue;

      type = TREE_TYPE (parm);
      if (POINTER_TYPE_P (type))
	{
	  type = TREE_TYPE (type);

	  if ((!is_gimple_reg_type (type) && !AGGREGATE_TYPE_P (type))
	      || TREE_CODE (type) == FUNCTION_TYPE
	      || TYPE_VOLATILE (type))
	    continue;
	}
      else if (!AGGREGATE_TYPE_P (type))
	continue;

      if (!COMPLETE_TYPE_P (type)
	  || TREE_ADDRESSABLE (type)
	  || !host_integerp (TYPE_SIZE (type), 1)
          || tree_low_cst (TYPE_SIZE (type), 1) == 0)
	continue;

      if (AGGREGATE_TYPE_P (type)
	  && type_internals_preclude_sra_p (type))
	continue;

      bitmap_set_bit (candidate_bitmap, DECL_UID (parm));
      ret = true;
      if (dump_file)
	{
	  fprintf (dump_file, "Candidate (%d): ", DECL_UID (parm));
	  print_generic_expr (dump_file, parm, 0);
	  fprintf (dump_file, "\n");
	}
    }

  func_param_count = count;
  return ret;
}

/* If T is an SSA_NAME, return NULL if it is not a default def or return its
   base variable if it is.  Return T if it is not an SSA_NAME.  */

static tree
get_ssa_base_param (tree t)
{
  if (TREE_CODE (t) == SSA_NAME)
    {
      if (SSA_NAME_IS_DEFAULT_DEF (t))
	return SSA_NAME_VAR (t);
      else
	return NULL_TREE;
    }
  return t;
}

/* Create and insert access for EXPR. Return created access, or NULL if it is
   not possible.  */

static struct access *
create_access (tree expr, bool write)
{
  struct access *access;
  void **slot;
  VEC (access_p,heap) *vec;
  HOST_WIDE_INT offset, size, max_size;
  tree base = expr;
  bool ptr = false, unscalarizable_region = false;

  if (handled_component_p (expr))
    {
      base = get_ref_base_and_extent (expr, &offset, &size, &max_size);

      if (TREE_CODE (base) == INDIRECT_REF)
	{
	  base = TREE_OPERAND (base, 0);
	  ptr = true;
	}
    }
  else
    {
      tree tree_size;

      if (TREE_CODE (base) == INDIRECT_REF)
	{
	  base = TREE_OPERAND (base, 0);
	  ptr = true;
	  tree_size = TYPE_SIZE (TREE_TYPE (TREE_TYPE (base)));
	}
      else
	tree_size = TYPE_SIZE (TREE_TYPE (base));

      if (tree_size && host_integerp (tree_size, 1))
	size = max_size = tree_low_cst (tree_size, 1);
      else
	size = max_size = -1;

      offset = 0;
    }

  if (sra_mode == SRA_MODE_EARLY_IPA)
    base = get_ssa_base_param (base);

  if (!base || !DECL_P (base)
      || (ptr && TREE_CODE (base) != PARM_DECL)
      || !bitmap_bit_p (candidate_bitmap, DECL_UID (base)))
    return NULL;

  if (sra_mode == SRA_MODE_EARLY_IPA)
    {
      if (size < 0 || size != max_size)
	{
	  disqualify_candidate (base, "Encountered a variable sized access.");
	  return NULL;
	}
      else if ((offset % BITS_PER_UNIT) != 0 || (size % BITS_PER_UNIT) != 0)
	{
	  disqualify_candidate (base,
				"Encountered an acces not aligned to a byte.");
	  return NULL;
	}
    }
  else
    {
      if (size != max_size)
	{
	  size = max_size;
	  unscalarizable_region = true;
	}

      if (size < 0)
	{
	  disqualify_candidate (base, "Encountered an ultra variable sized "
				"access.");
	  return NULL;
	}
    }

  access = (struct access *) pool_alloc (access_pool);
  memset (access, 0, sizeof (struct access));

  access->base = base;
  access->offset = offset;
  access->size = size;
  access->expr = expr;
  access->type = TREE_TYPE (expr);
  access->write = write;
  access->stmt_no = stmt_no;
  access->bb = current_bb;
  access->grp_unscalarizable_region = unscalarizable_region;

  slot = pointer_map_contains (base_access_vec, base);
  if (slot)
    vec = (VEC (access_p, heap) *) *slot;
  else
    vec = VEC_alloc (access_p, heap, 32);

  VEC_safe_push (access_p, heap, vec, access);

  *((struct VEC (access_p,heap) **)
	pointer_map_insert (base_access_vec, base)) = vec;

  return access;
}


/* Callback of walk_tree.  Search the given tree for a declaration and exclude
   it from the candidates.  */

static tree
disqualify_all (tree *tp, int *walk_subtrees, void *data ATTRIBUTE_UNUSED)
{
  tree base = *tp;


  if (TREE_CODE (base) == SSA_NAME)
    base = SSA_NAME_VAR (base);

  if (DECL_P (base))
    {
      disqualify_candidate (base, "From within disqualify_all().");
      *walk_subtrees = 0;
    }
  else
    *walk_subtrees = 1;


  return NULL_TREE;
}

/* See if OP is an undereferenced use of pointer parameters and if it is,
   exclude it from the candidates and return true, otherwise return false.  */
static bool
disqualify_direct_ptr_params (tree op)
{
  if (!op)
    return false;

  op = get_ssa_base_param (op);

  if (op && TREE_CODE (op) == PARM_DECL && POINTER_TYPE_P (TREE_TYPE (op)))
    {
      disqualify_candidate (op, " Direct use of its pointer value.");
      return true;
    }
  return false;
}

/* Scan expression EXPR and create access structures for all accesses to
   candidates for scalarization.  Return the created access or NULL if none is
   created.  */

static struct access *
build_access_from_expr_1 (tree *expr_ptr,
			gimple_stmt_iterator *gsi ATTRIBUTE_UNUSED, bool write)
{
  struct access *ret = NULL;
  tree expr = *expr_ptr;
  tree safe_expr = expr;
  bool bit_ref;

  if (sra_mode == SRA_MODE_EARLY_IPA)
    {
      while (TREE_CODE (expr) == NOP_EXPR
	     || TREE_CODE (expr) == VIEW_CONVERT_EXPR)
	expr = TREE_OPERAND (expr, 0);

      if (disqualify_direct_ptr_params (expr))
	return NULL;
      bit_ref = false;
    }
  else
    {
      if (TREE_CODE (expr) == BIT_FIELD_REF)
	{
	  expr = TREE_OPERAND (expr, 0);
	  bit_ref = true;
	}
      else
	bit_ref = false;

      while (TREE_CODE (expr) == NOP_EXPR
	     || TREE_CODE (expr) == VIEW_CONVERT_EXPR
	     || TREE_CODE (expr) == REALPART_EXPR
	     || TREE_CODE (expr) == IMAGPART_EXPR)
	expr = TREE_OPERAND (expr, 0);
    }

  switch (TREE_CODE (expr))
    {
    case SSA_NAME:
    case INDIRECT_REF:
    case VAR_DECL:
    case PARM_DECL:
    case RESULT_DECL:
    case COMPONENT_REF:
    case ARRAY_REF:
      ret = create_access (expr, write);
      break;

    case ADDR_EXPR:
      if (sra_mode == SRA_MODE_EARLY_IPA)
	walk_tree (&safe_expr, disqualify_all, NULL, NULL);
      break;

    case REALPART_EXPR:
    case IMAGPART_EXPR:
      if (sra_mode != SRA_MODE_EARLY_IPA)
	{
	  expr = TREE_OPERAND (expr, 0);
	  ret = create_access (expr, write);
	  break;
	}
      /* conditional fall through  */

    case ARRAY_RANGE_REF:
    default:
      walk_tree (&safe_expr, disqualify_all, NULL, NULL);
      break;
    }

  if (write && bit_ref && ret)
    ret->grp_bfr_lhs = 1;

  return ret;
}

/* Scan expression EXPR and create access structures for all accesses to
   candidates for scalarization.  Return true if any access has been
   inserted.  */

static bool
build_access_from_expr (tree *expr_ptr,
			gimple_stmt_iterator *gsi ATTRIBUTE_UNUSED, bool write,
			void *data ATTRIBUTE_UNUSED)
{
  return build_access_from_expr_1 (expr_ptr, gsi, write) != NULL;
}

/* Disqualify LHS and RHS for scalarization if STMT must end its basic block in
   modes in which it matters, return true iff they have been disqualified.  RHS
   may be NULL, in that case ignore it.  If we scalarize an aggregate in
   intra-SRA we may need to add statements after each statement.  This is not
   possible if a statement unconditionally has to end the basic block.  */
static bool
disqualify_ops_if_throwing_stmt (gimple stmt, tree *lhs, tree *rhs)
{
  if ((sra_mode == SRA_MODE_EARLY_INTRA || sra_mode == SRA_MODE_INTRA)
      && (stmt_can_throw_internal (stmt) || stmt_ends_bb_p (stmt)))
    {
      walk_tree (lhs, disqualify_all, NULL, NULL);
      if (rhs)
	walk_tree (rhs, disqualify_all, NULL, NULL);
      return true;
    }
  return false;
}


/* Result code for scan_assign callback for scan_function.  */
enum scan_assign_result {SRA_SA_NONE,       /* nothing done for the stmt */
			 SRA_SA_PROCESSED,  /* stmt analyzed/changed */
			 SRA_SA_REMOVED};   /* stmt redundant and eliminated */


/* Scan expressions occuring in the statement pointed to by STMT_EXPR, create
   access structures for all accesses to candidates for scalarization and
   remove those candidates which occur in statements or expressions that
   prevent them from being split apart.  Return true if any access has been
   inserted.  */

static enum scan_assign_result
build_accesses_from_assign (gimple *stmt_ptr,
			    gimple_stmt_iterator *gsi ATTRIBUTE_UNUSED,
			    void *data ATTRIBUTE_UNUSED)
{
  gimple stmt = *stmt_ptr;
  tree *lhs_ptr, *rhs_ptr;
  struct access *lacc, *racc;

  if (sra_mode == SRA_MODE_EARLY_IPA
      && TREE_CODE (gimple_assign_rhs1 (stmt)) == CONSTRUCTOR)
    {
      walk_tree (gimple_assign_lhs_ptr (stmt), disqualify_all, NULL, NULL);
      return SRA_SA_NONE;
    }

  if (gimple_assign_rhs2 (stmt))
    {
      if (sra_mode == SRA_MODE_EARLY_IPA)
	{
	  disqualify_direct_ptr_params (gimple_assign_rhs1 (stmt));
	  disqualify_direct_ptr_params (gimple_assign_rhs2 (stmt));
	}

      return SRA_SA_NONE;
    }

  lhs_ptr = gimple_assign_lhs_ptr (stmt);
  rhs_ptr = gimple_assign_rhs1_ptr (stmt);

  if (disqualify_ops_if_throwing_stmt (stmt, lhs_ptr, rhs_ptr))
    return SRA_SA_NONE;

  racc = build_access_from_expr_1 (rhs_ptr, gsi, false);
  lacc = build_access_from_expr_1 (lhs_ptr, gsi, true);

  if (lacc && racc
      && !lacc->grp_unscalarizable_region
      && !racc->grp_unscalarizable_region
      && (sra_mode == SRA_MODE_EARLY_INTRA || sra_mode == SRA_MODE_INTRA)
      && AGGREGATE_TYPE_P (TREE_TYPE (*lhs_ptr))
      && useless_type_conversion_p (lacc->type, racc->type))
    {
      struct assign_link *link;

      gcc_assert (lacc->size == racc->size);
      link = (struct assign_link *) pool_alloc (link_pool);
      memset (link, 0, sizeof (struct assign_link));

      link->lacc = lacc;
      link->racc = racc;

      add_link_to_rhs (racc, link);
    }

  return (lacc || racc) ? SRA_SA_PROCESSED : SRA_SA_NONE;
}

/* If ANALYSIS_STAGE is true disqualify all parameters that have their address
   taken in a phi node of basic block BB and, if non-NULL, call HANDLE_SSA_DEFS
   on each such phi node.  Return true iff any call to HANDLE_SSA_DEFS did
   so.  */

static bool
scan_phi_nodes (basic_block bb, bool analysis_stage,
		bool (*handle_ssa_defs)(gimple, void *), void *data)
{
  gimple_stmt_iterator gsi;
  bool ret = false;
  for (gsi = gsi_start_phis (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    {
      gimple phi = gsi_stmt (gsi);
      use_operand_p arg_p;
      ssa_op_iter i;
      bool any = false;

      if (analysis_stage)
	FOR_EACH_PHI_ARG (arg_p, phi, i, SSA_OP_USE)
	  {
	    tree op = USE_FROM_PTR (arg_p);
	    if (TREE_CODE (op) == ADDR_EXPR)
	      {
		op = TREE_OPERAND (op, 0);
		if (DECL_P (op))
		  disqualify_candidate (op,
					"Its address is taken in a phi node.");
	      }
	    else
	      disqualify_direct_ptr_params (op);
	  }

      if (handle_ssa_defs)
	ret |= handle_ssa_defs (phi, data);
      if (any)
	{
	  ret = true;

	  if (!analysis_stage)
	    update_stmt (phi);
	}
    }
  return ret;
}

/* Scan function and look for interesting statements. Return true if any has
   been found or processed, as indicated by callbacks.  SCAN_EXPR is a callback
   called on all expressions within statements except assign statements and
   those deemed entirely unsuitable for some reason (all operands in such
   statements and expression are removed from candidate_bitmap).  SCAN_ASSIGN
   is a callback called on all assign statements, HANDLE_SSA_DEFS is a callback
   called on assign statements and those call statements which have a lhs and
   it is the only callback which can be NULL. ANALYSIS_STAGE is true when
   running in the analysis stage of a pass and thus no statement is being
   modified.  DATA is a pointer passed to all callbacks.  If any single
   callback returns true, this function also returns true, otherwise it returns
   false.  */

static bool
scan_function (bool (*scan_expr) (tree *, gimple_stmt_iterator *, bool, void *),
	       enum scan_assign_result (*scan_assign) (gimple *,
						       gimple_stmt_iterator *,
						       void *),
	       bool (*handle_ssa_defs)(gimple, void *),
	       bool analysis_stage, void *data)
{
  gimple_stmt_iterator gsi;
  basic_block bb;
  unsigned i;
  tree *t;
  bool ret = false;

  FOR_EACH_BB (bb)
    {
      bool bb_changed = false;
      current_bb = bb;

      if (sra_mode == SRA_MODE_EARLY_IPA)
	scan_phi_nodes (bb, analysis_stage, handle_ssa_defs, data);

      stmt_no = 0;
      gsi = gsi_start_bb (bb);
      while (!gsi_end_p (gsi))
	{
	  gimple stmt = gsi_stmt (gsi);
	  enum scan_assign_result assign_result;
	  bool any = false, deleted = false;

	  if (stmt_can_throw_external (stmt))
	    encountered_external_throw = true;

	  if (bb == safe_bb)
	    stmt_no ++;
	  else
	    stmt_no = -1;
	  switch (gimple_code (stmt))
	    {
	    case GIMPLE_RETURN:
	      t = gimple_return_retval_ptr (stmt);
	      if (*t != NULL_TREE)
		any |= scan_expr (t, &gsi, false, data);
	      break;

	    case GIMPLE_ASSIGN:
	      assign_result = scan_assign (&stmt, &gsi, data);
	      any |= assign_result == SRA_SA_PROCESSED;
	      deleted = assign_result == SRA_SA_REMOVED;
	      if (handle_ssa_defs && assign_result != SRA_SA_REMOVED)
		any |= handle_ssa_defs (stmt, data);
	      break;

	    case GIMPLE_CALL:
	      if (analysis_stage
		  && (gimple_call_fndecl (stmt)
		      == built_in_decls[BUILT_IN_VA_START]))
		encountered_va_start = true;

	      /* Operands must be processed before the lhs.  */
	      for (i = 0; i < gimple_call_num_args (stmt); i++)
		{
		  tree *argp = gimple_call_arg_ptr (stmt, i);
		  any |= scan_expr (argp, &gsi, false, data);
		}

	      if (gimple_call_lhs (stmt))
		{
		  tree *lhs_ptr = gimple_call_lhs_ptr (stmt);
		  if (!analysis_stage ||
		      !disqualify_ops_if_throwing_stmt (stmt, lhs_ptr, NULL))
		    {
		      any |= scan_expr (lhs_ptr, &gsi, true, data);
		      if (handle_ssa_defs)
			any |= handle_ssa_defs (stmt, data);
		    }
		}
	      break;

	    case GIMPLE_ASM:
	      for (i = 0; i < gimple_asm_ninputs (stmt); i++)
		{
		  tree *op = &TREE_VALUE (gimple_asm_input_op (stmt, i));
		  any |= scan_expr (op, &gsi, false, data);
		}
	      for (i = 0; i < gimple_asm_noutputs (stmt); i++)
		{
		  tree *op = &TREE_VALUE (gimple_asm_output_op (stmt, i));
		  any |= scan_expr (op, &gsi, true, data);
		}

	    default:
	      if (analysis_stage)
		walk_gimple_op (stmt, disqualify_all, NULL);
	      break;
	    }

	  if (any)
	    {
	      ret = true;
	      bb_changed = true;

	      if (!analysis_stage)
		{
		  update_stmt (stmt);
		  if (!stmt_could_throw_p (stmt))
		    remove_stmt_from_eh_region (stmt);
		}
	    }
	  if (deleted)
	    bb_changed = true;
	  else
	    {
	      gsi_next (&gsi);
	      ret = true;
	    }
	}
      stmt_no = -1;
      if (!analysis_stage && bb_changed)
	gimple_purge_dead_eh_edges (bb);
    }

  return ret;
}

/* Return the representative access for the parameter declaration PARM if it is
   a scalar passed by reference which is not written to and the pointer value
   is not used directly.  Thus, if it is legal to dereference it in the caller
   and we can rule out modifications through aliases, such parameter should be
   turned into one passed by value.  Return NULL otherwise.  */

static struct access *
unmodified_by_ref_scalar_representative (tree parm)
{
  int i, access_count;
  struct access *access;
  VEC (access_p, heap) *access_vec;

  access_vec = get_base_access_vector (parm);
  gcc_assert (access_vec);
  access_count = VEC_length (access_p, access_vec);

  for (i = 0; i < access_count; i++)
    {
      access = VEC_index (access_p, access_vec, i);
      if (access->write)
	return NULL;
    }

  access = VEC_index (access_p, access_vec, 0);
  access->grp_read = 1;
  access->grp_scalar_ptr = 1;
  return access;
}

/* Helper of QSORT function. There are pointers to accesses in the array.  An
   access is considered smaller than another if it has smaller offset or if the
   offsets are the same but is size is bigger. */

static int
compare_access_positions (const void *a, const void *b)
{
  const access_p *fp1 = (const access_p *) a;
  const access_p *fp2 = (const access_p *) b;
  const access_p f1 = *fp1;
  const access_p f2 = *fp2;

  if (f1->offset != f2->offset)
    return f1->offset < f2->offset ? -1 : 1;

  if (f1->size == f2->size)
    return 0;
  /* We want the bigger accesses first, thus the opposite operator in the next
     line: */
  return f1->size > f2->size ? -1 : 1;
}


/* Sort collected accesses for parameter PARM, identify representatives for
   each accessed region and link them together.  Return NULL if there are no
   accesses or if there are different but overlapping accesses, return the
   special ptr value meaning there are no accesses for this parameter if that
   is the case and return the first representative otherwise.  If non-NULL, set
   *RO_GRP if there is a group of accesses with only read (i.e. no write)
   accesses. */

static struct access *
splice_param_accesses (tree parm, bool *ro_grp)
{
  int i, j, access_count, group_count;
  int agg_size, total_size = 0;
  struct access *access, *res, **prev_acc_ptr = &res;
  VEC (access_p, heap) *access_vec;

  access_vec = get_base_access_vector (parm);
  if (!access_vec)
    return &no_accesses_representant;
  access_count = VEC_length (access_p, access_vec);

  /* Sort by <OFFSET, SIZE>.  */
  qsort (VEC_address (access_p, access_vec), access_count, sizeof (access_p),
	 compare_access_positions);

  if (dump_file)
    {
      fprintf (dump_file, "Splicing PARAM accesses for ");
      print_generic_expr (dump_file, parm, 0);
      fprintf (dump_file, " (UID: %u): \n", DECL_UID (parm));
      for (i = 0; i < access_count; i++)
	dump_access (VEC_index (access_p, access_vec, i), false);
    }

  i = 0;
  total_size = 0;
  group_count = 0;
  while (i < access_count)
    {
      bool modification;
      access = VEC_index (access_p, access_vec, i);
      modification = access->write;

      /* Access is about to become group representative unless we find some
	 nasty overlap which would preclude us from breaking this parameter
	 apart. */

      j = i + 1;
      while (j < access_count)
	{
	  struct access *ac2 = VEC_index (access_p, access_vec, j);
	  if (ac2->offset != access->offset)
	    {
	      /* All or nothing law for parameters. */
	      if (access->offset + access->size > ac2->offset)
		return NULL;
	      else
		break;
	    }
	  else if (ac2->size != access->size)
	    return NULL;

	  modification |= ac2->write;
	  if (ac2->stmt_no == -1
	      || (access->stmt_no != 1 && ac2->stmt_no > access->stmt_no))
	    access->stmt_no = ac2->stmt_no;

	  j++;
	}

      group_count++;
      access->grp_maybe_modified = modification;
      if (!modification && ro_grp)
	*ro_grp = true;
      *prev_acc_ptr = access;
      prev_acc_ptr = &access->next_grp;
      total_size += access->size;
      i = j;
    }

  if (POINTER_TYPE_P (TREE_TYPE (parm)))
    agg_size = tree_low_cst (TYPE_SIZE (TREE_TYPE (TREE_TYPE (parm))), 1);
  else
    agg_size = tree_low_cst (TYPE_SIZE (TREE_TYPE (parm)), 1);
  if (total_size >= agg_size)
    return NULL;

  gcc_assert (group_count > 0);
  return res;
}

/* Decide whether parameters with representative accesses given by REPR should
   be reduced into components.  */

static int
decide_one_param_reduction (struct access *repr)
{
  int total_size, cur_parm_size, agg_size, new_param_count;
  bool by_ref;
  tree parm;

  parm = repr->base;
  gcc_assert (TREE_CODE (parm) == PARM_DECL);
  cur_parm_size = tree_low_cst (TYPE_SIZE (TREE_TYPE (parm)), 1);
  gcc_assert (cur_parm_size > 0);

  if (POINTER_TYPE_P (TREE_TYPE (parm)))
    {
      by_ref = true;
      agg_size = tree_low_cst (TYPE_SIZE (TREE_TYPE (TREE_TYPE (parm))), 1);
    }
  else
    {
      by_ref = false;
      agg_size = cur_parm_size;
    }

  if (dump_file)
    {
      struct access *acc;
      fprintf (dump_file, "Evaluating PARAM group sizes for ");
      print_generic_expr (dump_file, parm, 0);
      fprintf (dump_file, " (UID: %u): \n", DECL_UID (parm));
      for (acc = repr; acc; acc = acc->next_grp)
	dump_access (acc, true);
    }

  total_size = 0;
  new_param_count = 0;

  for (; repr; repr = repr->next_grp)
    {
      gcc_assert (parm == repr->base);
      new_param_count++;

      if (!by_ref || (!repr->grp_maybe_modified && repr->always_safe))
	total_size += repr->size;
      else
	total_size += cur_parm_size;
    }

  gcc_assert (new_param_count > 0);
  /* FIXME: 2 probably needs to be replaced by a parameter */
  if (total_size < agg_size
      && total_size <= 2 * cur_parm_size)
    {
      if (dump_file)
	fprintf (dump_file, "    ....will be split into %i components\n",
		 new_param_count);
      return new_param_count;
    }
  else
    return 0;
}

/* Return true iff PARM (which must be a parm_decl) is an unused scalar
   parameter.  */

static bool
is_unused_scalar_param (tree parm)
{
  tree name;
  return (is_gimple_reg (parm)
	  && (!(name = gimple_default_def (cfun, parm))
	      || has_zero_uses(name)));
}



/* The order of the following enums is important, we need to do extra work for
   UNUSED_PARAMS, BY_VAL_ACCESSES and UNMODIF_BY_REF_ACCESSES.  */
enum ipa_splicing_result {NO_GOOD_ACCESS, UNUSED_PARAMS, BY_VAL_ACCESSES,
			  MODIF_BY_REF_ACCESSES, UNMODIF_BY_REF_ACCESSES};

/* Identify representatives of all accesses to all candidate parameters for
   IPA-SRA.  Return result based on what representatives have been found. */

static enum ipa_splicing_result
splice_all_param_accesses (VEC (access_p, heap) **representatives)
{
  enum ipa_splicing_result result = NO_GOOD_ACCESS;
  tree parm;
  struct access *repr;

  *representatives = VEC_alloc (access_p, heap, func_param_count);

  for (parm = DECL_ARGUMENTS (current_function_decl);
       parm;
       parm = TREE_CHAIN (parm))
    {
      if (is_unused_scalar_param (parm))
	{
	  VEC_quick_push (access_p, *representatives,
			  &no_accesses_representant);
	  if (result == NO_GOOD_ACCESS)
	    result = UNUSED_PARAMS;
	}
      else if (POINTER_TYPE_P (TREE_TYPE (parm))
	       && is_gimple_reg_type (TREE_TYPE (TREE_TYPE (parm)))
	       && bitmap_bit_p (candidate_bitmap, DECL_UID (parm)))
	{
	  repr = unmodified_by_ref_scalar_representative (parm);
	  VEC_quick_push (access_p, *representatives, repr);
	  if (repr)
	    result = UNMODIF_BY_REF_ACCESSES;
	}
      else if (bitmap_bit_p (candidate_bitmap, DECL_UID (parm)))
	{
	  bool ro_grp = false;
	  repr = splice_param_accesses (parm, &ro_grp);
	  VEC_quick_push (access_p, *representatives, repr);

	  if (repr && !no_accesses_p (repr))
	    {
	      if (POINTER_TYPE_P (TREE_TYPE (parm)))
		{
		  if (ro_grp)
		    result = UNMODIF_BY_REF_ACCESSES;
		  else if (result < MODIF_BY_REF_ACCESSES)
		    result = MODIF_BY_REF_ACCESSES;
		}
	      else if (result < BY_VAL_ACCESSES)
		result = BY_VAL_ACCESSES;
	    }
	  else if (no_accesses_p (repr) && (result == NO_GOOD_ACCESS))
	    result = UNUSED_PARAMS;
	}
      else
	VEC_quick_push (access_p, *representatives, NULL);
    }

  if (result == NO_GOOD_ACCESS)
    {
      VEC_free (access_p, heap, *representatives);
      *representatives = NULL;
      return NO_GOOD_ACCESS;
    }

  return result;
}

/* Return the index of BASE in PARMS.  Abort if it i not found.  */

static inline int
get_param_index (tree base, VEC(tree, heap) *parms)
{
  int i, len;

  len = VEC_length (tree, parms);
  for (i = 0; i < len; i++)
    if (VEC_index (tree, parms, i) == base)
      return i;
  gcc_unreachable ();
}

/* Convert the decisions made at the representative level into compact notes.
   REPRESENTATIVES are pointers to first representatives of each param
   accesses, NOTE_COUNT is the expected final number of notes.  */

static VEC (ipa_parm_note_t, heap) *
turn_representatives_into_notes (VEC (access_p, heap) *representatives,
				 int note_count)
{
  VEC (tree, heap) *parms;
  VEC (ipa_parm_note_t, heap) *notes;
  tree parm;
  int i;

  gcc_assert (note_count > 0);
  parms = ipa_get_vector_of_formal_parms (current_function_decl);
  notes = VEC_alloc (ipa_parm_note_t, heap, note_count);
  parm = DECL_ARGUMENTS (current_function_decl);
  for (i = 0; i < func_param_count; i++, parm = TREE_CHAIN (parm))
    {
      struct access *repr = VEC_index (access_p, representatives, i);

      if (!repr || no_accesses_p (repr))
	{
	  struct ipa_parm_note *note;

	  note = VEC_quick_push (ipa_parm_note_t, notes, NULL);
	  memset (note, 0, sizeof (*note));
	  note->base_index = get_param_index (parm, parms);
	  note->base = parm;
	  if (!repr)
	    note->copy_param = 1;
	  else
	    note->remove_param = 1;
	}
      else
	{
	  struct ipa_parm_note *note;
	  int index = get_param_index (parm, parms);

	  for (; repr; repr = repr->next_grp)
	    {
	      note = VEC_quick_push (ipa_parm_note_t, notes, NULL);
	      memset (note, 0, sizeof (*note));
	      gcc_assert (repr->base == parm);
	      note->base_index = index;
	      note->base = repr->base;
	      note->type = repr->type;
	      note->offset = repr->offset;
	      note->by_ref = (POINTER_TYPE_P (TREE_TYPE (repr->base))
			      && (repr->grp_maybe_modified
				  || !repr->always_safe));

	    }
	}
    }
  VEC_free (tree, heap, parms);
  return notes;
}

/* Analyze the collected accesses and produce a plan what to do with the
   parameters in the form of notes, NULL meaning nothing.  */

static VEC (ipa_parm_note_t, heap) *
analyze_all_param_acesses (void)
{
  enum ipa_splicing_result repr_state;
  bool proceed = false;
  int i, note_count = 0;
  VEC (access_p, heap) *representatives;
  VEC (ipa_parm_note_t, heap) *notes;

  repr_state = splice_all_param_accesses (&representatives);
  if (repr_state == NO_GOOD_ACCESS)
    return NULL;

  /* If there are any parameters passed by reference which are not modified
     directly, we need to check whether they can be modified indirectly.  */
  if (repr_state == UNMODIF_BY_REF_ACCESSES)
    {
      analyze_caller_dereference_legality ();
      analyze_modified_params (representatives);
    }

  for (i = 0; i < func_param_count; i++)
    {
      struct access *repr = VEC_index (access_p, representatives, i);

      if (repr && !no_accesses_p (repr))
	{
	  if (repr->grp_scalar_ptr)
	    {
	      note_count++;
	      if (!repr->always_safe || repr->grp_maybe_modified)
		VEC_replace (access_p, representatives, i, NULL);
	      else
		proceed = true;
	    }
	  else
	    {
	      int new_components = decide_one_param_reduction (repr);

	      if (new_components == 0)
		{
		  VEC_replace (access_p, representatives, i, NULL);
		  note_count++;
		}
	      else
		{
		  note_count += new_components;
		  proceed = true;
		}
	    }
	}
      else
	{
	  if (no_accesses_p (repr))
	    proceed = true;
	  note_count++;
	}
    }

  if (!proceed && dump_file)
    fprintf (dump_file, "NOT proceeding to change params.\n");

  if (proceed)
    notes = turn_representatives_into_notes (representatives, note_count);
  else
    notes = NULL;

  VEC_free (access_p, heap, representatives);
  return notes;
}

/* Append a name of the declaration to the name obstack.  A helper function for
   make_fancy_name.  */

static void
make_fancy_decl_name (tree decl)
{
  char buffer[32];

  tree name = DECL_NAME (decl);
  if (name)
    obstack_grow (&name_obstack, IDENTIFIER_POINTER (name),
		  IDENTIFIER_LENGTH (name));
  else
    {
      sprintf (buffer, "D%u", DECL_UID (decl));
      obstack_grow (&name_obstack, buffer, strlen (buffer));
    }
}

/* Helper for make_fancy_name.  */

static void
make_fancy_name_1 (tree expr)
{
  char buffer[32];
  tree index;

  if (DECL_P (expr))
    {
      make_fancy_decl_name (expr);
      return;
    }

  switch (TREE_CODE (expr))
    {
    case COMPONENT_REF:
      make_fancy_name_1 (TREE_OPERAND (expr, 0));
      obstack_1grow (&name_obstack, '$');
      make_fancy_decl_name (TREE_OPERAND (expr, 1));
      break;

    case ARRAY_REF:
      make_fancy_name_1 (TREE_OPERAND (expr, 0));
      obstack_1grow (&name_obstack, '$');
      /* Arrays with only one element may not have a constant as their
	 index. */
      index = TREE_OPERAND (expr, 1);
      if (TREE_CODE (index) != INTEGER_CST)
	break;
      sprintf (buffer, HOST_WIDE_INT_PRINT_DEC, TREE_INT_CST_LOW (index));
      obstack_grow (&name_obstack, buffer, strlen (buffer));

      break;

    case BIT_FIELD_REF:
    case REALPART_EXPR:
    case IMAGPART_EXPR:
      gcc_unreachable (); 	/* we treat these as scalars.  */
      break;
    default:
      break;
    }
}

/* Create a human readable name for replacement variable of ACCESS. */
static char *
make_fancy_name (tree expr)
{
  make_fancy_name_1 (expr);
  obstack_1grow (&name_obstack, '\0');
  return XOBFINISH (&name_obstack, char *);
}

/* If a parameter replacement identified by NOTE does not yet exist in the form
   of declaration, create it and record it, otherwise return the previously
   created one.  */

static tree
get_replaced_param_substitute (struct ipa_parm_note *note)
{
  tree repl;
  if (!note->new_ssa_base)
    {
      char *pretty_name = make_fancy_name (note->base);

      repl = make_rename_temp (TREE_TYPE (note->base), "ISR");
      DECL_NAME (repl) = get_identifier (pretty_name);
      obstack_free (&name_obstack, pretty_name);

      get_var_ann (repl);
      add_referenced_var (repl);
      note->new_ssa_base = repl;
    }
  else
    repl = note->new_ssa_base;
  return repl;
}

/* Callback for scan_function.  If the statement STMT defines an SSA_NAME of a
   parameter which is to be removed because its value is not used, replace the
   SSA_NAME with a one relating to a created VAR_DECL and replace all of its
   uses too.  DATA is a pointer to a note vector.  */

static bool
replace_removed_params_ssa_names (gimple stmt, void *data)
{
  VEC (ipa_parm_note_t, heap) *notes = (VEC (ipa_parm_note_t, heap) *) data;
  tree lhs, decl;
  int i, len;

  if (gimple_code (stmt) == GIMPLE_PHI)
    lhs = gimple_phi_result (stmt);
  else if (is_gimple_assign (stmt))
    lhs = gimple_assign_lhs (stmt);
  else if (is_gimple_call (stmt))
    lhs = gimple_call_lhs (stmt);
  else
    gcc_unreachable ();

  if (TREE_CODE (lhs) != SSA_NAME)
    return false;
  decl = SSA_NAME_VAR (lhs);
  if (TREE_CODE (decl) != PARM_DECL)
    return false;

  len = VEC_length (ipa_parm_note_t, notes);
  for (i = 0; i < len; i++)
    {
      tree repl, name;
      struct ipa_parm_note *note = VEC_index (ipa_parm_note_t, notes, i);

      if (note->copy_param || note->base != decl)
	continue;

      gcc_assert (!SSA_NAME_IS_DEFAULT_DEF (lhs));
      repl = get_replaced_param_substitute (note);
      name = make_ssa_name (repl, stmt);

      if (dump_file)
	{
	  fprintf (dump_file, "replacing SSA name of removed param ");
	  print_generic_expr (dump_file, lhs, 0);
	  fprintf (dump_file, " with ");
	  print_generic_expr (dump_file, name, 0);
	  fprintf (dump_file, "\n");
	}

      if (is_gimple_assign (stmt))
	gimple_assign_set_lhs (stmt, name);
      else if (is_gimple_call (stmt))
	gimple_call_set_lhs (stmt, name);
      else
	gimple_phi_set_result (stmt, name);

      replace_uses_by (lhs, name);
      return true;
    }
  return false;
}

/* Callback for scan_function.  If the expression *EXPR should be replaced by a
   reduction of a parameter, do so.  DATA is a pointer to a vector of
   notes.  */

static bool
sra_ipa_modify_expr (tree *expr, gimple_stmt_iterator *gsi ATTRIBUTE_UNUSED,
		     bool write ATTRIBUTE_UNUSED, void *data)
{
  VEC (ipa_parm_note_t, heap) *notes = (VEC (ipa_parm_note_t, heap) *) data;
  int i, len = VEC_length (ipa_parm_note_t, notes);
  struct ipa_parm_note *note, *cand = NULL;
  HOST_WIDE_INT offset, size, max_size;
  tree base, src;

  while (TREE_CODE (*expr) == NOP_EXPR
	 || TREE_CODE (*expr) == VIEW_CONVERT_EXPR)
    expr = &TREE_OPERAND (*expr, 0);

  if (handled_component_p (*expr))
    {
      base = get_ref_base_and_extent (*expr, &offset, &size, &max_size);
      if (!base || size == -1 || max_size == -1)
	return false;

      if (TREE_CODE (base) == INDIRECT_REF)
	base = TREE_OPERAND (base, 0);

      base = get_ssa_base_param (base);
      if (!base || TREE_CODE (base) == INTEGER_CST)
	return false;
    }
  else if (TREE_CODE (*expr) == INDIRECT_REF)
    {
      tree tree_size;
      base = TREE_OPERAND (*expr, 0);

      base = get_ssa_base_param (base);
      if (!base || TREE_CODE (base) == INTEGER_CST)
	return false;

      offset = 0;
      tree_size = TYPE_SIZE (TREE_TYPE (base));
      if (tree_size && host_integerp (tree_size, 1))
	size = max_size = tree_low_cst (tree_size, 1);
      else
	return false;
    }
  else
    return false;

  gcc_assert (DECL_P (base));
  for (i = 0; i < len; i++)
    {
      note = VEC_index (ipa_parm_note_t, notes, i);

      if (note->base == base &&
	  (note->offset == offset || note->remove_param))
	{
	  cand = note;
	  break;
	}
    }
  if (!cand || cand->copy_param || cand->remove_param)
    return false;

  if (cand->by_ref)
    {
      tree folded;
      src = build1 (INDIRECT_REF, TREE_TYPE (TREE_TYPE (cand->reduction)),
		    cand->reduction);
      folded = gimple_fold_indirect_ref (src);
      if (folded)
        src = folded;
    }
  else
    src = cand->reduction;

  if (dump_file)
    {
      fprintf (dump_file, "About to replace expr ");
      print_generic_expr (dump_file, *expr, 0);
      fprintf (dump_file, " with ");
      print_generic_expr (dump_file, src, 0);
      fprintf (dump_file, "\n");
    }

  if (!useless_type_conversion_p (TREE_TYPE (*expr), cand->type))
    {
      tree vce = build1 (VIEW_CONVERT_EXPR, TREE_TYPE (*expr), src);
      *expr = vce;
    }
    else
      *expr = src;
  return true;
}

/* Callback for scan_function to process assign statements.  Performs
   essentially the same function like sra_ipa_modify_expr.  */

static enum scan_assign_result
sra_ipa_modify_assign (gimple *stmt_ptr,
		       gimple_stmt_iterator *gsi ATTRIBUTE_UNUSED, void *data)
{
  gimple stmt = *stmt_ptr;
  bool any = false;

  if (gimple_assign_rhs2 (stmt)
      || TREE_CODE (gimple_assign_rhs1 (stmt)) == CONSTRUCTOR)
    return SRA_SA_NONE;

  /* The order of processing rhs and lhs is important.  */
  any |= sra_ipa_modify_expr (gimple_assign_rhs1_ptr (stmt), gsi, false,
			      data);
  any |= sra_ipa_modify_expr (gimple_assign_lhs_ptr (stmt), gsi, true, data);

  return any ? SRA_SA_PROCESSED : SRA_SA_NONE;
}

/* helper function for build_access_expr.  */

static bool
build_ref_for_offset_1 (tree *res, tree type, HOST_WIDE_INT offset,
			tree exp_type)
{
  while (1)
    {
      tree fld;
      tree tr_size, index;
      HOST_WIDE_INT el_size;

      if (offset == 0 && exp_type
	  && useless_type_conversion_p (exp_type, type))
	return true;

      switch (TREE_CODE (type))
	{
	case UNION_TYPE:
	case QUAL_UNION_TYPE:
	case RECORD_TYPE:
	  /* Some ADA records are half-unions, treat all of them the same.  */
	  for (fld = TYPE_FIELDS (type); fld; fld = TREE_CHAIN (fld))
	    {
	      HOST_WIDE_INT pos, size;
	      tree expr, *expr_ptr;

	      if (TREE_CODE (fld) != FIELD_DECL)
		continue;

	      pos = int_bit_position (fld);
	      gcc_assert (TREE_CODE (type) == RECORD_TYPE || pos == 0);
	      size = tree_low_cst (DECL_SIZE (fld), 1);
	      if (pos > offset || (pos + size) <= offset)
		continue;

	      if (res)
		{
		  expr = build3 (COMPONENT_REF, TREE_TYPE (fld), *res, fld,
				 NULL_TREE);
		  expr_ptr = &expr;
		}
	      else
		expr_ptr = NULL;
	      if (build_ref_for_offset_1 (expr_ptr, TREE_TYPE (fld),
					  offset - pos, exp_type))
		{
		  if (res)
		    *res = expr;
		  return true;
		}
	    }
	  return false;

	case ARRAY_TYPE:
	  tr_size = TYPE_SIZE (TREE_TYPE (type));
	  if (!tr_size || !host_integerp (tr_size, 1))
	    return false;
	  el_size = tree_low_cst (tr_size, 1);

	  index = build_int_cst (TYPE_DOMAIN (type), offset / el_size);
	  if (!integer_zerop (TYPE_MIN_VALUE (TYPE_DOMAIN (type))))
	    index = int_const_binop (PLUS_EXPR, index,
				     TYPE_MIN_VALUE (TYPE_DOMAIN (type)), 0);
	  if (res)
	    *res = build4 (ARRAY_REF, TREE_TYPE (type), *res, index, NULL_TREE,
			   NULL_TREE);
	  offset = offset % el_size;
	  type = TREE_TYPE (type);
	  break;

	default:
	  if (offset != 0)
	    return false;

	  if (exp_type)
	    return false;
	  else
	    return true;
	}
    }
}

/* Construct an expression that would reference a part of aggregate *EXPR of
   type TYPE at the given OFFSET of the type EXP_TYPE.  If EXPR is NULL, the
   function only determines whether it can build such a reference without
   actually doing it.

   FIXME: Eventually this should be replaced with
   maybe_fold_offset_to_reference() from tree-ssa-ccp.c but that requires a
   minor rewrite of fold_stmt.
 */

bool
build_ref_for_offset (tree *expr, tree type, HOST_WIDE_INT offset,
		      tree exp_type, bool allow_ptr)
{
  if (allow_ptr && POINTER_TYPE_P (type))
    {
      type = TREE_TYPE (type);
      if (expr)
	*expr = fold_build1 (INDIRECT_REF, type, *expr);
    }

  return build_ref_for_offset_1 (expr, type, offset, exp_type);
}

/* Convert all callers of NODE to pass parameters as given in NOTES.  */

static void
convert_callers (struct cgraph_node *node, VEC (ipa_parm_note_t, heap) *notes)
{
  tree old_cur_fndecl = current_function_decl;
  struct cgraph_edge *cs;
  basic_block this_block;

  for (cs = node->callers; cs; cs = cs->next_caller)
    {
      current_function_decl = cs->caller->decl;
      push_cfun (DECL_STRUCT_FUNCTION (cs->caller->decl));

      if (dump_file)
	fprintf (dump_file, "Checking call %s -> %s\n",
		 cgraph_node_name (cs->caller),
		 cgraph_node_name (cs->callee));

      ipa_modify_call_arguments (cs, cs->call_stmt, notes);
      compute_inline_parameters (cs->caller);

      pop_cfun ();
    }
  current_function_decl = old_cur_fndecl;
  FOR_EACH_BB (this_block)
    {
      gimple_stmt_iterator gsi;

      for (gsi = gsi_start_bb (this_block); !gsi_end_p (gsi); gsi_next (&gsi))
        {
	  gimple stmt = gsi_stmt (gsi);
	  if (gimple_code (stmt) == GIMPLE_CALL
	      && gimple_call_fndecl (stmt) == node->decl)
	    {
	      if (dump_file)
		fprintf (dump_file, "Checking recursive call");
	      ipa_modify_call_arguments (NULL, stmt, notes);
	    }
	}
    }

  return;
}

/* Returns a constructor for aggregate of TYPE type which is a part of DECL at
   offset OFFSET and accessible through EXPR.  SEARCH is a callback function
   which locates values given their offset and size.  If EXPR is NULL, the
   parts of the aggregate which have not been located by SEARCH have been
   optimized out.  Returns error_mark_node if something goes wrong and this
   cannot be achieved, for example when array index bounds cannot be
   edetermined.  DATA is passed to SEARCH and not interpreted in any way.  */

static tree
create_debug_constructor (tree type, tree decl, tree expr,
			  HOST_WIDE_INT offset,
			  tree (*search) (tree, HOST_WIDE_INT, HOST_WIDE_INT,
					  void *), void *data)
{
  tree fld, el, domain, index, max, tree_size;
  HOST_WIDE_INT size;
  VEC(constructor_elt,gc) *vals = VEC_alloc (constructor_elt, gc, 8);

  gcc_assert (AGGREGATE_TYPE_P (type));

  switch (TREE_CODE (type))
    {
    case UNION_TYPE:
    case QUAL_UNION_TYPE:
    case RECORD_TYPE:
      for (fld = TYPE_FIELDS (type); fld; fld = TREE_CHAIN (fld))
	{
	  HOST_WIDE_INT pos;
	  tree fld_type, ref;
	  constructor_elt *elt;
	  tree value;

	  if (TREE_CODE (fld) != FIELD_DECL)
	    continue;
	  pos = int_bit_position (fld);
	  elt = VEC_safe_push (constructor_elt, gc, vals, NULL);
	  elt->index = fld;
	  if (expr)
	    ref = build3 (COMPONENT_REF, TREE_TYPE (fld), expr, fld,
			  NULL_TREE);
	  else
	    ref = NULL;
	  fld_type = TREE_TYPE (fld);
	  tree_size = TYPE_SIZE (TREE_TYPE (fld));
	  gcc_assert (tree_size && host_integerp (tree_size, 1));
	  size = tree_low_cst (tree_size, 1);

	  value = search (decl, offset + pos, size, data);
	  if (!value)
	    {
	      if (AGGREGATE_TYPE_P (fld_type))
		value = create_debug_constructor (fld_type, decl, ref,
						  offset + pos, search, data);
	      else
		value = ref;
	    }
	  elt->value = value;
	}

      break;
    case ARRAY_TYPE:
      domain = TYPE_DOMAIN (type);

      if (!domain || !TYPE_MIN_VALUE (domain) || !TYPE_MAX_VALUE (domain))
	return error_mark_node;

      el = TREE_TYPE (type);
      tree_size = TYPE_SIZE (el);
      gcc_assert (tree_size && host_integerp (tree_size, 1));
      size = tree_low_cst (tree_size, 1);
      index =  TYPE_MIN_VALUE (domain);
      max = TYPE_MAX_VALUE (domain);
      while (!tree_int_cst_lt (max, index))
	{
	  constructor_elt *elt;
	  tree ref, value;

	  if (expr)
	    ref = build4 (ARRAY_REF, TREE_TYPE (type), expr, index,
			  NULL_TREE, NULL_TREE);
	  else
	    ref = NULL;

	  elt = VEC_safe_push (constructor_elt, gc, vals, NULL);
	  elt->index = index;

	  value = search (decl, offset, size, data);
	  if (!value)
	    {
	      if (AGGREGATE_TYPE_P (el))
		value = create_debug_constructor (el, decl, ref, offset,
						  search, data);
	      else
		value = ref;
	    }
	  elt->value = value;
	  offset += size;
	  index = int_const_binop (PLUS_EXPR, index, integer_one_node, 0);
	}

      break;
    default:
      gcc_unreachable ();
    }

  return build_constructor (type, vals);
}

/* Perform all the modification required in IPA-SRA for NODE to have parameters
   as given in NOTES.  */

static void
modify_function (struct cgraph_node *node, VEC (ipa_parm_note_t, heap) *notes)
{
  ipa_modify_formal_parameters (current_function_decl, notes, "ISRA");
  scan_function (sra_ipa_modify_expr, sra_ipa_modify_assign,
		 replace_removed_params_ssa_names, false, notes);
  convert_callers (node, notes);
  cgraph_make_node_local (node);
  return;
}


/* Perform early interprocedural SRA.  */

static unsigned int
ipa_early_sra (void)
{
  struct cgraph_node *node = cgraph_node (current_function_decl);
  VEC (ipa_parm_note_t, heap) *notes;
  int ret = 0;

  if (!cgraph_node_can_be_local_p (node))
    {
      if (dump_file)
	fprintf (dump_file, "Function not local to this compilation unit.\n");
      return 0;
    }

  if (DECL_VIRTUAL_P (current_function_decl))
    {
      if (dump_file)
	fprintf (dump_file, "Function is a virtual method.\n");
      return 0;
    }

  if ((DECL_COMDAT (node->decl) || DECL_EXTERNAL (node->decl))
      && node->global.size >= MAX_INLINE_INSNS_AUTO)
    {
      if (dump_file)
	fprintf (dump_file, "Function too big to be made truly local.\n");
      return 0;
    }

  if (!node->callers)
    {
      if (dump_file)
	fprintf (dump_file,
		 "Function has no callers in this compilation unit.\n");
      return 0;
    }

  sra_initialize ();
  sra_mode = SRA_MODE_EARLY_IPA;

  find_param_candidates ();
  scan_function (build_access_from_expr, build_accesses_from_assign,
		 NULL, true, NULL);
  if (encountered_va_start)
    {
      if (dump_file)
	fprintf (dump_file, "Function calls va_start().\n\n");
      goto out;
    }

  notes = analyze_all_param_acesses ();
  if (!notes)
    goto out;
  if (dump_file)
    ipa_dump_param_notes (dump_file, notes, current_function_decl);

  modify_function (node, notes);
  VEC_free (ipa_parm_note_t, heap, notes);
  ret = TODO_update_ssa;

 out:
  sra_deinitialize ();
  return ret;
}

/* Return if early ipa sra shall be performed.  */
static bool
ipa_early_sra_gate (void)
{
  return flag_early_ipa_sra;
}

struct gimple_opt_pass pass_early_ipa_sra =
{
 {
  GIMPLE_PASS,
  "eipa_sra",	 			/* name */
  ipa_early_sra_gate,			/* gate */
  ipa_early_sra,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_IPA_SRA,				/* tv_id */
  0,	                                /* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func | TODO_dump_cgraph 	/* todo_flags_finish */
 }
};


/* The very first phase of intraprocedural SRA.  It marks in candidate_bitmap
   those with type which is suitable for scalarization.  */

static bool
find_var_candidates (void)
{
  tree var, type;
  referenced_var_iterator rvi;
  bool ret = false;

  FOR_EACH_REFERENCED_VAR (var, rvi)
    {
      if (TREE_CODE (var) != VAR_DECL && TREE_CODE (var) != PARM_DECL)
        continue;
      type = TREE_TYPE (var);

      if (!AGGREGATE_TYPE_P (type)
	  || needs_to_live_in_memory (var)
	  || TREE_THIS_VOLATILE (var)
	  || !COMPLETE_TYPE_P (type)
	  || !host_integerp (TYPE_SIZE (type), 1)
          || tree_low_cst (TYPE_SIZE (type), 1) == 0
	  || type_internals_preclude_sra_p (type))
	continue;

      bitmap_set_bit (candidate_bitmap, DECL_UID (var));

      if (dump_file)
	{
	  fprintf (dump_file, "Candidate (%d): ", DECL_UID (var));
	  print_generic_expr (dump_file, var, 0);
	  fprintf (dump_file, "\n");
	}
      ret = true;
    }

  return ret;
}

/* Return true if TYPE should be considered a scalar type by SRA.  */

static bool
is_sra_scalar_type (tree type)
{
  enum tree_code code = TREE_CODE (type);
  return (INTEGRAL_TYPE_P (type) || SCALAR_FLOAT_TYPE_P (type)
	  || FIXED_POINT_TYPE_P (type) || POINTER_TYPE_P (type)
	  || code == VECTOR_TYPE || code == COMPLEX_TYPE
	  || code == OFFSET_TYPE);
}


/* Sort all accesses for the given variable, check for partial overlaps and
   return NULL if there are any.  If there are none, pick a representative for
   each combination of offset and size and create a linked list out of them.
   Return the pointer to the first representative and make sure it is the first
   one in the vector of accesses.  */
static struct access *
sort_and_splice_var_accesses (tree var)
{
  int i, j, access_count;
  struct access *res, **prev_acc_ptr = &res;
  VEC (access_p, heap) *access_vec;
  bool first = true;
  HOST_WIDE_INT low = -1, high = 0;

  access_vec = get_base_access_vector (var);
  if (!access_vec)
    return NULL;
  access_count = VEC_length (access_p, access_vec);

  /* Sort by <OFFSET, SIZE>.  */
  qsort (VEC_address (access_p, access_vec), access_count, sizeof (access_p),
	 compare_access_positions);

  i = 0;
  while (i < access_count)
    {
      struct access *access = VEC_index (access_p, access_vec, i);
      bool modification = access->write;
      bool grp_read = !access->write;
      bool grp_bfr_lhs = access->grp_bfr_lhs;
      bool first_scalar = is_sra_scalar_type (access->type);
      bool unscalarizable_region = access->grp_unscalarizable_region;

      if (first || access->offset >= high)
	{
	  first = false;
	  low = access->offset;
	  high = access->offset + access->size;
	}
      else if (access->offset > low && access->offset + access->size > high)
	return NULL;
      else
	gcc_assert (access->offset >= low
		    && access->offset + access->size <= high);

      j = i + 1;
      while (j < access_count)
	{
	  struct access *ac2 = VEC_index (access_p, access_vec, j);
	  if (ac2->offset != access->offset || ac2->size != access->size)
	    break;
	  modification |= ac2->write;
	  grp_read |= !ac2->write;
	  grp_bfr_lhs |= ac2->grp_bfr_lhs;
	  unscalarizable_region |= ac2->grp_unscalarizable_region;
	  relink_to_new_repr (access, ac2);

	  /* If one of the equivalent accesses is scalar, use it as a
	     representative (this happens when when there is for example on a
	     single scalar field in a structure).  */
	  if (!first_scalar && is_sra_scalar_type (ac2->type))
	    {
	      struct access tmp_acc;
	      first_scalar = true;

	      memcpy (&tmp_acc, ac2, sizeof (struct access));
	      memcpy (ac2, access,  sizeof (struct access));
	      memcpy (access, &tmp_acc, sizeof (struct access));
	    }
	  ac2->group_representative = access;
	  j++;
	}

      i = j;

      access->group_representative = access;
      access->grp_write = modification;
      access->grp_read = grp_read;
      access->grp_maybe_modified = modification;
      access->grp_bfr_lhs = grp_bfr_lhs;
      access->grp_unscalarizable_region = unscalarizable_region;
      if (access->first_link)
	add_access_to_work_queue (access);

      *prev_acc_ptr = access;
      prev_acc_ptr = &access->next_grp;
    }

  gcc_assert (res == VEC_index (access_p, access_vec, 0));
  return res;
}

/* Create a variable for the given ACCESS which determines the type, name and a
   few other properties.  Return the variable declaration and store it also to
   ACCESS->replacement.  */

static tree
create_access_replacement (struct access *access)
{
  tree repl;

  repl = make_rename_temp (access->type, "SR");
  get_var_ann (repl);
  add_referenced_var (repl);

  DECL_SOURCE_LOCATION (repl) = DECL_SOURCE_LOCATION (access->base);
  DECL_ARTIFICIAL (repl) = 1;

  if (DECL_NAME (access->base) && !DECL_IGNORED_P (access->base))
    {
      char *pretty_name = make_fancy_name (access->expr);

      DECL_NAME (repl) = get_identifier (pretty_name);
      obstack_free (&name_obstack, pretty_name);

      SET_DECL_DEBUG_EXPR (repl, access->expr);
      DECL_DEBUG_EXPR_IS_FROM (repl) = 1;
      DECL_IGNORED_P (repl) = 0;
      TREE_NO_WARNING (repl) = TREE_NO_WARNING (access->base);
    }
  else
    {
      DECL_IGNORED_P (repl) = 1;
      TREE_NO_WARNING (repl) = 1;
    }

  if (access->grp_bfr_lhs)
    DECL_GIMPLE_REG_P (repl) = 0;

  if (dump_file)
    {
      fprintf (dump_file, "Created a replacement for ");
      print_generic_expr (dump_file, access->base, 0);
      fprintf (dump_file, " offset: %u, size: %u: ",
	       (unsigned) access->offset, (unsigned) access->size);
      print_generic_expr (dump_file, repl, 0);
      fprintf (dump_file, "\n");
    }

  return repl;
}

/* Return ACCESS scalar replacement, create it if it does not exist yet.  */

static inline tree
get_access_replacement (struct access *access)
{
  gcc_assert (access->to_be_replaced);

  if (access->replacement_decl)
    return access->replacement_decl;

  access->replacement_decl = create_access_replacement (access);
  return access->replacement_decl;
}

/* Build a subtree of accesses rooted in *ACCESS, and move the pointer in the
   linked list along the way.  Stop when *ACCESS is NULL or the access pointed
   to it is not "within" the root.  */

static void
build_access_tree_1 (struct access **access)
{
  struct access *root = *access, *last_child = NULL;
  HOST_WIDE_INT limit = root->offset + root->size;

  *access = (*access)->next_grp;
  while  (*access && (*access)->offset + (*access)->size <= limit)
    {
      if (!last_child)
	root->first_child = *access;
      else
	last_child->next_sibling = *access;
      last_child = *access;

      build_access_tree_1 (access);
    }
}

/* Build a tree of access representatives, ACCESS is the pointer to the first
   one, others are linked in a list by the next_grp field.  Decide about scalar
   replacements on the way, return true iff any are to be created.  */

static void
build_access_trees (struct access *access)
{
  while (access)
    {
      struct access *root = access;

      build_access_tree_1 (&access);
      root->next_grp = access;
    }
}

/* Analyze the subtree of accesses rooted in ROOT, scheduling replacements when
   both seemeing beneficial and when ALLOW_REPLACEMENTS allows it.  Also set
   all sorts of access flags appropriately along the way, notably always ser
   grp_read when MARK_READ is true and grp_write when MARK_WRITE is true.  */

static bool
analyze_access_tree_1 (struct access *root, bool allow_replacements,
		       bool mark_read, bool mark_write)
{
  struct access *child;
  HOST_WIDE_INT limit = root->offset + root->size;
  HOST_WIDE_INT covered_to = root->offset;
  bool scalar = is_sra_scalar_type (root->type);
  bool hole = false, sth_created = false;

  if (mark_read)
    root->grp_read = true;
  else if (root->grp_read)
    mark_read = true;

  if (mark_write)
    root->grp_write = true;
  else if (root->grp_write)
    mark_write = true;

  if (root->grp_unscalarizable_region)
    allow_replacements = false;

  for (child = root->first_child; child; child = child->next_sibling)
    {
      if (!hole && child->offset < covered_to)
	hole = true;
      else
	covered_to += child->size;

      sth_created |= analyze_access_tree_1 (child,
					    allow_replacements && !scalar,
					    mark_read, mark_write);

      root->grp_unscalarized_data |= child->grp_unscalarized_data;
      hole |= !child->grp_covered;
    }

  if (allow_replacements && scalar && !root->first_child)
    {
      if (dump_file)
	{
	  fprintf (dump_file, "Marking ");
	  print_generic_expr (dump_file, root->base, 0);
	  fprintf (dump_file, " offset: %u, size: %u: ",
		   (unsigned) root->offset, (unsigned) root->size);
	  fprintf (dump_file, " to be replaced.\n");
	}

      root->to_be_replaced = 1;
      sth_created = true;
      hole = false;
    }
  else if (covered_to < limit)
    hole = true;

  if (sth_created && !hole)
    {
      root->grp_covered = 1;
      return true;
    }
  if (root->grp_write || TREE_CODE (root->base) == PARM_DECL)
    root->grp_unscalarized_data = 1; /* not covered and written to */
  if (sth_created)
    return true;
  return false;
}

/* Analyze all access trees linked by next_grp by the means of
   analyze_access_tree_1.  */
static bool
analyze_access_trees (struct access *access)
{
  bool ret = false;

  while (access)
    {
      if (analyze_access_tree_1 (access, true, false, false))
	ret = true;
      access = access->next_grp;
    }

  return ret;
}

/* Return true iff a potential new childof LACC at offset OFFSET and with size
   SIZE would conflict with an already existing one.  If exactly such a child
   already exists in LACC, store a pointer to it in EXACT_MATCH.  */
static bool
child_would_conflict_in_lacc (struct access *lacc, HOST_WIDE_INT norm_offset,
			      HOST_WIDE_INT size, struct access **exact_match)
{
  struct access *child;

  for (child = lacc->first_child; child; child = child->next_sibling)
    {
      if (child->offset == norm_offset && child->size == size)
	{
	  *exact_match = child;
	  return true;
	}

      if (child->offset < norm_offset + size
	  && child->offset + child->size > norm_offset)
	return true;
    }

  return false;
}

/* Create a new child access of PARENT, with all properties just like MODEL
   except for its offset and with its grp_write false and grp_read true.
   Return the new access. Note that this access is created long after all
   splicing and sorting, it's not located in any access vector and is
   automatically a representative of its group.  */

static struct access *
create_artificial_child_access (struct access *parent, struct access *model,
				HOST_WIDE_INT new_offset)
{
  struct access *access;
  struct access **child;
  tree t, expr = unshare_expr (model->expr);

  gcc_assert (!model->grp_unscalarizable_region);
  gcc_assert (handled_component_p (expr));
  t = expr;
  while (handled_component_p (TREE_OPERAND (t, 0)))
    t = TREE_OPERAND (t, 0);
  gcc_assert (TREE_OPERAND (t, 0) == model->base);
  TREE_OPERAND (t, 0) = parent->base;

  access = (struct access *) pool_alloc (access_pool);

  memset (access, 0, sizeof (struct access));
  access->base = parent->base;
  access->offset = new_offset;
  access->size = model->size;
  access->expr = expr;
  access->type = model->type;
  access->grp_write = false;
  access->grp_read = true;

  child = &parent->first_child;
  while (*child && (*child)->offset < new_offset)
    child = &(*child)->next_sibling;

  access->next_sibling = *child;
  *child = access;

  return access;
}


/* Propagate all subaccesses of RACC accross an assignment link to LACC. Return
   true if any new subaccess was created.  */

static bool
propagate_subacesses_accross_link (struct access *lacc, struct access *racc)
{
  struct access *rchild;
  HOST_WIDE_INT norm_delta = lacc->offset - racc->offset;
  bool ret = false;

  if (is_sra_scalar_type (lacc->type) && lacc->grp_unscalarizable_region)
    return false;

  gcc_assert (lacc->size == racc->size);

  for (rchild = racc->first_child; rchild; rchild = rchild->next_sibling)
    {
      struct access *new_acc = NULL;
      HOST_WIDE_INT norm_offset = rchild->offset + norm_delta;

      if (rchild->grp_unscalarizable_region)
	continue;

      if (child_would_conflict_in_lacc (lacc, norm_offset, rchild->size,
					&new_acc))
	{
	  if (new_acc && rchild->first_child)
	    ret |= propagate_subacesses_accross_link (new_acc, rchild);
	  continue;
	}

      new_acc = create_artificial_child_access (lacc, rchild, norm_offset);
      if (racc->first_child)
	propagate_subacesses_accross_link (new_acc, rchild);

      ret = true;
    }

  return ret;
}

/* Propagate all subaccesses accross assignment links.  */

static void
propagate_all_subaccesses (void)
{
  while (work_queue_head)
    {
      struct access *racc = pop_access_from_work_queue ();
      struct assign_link *link;

      gcc_assert (racc->first_link);

      for (link = racc->first_link; link; link = link->next)
	{
	  struct access *lacc = link->lacc;

	  if (!bitmap_bit_p (candidate_bitmap, DECL_UID (lacc->base)))
	    continue;
	  lacc = lacc->group_representative;
	  if (propagate_subacesses_accross_link (lacc, racc)
	      && lacc->first_link)
	    add_access_to_work_queue (lacc);
	}
    }
}


/* Dump a subtree rooted in ACCESS, indent by LEVEL.  */

static void
dump_access_tree_1 (struct access *access, int level)
{
  do
    {
      int i;

      for (i = 0; i < level; i++)
	fputs ("* ", dump_file);

      dump_access (access, true);

      if (access->first_child)
	dump_access_tree_1 (access->first_child, level + 1);

      access = access->next_sibling;
    }
  while (access);
}

/* Dump all access trees for a variable, given the pointer to the first root in
   ACCESS.  */

static void
dump_access_tree (struct access *access)
{
  for (; access; access = access->next_grp)
    dump_access_tree_1 (access, 0);
}

/* Go through all accesses collected throughout the (intraprocedural) analysis
   stage, exclude overlapping ones, identify representatives and build trees
   out of them, making decisions about scalarization on the way.  Return true
   iff there are any to-be-scalarized variables after this stage. */

static bool
analyze_all_variable_accesses (void)
{
  tree var;
  referenced_var_iterator rvi;
  bool res = false;

  FOR_EACH_REFERENCED_VAR (var, rvi)
    if (bitmap_bit_p (candidate_bitmap, DECL_UID (var)))
      {
	struct access *access;

	access = sort_and_splice_var_accesses (var);
	if (access)
	  build_access_trees (access);
	else
	  disqualify_candidate (var,
				"No or inhibitingly overlapping accesses.");
      }

  propagate_all_subaccesses ();

  FOR_EACH_REFERENCED_VAR (var, rvi)
    if (bitmap_bit_p (candidate_bitmap, DECL_UID (var)))
      {
	struct access *access = get_first_repr_for_decl (var);

	if (analyze_access_trees (access))
	  {
	    res = true;
	    if (dump_file)
	      {
		fprintf (dump_file, "\nAccess trees for ");
		print_generic_expr (dump_file, var, 0);
		fprintf (dump_file, " (UID: %u): \n", DECL_UID (var));
		dump_access_tree (access);
		fprintf (dump_file, "\n");
	      }
	  }
	else
	  disqualify_candidate (var, "No scalar replacements to be created.");
      }

  return res;
}

/* Generate statements copying scalar replacements of accesses within a subtree
   into or out of AGG.  ACCESS is the first child of the root of the subtree to
   be processed.  AGG is an aggregate type expression (can be a declaration but
   does not have to be, it can for example also be an indirect_ref).
   TOP_OFFSET is the offset of the processed subtree which has to be subtracted
   from offsets of individual accesses to get corresponding offsets for AGG.
   If CHUNK_SIZE is non-null, copy only replacements in the interval
   <start_offset, start_offset + chunk_size>, otherwise copy all.  GSI is a
   statement iterator used to place the new statements.  WRITE should be true
   when the statements should write from AGG to the replacement and false if
   vice versa.  if INSERT_AFTER is true, new statements will be added after the
   current statement in GSI, they will be added before the statement
   otherwise.  */

static void
generate_subtree_copies (struct access *access, tree agg,
			 HOST_WIDE_INT top_offset,
			 HOST_WIDE_INT start_offset, HOST_WIDE_INT chunk_size,
			 gimple_stmt_iterator *gsi, bool write,
			 bool insert_after)
{
  do
    {
      tree expr = unshare_expr (agg);

      if (chunk_size && access->offset >= start_offset + chunk_size)
	return;

      if (access->to_be_replaced
	  && (chunk_size == 0
	      || access->offset + access->size > start_offset))

	{
	  bool repl_found;
	  gimple stmt;

	  repl_found = build_ref_for_offset (&expr, TREE_TYPE (agg),
					     access->offset - top_offset,
					     access->type, false);
	  gcc_assert (repl_found);

	  if (write)
	    stmt = gimple_build_assign (get_access_replacement (access), expr);
	  else
	    {
	      tree repl = get_access_replacement (access);
	      TREE_NO_WARNING (repl) = 1;
	      stmt = gimple_build_assign (expr, repl);
	    }

	  if (insert_after)
	    gsi_insert_after (gsi, stmt, GSI_NEW_STMT);
	  else
	    gsi_insert_before (gsi, stmt, GSI_SAME_STMT);
	}

      if (access->first_child)
	generate_subtree_copies (access->first_child, agg, top_offset,
				 start_offset, chunk_size, gsi,
				 write, insert_after);

      access = access->next_sibling;
    }
  while (access);
}

/* Assign zero to all scalar replacements in an access subtree.  ACCESS is the
   the root of the subtree to be processed.  GSI is the statement iterator used
   for inserting statements which are added after the current statement if
   INSERT_AFTER is true or before it otherwise.  */
static void
init_subtree_with_zero (struct access *access, gimple_stmt_iterator *gsi,
			bool insert_after)

{
  struct access *child;

  if (access->to_be_replaced)
    {
      gimple stmt;

      stmt = gimple_build_assign (get_access_replacement (access),
				  fold_convert (access->type,
						integer_zero_node));
      if (insert_after)
	gsi_insert_after (gsi, stmt, GSI_NEW_STMT);
      else
	gsi_insert_before (gsi, stmt, GSI_SAME_STMT);

    }

  for (child = access->first_child; child; child = child->next_sibling)
    init_subtree_with_zero (child, gsi, insert_after);
}

/* Search for an access representative for the given expression EXPR and
   return it or NULL if it cannot be found.  */

static struct access *
get_access_for_expr (tree expr)
{
  HOST_WIDE_INT offset, size, max_size;
  tree base;

  if (TREE_CODE (expr) == NOP_EXPR
      || TREE_CODE (expr) == VIEW_CONVERT_EXPR)
    expr = TREE_OPERAND (expr, 0);

  if (handled_component_p (expr))
    {
      base = get_ref_base_and_extent (expr, &offset, &size, &max_size);
      size = max_size;
      if (size == -1 || !base || !DECL_P (base))
	return NULL;
    }
  else if (DECL_P (expr))
    {
      tree tree_size;

      base = expr;
      tree_size = TYPE_SIZE (TREE_TYPE (base));
      if (tree_size && host_integerp (tree_size, 1))
	size = max_size = tree_low_cst (tree_size, 1);
      else
	return NULL;

      offset = 0;
    }
  else
    return NULL;

  if (!bitmap_bit_p (candidate_bitmap, DECL_UID (base)))
    return NULL;

  return get_var_base_offset_size_access (base, offset, size);
}

/* Substitute into *EXPR an expression of type TYPE with the value of the
   replacement of ACCESS.  This is done either by producing a special V_C_E
   assignment statement converting the replacement to a new temporary of the
   requested type if TYPE is not TREE_ADDRESSABLE or by going through the base
   aggregate if it is.  */

static void
sra_fix_incompatible_types_for_expr (tree *expr, tree type,
				     struct access *access,
				     gimple_stmt_iterator *gsi, bool write)
{
  tree repl = get_access_replacement (access);
  if (!TREE_ADDRESSABLE (type))
    {
      tree tmp = make_rename_temp (type, "SRvce");
      if (write)
	{
	  gimple stmt;
	  tree conv = fold_build1 (VIEW_CONVERT_EXPR, TREE_TYPE (repl), tmp);

	  *expr = tmp;
	  stmt = gimple_build_assign (repl, conv);
	  gsi_insert_after (gsi, stmt, GSI_NEW_STMT);
	}
      else
	{
	  gimple stmt;
	  tree conv = fold_build1 (VIEW_CONVERT_EXPR, type, repl);

	  stmt = gimple_build_assign (tmp, conv);
	  gsi_insert_before (gsi, stmt, GSI_SAME_STMT);
	  *expr = tmp;
	}
    }
  else
    {
      if (write)
	{
	  gimple stmt;

	  stmt = gimple_build_assign (repl, unshare_expr (access->expr));
	  gsi_insert_after (gsi, stmt, GSI_NEW_STMT);
	}
      else
	{
	  gimple stmt;

	  stmt = gimple_build_assign (unshare_expr (access->expr), repl);
	  gsi_insert_before (gsi, stmt, GSI_SAME_STMT);
	}
    }
}


/* Callback for scan_function.  Replace the expression EXPR with a scalar
   replacement if there is one and generate other statements to do type
   conversion or subtree copying if necessary.  GSI is used to place newly
   created statements, WRITE is true if the expression is being written to (it
   is on a LHS of a statement or output in an assembly statement).  */

static bool
sra_intra_modify_expr (tree *expr, gimple_stmt_iterator *gsi, bool write,
		       void *data ATTRIBUTE_UNUSED)
{
  struct access *access;
  tree type, bfr;

  if (TREE_CODE (*expr) == BIT_FIELD_REF)
    {
      bfr = *expr;
      expr = &TREE_OPERAND (*expr, 0);
    }
  else
    bfr = NULL_TREE;

  if (TREE_CODE (*expr) == REALPART_EXPR || TREE_CODE (*expr) == IMAGPART_EXPR)
    expr = &TREE_OPERAND (*expr, 0);
  type = TREE_TYPE (*expr);

  access = get_access_for_expr (*expr);
  if (!access)
    return false;

  if (access->to_be_replaced)
    {
      gimple *stmt = gsi_stmt_ptr (gsi);

      if (!useless_type_conversion_p (type, access->type))
	sra_fix_incompatible_types_for_expr (expr, type, access, gsi, write);
      else
	*expr = get_access_replacement (access);
      update_stmt (*stmt);
    }

  if (access->first_child)
    {
      HOST_WIDE_INT start_offset, chunk_size;
      if (bfr
	  && host_integerp (TREE_OPERAND (bfr, 1), 1)
	  && host_integerp (TREE_OPERAND (bfr, 2), 1))
	{
	  start_offset = tree_low_cst (TREE_OPERAND (bfr, 1), 1);
	  chunk_size = tree_low_cst (TREE_OPERAND (bfr, 2), 1);
	}
      else
	start_offset = chunk_size = 0;

      generate_subtree_copies (access->first_child, access->base, 0,
			       start_offset, chunk_size, gsi, write, write);
    }
  return true;
}

/* Store all replacements in the access tree rooted in TOP_RACC either to their
   base aggregate if there are unscalarized data or directly to LHS
   otherwise.  */

static void
handle_unscalarized_data_in_subtree (struct access *top_racc, tree lhs,
				     gimple_stmt_iterator *gsi)
{
  if (top_racc->grp_unscalarized_data)
    generate_subtree_copies (top_racc->first_child, top_racc->base, 0, 0, 0,
			     gsi, false, false);
  else
    generate_subtree_copies (top_racc->first_child, lhs, top_racc->offset,
			     0, 0, gsi, false, false);
}


/* Try generate statements to load all sub-replacements in an access (sub)tree
   (LACC is the first child) from scalar replacements in the TOP_RACC
   (sub)tree.  If that is not possible, refresh the TOP_RACC base aggregate and
   load the accesses from it.  LEFT_OFFSET is the offset of the left whole
   subtree being copied, RIGHT_OFFSET is the same thing for the right subtree.
   GSI is stmt iterator used for statement insertions.  *REFRESHED is true iff
   the rhs top aggregate has already been refreshed by contents of its scalar
   reductions and is set to true if this function has to do it.  */
static void
load_assign_lhs_subreplacements (struct access *lacc, struct access *top_racc,
				 HOST_WIDE_INT left_offset,
				 HOST_WIDE_INT right_offset,
				 gimple_stmt_iterator *gsi, bool *refreshed,
				 tree lhs)
{
  do
    {
      if (lacc->to_be_replaced)
	{
	  struct access *racc;
	  HOST_WIDE_INT offset = lacc->offset - left_offset + right_offset;

	  racc = find_access_in_subtree (top_racc, offset, lacc->size);
	  if (racc && racc->to_be_replaced)
	    {
	      gimple stmt;

	      if (useless_type_conversion_p (lacc->type, racc->type))
		stmt = gimple_build_assign (get_access_replacement (lacc),
					    get_access_replacement (racc));
	      else
		{
		  tree rhs = fold_build1 (VIEW_CONVERT_EXPR, lacc->type,
					  get_access_replacement (racc));
		  stmt = gimple_build_assign (get_access_replacement (lacc),
					      rhs);
		}

	      gsi_insert_before (gsi, stmt, GSI_SAME_STMT);
	    }
	  else
	    {
	      tree expr = unshare_expr (top_racc->base);
	      bool repl_found;
	      gimple stmt;

	      /* No suitable access on the right hand side, need to load from
		 the aggregate.  See if we have to update it first... */
	      if (!*refreshed)
		{
		  gcc_assert (top_racc->first_child);
		  generate_subtree_copies (top_racc->first_child,
					   top_racc->base, 0, 0, 0, gsi,
					   false, false);
		  *refreshed = true;
		}

	      repl_found = build_ref_for_offset (&expr,
						 TREE_TYPE (top_racc->base),
						 lacc->offset - left_offset,
						 lacc->type, false);
	      gcc_assert (repl_found);

	      stmt = gimple_build_assign (get_access_replacement (lacc), expr);
	      gsi_insert_before (gsi, stmt, GSI_SAME_STMT);
	    }
	}
      else if (lacc->grp_read && !lacc->grp_covered && !*refreshed)
	{
	  handle_unscalarized_data_in_subtree (top_racc, lhs, gsi);
	  *refreshed = true;
	}

      if (lacc->first_child)
	load_assign_lhs_subreplacements (lacc->first_child, top_racc,
					 left_offset, right_offset,
					 gsi, refreshed, lhs);
      lacc = lacc->next_sibling;
    }
  while (lacc);
}

/* Modify assignments with a CONSTRUCTOR on their RHS.  STMT contains a pointer
   to the assignment and GSI is the statement iterator pointing at it.  Returns
   the same values as sra_intra_modify_assign.  */

static enum scan_assign_result
sra_intra_modify_constructor_assign (gimple *stmt, gimple_stmt_iterator *gsi)
{
  tree lhs = gimple_assign_lhs (*stmt);
  struct access *acc;

  gcc_assert (TREE_CODE (lhs) != REALPART_EXPR
	      && TREE_CODE (lhs) != IMAGPART_EXPR);
  acc = get_access_for_expr (lhs);
  if (!acc)
    return SRA_SA_NONE;

  if (VEC_length (constructor_elt,
		  CONSTRUCTOR_ELTS (gimple_assign_rhs1 (*stmt))) > 0)
    {
      /* I have never seen this code path trigger but if it can happen the
	 following should handle it gracefully.  */
      if (acc && acc->first_child)
	generate_subtree_copies (acc->first_child, acc->base, 0, 0, 0, gsi,
				 true, true);
      return SRA_SA_PROCESSED;
    }

  if (!acc->grp_read || acc->grp_covered)
    {
      init_subtree_with_zero (acc, gsi, false);
      unlink_stmt_vdef (*stmt);
      gsi_remove (gsi, true);
      return SRA_SA_REMOVED;
    }
  else
    {
      init_subtree_with_zero (acc, gsi, true);
      return SRA_SA_PROCESSED;
    }
}


/* Modify statements with IMAGPART_EXPR or REALPART_EXPR on their lhs with
   to-be-scalarized expressions with them.  STMT is the statement and GSI is
   the iterator used to place new helper statements.  Returns the same values
   as sra_intra_modify_assign.  */

static enum scan_assign_result
sra_modify_partially_complex_lhs (gimple stmt, gimple_stmt_iterator *gsi)
{
  tree lhs, complex, ptype, rp, ip;
  struct access *access;
  gimple new_stmt, aux_stmt;

  lhs = gimple_assign_lhs (stmt);
  complex = TREE_OPERAND (lhs, 0);

  access = get_access_for_expr (complex);

  if (!access || !access->to_be_replaced)
    return SRA_SA_NONE;

  ptype = TREE_TYPE (TREE_TYPE (complex));
  rp = make_rename_temp (ptype, "SRr");
  ip = make_rename_temp (ptype, "SRp");

  if (TREE_CODE (lhs) == IMAGPART_EXPR)
    {
      aux_stmt = gimple_build_assign (rp, fold_build1 (REALPART_EXPR, ptype,
					     get_access_replacement (access)));
      gimple_assign_set_lhs (stmt, ip);
    }
  else
    {
      aux_stmt = gimple_build_assign (ip, fold_build1 (IMAGPART_EXPR, ptype,
					     get_access_replacement (access)));
      gimple_assign_set_lhs (stmt, rp);
    }

  gsi_insert_before (gsi, aux_stmt, GSI_SAME_STMT);
  new_stmt = gimple_build_assign (get_access_replacement (access),
				  fold_build2 (COMPLEX_EXPR, access->type,
					       rp, ip));
  gsi_insert_after (gsi, new_stmt, GSI_NEW_STMT);
  return SRA_SA_PROCESSED;
}

/* Change STMT to assign compatible types by means of adding component or array
   references or VIEW_CONVERT_EXPRs.  All parameters have the same meaning as
   variable with the same names in sra_intra_modify_assign.  This is done in a
   such a complicated way in order to make
   testsuite/g++.dg/tree-ssa/ssa-sra-2.C happy and so it helps in at least some
   cases.  */

static void
fix_modified_assign_compatibility (gimple_stmt_iterator *gsi, gimple *stmt,
				   struct access *lacc, struct access *racc,
				   tree lhs, tree *rhs, tree ltype, tree rtype)
{
  if (racc && racc->to_be_replaced && AGGREGATE_TYPE_P (ltype)
      && (!lacc || !lacc->first_child))
    {
      tree expr = unshare_expr (lhs);
      bool found = build_ref_for_offset (&expr, ltype, racc->offset, rtype,
					 false);
      if (found)
	{
	  gimple_assign_set_lhs (*stmt, expr);
	  return;
	}
    }

  if (lacc && lacc->to_be_replaced && AGGREGATE_TYPE_P (rtype)
      && (!racc || !racc->first_child))
    {
      tree expr = unshare_expr (*rhs);
      bool found = build_ref_for_offset (&expr, rtype, lacc->offset, ltype,
					 false);
      if (found)
	{
	  gimple_assign_set_rhs1 (*stmt, expr);
	  return;
	}
    }

  *rhs = fold_build1 (VIEW_CONVERT_EXPR, ltype, *rhs);
  gimple_assign_set_rhs_from_tree (gsi, *rhs);
  *stmt = gsi_stmt (*gsi);
}

/* Callback of scan_function to process assign statements.  It examines both
   sides of the statement, replaces them with a scalare replacement if there is
   one and generating copying of replacements if scalarized aggregates have been
   used in the assignment.  STMT is a pointer to the assign statement, GSI is
   used to hold generated statements for type conversions and subtree
   copying.  */

static enum scan_assign_result
sra_intra_modify_assign (gimple *stmt, gimple_stmt_iterator *gsi,
			 void *data ATTRIBUTE_UNUSED)
{
  struct access *lacc, *racc;
  tree ltype, rtype;
  tree lhs, rhs;
  bool modify_this_stmt;

  if (gimple_assign_rhs2 (*stmt))
    return SRA_SA_NONE;
  lhs = gimple_assign_lhs (*stmt);
  rhs = gimple_assign_rhs1 (*stmt);

  if (TREE_CODE (rhs) == CONSTRUCTOR)
    {
      if (sra_mode == SRA_MODE_EARLY_IPA)
	return SRA_SA_NONE;
      else
	return sra_intra_modify_constructor_assign (stmt, gsi);
    }

  if (TREE_CODE (lhs) == REALPART_EXPR || TREE_CODE (lhs) == IMAGPART_EXPR)
    return sra_modify_partially_complex_lhs (*stmt, gsi);

  if (TREE_CODE (rhs) == REALPART_EXPR || TREE_CODE (rhs) == IMAGPART_EXPR
      || TREE_CODE (rhs) == BIT_FIELD_REF || TREE_CODE (lhs) == BIT_FIELD_REF)
    {
      modify_this_stmt = sra_intra_modify_expr (gimple_assign_rhs1_ptr (*stmt),
						gsi, false, data);
      modify_this_stmt |= sra_intra_modify_expr (gimple_assign_lhs_ptr (*stmt),
						 gsi, true, data);
      return modify_this_stmt ? SRA_SA_PROCESSED : SRA_SA_NONE;
    }

  lacc = get_access_for_expr (lhs);
  racc = get_access_for_expr (rhs);
  if (!lacc && !racc)
    return SRA_SA_NONE;

  modify_this_stmt = ((lacc && lacc->to_be_replaced)
		      || (racc && racc->to_be_replaced));

  if (lacc && lacc->to_be_replaced)
    {
      lhs = get_access_replacement (lacc);
      gimple_assign_set_lhs (*stmt, lhs);
      ltype = lacc->type;
    }
  else
    ltype = TREE_TYPE (lhs);

  if (racc && racc->to_be_replaced)
    {
      rhs = get_access_replacement (racc);
      gimple_assign_set_rhs1 (*stmt, rhs);
      rtype = racc->type;
    }
  else
    rtype = TREE_TYPE (rhs);

  /* The possibility that gimple_assign_set_rhs_from_tree() might reallocate
     the statement makes the position of this pop_stmt_changes() a bit awkward
     but hopefully make some sense.  */
  if (modify_this_stmt)
    {
      if (!useless_type_conversion_p (ltype, rtype))
	fix_modified_assign_compatibility (gsi, stmt, lacc, racc,
					   lhs, &rhs, ltype, rtype);
    }

  if (contains_view_convert_expr_p (rhs) || contains_view_convert_expr_p (lhs))
    {
      if (racc && racc->first_child)
	generate_subtree_copies (racc->first_child, racc->base, 0, 0, 0,
				 gsi, false, false);
      if (lacc && lacc->first_child)
	generate_subtree_copies (lacc->first_child, lacc->base, 0, 0, 0,
				 gsi, true, true);
    }
  else
    {
      if (lacc && racc && lacc->first_child && racc->first_child)
	{
	  bool refreshed;

	  if (lacc->grp_read && !lacc->grp_covered)
	    {
	      handle_unscalarized_data_in_subtree (racc, lhs, gsi);
	      refreshed = true;
	    }
	  else
	    refreshed = false;

	  load_assign_lhs_subreplacements (lacc->first_child, racc,
					   lacc->offset, racc->offset,
					   gsi, &refreshed, lhs);
	  if (!refreshed || !racc->grp_unscalarized_data)
	    {
	      gcc_assert (*stmt == gsi_stmt (*gsi));
	      unlink_stmt_vdef (*stmt);
	      gsi_remove (gsi, true);
	      return SRA_SA_REMOVED;
	    }
	}
      else
	{
	  if (racc && racc->first_child)
	    {
	      if (!racc->grp_unscalarized_data)
		{
		  generate_subtree_copies (racc->first_child,
					   gimple_assign_lhs (*stmt),
					   racc->offset, 0, 0, gsi,
					   false, false);
		  gcc_assert (*stmt == gsi_stmt (*gsi));
		  unlink_stmt_vdef (*stmt);
		  gsi_remove (gsi, true);
		  return SRA_SA_REMOVED;
		}
	      else
		generate_subtree_copies (racc->first_child,
					 gimple_assign_lhs (*stmt),
					 racc->offset, 0, 0, gsi, false, true);
	    }
	  else if (lacc && lacc->first_child)
	    generate_subtree_copies (lacc->first_child, rhs, lacc->offset,
				     0, 0, gsi, true, false);
	}
    }

  return modify_this_stmt ? SRA_SA_PROCESSED : SRA_SA_NONE;
}

/* Generate statements initializing scalar replacements of parts of function
   parameters.  */

static void
initialize_parameter_reductions (void)
{
  gimple_stmt_iterator gsi;
  gimple_seq seq = NULL;
  tree parm;

  for (parm = DECL_ARGUMENTS (current_function_decl);
       parm;
       parm = TREE_CHAIN (parm))
    {
      VEC (access_p, heap) *access_vec;
      struct access *access;

      if (!bitmap_bit_p (candidate_bitmap, DECL_UID (parm)))
	continue;
      access_vec = get_base_access_vector (parm);
      if (!access_vec)
	continue;

      if (!seq)
	{
	  seq = gimple_seq_alloc ();
	  gsi = gsi_start (seq);
	}

      for (access = VEC_index (access_p, access_vec, 0);
	   access;
	   access = access->next_grp)
	generate_subtree_copies (access, parm, 0, 0, 0, &gsi, true, true);
    }

  if (seq)
    gsi_insert_seq_on_edge_immediate (single_succ_edge (ENTRY_BLOCK_PTR), seq);

}

/* Callback used by build_debug_constructor to locate a replacement in access
   tree.  */

static tree
access_tree_debug_search (tree decl, HOST_WIDE_INT offset,
			  HOST_WIDE_INT size, void *data ATTRIBUTE_UNUSED)
{
  struct access *access = get_var_base_offset_size_access (decl, offset, size);
  if (access && access->to_be_replaced)
    {
      /* We read replacement_decl here directly because after the
	 function has been modified it really ought to exist.  */
      gcc_assert (access->replacement_decl);
      return access->replacement_decl;
    }
  else
    return NULL;
}

/* If any of values of NONLOCALIZED_VARS holds an aggregate that was split into
   components by intra-SRA,  replace it with an appropriate constructor.  */

static void
intra_remap_nonlocalized_vars (tree block)
{
  int i, n;
  tree t;

  n = BLOCK_NUM_NONLOCALIZED_VARS (block);
  for (i = 0; i < n; i++)
    {
      tree var = BLOCK_NONLOCALIZED_VAR_VALUE (block, i);

      if (var && DECL_P (var)
	  && bitmap_bit_p (candidate_bitmap, DECL_UID (var)))
	{
	  BLOCK_NONLOCALIZED_VAR_VALUE (block, i)
	    = create_debug_constructor (TREE_TYPE (var), var, var, 0,
					access_tree_debug_search, NULL);
	}
    }

  for (t = BLOCK_SUBBLOCKS (block); t ; t = BLOCK_CHAIN (t))
    intra_remap_nonlocalized_vars (t);
}

/* Store information describing how aggregates were reduced by intra-SRA to be
   used later when generating debug info, if the debug info level requires it
   it.  */

static void
intra_store_all_debug_information (void)
{
  tree var;
  referenced_var_iterator rvi;

  if (debug_info_level <= DINFO_LEVEL_TERSE)
    return;

  FOR_EACH_REFERENCED_VAR (var, rvi)
    {
      if (bitmap_bit_p (candidate_bitmap, DECL_UID (var)))
	{
	  tree cst;

	  cst = create_debug_constructor (TREE_TYPE (var), var, var, 0,
					  access_tree_debug_search, NULL);
	  SET_DECL_VALUE_EXPR (var, cst);
	}
    }

  intra_remap_nonlocalized_vars (DECL_INITIAL (current_function_decl));
}

/* The "main" function of intraprocedural SRA passes.  Runs the analysis and if
   it reveals there are components of some aggregates to be scalarized, it runs
   the required transformations.  */
static unsigned int
perform_new_intra_sra (void)
{
  int ret = 0;
  sra_initialize ();

  if (!find_var_candidates ())
    goto out;

  if (!scan_function (build_access_from_expr, build_accesses_from_assign, NULL,
		      true, NULL))
    goto out;

  if (!analyze_all_variable_accesses ())
    goto out;

  scan_function (sra_intra_modify_expr, sra_intra_modify_assign, NULL,
		 false, NULL);
  initialize_parameter_reductions ();
  intra_store_all_debug_information();

  if (sra_mode == SRA_MODE_EARLY_INTRA)
    ret = TODO_update_ssa;
  else
    ret = TODO_update_ssa | TODO_rebuild_alias;

 out:
  sra_deinitialize ();
  return ret;
}

/* Perform early intraprocedural SRA.  */
static unsigned int
new_early_intra_sra (void)
{
  sra_mode = SRA_MODE_EARLY_INTRA;
  return perform_new_intra_sra ();
}

/* Perform "late" intraprocedural SRA.  */
static unsigned int
new_intra_sra (void)
{
  sra_mode = SRA_MODE_INTRA;
  return perform_new_intra_sra ();
}

static bool
new_sra_gate (void)
{
  return flag_tree_sra != 0;
}


struct gimple_opt_pass pass_early_new_sra =
{
 {
  GIMPLE_PASS,
  "ensra",	 			/* name */
  new_sra_gate,				/* gate */
  new_early_intra_sra,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_SRA,				/* tv_id */
  PROP_cfg | PROP_ssa,                  /* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func | TODO_verify_ssa 	/* todo_flags_finish */
 }
};


struct gimple_opt_pass pass_new_sra =
{
 {
  GIMPLE_PASS,
  "nsra",	 			/* name */
  new_sra_gate,				/* gate */
  new_intra_sra,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_SRA,				/* tv_id */
  PROP_cfg | PROP_ssa,                  /* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func | TODO_verify_ssa 	/* todo_flags_finish */
 }
};
