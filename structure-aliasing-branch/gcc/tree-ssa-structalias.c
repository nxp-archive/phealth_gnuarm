  /* Tree based points-to analysis
   Copyright (C) 2002, 2003, 2004 Free Software Foundation, Inc.
   Contributed by Daniel Berlin <dberlin@dberlin.org>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "ggc.h"
#include "bitmap.h"
#include "tree-ssa-structalias.h"
#include "flags.h"
#include "rtl.h"
#include "tm_p.h"
#include "hard-reg-set.h"
#include "basic-block.h"
#include "output.h"
#include "errors.h"
#include "expr.h"
#include "diagnostic.h"
#include "tree.h"
#include "c-common.h"
#include "tree-flow.h"
#include "tree-inline.h"
#include "varray.h"
#include "c-tree.h"
#include "tree-gimple.h"
#include "hashtab.h"
#include "function.h"
#include "cgraph.h"
#include "tree-pass.h"
#include "timevar.h"
#include "alloc-pool.h"
#include "splay-tree.h"

/* The idea is to generate constraints from the program, then solve
   the resulting constraints in order to generate the points-to sets.
   There are three types of constraint expressions, DEREF, ADDRESSOF, and
   SCALAR.  Each constraint expression consists of a type, a variable,
   and an offset.  
   
   SCALAR is a constraint expression type used to represent x, whether
   it appears on the LHS or the RHS of a statement.
   DEREF is a constraint expression type used to represent *x, whether
   it appears on the LHS or the RHS of a statement. 
   ADDRESSOF is a constraint expression used to represent &x, whether
   it apepars on the LHS or the RHS of a statement.
   
   Each variable in the program is assigned an integer id, and each
   field of a variable is assigned an integer id as well.  There is an
   end field that tells us the "length" (in fields) of a variable (to
   avoid spurious points-to sets due to inaccuracy that can arise in
   handling structure casting.
   
   a variable ranges from id0 ... idx < where x < end (not <=)
   struct f
   {
     int a;
     int b;
   } foo;
   int c bar;
   ->
   foo -> id 1, end 4
   foo#a -> id 2, end 4
   foo#b -> id 3, end 4 
   bar -> id 4, end 5

   
   After constructing constraints, we put them into a constraint graph, where
   the edges of the graph represent copy constraints (SCALAR-> SCALAR
   constraints).
   We then perform static cycle elimination on the constraint graph, as well
   as off-line variable substitution.
   Finally, we solve the constraint graph, producing our points-to solutions.

   TODO: We currently say things assigned from integers point to anything.
   This is stupid.  We only need to care about integers if an integer is later
   casted to a pointer type, or a pointer casted to an integer type.

   TODO: For extra speed in iterations, we should sort types so that types
   with their address taken come first.

   TODO: Adding offsets to pointer-to-structures can be handled (IE not punted
   on and turned into anything), but isn't.  You can just see what field
   numbre inside the pointed-to struct it's going to access.
   
   TODO: Constant bounded arrays can be handled as if they were structs of the
   same number of elements. 

   TODO: Modeling heap and incoming pointers becomes much better if we can add
   fields to them at solve time.  We could do this by making varmap a
   hashtable, mapping from real var + bit offset -> our var id.
   Then we make our edge weights bit offsets, and do lookups instead of simple
   addition.
   This would probably significantly slow the solver, however.
   We'll have to investigate.

   TODO: Use malloc vectors and a bitmap obstack, not ggc_alloc'd things.
   Also, we allocate a lot of bitmaps we don't need.  */
static unsigned int create_variable_info_for (tree, const char *, unsigned int);
static struct constraint_expr get_constraint_for (tree);
static void build_constraint_graph (void);
static unsigned int max_fields;
static splay_tree num_fields_for_type;
DEF_VEC_GC_P(constraint_t);
static struct constraint_stats
{
  unsigned int total_vars;
  unsigned int collapsed_vars;
  unsigned int unified_vars_static;
  unsigned int unified_vars_dynamic;
  unsigned int iterations;
} stats;

struct variable_info
{
  /* Name of this variable */
  const char *name;
  /* Tree that this variable is associated with.  */
  tree decl;
  /* ID of the last field/etc of this variable.  */
  unsigned int end;
  /* Node in the graph that represents the constraints and points-to
     solution for the variable.  */
  unsigned int node;
  /* True if the address of this variable is taken.  Needed for
     rountev-chandra.  */
  unsigned int address_taken:1;
  /* True if this variable is the target of a dereference.  Needed for
     rountev-chandra.  */
  unsigned int indirect_target:1;
  /* True if this is a variable created by the constraint analysis, such as
     heap variables and constraints we had to break up.  */
  unsigned int is_artificial_var:1;
  /* Because we punt on union vars right now, we have to identify them so that
     we can mark them as not type safe. */
  unsigned int is_unknown_size_var:1;  
  /* Points-to set for this variable.  */
  bitmap solution;
  /* Variable ids represented by this variable node.  */
  bitmap variables;
  /* Vector of complex constraints for this node.  Complex
     constraints are those involving dereferences.  */
  VEC(constraint_t) *complicated;
};
typedef struct variable_info *varinfo_t;

/* Pool of variable info structures.  */
static alloc_pool variable_info_pool;
DEF_VEC_GC_P (varinfo_t);
static VEC(varinfo_t) *varmap;
#define get_varinfo(n) VEC_index(varinfo_t, varmap, n)

/* Variable that represents the unknown pointer.  */
static varinfo_t var_anything;
static tree anything_tree;
static unsigned int anything_id;

/* Variable that represents the NULL pointer.  */
static varinfo_t var_nothing;
static tree nothing_tree;
static unsigned int nothing_id;

/* Variable that represents readonly memory.  */
static varinfo_t var_readonly;
static tree readonly_tree;
static unsigned int readonly_id;

/* Return a new variable info structure consisting for a variable
   named NAME, ending at id END, and using constraint graph node
   NODE.  */
static varinfo_t
new_var_info (tree t, const char *name, unsigned int end, unsigned int node)
{
  varinfo_t ret = pool_alloc (variable_info_pool);
  ret->name = name;
  ret->decl = t;
  ret->end = end;
  ret->node = node;
  ret->address_taken = false;
  ret->indirect_target = false;
  ret->is_artificial_var = false;
  ret->is_unknown_size_var = false;
  ret->solution = BITMAP_GGC_ALLOC ();
  bitmap_clear (ret->solution);
  ret->variables = BITMAP_GGC_ALLOC ();
  bitmap_clear (ret->variables);
  ret->complicated = NULL;
  return ret;
}

typedef enum {SCALAR, DEREF, ADDRESSOF} constraint_expr_type;

/* An expression that appears in a constraint.  */
struct constraint_expr 
{
  /* Constraint type.  */
  constraint_expr_type type;
  /* Variable we are referring to in the constraint.  */
  unsigned int var;
  /* Offset is actually specified in field numbers right now, not bits.  */
  unsigned int offset;
};

static struct constraint_expr do_deref (struct constraint_expr);

/* Constraints are made up of two constraint expressions, one LHS, and one
   RHS.  */
struct constraint
{
  struct constraint_expr lhs;
  struct constraint_expr rhs;
};

/* List of constraints that we use to build the constraint graph from.  */
static VEC(constraint_t) *constraints;
static alloc_pool constraint_pool;

/* An edge in the constraint graph.  We technically have no use for
   the src, since it will always be the same node that we are indexing
   into the pred/succ arrays with, but it's nice for checking
   purposes. 
   The edges are weighted, with a bit set in weights if we have an edge with
   that weight.  */
struct constraint_edge
{
  unsigned int src;
  unsigned int dest;
  bitmap weights;
};

typedef struct constraint_edge *constraint_edge_t;
static alloc_pool constraint_edge_pool;

/* Return a new constraint edge from SRC to DEST.  */
static constraint_edge_t
new_constraint_edge (unsigned int src, unsigned int dest)
{
  constraint_edge_t ret = pool_alloc (constraint_edge_pool);
  ret->src = src;
  ret->dest = dest;
  ret->weights = NULL;
  return ret;
}

DEF_VEC_GC_P (constraint_edge_t);

/* The constraint graph is simply a set of adjacency vectors, one per
   variable. succs[x] is the vector of successors for variable x, and preds[x]
   is the vector of predecessors for variable x.  */
struct constraint_graph
{
  VEC(constraint_edge_t) **succs;
  VEC(constraint_edge_t) **preds;
};

typedef struct constraint_graph *constraint_graph_t;

static constraint_graph_t graph;

/* Create a new constraint consisting of LHS and RHS expressions.  */

static constraint_t 
new_constraint (const struct constraint_expr lhs,
		const struct constraint_expr rhs)
{
  constraint_t ret = pool_alloc (constraint_pool);
  ret->lhs = lhs;
  ret->rhs = rhs;
  return ret;
}

/* Print out constraint C to FILE.  */

void
print_constraint (FILE *file, constraint_t c)
{
  if (c->lhs.type == ADDRESSOF)
    fprintf (file, "&");
  else if (c->lhs.type == DEREF)
    fprintf (file, "*");  
  fprintf (file, "%s ", get_varinfo (c->lhs.var)->name);
  if (c->lhs.offset != 0)
    fprintf (file, "+ %d ", c->lhs.offset);
  fprintf (file, " = ");
  if (c->rhs.type == ADDRESSOF)
    fprintf (file, "&");
  else if (c->rhs.type == DEREF)
    fprintf (file, "*");
  fprintf (file, "%s ", get_varinfo (c->rhs.var)->name);
  if (c->rhs.offset != 0)
    fprintf (file, "+ %d ", c->rhs.offset);
  fprintf (file, "\n");
}
void
debug_constraint (constraint_t c)
{
  print_constraint (stdout, c);
}

void
print_constraints (FILE *file)
{
  int i;
  constraint_t c;
  for (i = 0; VEC_iterate (constraint_t, constraints, i, c); i++)
    print_constraint (file, c);
}

void
debug_constraints (void)
{
  print_constraints (stdout);
}

/* Map from trees to variable ids.  */    
static htab_t id_for_tree;

typedef struct tree_id
{
  tree t;
  unsigned int id;
} *tree_id_t;

static hashval_t 
tree_id_hash (const void *p)
{
  const tree_id_t ta = (tree_id_t) p;
  return htab_hash_pointer (ta->t);
}

static int
tree_id_eq (const void *p1, const void *p2)
{
  const tree_id_t ta1 = (tree_id_t) p1;
  const tree_id_t ta2 = (tree_id_t) p2;
  return ta1->t == ta2->t;
}

/* Insert ID as the variable id for tree T in the hashtable.  */
static void 
insert_id_for_tree (tree t, int id)
{
  void **slot;
  struct tree_id finder;
  tree_id_t new_pair;
  
  finder.t = t;
  slot = htab_find_slot (id_for_tree, &finder, INSERT);
  gcc_assert (*slot == NULL);
  new_pair = xmalloc (sizeof (struct tree_id));
  new_pair->t = t;
  new_pair->id = id;
  *slot = (void *)new_pair;
}

#if 0
/* Find the variable ID for tree T in the hashtable.  */
static unsigned int
lookup_id_for_tree (tree t)
{
  tree_id_t pair;
  struct tree_id finder;
  finder.t = t;
  pair = htab_find (id_for_tree,  &finder);
  gcc_assert (pair != NULL);
  return pair->id;
}
#endif
static unsigned int get_num_fields_for_var (tree);

/* Find the variable ID for tree T in the hashtable.  */
static unsigned int
get_id_for_tree (tree t)
{
  tree_id_t pair;
  struct tree_id finder;
  finder.t = t;
  pair = htab_find (id_for_tree,  &finder);
  if (pair == NULL)
    return create_variable_info_for (t, alias_get_name (t),
				     get_num_fields_for_var (t));
  
  return pair->id;
}

/* Get a constraint expression from an SSA_VAR_P node.  */
static struct constraint_expr
get_constraint_exp_from_ssa_var (tree t)
{
  struct constraint_expr cexpr;

  if (TREE_CODE (t) == SSA_NAME 
      && TREE_CODE (SSA_NAME_VAR (t)) == PARM_DECL 
      && default_def (SSA_NAME_VAR (t)) == t
      /*&& POINTER_TYPE_P (TREE_TYPE (t))*/)
    return get_constraint_exp_from_ssa_var (SSA_NAME_VAR (t));

  cexpr.type = SCALAR;

  if (DECL_P (t) 
      && (TREE_STATIC (t) || TREE_PUBLIC (t))
      && !TREE_READONLY (t))
    {
      cexpr.type = ADDRESSOF;
      cexpr.var = anything_id;
    }
  else if (TREE_READONLY (t))
    {
      cexpr.type = ADDRESSOF;
      cexpr.var = readonly_id;
    }
  else
    cexpr.var = get_id_for_tree (t);    
    
  cexpr.offset = 0;
  return cexpr;
}

/* Process a completed constraint T, and add it to the constraint
   list.  */

static void
process_constraint (constraint_t t)
{
  struct constraint_expr rhs = t->rhs;
  struct constraint_expr lhs = t->lhs;
  
  gcc_assert (rhs.var < VEC_length (varinfo_t, varmap));
  gcc_assert (lhs.var < VEC_length (varinfo_t, varmap));
  /* ANYTHING == ANYTHING is pointless.  */
  if (lhs.var == anything_id && lhs.var == anything_id)
    return;
  /* If we have &ANYTHING = something, convert to SOMETHING = &ANYTHING) */
  else if (lhs.var == anything_id && lhs.type == ADDRESSOF)
    {
      rhs = t->lhs;
      lhs = t->rhs;
      lhs.type = SCALAR;
      process_constraint (t);
    }   
  /* This can happen in our IR with things like n->a = *p */
  else if (rhs.type == DEREF && lhs.type == DEREF)
    {
      /* Split into tmp = *rhs, *lhs = tmp */
      tree tmpvar = create_tmp_var_raw (ptr_type_node, "doubledereftmp");
      struct constraint_expr tmplhs = get_constraint_exp_from_ssa_var (tmpvar);
      process_constraint (new_constraint (tmplhs, rhs));
      process_constraint (new_constraint (lhs, tmplhs));
    }
  else if (rhs.type == ADDRESSOF)
    {
      /* Normalize the constraint.  */
      unsigned int v = rhs.var + rhs.offset;     
      unsigned int end = get_varinfo (rhs.var)->end;
      constraint_t nc;
      struct constraint_expr new_expr;
      new_expr.type = ADDRESSOF;
      new_expr.var = v;
      new_expr.offset = 0;
      nc = new_constraint (lhs, new_expr);
      while (v < end)
	{
	  get_varinfo (v)->address_taken = true;
	  v++;
	}
      VEC_safe_push (constraint_t, constraints, nc);
    }
  else
    {
      if (lhs.type != DEREF && rhs.type == DEREF)
	get_varinfo (lhs.var)->indirect_target = true;
      VEC_safe_push (constraint_t, constraints, t);
    }
}

/* Map from type, offset to fieldnumber.  */
struct fieldnum_offset
{
  tree type;
  HOST_WIDE_INT offset;
  unsigned int fieldnum;
};
typedef struct fieldnum_offset *fieldnum_offset_t;

static unsigned int get_num_fields_for_type (tree);

static hashval_t
fieldnum_offset_hash (const void *p)
{
  const fieldnum_offset_t fo = (fieldnum_offset_t) p;
  return iterative_hash_object (TYPE_HASH (fo->type), fo->offset);
}
 
static int
fieldnum_offset_eq (const void *p1, const void *p2)
{
  const fieldnum_offset_t fo1 = (fieldnum_offset_t) p1;
  const fieldnum_offset_t fo2 = (fieldnum_offset_t) p2;
  return fo1->type == fo2->type && fo1->offset == fo2->offset;
}
static htab_t fieldnum_table;

/* Find {type,offset} in the hashtable, and return the corresponding field
   number.  */

static fieldnum_offset_t
lookup_fieldnum_of_offset (tree type, HOST_WIDE_INT offset)
{
  struct fieldnum_offset lookfor;
  void **slot;
  lookfor.type = type;
  lookfor.offset = offset;
  slot = htab_find_slot (fieldnum_table, &lookfor, NO_INSERT);
  if (!slot)
    return NULL;
  return ((fieldnum_offset_t) *slot);
}

/*  Insert {type,offset}->fieldnum into the fieldnum hashtable.  */

static void
insert_offset_for_fieldnum (tree type, HOST_WIDE_INT offset,
			    unsigned int fieldnum)
{
  void **slot;
  fieldnum_offset_t new_fo = xmalloc (sizeof (struct fieldnum_offset));
  new_fo->type = type;
  new_fo->offset = offset;
  new_fo->fieldnum = fieldnum;
  slot = htab_find_slot (fieldnum_table, new_fo, INSERT);
  if (*slot)
    free (*slot);
  *slot = (void *)new_fo;
}

/* Return the position, in bits, of FIELD_DECL from the beginning of it's
   structure.  */
	    
static HOST_WIDE_INT
bitpos_of_field (const tree fdecl)
{
  return (tree_low_cst (DECL_FIELD_OFFSET (fdecl), 1) * 8) 
    + tree_low_cst (DECL_FIELD_BIT_OFFSET (fdecl), 1);
}

/* Find the field number for a given FIELD_DECL, and return it.
   abort if we can't find one.  */

static unsigned int
get_fieldnum_for_field_decl (tree t)
{
  fieldnum_offset_t result;
  result = lookup_fieldnum_of_offset (DECL_FIELD_CONTEXT (t), 
				      bitpos_of_field (t));

  /* This can happen in the degenerate case that none of the variables
     themselves directly have this type, but are pointers to it, etc.
     So we process that type and retry the lookup.  If this fails, of
     course, then something is definitely broken. */
  if (!result)
    {
      get_num_fields_for_type (DECL_FIELD_CONTEXT (t));
      result = lookup_fieldnum_of_offset (DECL_FIELD_CONTEXT (t),
					  bitpos_of_field (t));
      gcc_assert (result != NULL);
    }
  return result->fieldnum;
}

/* Given a component ref, return the constraint_expr for it.  */

static struct constraint_expr
get_constraint_for_component_ref (tree t)
{
  struct constraint_expr result;
  struct constraint_expr varlookup;
  varray_type stack;
  VARRAY_TREE_INIT (stack, 1, "component ref stack");

  result.offset = 0;
  result.type = SCALAR;
  result.var = 0;
  
  while (!SSA_VAR_P (t) && TREE_CODE_CLASS (TREE_CODE (t)) != 'c')
    {
      VARRAY_PUSH_TREE (stack, t);
      t = TREE_OPERAND (t, 0);
    }

  if (!integer_zerop (t))
    {
      varlookup = get_constraint_exp_from_ssa_var (t);
      if (varlookup.var <= readonly_id)
	result.type = DEREF;
      
      result.var = varlookup.var;
    }
  else
    {    
      result.var = nothing_id;
    }

  while (VARRAY_ACTIVE_SIZE (stack) > 0)
    {
      tree operator = VARRAY_TOP_TREE (stack);
      tree operand = NULL_TREE;

      VARRAY_POP (stack);
      if (TREE_CODE (operator) != INDIRECT_REF)
	operand = TREE_OPERAND (operator, 1);

      if (TREE_CODE (operator) == COMPONENT_REF
	  && TREE_CODE (DECL_FIELD_CONTEXT (operand)) == RECORD_TYPE)
	result.offset += get_fieldnum_for_field_decl (operand);
      else if (TREE_CODE (operator) == COMPONENT_REF)
	{
	  break;
	}
      else if (TREE_CODE (operator) == ARRAY_REF)
	{
	  result = do_deref (result);
	  break;
	}
      else if (TREE_CODE (operator) == INDIRECT_REF)
	{
	  result = do_deref (result);
	}
    }
  if (result.type == SCALAR)
    {
      result.var += result.offset;
      result.offset= 0;
    }
  
  return result;
}

/* Dereference the constraint expression CONS, and return the result.
   DEREF (ADDRESSOF) = SCALAR
   DEREF (SCALAR) = DEREF
   DEREF (DEREF) = temp = DEREF1; result = DEREF(temp)
   This is needed so that we can handle dereferencing DEREF constraints.  */
static struct constraint_expr
do_deref (struct constraint_expr cons)
{
  if (cons.type == SCALAR)
    {
      cons.type = DEREF;
      return cons;
    }
  else if (cons.type == ADDRESSOF)
    {
      cons.type = SCALAR;
      return cons;
    }
  else if (cons.type == DEREF)
    {
      tree tmpvar = create_tmp_var_raw (ptr_type_node, "derefmp");
      struct constraint_expr tmplhs = get_constraint_exp_from_ssa_var (tmpvar);
      process_constraint (new_constraint (tmplhs, cons));
      cons.var = tmplhs.var;
      return cons;
    }
  gcc_unreachable ();
}

/* Given a TREE, return the constraint expression for it.  */
static struct constraint_expr
get_constraint_for (tree t)
{
  struct constraint_expr temp;

  if (is_gimple_min_invariant (t) && integer_zerop (t))
    {
      temp.var = nothing_id;
      temp.type = SCALAR;
      temp.offset = 0;
      return temp;
    }

  switch (TREE_CODE_CLASS (TREE_CODE (t)))
    {
    case 'e':
      {
	switch (TREE_CODE (t))
	  {
	  case ADDR_EXPR:
	    {
	      temp = get_constraint_for (TREE_OPERAND (t, 0));
	       if (temp.type == DEREF)
		 temp.type = SCALAR;
	       else
		 temp.type = ADDRESSOF;
	      return temp;
	    }
	  case CALL_EXPR:
	    if (call_expr_flags (t) & (ECF_MALLOC | ECF_MAY_BE_ALLOCA))
	      {
		tree heapvar = create_tmp_var_raw (ptr_type_node, "HEAP");
		temp.var = create_variable_info_for (heapvar,
						     alias_get_name (heapvar),
						     1);
		get_varinfo (temp.var)->is_artificial_var = 1;
		temp.type = ADDRESSOF;
		temp.offset = 0;
		return temp;
	      }
	  default:
	    {
	      temp.type = ADDRESSOF;
	      temp.var = anything_id;
	      temp.offset = 0;
	      return temp;
	    }
	  }
      }
    case 'r':
      {
	switch (TREE_CODE (t))
	  {
	  case INDIRECT_REF:
	    {
	      temp = get_constraint_for (TREE_OPERAND (t, 0));
	      temp = do_deref (temp);
	      return temp;
	    }
	  case ARRAY_REF:
	  case COMPONENT_REF:
	    temp = get_constraint_for_component_ref (t);
	    return temp;
	    /*	    return get_constraint_for (get_base_address (t));*/
	  default:
	    {
	      temp.type = ADDRESSOF;
	      temp.var = anything_id;
	      temp.offset = 0;
	      return temp;
	    }
	  }
      }
    case 'x':
      {
	switch (TREE_CODE (t))
	  {
	  case PHI_NODE:	   
	    return get_constraint_for (PHI_RESULT (t));
	  case SSA_NAME:
	      return get_constraint_exp_from_ssa_var (t);
	  default:
	    {
	      temp.type = ADDRESSOF;
	      temp.var = anything_id;
	      temp.offset = 0;
	      return temp;
	    }
	  }
      }
    case 'd':
      return get_constraint_exp_from_ssa_var (t);
    default:
      {
	temp.type = ADDRESSOF;
	temp.var = anything_id;
	temp.offset = 0;
	return temp;
      }
    }
}


/* Handle aggregate copies by expanding into copies of the respective
   fields of the structures.  */

static void
do_structure_copy (tree lhsop, tree rhsop)
{
  struct constraint_expr lhs, rhs;
  size_t size;
  size_t i;
  
  lhs = get_constraint_for (lhsop);  
  rhs = get_constraint_for (rhsop);
  /* If the RHS is anything, set all the LHS fields to anything */
  if (rhs.var == anything_id)
    {
      size = get_num_fields_for_type (TREE_TYPE (lhsop));
      for (i = 0; i < size; i++)
	{
	  struct constraint_expr templhs = lhs;
	  struct constraint_expr temprhs = rhs;
	  if (templhs.type == SCALAR)
	    templhs.var += i;
	  else
	    templhs.offset +=i;
	  process_constraint (new_constraint (templhs, temprhs));
	}
    }
  else
    {
      size = get_num_fields_for_type (TREE_TYPE (rhsop));
      for (i = 0; i < size; i++)
	{
	  struct constraint_expr templhs = lhs;
	  struct constraint_expr temprhs = rhs;
	  
	  if (templhs.type == SCALAR)
	    templhs.var += i;
	  else
	    templhs.offset += i;
	  
	  if (temprhs.type == SCALAR)
	    temprhs.var += i;
	  else
	    temprhs.offset += i;
	  process_constraint (new_constraint (templhs, temprhs));
	}
    }
}

/*  Tree walker that is the heart of the aliasing infrastructure.
    TP is a pointer to the current tree.
    WALK_SUBTREES specifies whether to continue traversing subtrees or
    not.
    Returns NULL_TREE when we should stop.
    
    This function is the main part of the aliasing infrastructure. It
    walks the trees, calling the appropriate alias analyzer functions to process
    various statements.  */

static void
find_func_aliases (tree t)
{
  struct constraint_expr lhs, rhs;
  switch (TREE_CODE (t))
    {      
    case PHI_NODE:
      {
	int i;
	lhs = get_constraint_for (PHI_RESULT (t));
	for (i = 0; i < PHI_NUM_ARGS (t); i++)
	  {
	    rhs = get_constraint_for (PHI_ARG_DEF (t, i));
	    process_constraint (new_constraint (lhs, rhs));
	  }
      }
      break;
    case MODIFY_EXPR:
      {
	tree lhsop = TREE_OPERAND (t, 0);
	tree rhsop = TREE_OPERAND (t, 1);
	int i;	
	if (AGGREGATE_TYPE_P (TREE_TYPE (lhsop)) 
	    && AGGREGATE_TYPE_P (TREE_TYPE (rhsop)))
	  {
	    do_structure_copy (lhsop, rhsop);
	  }
	else
	  {
	    lhs = get_constraint_for (lhsop);
	    switch (TREE_CODE_CLASS (TREE_CODE (rhsop)))
	      {
		/* RHS's that are directly some other variable or thing, we
		   just get the single constraint for.  */
	      case 'r':
	      case 'd':
	      case 'c':
	      case 'x':
	      case 'e':
		{
		  rhs = get_constraint_for ( rhsop);
		  process_constraint (new_constraint (lhs, rhs));
		}
		break;
	      default:
		for (i = 0; i < TREE_CODE_LENGTH (TREE_CODE (rhsop)); i++)
		  {
		    tree op = TREE_OPERAND (rhsop, i);
		    rhs = get_constraint_for (op);
		    process_constraint (new_constraint (lhs, rhs));
		  }
	      }      
	  }
      }
      break;
    default:
      break;
    }
}

/* Create a varinfo structure for name, and add it to the varmap.  */
static unsigned int
create_variable_info_for (tree decl, const char *name, unsigned int num_fields)
{
  unsigned int index = VEC_length (varinfo_t, varmap);
  unsigned int end;
  unsigned int j = 0;
  varinfo_t vi;
  if (num_fields == 0)
    num_fields = 1;
 
  end = VEC_length (varinfo_t, varmap) + num_fields;
  vi = new_var_info (decl, name, end, index);
  vi->decl = decl;
  if (decl 
      && (TREE_CODE (TREE_TYPE (decl)) == UNION_TYPE 
	  || TREE_CODE (TREE_TYPE (decl)) == QUAL_UNION_TYPE
	  || TREE_CODE (TREE_TYPE (decl)) == ARRAY_TYPE))
    vi->is_unknown_size_var = true;
  insert_id_for_tree (vi->decl, index);  
  VEC_safe_push (varinfo_t, varmap, vi);
  stats.total_vars++;
  while (--num_fields > 0)
    {
      unsigned int newindex = VEC_length (varinfo_t, varmap);
      char *newname = xcalloc (1, strlen (vi->name) + 4);
      sprintf (newname, "%s#%d", name, ++j);
      vi = new_var_info (decl, newname, end, newindex);
      VEC_safe_push (varinfo_t, varmap, vi);
      stats.total_vars++;
    }
  return index;
}

/* Print out the points-to solution for VAR to FILE.  */

void
print_solution_for_var (FILE *file, unsigned int var)
{
  varinfo_t vi = get_varinfo (var);
  unsigned int i;
  fprintf (file, "%s = {", vi->name);
  
  EXECUTE_IF_SET_IN_BITMAP (get_varinfo (vi->node)->solution, 0, i,
  {
    fprintf (file, "%s,", get_varinfo (i)->name);
  });
  fprintf (file, "}\n");
}

void
debug_solution_for_var (unsigned int var)
{
  print_solution_for_var (stdout, var);
}

/* Return the number of fields for type T. 
   We punt on arrays and unions.  */

static unsigned int
get_num_fields_for_type (tree t)
{
  unsigned int counter = 0;
  splay_tree_node result;
  result = splay_tree_lookup (num_fields_for_type, (splay_tree_key) t);
  if (result)
    return result->value;

  if (TREE_CODE (t) == RECORD_TYPE)
    {
      tree field;
      for (field = TYPE_FIELDS (t); field; field = TREE_CHAIN (field))
	{	
	  if (TREE_CODE (field) == FIELD_DECL)
	    {
	      tree fieldtype = TREE_TYPE (field);	     
	      insert_offset_for_fieldnum (t, 
					  bitpos_of_field (field),
					  counter);
	      if (AGGREGATE_TYPE_P (fieldtype) && fieldtype != t)
		counter += get_num_fields_for_type (fieldtype);
	      else
	    counter++;
	    }
	}
    }
  else if (TREE_CODE (t) == ARRAY_TYPE)
    { 
      return 1;
    }
  /* Must be a union */
  else if (TREE_CODE (t) == UNION_TYPE || TREE_CODE (t) == QUAL_UNION_TYPE)
    {
      return 1;
    }
  else
    return 1;
  max_fields = MAX (max_fields, counter);
  splay_tree_insert (num_fields_for_type, (splay_tree_key) t, 
		     (splay_tree_value) counter);
  return counter;
}
	    
static unsigned int
get_num_fields_for_var (tree t)
{
  if (!AGGREGATE_TYPE_P (TREE_TYPE (t)))
    return 0;
  return get_num_fields_for_type (TREE_TYPE (t));
}
/* Create varinfo structures for all of the variables in the
   function.  */

static void
create_variable_infos (void)
{
  tree t;
  size_t i;
#if 0
  tree tmpvar;
  
  tmpvar = create_tmp_var_raw (ptr_type_node, "OUTSIDEOFFUNC");
  i = create_variable_info_for (tmpvar, "OUTSIDEOFFUNC", 0);
  get_varinfo (i)->is_artificial_var = 1;
#endif
  for (t = DECL_ARGUMENTS (current_function_decl);
       t;
       t = TREE_CHAIN (t))
    {
      struct constraint_expr lhs;
      struct constraint_expr rhs;
      size_t lhsvar;
      lhs.offset = 0;
      lhs.type = SCALAR;
      lhs.var  = create_variable_info_for (t, alias_get_name (t),
					   get_num_fields_for_var (t));
      get_varinfo (lhs.var)->is_artificial_var = true;
#if 0
      rhs.var = lookup_id_for_tree (tmpvar);
#else
      rhs.var = anything_id;
#endif
      rhs.type = ADDRESSOF;
      rhs.offset = 0;
      lhsvar = lhs.var;
      for (i = 0 ; i < get_num_fields_for_var (t); i++)
	{
	  struct constraint_expr temp = lhs;
	  temp.var += i;
	  
	  process_constraint (new_constraint (temp, rhs));
	}
    }	

}

/* Return true if two constraint expressions are equal.  */
static bool
constraint_expr_equal (struct constraint_expr a, struct constraint_expr b)
{
  return a.type == b.type
    && a.var == b.var
    && a.offset == b.offset;
}

/* Return true if constraint expression a is less than constraint expression
   b.  This is just arbitrary, but consistent, in order to give them an
   ordering.  */
static bool
constraint_expr_less (struct constraint_expr a, struct constraint_expr b)
{
  if (a.type == b.type)
    {
      if (a.var == b.var)
	return a.offset < b.offset;
      else
	return a.var < b.var;
    }
  else
    return a.type < b.type;
}

/* Return true if constraint e is less than constraint b.  This is just
   arbitrary, but consistent, in order to give them an ordering.  */
static bool
constraint_less (const constraint_t a, const constraint_t b)
{
  if (constraint_expr_less (a->lhs, b->lhs))
    return true;
  else if (constraint_expr_less (b->lhs, a->lhs))
    return false;
  else
    return constraint_expr_less (a->rhs, b->rhs);
}

/* Return true if two constraints are equal.  */
  
static bool
constraint_equal (struct constraint a, struct constraint b)
{
  return constraint_expr_equal (a.lhs, b.lhs) 
    && constraint_expr_equal (a.rhs, b.rhs);
}


/* Find a constraint LOOKFOR in the sorted constraint vector VEC */

static constraint_t
constraint_vec_find (VEC(constraint_t) *vec,
		     struct constraint lookfor)
{
  unsigned int place;  
  constraint_t found;
  if (vec == NULL)
    return NULL;

  place = VEC_lower_bound (constraint_t, vec, &lookfor, constraint_less);
  if (place >= VEC_length (constraint_t, vec))
    return NULL;
  found = VEC_index (constraint_t, vec, place);
  if (!constraint_equal (*found, lookfor))
    return NULL;
  return found;
}

/* Union two constraint vectors, TO and FROM.  Put the result in TO.  */

static void
constraint_set_union (VEC(constraint_t) **to,
		      VEC(constraint_t) **from)
{
  int i;
  constraint_t c;
  for (i = 0; VEC_iterate (constraint_t,*from, i, c); i++)
    {
      if (constraint_vec_find (*to, *c) == NULL)
	{
	  unsigned int place = VEC_lower_bound (constraint_t, *to, c,
						constraint_less);
	  VEC_safe_insert (constraint_t, *to, place, c);
	}
    }
}

/* Take a solution set SET, add OFFSET to each member of the set, 
   and return the result.  */

static void
solution_set_add (bitmap set, unsigned int offset)
{
  bitmap result = BITMAP_XMALLOC ();
  unsigned int i;
  EXECUTE_IF_SET_IN_BITMAP (set, 0, i,
  {
    /* If this is a properly sized variable, only add offset if it's less than
       end.  Otherwise, it is globbed to a single variable.  */
      
    if ((i + offset) < get_varinfo (i)->end)
      {
	bitmap_set_bit (result, i + offset);
      }
    else if (get_varinfo (i)->is_artificial_var 
	     || get_varinfo (i)->is_unknown_size_var)
      {
	bitmap_set_bit (result, i);
      }
  });
  
  bitmap_copy (set, result);  
  BITMAP_XFREE (result);
}

/* Union solution sets TO and FROM, and add INC to each member of FROM in the
   process.  */

static bool
set_union_with_increment  (bitmap to, bitmap from, unsigned int inc)
{
  if (inc == 0)
    return bitmap_a_or_b (to, to, from);
  else
    {
      bitmap tmp;
      bool res;
      tmp = BITMAP_XMALLOC ();
      bitmap_copy (tmp, from);
      solution_set_add (tmp, inc);
      res = bitmap_a_or_b (to, to, tmp);
      BITMAP_XFREE (tmp);
      return res;
    }
}

/* Insert C into the list of complex constraints for VAR.  */

static void
insert_into_complicated (unsigned int var, constraint_t c)
{
  varinfo_t vi = get_varinfo (var);
  unsigned int place = VEC_lower_bound (constraint_t, vi->complicated, c,
					constraint_less);
  VEC_safe_insert (constraint_t, vi->complicated, place, c);
}


/* Compare two constraint edges, return true if they are equal.  */

static bool
constraint_edge_equal (struct constraint_edge a, struct constraint_edge b)
{
  return a.src == b.src && a.dest == b.dest;
}

/* Compare two constraint edges, return true if A is less than B */

static bool
constraint_edge_less (const constraint_edge_t a, const constraint_edge_t b)
{
  if (a->dest < b->dest)
    return true;
  else if (a->dest == b->dest)
    return a->src < b->src;
  else
    return false;
}


static constraint_edge_t 
constraint_edge_vec_find (VEC(constraint_edge_t) *vec, 
			  struct constraint_edge lookfor)
{
  unsigned int place;  
  constraint_edge_t edge;
  place = VEC_lower_bound (constraint_edge_t, vec, &lookfor, 
			   constraint_edge_less);
  edge = VEC_index (constraint_edge_t, vec, place);
  if (!constraint_edge_equal (*edge, lookfor))
    return NULL;
  return edge;
}

/* Condense two variable nodes into a single variable node */

static void 
condense_varmap_nodes (unsigned int to, unsigned int src)
{
  varinfo_t tovi = get_varinfo (to);
  varinfo_t srcvi = get_varinfo (src);
  int i;
  constraint_t c;
  
  /* the src node, and all it's variables, are now the to node.  */
  srcvi->node = to;
  EXECUTE_IF_SET_IN_BITMAP (srcvi->variables, 0, i,
  {
    get_varinfo (i)->node = to;
  });
  
  /* Merge the src node variables and the to node variables.  */
  bitmap_set_bit (tovi->variables, src);
  bitmap_a_or_b (tovi->variables, tovi->variables, srcvi->variables);
  bitmap_clear (srcvi->variables);
  
  /* Move all complex constraints to the to node  */
  for (i = 0; VEC_iterate (constraint_t, srcvi->complicated, i, c); i++)
    {
      if (c->rhs.type == DEREF)
	{
	  c->rhs.var = to;
	}
      else
	{
	  c->lhs.var = to;
	}
    }
  constraint_set_union (&tovi->complicated, &srcvi->complicated);
  srcvi->complicated = NULL;
}

/* Erase EDGE from the graph.  */
static void
erase_graph_edge (constraint_graph_t graph, struct constraint_edge edge)
{
  VEC(constraint_edge_t) *predvec = graph->preds[edge.src];
  VEC(constraint_edge_t) *succvec = graph->succs[edge.dest];
  struct constraint_edge succe;
  unsigned int place;

  /* The successor will have the edges reversed.  */
  succe.dest = edge.src;
  succe.src = edge.dest;
  /* Remove from the successors.  */
  place = VEC_lower_bound (constraint_edge_t, succvec, &succe, 
			   constraint_edge_less);
  VEC_ordered_remove (constraint_edge_t, succvec, place);
  /* Remove from the predecessors.  */
  place = VEC_lower_bound (constraint_edge_t, predvec, &edge,
			   constraint_edge_less);
  VEC_ordered_remove (constraint_edge_t, predvec, place);
}

/* Remove edges involving node from graph.  */
static void
clear_edges_for_node (constraint_graph_t graph, unsigned int node)
{
  VEC(constraint_edge_t) *succvec = graph->succs[node];
  VEC(constraint_edge_t) *predvec = graph->preds[node];
  constraint_edge_t c;
  int i;
  
  /* Walk the successors, erase the associated preds.  */
  for (i = 0; VEC_iterate (constraint_edge_t, succvec, i, c); i++)
    if (c->dest != node)
      {
	unsigned int place;
	struct constraint_edge lookfor;
	lookfor.src = c->dest;
	lookfor.dest = node;
	place = VEC_lower_bound (constraint_edge_t, graph->preds[c->dest], 
				 &lookfor, constraint_edge_less);
	VEC_ordered_remove (constraint_edge_t, graph->preds[c->dest], place);
      }
  /* Walk the preds, erase the associated succs.  */
  for (i =0; VEC_iterate (constraint_edge_t, predvec, i, c); i++)
    if (c->dest != node)
      {
	unsigned int place;
	struct constraint_edge lookfor;
	lookfor.src = c->dest;
	lookfor.dest = node;
	place = VEC_lower_bound (constraint_edge_t, graph->succs[c->dest],
				 &lookfor, constraint_edge_less);
	VEC_ordered_remove (constraint_edge_t, graph->succs[c->dest], place);
      }    
  
  graph->preds[node] = NULL;
  graph->succs[node] = NULL;
}
static bool int_add_graph_edge (constraint_graph_t, unsigned int, 
				unsigned int, unsigned int);
static bool add_graph_edge (constraint_graph_t, struct constraint_edge);
static bitmap get_graph_weights (constraint_graph_t, struct constraint_edge);
static void 

/* Merge graph nodes w and n into node n.  */

merge_graph_nodes (constraint_graph_t graph, unsigned int n, unsigned int w)
{
  VEC(constraint_edge_t) *succvec = graph->succs[w];
  VEC(constraint_edge_t) *predvec = graph->preds[w];
  int i;
  constraint_edge_t c;
  
  for (i = 0; VEC_iterate (constraint_edge_t, predvec, i, c); i++)
    {
      unsigned int d = c->dest;
      struct constraint_edge olde;
      struct constraint_edge newe;
      bitmap temp;
      bitmap weights;
      if (c->dest == w)
	d = n;
      newe.src = n;
      newe.dest = d;
      add_graph_edge (graph, newe);
      olde.src = w;
      olde.dest = c->dest;
      temp = get_graph_weights (graph, olde);
      weights = get_graph_weights (graph, newe);
      bitmap_a_or_b (weights, weights, temp);
    }
  
  for (i = 0; VEC_iterate (constraint_edge_t, succvec, i, c); i++)
    {
      unsigned int d = c->dest;
      struct constraint_edge olde;
      struct constraint_edge newe;
      bitmap temp;
      bitmap weights;
      if (c->dest == w)
	d = n;
      newe.src = d;
      newe.dest = n;
      add_graph_edge (graph, newe);
      olde.src = c->dest;
      olde.dest = w;
      temp = get_graph_weights (graph, olde);
      weights = get_graph_weights (graph, newe);
      bitmap_a_or_b (weights, weights, temp);
    }
  clear_edges_for_node (graph, w);
}

/* Add a graph edge going from TO to FROM, with WEIGHT.  */

static bool
int_add_graph_edge (constraint_graph_t graph, unsigned int to, 
		    unsigned int from, unsigned int weight)
{
  if (to == from && weight == 0)
    {
      return false;
    }
  else
    {
      bool r;
      struct constraint_edge edge;
      edge.src = to;
      edge.dest = from;
      r = add_graph_edge (graph, edge);
      r |= !bitmap_bit_p (get_graph_weights (graph, edge), weight);
      bitmap_set_bit (get_graph_weights (graph, edge), weight);
      return r;
    }
}

  
/* Add edge NEWE to the graph.  */
static bool
add_graph_edge (constraint_graph_t graph, struct constraint_edge newe)
{
  unsigned int place;
  unsigned int src = newe.src;
  unsigned int dest = newe.dest;
  VEC(constraint_edge_t) *vec;
  vec = graph->preds[src];
  place = VEC_lower_bound (constraint_edge_t, vec, &newe, 
			   constraint_edge_less);
  if (place == VEC_length (constraint_edge_t, vec)
      || VEC_index (constraint_edge_t, vec, place)->dest != dest)
    {
      constraint_edge_t edge = new_constraint_edge (src, dest);
      bitmap weightbitmap;
      weightbitmap = BITMAP_GGC_ALLOC ();
      edge->weights = weightbitmap;
      VEC_safe_insert (constraint_edge_t, graph->preds[edge->src], place, edge);
      edge = new_constraint_edge (dest, src);
      edge->weights = weightbitmap;
      place = VEC_lower_bound (constraint_edge_t, graph->succs[edge->src],
			       edge, constraint_edge_less);
      VEC_safe_insert (constraint_edge_t, graph->succs[edge->src], place, edge);
      return true;
    }
  else
    return false;
}

/* Return true if LOOKFOR is an existing graph edge.  */
static bool
valid_graph_edge (constraint_graph_t graph, struct constraint_edge lookfor)
{
  return constraint_edge_vec_find (graph->preds[lookfor.src], lookfor) != NULL;
}


/* Return the bitmap representing the weights of edge LOOKFOR */
static bitmap
get_graph_weights (constraint_graph_t graph, struct constraint_edge lookfor)
{
  constraint_edge_t edge;
  unsigned int src = lookfor.src;
  VEC(constraint_edge_t) *vec;
  vec = graph->preds[src];
  edge = constraint_edge_vec_find (vec, lookfor);
  gcc_assert (edge != NULL);
  return edge->weights;
}

/* Build the constraint graph.  */

static void
build_constraint_graph (void)
{
  int i = 0;
  constraint_t c;
  graph = ggc_alloc (sizeof (struct constraint_graph));
  graph->succs = ggc_alloc_cleared (VEC_length (varinfo_t, varmap) * sizeof (*graph->succs));
  graph->preds = ggc_alloc_cleared (VEC_length (varinfo_t, varmap) * sizeof (*graph->preds));
  for (i = 0; VEC_iterate (constraint_t, constraints, i, c); i++)
    {
      struct constraint_expr lhs = c->lhs;
      struct constraint_expr rhs = c->rhs;
      if (lhs.type == DEREF)
	{
	  /* *x = y or *x = &y (complex) */
	  if (rhs.type == ADDRESSOF || rhs.var > anything_id)
	    insert_into_complicated (lhs.var, c);
	}
      else if (rhs.type == DEREF)
	{
	  /* NOT UNKNOWN = *y */
	  if (lhs.var > anything_id) 
	    insert_into_complicated (rhs.var, c);
	}
      else if (rhs.type == ADDRESSOF)
	{
	  /* x = &y */
	  bitmap_set_bit (get_varinfo (lhs.var)->solution, rhs.var);
	}
      else if (rhs.var > anything_id && lhs.var > anything_id)
	{
	  struct constraint_edge edge;
	  edge.src = lhs.var;
	  edge.dest = rhs.var;
	  /* x = y (simple) */
	  add_graph_edge (graph, edge);
	  bitmap_set_bit (get_graph_weights (graph, edge),
			  rhs.offset);
	}
    }
}
/* Changed variables on the last iteration.  */
static unsigned int changed_count;
static sbitmap changed;

/* Strongly Connected Component visitation info.  */
struct scc_info
{
  sbitmap visited;
  sbitmap in_component;
  int current_index;
  unsigned int *visited_index;
  varray_type scc_stack;
  varray_type unification_queue;
};

/* Recursive routine to find strongly connected components in GRAPH.  */

static void
scc_visit (constraint_graph_t graph, struct scc_info *si, unsigned int n)
{
  constraint_edge_t c;
  int i;
  gcc_assert (get_varinfo (n)->node == n);
  SET_BIT (si->visited, n);
  RESET_BIT (si->in_component, n);
  si->visited_index[n] = si->current_index ++;
  
  /* Visit all the successors.  */
  for (i = 0; VEC_iterate (constraint_edge_t, graph->succs[n], i, c); i++)
    {
      /* We only want to collapse the zero weight edges. */
      if (bitmap_bit_p (c->weights, 0))
	{
	  unsigned int w = c->dest;
	  if (!TEST_BIT (si->visited, w))
	    scc_visit (graph, si, w);
	  if (!TEST_BIT (si->in_component, w))
	    {
	      unsigned int t = get_varinfo (w)->node;
	      unsigned int nnode = get_varinfo (n)->node;
	      if (si->visited_index[t] < si->visited_index[nnode])
		{
		  get_varinfo (n)->node = t;
		}
	    }
	}
    }
  
  /* See if any components have been identified.  */
  if (get_varinfo (n)->node == n)
    {
      unsigned int t = si->visited_index[n];
      SET_BIT (si->in_component, n);
      while (VARRAY_ACTIVE_SIZE (si->scc_stack) != 0 
	     && t < si->visited_index[VARRAY_TOP_UINT (si->scc_stack)])
	{
	  unsigned int w = VARRAY_TOP_UINT (si->scc_stack);
	  get_varinfo (w)->node = n;
	  SET_BIT (si->in_component, w);
	  /* Mark this node for collapsing.  */
	  VARRAY_PUSH_UINT (si->unification_queue, w);
	  VARRAY_POP (si->scc_stack);
	} 
    }
  else
    VARRAY_PUSH_UINT (si->scc_stack, n);
}

/* Collapse two variables into one variable.  */

static void
collapse_nodes (constraint_graph_t graph, unsigned int to, unsigned int from)
{
  bitmap tosol, fromsol;
  struct constraint_edge edge;
  condense_varmap_nodes (to, from);
  tosol = get_varinfo (to)->solution;
  fromsol = get_varinfo (from)->solution;
  bitmap_a_or_b (tosol, tosol, fromsol);
  merge_graph_nodes (graph, to, from);
  edge.src = to;
  edge.dest = to;
  if (valid_graph_edge (graph, edge))
    {
      bitmap weights = get_graph_weights (graph, edge);
      bitmap_clear_bit (weights, 0);
      if (bitmap_first_set_bit (weights) == -1)
	erase_graph_edge (graph, edge);
    }
  bitmap_clear (fromsol);
  get_varinfo (to)->address_taken |= get_varinfo (from)->address_taken;
  get_varinfo (to)->indirect_target |= get_varinfo (from)->indirect_target;
}


/* Unify nodes that we have found to be part of a cycle.  */
static void
process_unification_queue (constraint_graph_t graph, struct scc_info *si,
			   bool update_changed)
{
  size_t i = 0;
  bitmap tmp = BITMAP_GGC_ALLOC ();
  while (i != VARRAY_ACTIVE_SIZE (si->unification_queue))
    {
      unsigned int tounify = VARRAY_UINT (si->unification_queue, i);
      unsigned int n = get_varinfo (tounify)->node;
      bool domore = false;
      if (dump_file && (dump_flags & TDF_DETAILS))
	fprintf (dump_file, "Unifying %d to %d\n", tounify, n);
      if (update_changed)
	stats.unified_vars_dynamic++;
      else
	stats.unified_vars_static++;
      bitmap_a_or_b (tmp, tmp, get_varinfo (tounify)->solution);
      merge_graph_nodes (graph, n, tounify);
      condense_varmap_nodes (n, tounify);
      
      if (update_changed && TEST_BIT (changed, tounify))
	{
	  RESET_BIT (changed, tounify);
	  if (!TEST_BIT (changed, n))
	    SET_BIT (changed, n);
	  else
	    {
	      gcc_assert (changed_count > 0);
	      changed_count--;
	    }
	}
      bitmap_clear (get_varinfo (tounify)->solution);
      ++i;
      if (i == VARRAY_ACTIVE_SIZE (si->unification_queue))
	  domore = true;
      if (!domore)
	{
	  tounify = VARRAY_UINT (si->unification_queue, i);
	  if (get_varinfo (tounify)->node != n)
	    domore = true;
	}
      if (domore)
	{
	  struct constraint_edge edge;
	  if (bitmap_a_or_b (tmp, tmp, get_varinfo (n)->solution))
	    {
	      bitmap_copy (get_varinfo (n)->solution, tmp);
	      if (update_changed && !TEST_BIT (changed, n))
		{
		  SET_BIT (changed, n);
		  changed_count++;
		}
	    }
	  bitmap_clear (tmp);
	  edge.src = n;
	  edge.dest = n;
	  if (valid_graph_edge (graph, edge))
	    {
	      bitmap weights = get_graph_weights (graph, edge);
	      bitmap_clear_bit (weights, 0);
	      if (bitmap_first_set_bit (weights) == -1)
		erase_graph_edge (graph, edge);
	    }
	}
    }
}

/* Information needed to compute the topographic ordering of a graph.  */
struct topo_info
{
  /* sbitmap of visited nodes.  */
  sbitmap visited;
  /* Array that stores the topographic order of the graph, *in
     reverse*.  */
  varray_type topo_order;
};

/* Initialize and return a topograph info structure.  */
static struct topo_info *
init_topo_info (void)
{
  size_t size = VEC_length (varinfo_t, varmap);
  struct topo_info *ti = xmalloc (sizeof (struct topo_info));
  ti->visited = sbitmap_alloc (size);
  sbitmap_zero (ti->visited);
  VARRAY_UINT_INIT (ti->topo_order, 1, "Topological ordering");
  return ti;
}

/* Free the topographic sort info pointed to by TI.  */
static void
free_topo_info (struct topo_info *ti)
{
  sbitmap_free (ti->visited);
  VARRAY_CLEAR (ti->topo_order);
  free (ti);
}

/* Visit the graph in topographical order, and store the order in the
   topo_info structure.  */
static void
topo_visit (constraint_graph_t graph, struct topo_info *ti,
	    unsigned int n)
{
  VEC(constraint_edge_t) *succs = graph->succs[n];
  constraint_edge_t c;
  int i;
  SET_BIT (ti->visited, n);
  for (i = 0; VEC_iterate (constraint_edge_t, succs, i, c); i++)
    {
      if (!TEST_BIT (ti->visited, c->dest))
	topo_visit (graph, ti, c->dest);
    }
  VARRAY_PUSH_UINT (ti->topo_order, n);
}

/* Return true if variable N + OFFSET is a legal field of N.  */
static bool 
type_safe (unsigned int n, unsigned int *offset)
{
  varinfo_t ninfo = get_varinfo (n);
  /* For things we've globbed to single variables, any offset into the
     variable acts like the entire variable, so that it becomes offset 0.  */
  if (n == anything_id
      || ninfo->is_artificial_var
      || ninfo->is_unknown_size_var)
    {
      *offset = 0;
      return true;
    }
  return n > anything_id && (n + *offset) < get_varinfo (n)->end;
}

/* Process a constraint C that represents *x = &y.  */

static void
do_da_constraint (constraint_graph_t graph ATTRIBUTE_UNUSED,
		  constraint_t c, bitmap delta)
{
  unsigned int rhs = c->rhs.var;
  unsigned int offset = c->lhs.offset;
  unsigned int j;
  EXECUTE_IF_SET_IN_BITMAP (delta, 0, j,
  {
    if (type_safe (j, &offset))
      {
	/* *x != NULL && *x != UNKNOWN */
	unsigned int t = get_varinfo (j + offset)->node;
	bitmap tmp = get_varinfo (t)->solution;
	if (!bitmap_bit_p (tmp, rhs))
	  {
	    bitmap sol = get_varinfo (t)->solution;
	    bitmap_set_bit (tmp, rhs);
	    bitmap_a_or_b (sol, sol, tmp);
	    if (!TEST_BIT (changed, t))
	      {
		SET_BIT (changed, t);
		changed_count++;
	      }
	  }
      }
    else if (dump_file)
      fprintf (dump_file, "Untypesafe usage in do_da_constraint.\n");

  });
}

/* Process a constraint C that represents x = *y, using DELTA as the
   starting solution.  */

static void
do_sd_constraint (constraint_graph_t graph, constraint_t c,
		  bitmap delta)
{
  unsigned int lhs = get_varinfo (c->lhs.var)->node;
  unsigned int roffset = c->rhs.offset;
  bool flag = false;
  bitmap sol = get_varinfo (lhs)->solution;
  unsigned int j;
  
  /* For each variable j in delta (the starting solution point), we add
     an edge in the graph from the LHS to j + RHS offset, and update
     the current LHS solution.  */
  EXECUTE_IF_SET_IN_BITMAP (delta, 0, j,
  {
    if (type_safe (j, &roffset))
      {
	unsigned int t = get_varinfo (j + roffset)->node;
	if (int_add_graph_edge (graph, lhs, t, 0))
	  flag |= bitmap_a_or_b (sol, sol, get_varinfo (t)->solution);
      }
    else if (dump_file)
      fprintf (dump_file, "Untypesafe usage in do_sd_constraint\n");

  });

  /* If the LHS solution changed, update it.  */
  if (flag)
    {
      get_varinfo (lhs)->solution = sol;
      if (!TEST_BIT (changed, lhs))
	{
	  SET_BIT (changed, lhs);
	  changed_count++;
	}
    }    
}

/* Process a constraint C that represents *x = y.  */

static void
do_ds_constraint (constraint_graph_t graph, constraint_t c, bitmap delta)
{
  unsigned int rhs = get_varinfo (c->rhs.var)->node;
  unsigned int loff = c->lhs.offset;
  unsigned int roff = c->rhs.offset;
  bitmap sol = get_varinfo (rhs)->solution;
  unsigned int j;
  
  EXECUTE_IF_SET_IN_BITMAP (delta, 0, j,
  {
    if (type_safe (j, &loff))
      {
	unsigned int t = get_varinfo (j + loff)->node;
	if (int_add_graph_edge (graph, t, rhs, roff))
	  {
	    bitmap tmp = get_varinfo (t)->solution;
	    if (set_union_with_increment (tmp, sol, roff))
	      {
		get_varinfo (t)->solution = tmp;
		if (t == rhs)
		  {
		    sol = get_varinfo (rhs)->solution;
		  }
		if (!TEST_BIT (changed, t))
		  {
		    SET_BIT (changed, t);
		    changed_count++;
		  }
	      }
	  }
      }    
    else if (dump_file)
      fprintf (dump_file, "Untypesafe usage in do_ds_constraint\n");
  });
}

			      
static void
do_complex_constraint (constraint_graph_t graph, constraint_t c, bitmap delta)
{
  if (c->lhs.type == DEREF)
    {
      if (c->rhs.type == ADDRESSOF)
	{
	  /* *x = &y */
	  do_da_constraint (graph, c, delta);
	}
      else
	{
	  /* *x = y */
	  do_ds_constraint (graph, c, delta);
	}
    }
  else
    {
      /* x = *y */
      do_sd_constraint (graph, c, delta);
    }
}

/* Initialize and return a new SCC info structure.  */

static struct scc_info *
init_scc_info (void)
{
  struct scc_info *si = xmalloc (sizeof (struct scc_info));
  size_t size = VEC_length (varinfo_t, varmap);
  si->current_index = 0;
  si->visited = sbitmap_alloc (size);
  sbitmap_zero (si->visited);
  si->in_component = sbitmap_alloc (size);
  sbitmap_ones (si->in_component);
  si->visited_index = xcalloc (sizeof (unsigned int), size + 1);
  VARRAY_UINT_INIT (si->scc_stack, 1, "SCC stack");
  VARRAY_UINT_INIT (si->unification_queue, 1, "Unification queue");
  return si;
}

/* Free an SCC info structure pointed to by SI */

static void
free_scc_info (struct scc_info *si)
{  
  sbitmap_free (si->visited);
  sbitmap_free (si->in_component);
  free (si->visited_index);
  VARRAY_CLEAR (si->scc_stack);
  VARRAY_CLEAR (si->unification_queue);
  free(si); 
}

static void
find_and_collapse_graph_cycles (constraint_graph_t graph, bool update_changed)
{
  unsigned int i;
  unsigned int size = VEC_length (varinfo_t, varmap);
  struct scc_info *si = init_scc_info ();

  for (i = 0; i != size; ++i)
    if (!TEST_BIT (si->visited, i) && get_varinfo (i)->node == i)
      scc_visit (graph, si, i);
  process_unification_queue (graph, si, update_changed);
  free_scc_info (si);
}

static void 
compute_topo_order (constraint_graph_t graph,
		    struct topo_info *ti)
{
  unsigned int i;
  unsigned int size = VEC_length (varinfo_t, varmap);
  
  for (i = 0; i != size; ++i)
    if (!TEST_BIT (ti->visited, i) && get_varinfo (i)->node == i)
      topo_visit (graph, ti, i);
}

/* Perform offline variable substitution, as per Rountev and Chandra.
   This is a linear time way of identifying variables that must have
   equivalent points-to sets, including those caused by static cycles,
   and single entry subgraphs, in the constraint graph.  */

static void
perform_rountev_chandra (constraint_graph_t graph)
{
  struct topo_info *ti = init_topo_info ();
 
  /* Compute the topographic ordering of the graph, then visit each
     node in topographic order.  */
  compute_topo_order (graph, ti);
 
  while (VARRAY_ACTIVE_SIZE (ti->topo_order) != 0)
    {
      unsigned int i = VARRAY_TOP_UINT (ti->topo_order);
      unsigned int pred;
      varinfo_t vi = get_varinfo (i);
      bool okay_to_elim = false;
      unsigned int root = VEC_length (varinfo_t, varmap);
      VEC(constraint_edge_t) *predvec = graph->preds[i];
      constraint_edge_t ce;
      bitmap tmp;

      VARRAY_POP (ti->topo_order);
      /* We can't eliminate things whose address is taken, or which is
	 the target of a dereference.  */
      if (vi->address_taken || vi->indirect_target)
	continue;
      for (pred = 0; VEC_iterate (constraint_edge_t, predvec, pred, ce); pred++)
	{
	  bitmap weight;
	  unsigned int w;
	  weight = get_graph_weights (graph, *ce);
	
	  /* We can't eliminate variables that have non-zero weighted
	     edges between them.  */
	  if (bitmap_first_set_bit (weight) != 0 
	      || bitmap_last_set_bit (weight) != 0)
	    {
	      okay_to_elim = false;
	      break;
	    }
	  w = get_varinfo (ce->dest)->node;
	  /* We can't eliminate the node if one of the predecessors is
	     part of a different strongly connected component.  */
	  if (!okay_to_elim)
	    {
	      root = w;
	      okay_to_elim = true;
	    }
	  else if (w != root)
	    {
	      okay_to_elim = false;
	      break;
	    }
	  /* Theorem 4 in Rountev and Chandra: If i is a direct node,
	     then Solution(i) is a subset of Solution (w), where w is a
	     predecessor in the graph.  
	     Corrolary: If all predecessors of i have the same
	     points-to set, then i has that same points-to set as
	     those predecessors.  */
	  tmp = BITMAP_XMALLOC ();
	  bitmap_operation (tmp, get_varinfo (i)->solution, 
			    get_varinfo (w)->solution,
			    BITMAP_AND_COMPL);
	  if (bitmap_first_set_bit (tmp) != -1)
	    {
	      okay_to_elim = false;
	      BITMAP_XFREE (tmp);
	      break;
	    }
	  BITMAP_XFREE (tmp);
	}
      /* See if the root is different than the original node. 
	 If so, we've found an equivalence.  */
      if (root != get_varinfo (i)->node && okay_to_elim)
	{
	  /* Found an equivalence */
	  get_varinfo (i)->node = root;
	  collapse_nodes (graph, root, i);
	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "Collapsing %s into %s\n",
		     get_varinfo (i)->name,
		     get_varinfo (root)->name);
	  stats.collapsed_vars++;
	}
    }
  free_topo_info (ti);
}

/* Solve the constraint graph GRAPH.  */
static void
solve_graph (constraint_graph_t graph)
{
  unsigned int size = VEC_length (varinfo_t, varmap);
  unsigned int i;
  changed_count = size;
  changed = sbitmap_alloc (size);
  sbitmap_ones (changed);
  
  /* The already collapsed/unreachable nodes will never change, so we
     need to  account for them in changed_count.  */
  for (i = 0; i < size; i++)
    if (get_varinfo (i)->node != i)
      changed_count--;
  
  while (changed_count > 0)
    {
      unsigned int i;
      struct topo_info *ti = init_topo_info ();
      stats.iterations++;
      find_and_collapse_graph_cycles (graph, true);
      compute_topo_order (graph, ti);
      while (VARRAY_ACTIVE_SIZE (ti->topo_order) != 0)
	{
	  i = VARRAY_TOP_UINT (ti->topo_order);
	  VARRAY_POP (ti->topo_order);
	  gcc_assert (get_varinfo (i)->node == i);

	  if (TEST_BIT (changed, i))
	    {
	      unsigned int j;
	      constraint_t c;
	      constraint_edge_t e;
	      bitmap solution;
	      VEC(constraint_t) *complicated = get_varinfo (i)->complicated;
	      VEC(constraint_edge_t) *succs;
	      RESET_BIT (changed, i);
	      changed_count--;
	      solution = get_varinfo (i)->solution;
	      for (j = 0; VEC_iterate (constraint_t, complicated, j, c); j++)
		{
		  do_complex_constraint (graph, c, solution);
		}
	      succs = graph->succs[i];
	      for (j = 0; VEC_iterate (constraint_edge_t, succs, j, e); j++)
		{
		  bitmap tmp = get_varinfo (e->dest)->solution;
		  bool flag = false;
		  unsigned int k;
		  /* Process weighted edges */
		  bitmap weights = e->weights;
		  gcc_assert (bitmap_first_set_bit (weights) != -1);
		  EXECUTE_IF_SET_IN_BITMAP (weights, 0, k,
                  {
		    flag |= set_union_with_increment (tmp, solution, k);
		  });
		  if (flag)
		    {
		      get_varinfo (e->dest)->solution = tmp;		    
		      if (!TEST_BIT (changed, e->dest))
			{
			  SET_BIT (changed, e->dest);
			  changed_count++;
			}
		    }
		}
	    }
	}
      free_topo_info (ti);
    }
  sbitmap_free (changed);
}


/* Create points-to sets for the current function.  */

static void
create_alias_vars (void)
{
  basic_block bb;
  struct constraint_expr lhs, rhs;

  max_fields = 0;
  num_fields_for_type = splay_tree_new (splay_tree_compare_pointers, NULL, NULL);
  fieldnum_table = htab_create (511, fieldnum_offset_hash, fieldnum_offset_eq,
				free);
  constraint_pool = create_alloc_pool ("Constraint pool", 
				       sizeof (struct constraint), 30);
  variable_info_pool = create_alloc_pool ("Variable info pool",
					  sizeof (struct variable_info), 30);
  constraint_edge_pool = create_alloc_pool ("Constraint edges",
					    sizeof (struct constraint_edge), 30);
  
  constraints = VEC_alloc (constraint_t, 8);
  varmap = VEC_alloc (varinfo_t, 8);
  id_for_tree = htab_create (10, tree_id_hash, tree_id_eq, free);
  memset (&stats, 0, sizeof (stats));

  /* Create the NULL variable, used to represent that a variable points
     to NULL.  */
  nothing_tree = create_tmp_var_raw (void_type_node, "NULL");
  var_nothing = new_var_info (nothing_tree, "NULL", 1, 0);
  insert_id_for_tree (nothing_tree, 0);
  var_nothing->is_artificial_var = 1;
  nothing_id = 0;
  VEC_safe_push (varinfo_t, varmap, var_nothing);

  /* Create the ANYTHING variable, used to represent that a variable
     points to some unknown piece of memory.  */
  anything_tree = create_tmp_var_raw (void_type_node, "ANYTHING");
  var_anything = new_var_info (anything_tree, "ANYTHING", 2, 1); 
  insert_id_for_tree (anything_tree, 1);
  var_anything->is_artificial_var = 1;
  anything_id = 1;
  VEC_safe_push (varinfo_t, varmap, var_anything);
  lhs.type = SCALAR;
  lhs.var = anything_id;
  lhs.offset = 0;
  rhs.type = ADDRESSOF;
  rhs.var = anything_id;
  rhs.offset = 0;
  var_anything->address_taken = true;
  VEC_safe_push (constraint_t, constraints, 
		 new_constraint (lhs, rhs));

  /* Create the READONLY variable, used to represent that a variable
     points to readonly memory.  */
  readonly_tree = create_tmp_var_raw (void_type_node, "READONLY");
  var_readonly = new_var_info (readonly_tree, "READONLY", 3, 2);
  var_readonly->is_artificial_var = 1;
  insert_id_for_tree (readonly_tree, 2);
  readonly_id = 2;
  VEC_safe_push (varinfo_t, varmap, var_readonly);
  lhs.type = SCALAR;
  lhs.var = readonly_id;
  lhs.offset = 0;
  rhs.type = ADDRESSOF;
  rhs.var = readonly_id;
  rhs.offset = 0;
  var_anything->address_taken = true;
  VEC_safe_push (constraint_t, constraints, 
		 new_constraint (lhs, rhs));

  create_variable_infos ();
  /* Now walk all statements and derive aliases.  */
  FOR_EACH_BB (bb)
    {
      block_stmt_iterator bsi; 
      tree phi;
      for (phi = phi_nodes (bb); phi; phi = TREE_CHAIN (phi))
	if (is_gimple_reg (PHI_RESULT (phi)))
	  find_func_aliases (phi);
      for (bsi = bsi_start (bb); !bsi_end_p (bsi); bsi_next (&bsi))
	find_func_aliases (bsi_stmt (bsi));
    }

  build_constraint_graph ();
  if (dump_file)
    {
      fprintf (dump_file, "Constraints:\n");
      print_constraints (dump_file);
    }
  if (dump_file)
    fprintf (dump_file, "Collapsing static cycles and doing variable substitution:\n");
  find_and_collapse_graph_cycles (graph, false);
  perform_rountev_chandra (graph);
  if (dump_file)
    fprintf (dump_file, "Solving graph:\n");
  solve_graph (graph);
  if (dump_file)
    {
      unsigned int i;
      if (dump_flags & TDF_STATS)
	{
	  fprintf (dump_file, "Stats:\n");
	  fprintf (dump_file, "Total vars:%d\n", stats.total_vars);
	  fprintf (dump_file, "Statically unified vars:%d\n", stats.unified_vars_static);
	  fprintf (dump_file, "Collapsed vars:%d\n", stats.collapsed_vars);
	  fprintf (dump_file, "Dynamically unified vars:%d\n", stats.unified_vars_dynamic);
	  fprintf (dump_file, "Iterations:%d\n", stats.iterations);
	}
      for (i = 0; i < VEC_length (varinfo_t, varmap); i++)
	print_solution_for_var (dump_file, i);
     
    }
  

}

struct tree_opt_pass pass_build_pta = 
{
  "pta",				/* name */
  NULL,					/* gate */
  create_alias_vars,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_PTA,				/* tv_id */
  PROP_cfg,				/* properties_required */
  PROP_pta,				/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0,                                    /* todo_flags_finish */
  0					/* letter */
};
 

/* Delete created points-to sets.  */

static void
delete_alias_vars (void)
{
  htab_delete (id_for_tree);
  free_alloc_pool (variable_info_pool);
  free_alloc_pool (constraint_pool); 
  free_alloc_pool (constraint_edge_pool);
}

struct tree_opt_pass pass_del_pta = 
{
  NULL,                                 /* name */
  NULL,					/* gate */
  delete_alias_vars,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_PTA,				/* tv_id */
  PROP_pta,				/* properties_required */
  0,					/* properties_provided */
  PROP_pta,				/* properties_destroyed */
  0,					/* todo_flags_start */
  0,                                    /* todo_flags_finish */
  0					/* letter */
};


#define MASK_POINTER(P)	((unsigned)((unsigned long)(P) & 0xffff))

const char *
alias_get_name (tree t)
{
  const char *name;
  if (TREE_CODE (t) == FUNCTION_DECL)
    name = IDENTIFIER_POINTER (DECL_NAME (t));
  else if (TREE_CODE (t) == FIELD_DECL)
    {
      tree typename = TYPE_NAME (DECL_FIELD_CONTEXT (t));
      const char *structname;
      char *result = alloca (128);
      char *newname;
      if (!typename)
	{
	  char *namep;
	  /* 2 = UF
	     4 = the masked pointer
	     2 = the <> around it
	     1 = the terminator.  */
	  namep = ggc_alloc (2 + 4 + 2 + 1);
	  sprintf (namep, "<UV%x>", MASK_POINTER (t));
	  structname = namep;
	}
      else
	structname = IDENTIFIER_POINTER (typename);
      sprintf (result, "%s.%s", structname, get_name (t));
      newname = ggc_alloc (strlen (result) + 1);
      strcpy (newname, result);
      name = newname;
    }
  else if (TREE_CODE (t) == RESULT_DECL)
    name = "<return value>";
  else if (TREE_CODE (t) == SSA_NAME)
    {
      char *result = alloca (128);
      char *newname;
      sprintf (result, "%s_%d", get_name (SSA_NAME_VAR (t)),
	       SSA_NAME_VERSION (t));
      newname = ggc_alloc (strlen (result) + 1);
      strcpy (newname, result);
      name = newname;
    }
  else if (TREE_CODE (t) == RESULT_DECL)
	  name = "<return value>";
  else
    name = get_name (t);
  
  if (!name)
    {
      char *namep;
      /* 2 = UF
	 4 = the masked pointer
	 2 = the <> around it
	 1 = the terminator.  */
      namep = ggc_alloc (2 + 4 + 2 + 1);
      sprintf (namep, "<UV%x>", MASK_POINTER (t));
      return namep;
    }
  return name;
}


