/* Scalar evolution detector.
   Copyright (C) 2003, 2004 Free Software Foundation, Inc.
   Contributed by Sebastian Pop <s.pop@laposte.net>

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

/* 
   Description: 
   
   This pass analyzes the evolution of scalar variables in loop
   structures.  The algorithm is based on the SSA representation,
   and on the loop hierarchy tree.  This algorithm is not based on
   the notion of versions of a variable, as it was the case for the
   previous implementations of the scalar evolution algorithm, but
   it assumes that each defined name is unique.
     
   A short sketch of the algorithm is:
     
   Given a scalar variable to be analyzed, follow the SSA edge to
   its definition:
     
   - When the definition is a MODIFY_EXPR: if the right hand side
   (RHS) of the definition cannot be statically analyzed, the answer
   of the analyzer is: "don't know", that corresponds to the
   conservative [-oo, +oo] element of the lattice of intervals.
   Otherwise, for all the variables that are not yet analyzed in the
   RHS, try to determine their evolution, and finally try to
   evaluate the operation of the RHS that gives the evolution
   function of the analyzed variable.

   - When the definition is a condition-phi-node: determine the
   evolution function for all the branches of the phi node, and
   finally merge these evolutions (see chrec_merge).

   - When the definition is a loop-phi-node: determine its initial
   condition, that is the SSA edge defined in an outer loop, and
   keep it symbolic.  Then determine the SSA edges that are defined
   in the body of the loop.  Follow the inner edges until ending on
   another loop-phi-node of the same analyzed loop.  If the reached
   loop-phi-node is not the starting loop-phi-node, then we keep
   this definition under a symbolic form.  If the reached
   loop-phi-node is the same as the starting one, then we compute a
   symbolic stride on the return path.  The result is then the
   symbolic chrec {initial_condition, +, symbolic_stride}_loop.

   Examples:
   
   Example 1: Illustration of the basic algorithm.
   
   | a = 3
   | loop_1
   |   b = phi (a, c)
   |   c = b + 1
   |   if (c > 10) exit_loop
   | endloop
   
   Suppose that we want to know the number of iterations of the
   loop_1.  The exit_loop is controlled by a COND_EXPR (c > 10).  We
   ask the scalar evolution analyzer two questions: what's the
   scalar evolution (scev) of "c", and what's the scev of "10".  For
   "10" the answer is "10" since it is a scalar constant.  For the
   scalar variable "c", it follows the SSA edge to its definition,
   "c = b + 1", and then asks again what's the scev of "b".
   Following the SSA edge, we end on a loop-phi-node "b = phi (a,
   c)", where the initial condition is "a", and the inner loop edge
   is "c".  The initial condition is kept under a symbolic form (it
   may be the case that the copy constant propagation has done its
   work and we end with the constant "3" as one of the edges of the
   loop-phi-node).  The update edge is followed to the end of the
   loop, and until reaching again the starting loop-phi-node: b -> c
   -> b.  At this point we have drawn a path from "b" to "b" from
   which we compute the stride in the loop: in this example it is
   "+1".  The resulting scev for "b" is "b -> {a, +, 1}_1".  Now
   that the scev for "b" is known, it is possible to compute the
   scev for "c", that is "c -> {a + 1, +, 1}_1".  In order to
   determine the number of iterations in the loop_1, we have to
   instantiate_parameters ({a + 1, +, 1}_1), that gives after some
   more analysis the scev {4, +, 1}_1, or in other words, this is
   the function "f (x) = x + 4", where x is the iteration count of
   the loop_1.  Now we have to solve the inequality "x + 4 > 10",
   and take the smallest iteration number for which the loop is
   exited: x = 7.  This loop runs from x = 0 to x = 7, and in total
   there are 8 iterations.  In terms of loop normalization, we have
   created a variable that is implicitly defined, "x" or just "_1",
   and all the other analyzed scalars of the loop are defined in
   function of this variable:
   
   a -> 3
   b -> {3, +, 1}_1
   c -> {4, +, 1}_1
     
   or in terms of a C program: 
     
   | a = 3
   | for (x = 0; x <= 7; x++)
   |   {
   |     b = x + 3
   |     c = x + 4
   |   }
     
   Example 2: Illustration of the algorithm on nested loops.
     
   | loop_1
   |   a = phi (1, b)
   |   c = a + 2
   |   loop_2  10 times
   |     b = phi (c, d)
   |     d = b + 3
   |   endloop
   | endloop
     
   For analyzing the scalar evolution of "a", the algorithm follows
   the SSA edge into the loop's body: "a -> b".  "b" is an inner
   loop-phi-node, and its analysis as in Example 1, gives: 
     
   b -> {c, +, 3}_2
   d -> {c + 3, +, 3}_2
     
   Following the SSA edge for the initial condition, we end on "c = a
   + 2", and then on the starting loop-phi-node "a".  From this point,
   the loop stride is computed: back on "c = a + 2" we get a "+2" in
   the loop_1, then on the loop-phi-node "b" we compute the overall
   effect of the inner loop that is "b = c + 30", and we get a "+30"
   in the loop_1.  That means that the overall stride in loop_1 is
   equal to "+32", and the result is: 
     
   a -> {1, +, 32}_1
   c -> {3, +, 32}_1
     
   Example 3: Higher degree polynomials.
     
   | loop_1
   |   a = phi (2, b)
   |   c = phi (5, d)
   |   b = a + 1
   |   d = c + a
   | endloop
     
   a -> {2, +, 1}_1
   b -> {3, +, 1}_1
   c -> {5, +, a}_1
   d -> {5 + a, +, a}_1
     
   instantiate_parameters ({5, +, a}_1) -> {5, +, 2, +, 1}_1
   instantiate_parameters ({5 + a, +, a}_1) -> {7, +, 3, +, 1}_1
     
   Example 4: Lucas, Fibonacci, or mixers in general.
     
   | loop_1
   |   a = phi (1, b)
   |   c = phi (3, d)
   |   b = c
   |   d = c + a
   | endloop
     
   a -> (1, c)_1
   c -> {3, +, a}_1
     
   The syntax "(1, c)_1" stands for a PEELED_CHREC that has the
   following semantics: during the first iteration of the loop_1, the
   variable contains the value 1, and then it contains the value "c".
   Note that this syntax is close to the syntax of the loop-phi-node:
   "a -> (1, c)_1" vs. "a = phi (1, c)".
     
   The symbolic chrec representation contains all the semantics of the
   original code.  What is more difficult is to use this information.
     
   Example 5: Flip-flops, or exchangers.
     
   | loop_1
   |   a = phi (1, b)
   |   c = phi (3, d)
   |   b = c
   |   d = a
   | endloop
     
   a -> (1, c)_1
   c -> (3, a)_1
     
   Based on these symbolic chrecs, it is possible to refine this
   information into the more precise PERIODIC_CHRECs: 
     
   a -> |1, 3|_1
   c -> |3, 1|_1
     
   This transformation is not yet implemented.
     
   Further readings:
   
   You can find a more detailed description of the algorithm in:
   http://icps.u-strasbg.fr/~pop/DEA_03_Pop.pdf
   http://icps.u-strasbg.fr/~pop/DEA_03_Pop.ps.gz.  But note that
   this is a preliminary report and some of the details of the
   algorithm have changed.  I'm working on a research report that
   updates the description of the algorithms to reflect the design
   choices used in this implementation.
     
   A set of slides show a high level overview of the algorithm and
   run an example through the scalar evolution analyzer:
   http://cri.ensmp.fr/~pop/gcc/mar04/slides.pdf
     
   Fixmes:
   
   FIXME taylor: This FIXME concerns all the cases where we have to
   deal with additions of exponential functions: "exp + exp" or
   "poly + exp" or "cst + exp".  This could be handled by a Taylor
   decomposition of the exponential function, but this is still
   under construction (not implemented yet, or chrec_top).
     
   The idea is to represent the exponential evolution functions
   using infinite degree polynomials:
     
   | a -> {1, *, 2}_1 = {1, +, 1, +, 1, +, ...}_1 = {1, +, a}_1
     
   Proof:
   \begin{eqnarray*}
   \{1, *, t+1\} (x) &=& exp \left(log (1) + log (t+1) \binom{x}{1} \right) \\
   &=& (t+1)^x \\
   &=& \binom{x}{0} + \binom{x}{1}t + \binom{x}{2}t^2 + 
   \ldots + \binom{x}{x}t^x \\
   &=& \{1, +, t, +, t^2, +, \ldots, +, t^x\} \\
   \end{eqnarray*}
     
   While this equality is simple to prove for exponentials of degree
   1, it is still work in progress for higher degree exponentials.
*/

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "errors.h"
#include "ggc.h"
#include "tree.h"

/* These RTL headers are needed for basic-block.h.  */
#include "rtl.h"
#include "basic-block.h"
#include "diagnostic.h"
#include "tree-flow.h"
#include "tree-dump.h"
#include "timevar.h"
#include "cfgloop.h"
#include "tree-fold-const.h"
#include "tree-chrec.h"
#include "tree-data-ref.h"
#include "tree-scalar-evolution.h"
#include "tree-pass.h"
#include "tree-vectorizer.h"
#include "flags.h"


static bool loop_phi_node_p (tree);

static bool follow_ssa_edge_in_rhs (struct loop *loop, tree, tree, tree *);
static bool follow_ssa_edge_in_condition_phi (struct loop *loop, tree, tree, tree *);
static bool follow_ssa_edge_inner_loop_phi (struct loop *loop, tree, tree, tree *);
static bool follow_ssa_edge (struct loop *loop, tree, tree, tree *);
static tree analyze_evolution_in_loop (tree, tree);
static tree analyze_initial_condition (tree);
static tree interpret_loop_phi (struct loop *loop, tree);
static tree interpret_condition_phi (struct loop *loop, tree);
static tree interpret_rhs_modify_expr (struct loop *loop, tree, tree);

/* The cached information about a ssa name VAR, claiming that inside LOOP,
   the value of VAR can be expressed as CHREC.  */

struct scev_info_str
{
  tree var;
  struct loop *loop;
  tree chrec;
};

/* The following trees are unique elements.  Thus the comparison of
   another element to these elements should be done on the pointer to
   these trees, and not on their value.  */

/* The SSA_NAMEs that are not yet analyzed are qualified with NULL_TREE.  */
tree chrec_not_analyzed_yet;

/* Reserved to the cases where the analyzer has detected an
   undecidable property at compile time.  */
tree chrec_top;

/* When the analyzer has detected that a property will never
   happen, then it qualifies it with chrec_bot.  */
tree chrec_bot;

static struct loops *current_loops;
static varray_type scalar_evolution_info;
static varray_type already_instantiated;

/* Statistics counters.  */
static unsigned stats_nb_chrecs = 0;
static unsigned stats_nb_peeled_affine = 0;
static unsigned stats_nb_affine = 0;
static unsigned stats_nb_affine_multivar = 0;
static unsigned stats_nb_higher_poly = 0;
static unsigned stats_nb_expo = 0;
static unsigned stats_nb_chrec_top = 0;
static unsigned stats_nb_interval_chrec = 0;
static unsigned stats_nb_undetermined = 0;

/* Flag to indicate availability of dependency info.  */
bool dd_info_available;


/* Constructs a new SCEV_INFO_STR structure.  */

static inline struct scev_info_str *
new_scev_info_str (struct loop *loop, tree var)
{
  struct scev_info_str *res;
  
  res = ggc_alloc (sizeof (struct scev_info_str));
  res->var = var;
  res->loop = loop;
  res->chrec = chrec_not_analyzed_yet;
  
  return res;
}

/* Get the index corresponding to VAR in the current LOOP.  If
   it's the first time we ask for this VAR, then we return
   chrec_not_analysed_yet for this VAR and return its index.  */

static tree *
find_var_scev_info (struct loop *loop, tree var)
{
  unsigned int i;
  struct scev_info_str *res;
  
  for (i = 0; i < VARRAY_ACTIVE_SIZE (scalar_evolution_info); i++)
    {
      res = VARRAY_GENERIC_PTR (scalar_evolution_info, i);
      if (res->var == var && res->loop == loop)
	return &res->chrec;
    }
  
  /* The variable is not in the table, create a new entry for it.  */
  res = new_scev_info_str (loop, var);
  VARRAY_PUSH_GENERIC_PTR (scalar_evolution_info, res);
  
  return &res->chrec;
}



/* This section contains the interface to the SSA IR.  */

/* This function determines whether PHI is a loop-phi-node.  Otherwise
   it is a condition-phi-node.  */

static bool
loop_phi_node_p (tree phi)
{
  /* The implementation of this function is based on the following
     property: "all the loop-phi-nodes of a loop are contained in the
     loop's header basic block".  */

  return loop_of_stmt (phi)->header == bb_for_stmt (phi);
}

/* Select the evolution function in the current LOOP and in the
   outer containing loops.  */

static tree
select_outer_and_current_evolutions (struct loop *loop, tree chrec)
{
  switch (TREE_CODE (chrec))
    {
    case POLYNOMIAL_CHREC:
      if (flow_loop_nested_p (loop_from_num (current_loops,
					     CHREC_VARIABLE (chrec)),
			      loop))
	return build_polynomial_chrec 
	  (CHREC_VARIABLE (chrec), 
	   select_outer_and_current_evolutions (loop, CHREC_LEFT (chrec)),
	   select_outer_and_current_evolutions (loop, CHREC_RIGHT (chrec)));
      
      else
	return select_outer_and_current_evolutions (loop, CHREC_LEFT (chrec));

    case EXPONENTIAL_CHREC:
      if (flow_loop_nested_p (loop_from_num (current_loops,
					       CHREC_VARIABLE (chrec)),
			      loop))
	return build_exponential_chrec 
	  (CHREC_VARIABLE (chrec), 
	   select_outer_and_current_evolutions (loop, CHREC_LEFT (chrec)),
	   select_outer_and_current_evolutions (loop, CHREC_RIGHT (chrec)));
      
      else
	return select_outer_and_current_evolutions (loop, CHREC_LEFT (chrec));
      
    default:
      return chrec;
    }
}

/* Compute the overall effect of a LOOP on a variable. 
   1. compute the number of iterations in the loop,
   2. compute the value of the variable after crossing the loop.  

   Example:  
   
   | i_0 = ...
   | loop 10 times
   |   i_1 = phi (i_0, i_2)
   |   i_2 = i_1 + 2
   | endloop
   
   This loop has the same effect as:
   
   | i_1 = i_0 + 20
*/

static tree 
compute_overall_effect_of_inner_loop (struct loop *loop, tree version)
{
  tree res;
  tree nb_iter, evolution_fn;
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "(compute_overall_effect_of_inner_loop \n");

  evolution_fn = analyze_scalar_evolution (loop, version);
  nb_iter = number_of_iterations_in_loop (loop);

  /* If the variable is an invariant, there is nothing to do.  */
  if (no_evolution_in_loop_p (evolution_fn, loop->num))
    res = evolution_fn;

  /* When the number of iterations is not known, set the evolution to
     chrec_top.  As an example, consider the following loop:
     
     | i = 5
     | loop 
     |   i = i + 1
     |   loop chrec_top times
     |     i = i + 3
     |   endloop
     | endloop
     
     since it is impossible to know the number of iterations in the
     inner loop, the evolution of i in the outer loop becomes unknown:
     
     | i = 5
     | loop 
     |   i = i + 1
     |   i = i + chrec_top
     | endloop
     */

  else if (nb_iter == chrec_top)
    res = chrec_top;
  
  else
    {
      /* Number of iterations is off by one (the ssa name we analyze must be
	 defined before the exit).  */
      nb_iter = chrec_fold_minus (chrec_type (nb_iter),
				  nb_iter,
				  convert (chrec_type (nb_iter),
					   integer_one_node));

      /* evolution_fn is the evolution function in LOOP.  Get its value in the
	 nb_iter-th iteration.  */
      res = chrec_apply (loop->num, evolution_fn, nb_iter);
    }
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, ")\n");
  return res;
}



/* The following section constitutes the interface with the chrecs.  */

/* Determine whether the CHREC is always positive/negative.  If the expression
   cannot be statically analyzed, return false, otherwise set the answer into
   VALUE.  */

bool
chrec_is_positive (tree chrec, bool *value)
{
  bool value0, value1;
  bool value2;
  tree end_value;
  tree nb_iter;
  
  switch (TREE_CODE (chrec))
    {
    case INTERVAL_CHREC:
      if (!chrec_is_positive (CHREC_LOW (chrec), &value0)
	  || !chrec_is_positive (CHREC_UP (chrec), &value1))
	return false;

      *value = value0;
      return value0 == value1;

    case POLYNOMIAL_CHREC:
    case EXPONENTIAL_CHREC:
      if (!chrec_is_positive (CHREC_LEFT (chrec), &value0)
	  || !chrec_is_positive (CHREC_RIGHT (chrec), &value1))
	return false;
     
      /* FIXME -- overflows.  */
      if (value0 == value1)
	{
	  *value = value0;
	  return true;
	}

      /* Otherwise the chrec is under the form: "{-197, +, 2}_1",
	 and the proof consists in showing that the sign never
	 changes during the execution of the loop, from 0 to
	 loop_nb_iterations ().  */
      if (!evolution_function_is_affine_p (chrec))
	return false;

      nb_iter = number_of_iterations_in_loop
	      (loop_from_num (current_loops, CHREC_VARIABLE (chrec)));
      nb_iter = chrec_fold_minus 
	(chrec_type (nb_iter), nb_iter,
	 convert (chrec_type (nb_iter), integer_one_node));

#if 0
      /* TODO -- If the test is after the exit, we may decrease the number of
	 iterations by one.  */
      if (after_exit)
	nb_iter = chrec_fold_minus 
		(chrec_type (nb_iter), nb_iter,
		 convert (chrec_type (nb_iter), integer_one_node));
#endif

      end_value = chrec_apply (CHREC_VARIABLE (chrec), chrec, nb_iter);
	      
      if (!chrec_is_positive (end_value, &value2))
	return false;
	
      *value = value0;
      return value0 == value1;
      
    case INTEGER_CST:
      *value = (tree_int_cst_sgn (chrec) == 1);
      return true;
      
    default:
      return false;
    }
}

/* Determine whether the set_chrec has to keep this expression
   symbolic. */

static tree 
set_scev_keep_symbolic (tree def,
			tree chrec)
{
  if (chrec == chrec_not_analyzed_yet)
    return chrec;

  if (chrec == chrec_top)
    /*    return def; */
    return chrec;
  
  switch (TREE_CODE (chrec))
    {
    case ADDR_EXPR:
    case ARRAY_REF:
    case INDIRECT_REF:
    case COMPONENT_REF:
      /* KEEP_IT_SYMBOLIC.  */
      return def;
      
    default:
      return chrec;
    }
}

/* Associate CHREC to SCALAR in LOOP.  */

static void
set_scalar_evolution (struct loop *loop, tree scalar, tree chrec)
{
  tree *scalar_info = find_var_scev_info (loop, scalar);
  chrec = set_scev_keep_symbolic (scalar, chrec);
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "(set_scalar_evolution \n");
      fprintf (dump_file, "  (scalar = ");
      print_generic_expr (dump_file, scalar, 0);
      fprintf (dump_file, ")\n  (scalar_evolution = ");
      print_generic_expr (dump_file, chrec, 0);
      fprintf (dump_file, "))\n");
    }
  
  *scalar_info = chrec;
}

/* Retrieve the chrec associated to SCALAR in the LOOP.  */

static tree
get_scalar_evolution (struct loop *loop, tree scalar)
{
  tree res;
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "(get_scalar_evolution \n");
      fprintf (dump_file, "  (loop_nb = %d)\n", loop->num);
      fprintf (dump_file, "  (scalar = ");
      print_generic_expr (dump_file, scalar, 0);
      fprintf (dump_file, ")\n");
    }
  
  switch (TREE_CODE (scalar))
    {
    case SSA_NAME:
      res = *find_var_scev_info (loop, scalar);
      break;

    case VAR_DECL:
    case PARM_DECL:
    case REAL_CST:
    case INTEGER_CST:
    case FLOAT_EXPR:
    case NEGATE_EXPR:
    case ABS_EXPR:
    case LSHIFT_EXPR:
    case RSHIFT_EXPR:
    case LROTATE_EXPR:
    case RROTATE_EXPR:
    case BIT_IOR_EXPR:
    case BIT_XOR_EXPR:
    case BIT_AND_EXPR:
    case BIT_NOT_EXPR:
    case TRUTH_ANDIF_EXPR:
    case TRUTH_ORIF_EXPR:
    case TRUTH_AND_EXPR:
    case TRUTH_OR_EXPR:
    case TRUTH_XOR_EXPR:
    case TRUTH_NOT_EXPR:
    case ADDR_EXPR:
    case ARRAY_REF:
    case INDIRECT_REF:
    case COMPONENT_REF:
      /* KEEP_IT_SYMBOLIC. These nodes are kept in "symbolic" form. */
      res = scalar;
      break;
      
    case CONVERT_EXPR:
    case NOP_EXPR:
      {
	/* KEEP_IT_SYMBOLIC.  In the case of a cast, keep it symbolic,
	   otherwise just answer chrec_top.  */
	tree opnd0 = TREE_OPERAND (scalar, 0);
	
	if (opnd0 && TREE_CODE (opnd0) == SSA_NAME)
	  res = scalar;
	else
	  res = chrec_top;
	break;
      }
      
    default:
      /* We don't want to do symbolic computations on these nodes.  */
      res = chrec_top;
      break;
    }
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "  (scalar_evolution = ");
      print_generic_expr (dump_file, res, 0);
      fprintf (dump_file, "))\n");
    }
  
  return res;
}

/* The expression CHREC_BEFORE has no evolution part in LOOP_NB.  
   This function constructs a new polynomial evolution function for this 
   loop.  The evolution part is TO_ADD.  */

static tree
build_polynomial_evolution_in_loop (unsigned loop_nb, 
				    tree chrec_before, 
				    tree to_add)
{
  switch (TREE_CODE (chrec_before))
    {
    case POLYNOMIAL_CHREC:
      if (CHREC_VARIABLE (chrec_before) < loop_nb)
	return build_polynomial_chrec 
	  (loop_nb, chrec_before, to_add);
      else
	return build_polynomial_chrec 
	  (CHREC_VARIABLE (chrec_before),
	   build_polynomial_evolution_in_loop 
	   (loop_nb, CHREC_LEFT (chrec_before), to_add),
	   CHREC_RIGHT (chrec_before));
      
    case EXPONENTIAL_CHREC:
      if (CHREC_VARIABLE (chrec_before) < loop_nb)
	return build_polynomial_chrec 
	  (loop_nb, chrec_before, to_add);
      else
	return build_exponential_chrec 
	  (CHREC_VARIABLE (chrec_before),
	   build_polynomial_evolution_in_loop 
	   (loop_nb, CHREC_LEFT (chrec_before), to_add),
	   CHREC_RIGHT (chrec_before));
      
    default:
      /* These nodes do not depend on a loop.  */
      return build_polynomial_chrec 
	(loop_nb, chrec_before, to_add);
    }
}

/* The expression CHREC_BEFORE has no evolution part in LOOP_NUM.  
   This function constructs a new exponential evolution function for this 
   loop.  The evolution part is TO_MULT.  */

static tree
build_exponential_evolution_in_loop (unsigned loop_num, 
				     tree chrec_before, 
				     tree to_mult)
{
  switch (TREE_CODE (chrec_before))
    {
    case POLYNOMIAL_CHREC:
      if (CHREC_VARIABLE (chrec_before) < loop_num)
	return build_exponential_chrec 
	  (loop_num, chrec_before, to_mult);
      else
	return build_polynomial_chrec 
	  (CHREC_VARIABLE (chrec_before),
	   build_exponential_evolution_in_loop 
	   (loop_num, CHREC_LEFT (chrec_before), to_mult),
	   CHREC_RIGHT (chrec_before));
      
    case EXPONENTIAL_CHREC:
      if (CHREC_VARIABLE (chrec_before) < loop_num)
	return build_exponential_chrec 
	  (loop_num, chrec_before, to_mult);
      else
	return build_exponential_chrec 
	  (CHREC_VARIABLE (chrec_before),
	   build_exponential_evolution_in_loop 
	   (loop_num, CHREC_LEFT (chrec_before), to_mult),
	   CHREC_RIGHT (chrec_before));
      
    default:
      /* These nodes do not depend on a loop.  */
      return build_exponential_chrec 
	(loop_num, chrec_before, to_mult);
    }
}

/* The expression CHREC_BEFORE has an evolution part in LOOP_NUM.  
   Add to this evolution the expression TO_ADD.  */

static tree
add_expr_to_loop_evolution (unsigned loop_num, 
			    tree chrec_before, 
			    enum tree_code code, 
			    tree to_add)
{
  switch (TREE_CODE (chrec_before))
    {
    case POLYNOMIAL_CHREC:
      if (CHREC_VARIABLE (chrec_before) == loop_num)
	{
	  if (code == MINUS_EXPR)
	    return build_polynomial_chrec 
	      (CHREC_VARIABLE (chrec_before), CHREC_LEFT (chrec_before), 
	       chrec_fold_minus (chrec_type (CHREC_RIGHT (chrec_before)), 
				 CHREC_RIGHT (chrec_before),
				 to_add));
	  else
	    return build_polynomial_chrec 
	      (CHREC_VARIABLE (chrec_before), CHREC_LEFT (chrec_before), 
	       chrec_fold_plus (chrec_type (CHREC_RIGHT (chrec_before)), 
				CHREC_RIGHT (chrec_before),
				to_add));
	}
      
      else
	/* Search the evolution in LOOP_NUM.  */
	return build_polynomial_chrec 
	  (CHREC_VARIABLE (chrec_before),
	   add_expr_to_loop_evolution (loop_num, 
				       CHREC_LEFT (chrec_before), 
				       code, to_add),
	   CHREC_RIGHT (chrec_before));
      
    case EXPONENTIAL_CHREC:
      if (CHREC_VARIABLE (chrec_before) == loop_num)
	return build_exponential_chrec
	  (loop_num, 
	   CHREC_LEFT (chrec_before),
	   /* We still don't know how to fold these operations that mix 
	      polynomial and exponential functions.  For the moment, give a 
	      rough approximation: [-oo, +oo].  */
	   chrec_top);
      else
	return build_exponential_chrec 
	  (CHREC_VARIABLE (chrec_before),
	   add_expr_to_loop_evolution (loop_num, 
				       CHREC_LEFT (chrec_before), 
				       code, to_add),
	   CHREC_RIGHT (chrec_before));
      
    default:
      /* Should not happen.  */
      return chrec_top;
    }
}

/* The expression CHREC_BEFORE has an evolution part in LOOP_NUM.  
   Multiply this evolution by the expression TO_MULT.  The invariant attribute 
   means that the TO_MULT expression is one of the nodes that do not depend
   on a loop: INTERVAL_CHREC, INTEGER_CST, VAR_DECL, ...  */

static tree
multiply_by_expr_the_loop_evolution (unsigned loop_num, 
				     tree chrec_before, 
				     tree to_mult)
{
#if defined ENABLE_CHECKING 
  if (chrec_before == NULL_TREE
      || to_mult == NULL_TREE)
    abort ();
#endif
  
  switch (TREE_CODE (chrec_before))
    {
    case POLYNOMIAL_CHREC:
      if (CHREC_VARIABLE (chrec_before) == loop_num)
	return build_polynomial_chrec 
	  (loop_num, 
	   CHREC_LEFT (chrec_before),
	   /* We still don't know how to fold these operations that mix 
	      polynomial and exponential functions.  For the moment, give a 
	      rough approximation: [-oo, +oo].  */
	   chrec_top);
      else
	return build_polynomial_chrec 
	  (CHREC_VARIABLE (chrec_before),
	   multiply_by_expr_the_loop_evolution 
	   (loop_num, CHREC_LEFT (chrec_before), to_mult),
	   /* Do not modify the CHREC_RIGHT part: this part is a fixed part
	      completely determined by the evolution of other scalar variables.
	      The same comment is included in the no_evolution_in_loop_p 
	      function.  */
	   CHREC_RIGHT (chrec_before));
      
    case EXPONENTIAL_CHREC:
      if (CHREC_VARIABLE (chrec_before) == loop_num
	  /* The evolution has to be multiplied on the leftmost position for 
	     loop_num.  */
	  && ((TREE_CODE (CHREC_LEFT (chrec_before)) != POLYNOMIAL_CHREC
	       && TREE_CODE (CHREC_LEFT (chrec_before)) != EXPONENTIAL_CHREC)
	      || (CHREC_VARIABLE (CHREC_LEFT (chrec_before)) != loop_num)))
	return build_exponential_chrec
	  (loop_num, 
	   CHREC_LEFT (chrec_before),
	   chrec_fold_multiply (chrec_type (to_mult), 
				CHREC_RIGHT (chrec_before), to_mult));
      else
	return build_exponential_chrec 
	  (CHREC_VARIABLE (chrec_before),
	   multiply_by_expr_the_loop_evolution
	   (loop_num, CHREC_LEFT (chrec_before), to_mult),
	   /* Do not modify the CHREC_RIGHT part: this part is a fixed part
	      completely determined by the evolution of other scalar variables.
	      The same comment is included in the no_evolution_in_loop_p 
	      function.  */
	   CHREC_RIGHT (chrec_before));
      
    default:
      /* Should not happen.  */
      return chrec_top;
    }
}


/* Add TO_ADD to the evolution part of CHREC_BEFORE in the dimension
   of LOOP_NB.  
   
   Description (provided for completeness, for those who read code in
   a plane, and for my poor 62 bytes brain that would have forgotten
   all this in the next two or three months):
   
   The algorithm of translation of programs from the SSA representation
   into the chrecs syntax is based on a pattern matching.  After having
   reconstructed the overall tree expression for a loop, there are only
   two cases that can arise:
   
   1. a = loop-phi (init, a + expr)
   2. a = loop-phi (init, expr)
   
   where EXPR is either a scalar constant with respect to the analyzed
   loop (this is a degree 0 polynomial), or an expression containing
   other loop-phi definitions (these are higher degree polynomials).
   
   Examples:
   
   1. 
   | init = ...
   | loop_1
   |   a = phi (init, a + 5)
   | endloop
   
   2. 
   | inita = ...
   | initb = ...
   | loop_1
   |   a = phi (inita, 2 * b + 3)
   |   b = phi (initb, b + 1)
   | endloop
   
   For the first case, the semantics of the SSA representation is: 
   
   | a (x) = init + \sum_{j = 0}^{x - 1} expr (j)
   
   that is, there is a loop index "x" that determines the scalar value
   of the variable during the loop execution.  During the first
   iteration, the value is that of the initial condition INIT, while
   during the subsequent iterations, it is the sum of the initial
   condition with the sum of all the values of EXPR from the initial
   iteration to the before last considered iteration.  
   
   For the second case, the semantics of the SSA program is:
   
   | a (x) = init, if x = 0;
   |         expr (x - 1), otherwise.
   
   The second case corresponds to the PEELED_CHREC, whose syntax is
   close to the syntax of a loop-phi-node: 
   
   | phi (init, expr)  vs.  (init, expr)_x
   
   The proof of the translation algorithm for the first case is a
   proof by structural induction based on the degree of EXPR.  
   
   Degree 0:
   When EXPR is a constant with respect to the analyzed loop, or in
   other words when EXPR is a polynomial of degree 0, the evolution of
   the variable A in the loop is an affine function with an initial
   condition INIT, and a step EXPR.  In order to show this, we start
   from the semantics of the SSA representation:
   
   f (x) = init + \sum_{j = 0}^{x - 1} expr (j)
   
   and since "expr (j)" is a constant with respect to "j",
   
   f (x) = init + x * expr 
   
   Finally, based on the semantics of the pure sum chrecs, by
   identification we get the corresponding chrecs syntax:
   
   f (x) = init * \binom{x}{0} + expr * \binom{x}{1} 
   f (x) -> {init, +, expr}_x
   
   Higher degree:
   Suppose that EXPR is a polynomial of degree N with respect to the
   analyzed loop_x for which we have already determined that it is
   written under the chrecs syntax:
   
   | expr (x)  ->  {b_0, +, b_1, +, ..., +, b_{n-1}} (x)
   
   We start from the semantics of the SSA program:
   
   | f (x) = init + \sum_{j = 0}^{x - 1} expr (j)
   |
   | f (x) = init + \sum_{j = 0}^{x - 1} 
   |                (b_0 * \binom{j}{0} + ... + b_{n-1} * \binom{j}{n-1})
   |
   | f (x) = init + \sum_{j = 0}^{x - 1} 
   |                \sum_{k = 0}^{n - 1} (b_k * \binom{j}{k}) 
   |
   | f (x) = init + \sum_{k = 0}^{n - 1} 
   |                (b_k * \sum_{j = 0}^{x - 1} \binom{j}{k}) 
   |
   | f (x) = init + \sum_{k = 0}^{n - 1} 
   |                (b_k * \binom{x}{k + 1}) 
   |
   | f (x) = init + b_0 * \binom{x}{1} + ... 
   |              + b_{n-1} * \binom{x}{n} 
   |
   | f (x) = init * \binom{x}{0} + b_0 * \binom{x}{1} + ... 
   |                             + b_{n-1} * \binom{x}{n} 
   |
   
   And finally from the definition of the chrecs syntax, we identify:
   | f (x)  ->  {init, +, b_0, +, ..., +, b_{n-1}}_x 
   
   This shows the mechanism that stands behind the add_to_evolution
   function.  An important point is that the use of symbolic
   parameters avoids the need of an analysis schedule.
   
   Example:
   
   | inita = ...
   | initb = ...
   | loop_1 
   |   a = phi (inita, a + 2 + b)
   |   b = phi (initb, b + 1)
   | endloop
   
   When analyzing "a", the algorithm keeps "b" symbolically:
   
   | a  ->  {inita, +, 2 + b}_1
   
   Then, after instantiation, the analyzer ends on the evolution:
   
   | a  ->  {inita, +, 2 + initb, +, 1}_1

*/

static tree 
add_to_evolution (unsigned loop_nb, 
		  tree chrec_before,
		  enum tree_code code,
		  tree to_add)
{
  tree res = NULL_TREE;
  
  if (to_add == NULL_TREE)
    return chrec_before;
  
  /* TO_ADD is either a scalar, or a parameter.  TO_ADD is not
     instantiated at this point.  */
  if (TREE_CODE (to_add) == POLYNOMIAL_CHREC
      || TREE_CODE (to_add) == EXPONENTIAL_CHREC)
    /* This should not happen.  */
    return chrec_top;
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "(add_to_evolution \n");
      fprintf (dump_file, "(loop_nb = %d)\n", loop_nb);
      fprintf (dump_file, "  (chrec_before = ");
      print_generic_expr (dump_file, chrec_before, 0);
      fprintf (dump_file, ")\n  (to_add = ");
      print_generic_expr (dump_file, to_add, 0);
      fprintf (dump_file, ")\n");
    }
  
  if (no_evolution_in_loop_p (chrec_before, loop_nb))
    {
      if (code == MINUS_EXPR)
	to_add = chrec_fold_multiply 
	  (chrec_type (to_add), to_add,
	   convert (chrec_type (to_add), integer_minus_one_node));
      
      /* testsuite/.../ssa-chrec-39.c.  */
      res = build_polynomial_evolution_in_loop 
	(loop_nb, chrec_before, to_add);
    }
  
  else
    /* testsuite/.../ssa-chrec-20.c.  */
    res = add_expr_to_loop_evolution (loop_nb, chrec_before, code, to_add);
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "  (res = ");
      print_generic_expr (dump_file, res, 0);
      fprintf (dump_file, "))\n");
    }
  
  return res;
}

/* Add TO_MULT to the evolution part of CHREC_BEFORE in the dimension
   of LOOP_NB.  */

static tree 
multiply_evolution (unsigned loop_nb, 
		    tree chrec_before,
		    tree to_mult)
{
  tree res = NULL_TREE;
  
  if (to_mult == NULL_TREE)
    return chrec_before;

  /* TO_MULT is either a scalar, or a parameter.  TO_MULT is not
     instantiated at this point.  */
  if (TREE_CODE (to_mult) == POLYNOMIAL_CHREC
      || TREE_CODE (to_mult) == EXPONENTIAL_CHREC)
    /* This should not happen.  */
    return chrec_top;
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "(multiply_evolution \n");
      fprintf (dump_file, "  (loop_nb = %d)\n", loop_nb);
      fprintf (dump_file, "  (chrec_before = ");
      print_generic_expr (dump_file, chrec_before, 0);
      fprintf (dump_file, ")\n  (to_mult = ");
      print_generic_expr (dump_file, to_mult, 0);
      fprintf (dump_file, ")\n");
    }
  
  if (no_evolution_in_loop_p (chrec_before, loop_nb))
    /* testsuite/.../ssa-chrec-22.c.  */
    res = build_exponential_evolution_in_loop 
      (loop_nb, chrec_before, to_mult);
  else
    res = multiply_by_expr_the_loop_evolution 
      (loop_nb, chrec_before, to_mult);
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "  (res = ");
      print_generic_expr (dump_file, res, 0);
      fprintf (dump_file, "))\n");
    }
  
  return res;
}



/* This section deals with the approximation of the number of
   iterations a loop will run.  */

/* Helper function that determines whether the considered types are
   compatible for finding a solution.  */
#if 0
static bool
types_forbid_solutions_p (enum tree_code code, 
			  tree type0, 
			  tree type1)
{
  switch (code)
    {
    case LE_EXPR:
      return tree_is_le (TYPE_MAX_VALUE (type0), 
			 TYPE_MIN_VALUE (type1));
      
    case LT_EXPR:
      return tree_is_lt (TYPE_MAX_VALUE (type0), 
			 TYPE_MIN_VALUE (type1));
      
    case EQ_EXPR:
      return false;
      
    case NE_EXPR:
      return (tree_is_lt (TYPE_MAX_VALUE (type0), 
			  TYPE_MIN_VALUE (type1))
	      || tree_is_lt (TYPE_MAX_VALUE (type1), 
			     TYPE_MIN_VALUE (type0)));
      
    default:
      abort ();
      return false;
    }
}
#endif

/* Helper function for the case when both evolution functions don't
   have an evolution in the considered loop.  */

static tree 
first_iteration_non_satisfying_noev_noev (enum tree_code code, 
					  unsigned loop_nb ATTRIBUTE_UNUSED, 
					  tree chrec0, 
					  tree chrec1)
{
  tree init0 = initial_condition (chrec0);
  tree init1 = initial_condition (chrec1);
  
  if (TREE_CODE (init0) != INTEGER_CST
      || TREE_CODE (init1) != INTEGER_CST)
    return chrec_top;

  if (!evolution_function_is_constant_p (chrec0)
      || !evolution_function_is_constant_p (chrec1))
    return chrec_top;
  
  switch (code)
    {
    case LE_EXPR:
      if (tree_is_gt (init0, init1))
	return integer_zero_node;
      else
	return chrec_bot;
      
    case LT_EXPR:
      if (tree_is_ge (init0, init1))
	return integer_zero_node;
      else
	return chrec_bot;
      
    case EQ_EXPR:
      if (tree_is_eq (init0, init1))
	return integer_zero_node;
      else
	return chrec_bot;
      
    case NE_EXPR:
      if (tree_is_ne (init0, init1))
	return integer_zero_node;
      else
	return chrec_bot;
      
    default:
      return chrec_top;
    }
}

/* Helper function for the case when CHREC0 has no evolution and
   CHREC1 has an evolution in the considered loop.  */

static tree 
first_iteration_non_satisfying_noev_ev (enum tree_code code, 
					unsigned loop_nb, 
					tree chrec0, 
					tree chrec1)
{
  tree type1 = chrec_type (chrec1);
  /*  tree tmax = TYPE_MAX_VALUE (type1); */
  tree ev_in_this_loop;
  tree init0, init1, step1;
  tree nb_iters;
  
  ev_in_this_loop = evolution_function_in_loop_num (chrec1, loop_nb);
  if (!evolution_function_is_affine_p (ev_in_this_loop))
    /* For the moment handle only polynomials of degree 1.  */
    return chrec_top;
  
  init1 = CHREC_LEFT (ev_in_this_loop);
  step1 = CHREC_RIGHT (ev_in_this_loop);
  init0 = initial_condition (chrec0);
  if (TREE_CODE (init0) != INTEGER_CST
      || TREE_CODE (init1) != INTEGER_CST
      || TREE_CODE (step1) != INTEGER_CST)
    /* For the moment we deal only with INTEGER_CSTs.  */
    return chrec_top;
  
  switch (code)
    {
    case LE_EXPR:
      {
	if (tree_is_gt (init0, init1))
	  {
	    if (evolution_function_is_constant_p (chrec0))
	      /* Example: "while (2 <= {0, +, 1}_2)".  */
	      return integer_zero_node;
	    else
	      /* Example: "while ({2, +, -1}_1 <= {0, +, 1}_2)".  The
		 number of iterations in loop_2 during the first two
		 iterations of loop_1 is equal to 0.  */
	      return chrec_top;
	  }
	
	if (tree_int_cst_sgn (step1) > 0)
	  {
	    if (evolution_function_is_constant_p (chrec0))
	      /* Example: "while (2 <= {3, +, 1}_2)".  */
	      return chrec_top;
	    /* nb_iters = tree_fold_plus 
	       (integer_type_node, 
	       tree_fold_floor_div (integer_type_node, 
	       tree_fold_minus (integer_type_node, 
	       tmax, init1), 
	       step1), 
	       integer_one_node);
	    */
	    else
	      /* Example: "while ({2, +, 1}_1 <= {3, +, 1}_2)".  */
	      return chrec_top;
	  }
	
	else
	  {
	    if (evolution_function_is_constant_p (chrec0))
	      /* Example: "while (2 <= {3, +, -1}_2)".  */
	      nb_iters = tree_fold_plus 
		(integer_type_node, 
		 tree_fold_floor_div (integer_type_node, 
				      tree_fold_minus (integer_type_node, 
						       init1, init0), 
				      tree_fold_abs (integer_type_node, 
						     step1)), 
		 integer_one_node);
	    else
	      /* Example: "while ({2, +, 1}_1 <= {3, +, -1}_2)".  */
	      return chrec_top;
	  }
	
	/* Verify the result.  */
	if (evolution_function_is_constant_p (chrec0)
	    && tree_is_gt (init0, 
			   tree_fold_plus (type1, init1, 
					   tree_fold_multiply 
					   (integer_type_node, 
					    nb_iters, step1))))
	  return nb_iters;
	
	else 
	  /* Difficult cases fall down there.  Example: When the
	     evolution step is big enough the wrapped value can be
	     bigger than init0.  In these cases the loop may end after
	     several wraps, or never end.  */
	  return chrec_top;
      }
      
    case LT_EXPR:
      {
	if (tree_is_ge (init0, init1))
	  {
	    if (evolution_function_is_constant_p (chrec0))
	      /* Example: "while (2 < {0, +, 1}_2)".  */
	      return integer_zero_node;
	    else
	      /* Example: "while ({2, +, 1}_1 < {0, +, 1}_2)".  */
	      return chrec_top;
	  }
	
	if (tree_int_cst_sgn (step1) > 0)
	  {
	    if (evolution_function_is_constant_p (chrec0))
	      /* Example: "while (2 < {3, +, 1}_2)".  */
	      return chrec_top;
	    /* nb_iters = tree_fold_ceil_div
	       (integer_type_node, 
	       tree_fold_minus (integer_type_node, tmax, init1), 
	       step1);
	    */
	    else
	      /* Example: "while ({2, +, 1}_1 < {3, +, 1}_2)".  */
	      return chrec_top;
	  }
	else 
	  {
	    if (evolution_function_is_constant_p (chrec0))
	      /* Example: "while (2 < {3, +, -1}_2)".  */
	      nb_iters = tree_fold_ceil_div
		(integer_type_node, 
		 tree_fold_minus (type1, init1, init0), 
		 tree_fold_abs (type1, step1));
	    else
	      /* Example: "while ({2, +, 1}_1 < {3, +, -1}_2)".  */
	      return chrec_top;
	  }
	
	/* Verify the result.  */
	if (evolution_function_is_constant_p (chrec0)
	    && tree_is_ge (init0, 
			   tree_fold_plus (type1, init1, 
					   tree_fold_multiply 
					   (integer_type_node, 
					    nb_iters, step1))))
	  return nb_iters;
	
	else 
	  /* Difficult cases fall down there.  */
	  return chrec_top;
      }
      
    case EQ_EXPR:
      {
	if (tree_is_ne (init0, init1))
	  {
	    if (evolution_function_is_constant_p (chrec0))
	      /* Example: "while (2 == {0, +, 1}_2)".  */
	      return integer_zero_node;
	    else
	      /* Example: "while ({2, +, -1}_1 == {0, +, 1}_2)".  */
	      return chrec_top;
	  }
	
	if (evolution_function_is_constant_p (chrec0))
	  {
	    if (integer_zerop (step1))
	      /* Example: "while (2 == {2, +, 0}_2)".  */
	      return chrec_bot;
	    else
	      return integer_one_node;
	  }
	else
	  return chrec_top;
      }
      
    case NE_EXPR:
      {
	if (tree_is_eq (init0, init1))
	  {
	    if (evolution_function_is_constant_p (chrec0))
	      /* Example: "while (0 != {0, +, 1}_2)".  */
	      return integer_zero_node;
	    else
	      /* Example: "while ({0, +, -1}_1 != {0, +, 1}_2)".  */
	      return chrec_top;
	  }
	
	if (tree_int_cst_sgn (step1) > 0)
	  {
	    if (evolution_function_is_constant_p (chrec0))
	      {
		if (tree_is_gt (init0, init1))
		  {
		    tree diff = tree_fold_minus (integer_type_node, 
						 init0, init1);
		    if (tree_fold_divides_p (integer_type_node, step1, diff))
		      /* Example: "while (3 != {2, +, 1}_2)".  */
		      nb_iters = tree_fold_exact_div 
			(integer_type_node, diff, step1);
		    else
		      /* Example: "while (3 != {2, +, 2}_2)".  */
		      return chrec_top;
		  }
		else
		  /* Example: "while (2 != {3, +, 1}_2)".  */
		  return chrec_top;
	      }
	    else
	      /* Example: "while ({2, +, 1}_1 != {3, +, 1}_2)".  */
	      return chrec_top;
	  }
	
	else
	  {
	    if (evolution_function_is_constant_p (chrec0))
	      {
		if (tree_is_lt (init0, init1))
		  {
		    tree diff = tree_fold_minus (integer_type_node, 
						 init1, init0);
		    if (tree_fold_divides_p (integer_type_node, step1, diff))
		      /* Example: "while (2 != {3, +, -1}_2)".  */
		      nb_iters = tree_fold_exact_div 
			(integer_type_node, diff, 
			 tree_fold_abs (integer_type_node, step1));
		    else
		      /* Example: "while (2 != {3, +, -2}_2)".  */
		      return chrec_top;
		  }
		else
		  /* Example: "while (3 != {2, +, -1}_2)".  */
		  return chrec_top;
	      }
	    else
	      /* Example: "while ({2, +, 1}_1 != {3, +, -1}_2)".  */
	      return chrec_top;
	  }

	/* Verify the result.  */
	if (evolution_function_is_constant_p (chrec0)
	    && tree_is_eq (init0, 
			   tree_fold_plus (type1, init1, 
					   tree_fold_multiply 
					   (integer_type_node, 
					    nb_iters, step1))))
	  return nb_iters;
	
	else 
	  /* Difficult cases fall down there.  */
	  return chrec_top;
      }

    default:
      return chrec_top;
    }
  return chrec_top;
}

/* Helper function for the case when CHREC1 has no evolution and
   CHREC0 has an evolution in the considered loop.  */

static tree 
first_iteration_non_satisfying_ev_noev (enum tree_code code, 
					unsigned loop_nb, 
					tree chrec0, 
					tree chrec1)
{
  tree type0 = chrec_type (chrec0);
  /*  tree tmin = TYPE_MIN_VALUE (type0); */
  tree ev_in_this_loop;
  tree init0, init1, step0;
  tree nb_iters;
  
  ev_in_this_loop = evolution_function_in_loop_num (chrec0, loop_nb);
  if (!evolution_function_is_affine_p (ev_in_this_loop))
    /* For the moment handle only polynomials of degree 1.  */
    return chrec_top;
  
  init0 = CHREC_LEFT (ev_in_this_loop);
  step0 = CHREC_RIGHT (ev_in_this_loop);
  init1 = initial_condition (chrec1);
  if (TREE_CODE (init1) != INTEGER_CST
      || TREE_CODE (init0) != INTEGER_CST
      || TREE_CODE (step0) != INTEGER_CST)
    /* For the moment we deal only with INTEGER_CSTs.  */
    return chrec_top;
  
  switch (code)
    {
    case LE_EXPR:
      {
	if (tree_is_gt (init0, init1))
	  {
	    if (evolution_function_is_constant_p (chrec1))
	      /* Example: "while ({2, +, 1}_2 <= 0)".  */
	      return integer_zero_node;
	    
	    else
	      /* Example: "while ({2, +, 1}_2 <= {0, +, 1}_1)".  */
	      return chrec_top;
	  }
	
	if (tree_int_cst_sgn (step0) < 0)
	  {
	    if (evolution_function_is_constant_p (chrec1))
	      /* Example: "while ({2, +, -1}_2 <= 3)".  */
	      return chrec_top;
	    /* nb_iters = tree_fold_plus 
	       (integer_type_node, 
	       tree_fold_floor_div (integer_type_node, 
	       tree_fold_minus (integer_type_node, 
	       init0, tmin), 
	       tree_fold_abs (integer_type_node, 
	       step0)), 
	       integer_one_node);
	    */
	    else
	      /* Example: "while ({2, +, -1}_2 <= {3, +, 1}_1)".  */
	      return chrec_top;
	  }
	else 
	  {
	    if (evolution_function_is_constant_p (chrec1))
	      /* Example: "while ({2, +, 1}_2 <= 3)".  */
	      nb_iters = tree_fold_plus 
		(integer_type_node, 
		 tree_fold_floor_div (integer_type_node, 
				      tree_fold_minus (integer_type_node, 
						       init1, init0), 
				      step0), 
		 integer_one_node);
	    else
	      /* Example: "while ({2, +, 1}_2 <= {3, +, 1}_1)".  */
	      return chrec_top;
	  }
	
	/* Verify the result.  */
	if (evolution_function_is_constant_p (chrec1)
	    && tree_is_gt (tree_fold_plus (type0, init0, 
					   tree_fold_multiply 
					   (integer_type_node, 
					    nb_iters, step0)), 
			   init1))
	  return nb_iters;
	
	else 
	  /* Difficult cases fall down there.  */
	  return chrec_top;
      }
      
    case LT_EXPR:
      {
	if (tree_is_ge (init0, init1))
	  {
	    if (evolution_function_is_constant_p (chrec1))
	      /* Example: "while ({2, +, 1}_2 < 0)".  */
	      return integer_zero_node;
	    
	    else
	      /* Example: "while ({2, +, 1}_2 < {0, +, 1}_1)".  */
	      return chrec_top;
	  }
	
	if (tree_int_cst_sgn (step0) < 0)
	  {
	    if (evolution_function_is_constant_p (chrec1))
	      /* Example: "while ({2, +, -1}_2 < 3)".  */
	      return chrec_top;
	    /* nb_iters = tree_fold_ceil_div 
	       (integer_type_node, 
	       tree_fold_minus (integer_type_node, init0, tmin), 
	       tree_fold_abs (integer_type_node, step0));
	    */
	    else
	      /* Example: "while ({2, +, -1}_2 < {3, +, 1}_1)".  */
	      return chrec_top;
	  }
	else 
	  {
	    if (evolution_function_is_constant_p (chrec1))
	      /* Example: "while ({2, +, 1}_2 < 3)".  */
	      nb_iters = tree_fold_ceil_div
		(integer_type_node, 
		 tree_fold_minus (integer_type_node, init1, init0), 
		 step0);
	    else
	      /* Example: "while ({2, +, 1}_2 < {3, +, 1}_1)".  */
	      return chrec_top;
	  }
	
	/* Verify the result.  */
	if (evolution_function_is_constant_p (chrec1)
	    && tree_is_ge (tree_fold_plus (type0, init0, 
					   tree_fold_multiply 
					   (integer_type_node, 
					    nb_iters, step0)),
			   init1))
	  return nb_iters;
	
	else 
	  /* Difficult cases fall down there.  */
	  return chrec_top;
      }
      
    case EQ_EXPR:
      {
	if (tree_is_ne (init0, init1))
	  {
	    if (evolution_function_is_constant_p (chrec1))
	      /* Example: "while ({2, +, 1}_2 == 0)".  */
	      return integer_zero_node;
	    else
	      /* Example: "while ({2, +, -1}_2 == {0, +, 1}_1)".  */
	      return chrec_top;
	  }
	
	if (evolution_function_is_constant_p (chrec1))
	  {
	    if (integer_zerop (step0))
	      /* Example: "while ({2, +, 0}_2 == 2)".  */
	      return chrec_bot;
	    else
	      return integer_one_node;
	  }
	else
	  return chrec_top;
      }	
      
    case NE_EXPR:
      {
	if (tree_is_eq (init0, init1))
	  {
	    if (evolution_function_is_constant_p (chrec1))
	      /* Example: "while ({0, +, 1}_2 != 0)".  */
	      return integer_zero_node;
	    else
	      /* Example: "while ({0, +, -1}_2 != {0, +, 1}_1)".  */
	      return chrec_top;
	  }
	
	if (tree_int_cst_sgn (step0) > 0)
	  {
	    if (evolution_function_is_constant_p (chrec1))
	      {
		if (tree_is_lt (init0, init1))
		  {
		    tree diff = tree_fold_minus (integer_type_node, 
						 init1, init0);
		    if (tree_fold_divides_p (integer_type_node, step0, diff))
		      /* Example: "while ({2, +, 1}_2 != 3)".  */
		      nb_iters = tree_fold_exact_div 
			(integer_type_node, diff, step0);
		    else
		      /* Example: "while ({2, +, 2}_2 != 3)".  */
		      return chrec_top;
		  }
		else
		  /* Example: "while ({3, +, 1}_2 != 2)".  */
		  return chrec_top;
	      }
	    else
	      /* Example: "while ({2, +, 1}_2 != {3, +, 1}_1)".  */
	      return chrec_top;
	  }
	
	else
	  {
	    if (evolution_function_is_constant_p (chrec1))
	      {
		if (tree_is_gt (init0, init1))
		  {
		    tree diff = tree_fold_minus (integer_type_node, 
						 init0, init1);
		    if (tree_fold_divides_p (integer_type_node, step0, diff))
		      /* Example: "while ({3, +, -1}_2 != 2)".  */
		      nb_iters = tree_fold_exact_div 
			(integer_type_node, diff, 
			 tree_fold_abs (integer_type_node, step0));
		    else
		      /* Example: "while ({3, +, -2}_2 != 2)".  */
		      return chrec_top;
		  }
		else
		  /* Example: "while ({2, +, -1}_2 != 3)".  */
		  return chrec_top;
	      }
	    else
	      /* Example: "while ({2, +, -1}_2 != {3, +, -1}_1)".  */
	      return chrec_top;
	  }

	/* Verify the result.  */
	if (evolution_function_is_constant_p (chrec1)
	    && tree_is_eq (tree_fold_plus (type0, init0, 
					   tree_fold_multiply 
					   (integer_type_node, 
					    nb_iters, step0)),
			   init1))
	  return nb_iters;
	else 
	  /* Difficult cases fall down there.  */
	  return chrec_top;
      }
      
    default:
      return chrec_top;
    }
  
  return chrec_top;
}

/* Helper function for the case when both CHREC0 and CHREC1 has an
   evolution in the considered loop.  */

static tree 
first_iteration_non_satisfying_ev_ev (enum tree_code code, 
				      unsigned loop_nb ATTRIBUTE_UNUSED, 
				      tree chrec0 ATTRIBUTE_UNUSED, 
				      tree chrec1 ATTRIBUTE_UNUSED)
{
  switch (code)
    {
    case LE_EXPR:
      
    case LT_EXPR:
      
    case EQ_EXPR:
      
    case NE_EXPR:
      
    default:
      return chrec_top;
    }
  
  return chrec_top;
}

/* Helper function.  */

static tree 
first_iteration_non_satisfying_1 (enum tree_code code, 
				  unsigned loop_nb, 
				  tree chrec0, 
				  tree chrec1)
{
  if (automatically_generated_chrec_p (chrec0)
      || automatically_generated_chrec_p (chrec1))
    return chrec_top;
  
  if (no_evolution_in_loop_p (chrec0, loop_nb))
    {
      if (no_evolution_in_loop_p (chrec1, loop_nb))
	return first_iteration_non_satisfying_noev_noev (code, loop_nb, 
							 chrec0, chrec1);
      else
	return first_iteration_non_satisfying_noev_ev (code, loop_nb, 
						       chrec0, chrec1);
    }
  
  else
    {
      if (no_evolution_in_loop_p (chrec1, loop_nb))
	return first_iteration_non_satisfying_ev_noev (code, loop_nb, 
						       chrec0, chrec1);
      else
	return first_iteration_non_satisfying_ev_ev (code, loop_nb, 
						     chrec0, chrec1);
    }
}

/* Try to compute the first iteration I of LOOP_NB that does not satisfy
   CODE: in the context of the computation of the number of iterations:
   - if (CODE is LE_EXPR) the loop exits when CHREC0 (I) > CHREC1 (I),
   - if (CODE is LT_EXPR) the loop exits when CHREC0 (I) >= CHREC1 (I),
   - if (CODE is EQ_EXPR) the loop exits when CHREC0 (I) != CHREC1 (I), 
   ...
   
   The result is one of the following: 
   - CHREC_TOP when the analyzer cannot determine the property, 
   - CHREC_BOT when the property is always true, 
   - an INTEGER_CST tree node, 
   - a CHREC, 
   - an expression containing SSA_NAMEs.
*/

tree 
first_iteration_non_satisfying (enum tree_code code, 
				unsigned loop_nb, 
				tree chrec0, 
				tree chrec1)
{
  switch (code)
    {
    case LT_EXPR:
      return first_iteration_non_satisfying_1 (LT_EXPR, loop_nb, 
					       chrec0, chrec1);
      
    case LE_EXPR:
      return first_iteration_non_satisfying_1 (LE_EXPR, loop_nb, 
					       chrec0, chrec1);
      
    case GT_EXPR:
      return first_iteration_non_satisfying_1 (LT_EXPR, loop_nb, 
					       chrec1, chrec0);
      
    case GE_EXPR:
      return first_iteration_non_satisfying_1 (LE_EXPR, loop_nb, 
					       chrec1, chrec0);
      
    case EQ_EXPR:
      return first_iteration_non_satisfying_1 (EQ_EXPR, loop_nb, 
					       chrec0, chrec1);
      
    case NE_EXPR:
      return first_iteration_non_satisfying_1 (NE_EXPR, loop_nb, 
					       chrec0, chrec1);
      
    default:
      return chrec_top;
    }
}

/* Helper function.  */

static inline tree
cannot_analyze_loop_nb_iterations_yet (void)
{
  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "  (nb_iterations cannot be determined))\n");
  
  /* Do not update the loop->nb_iterations.  */
  return chrec_top;
}

/* Helper function.  */

static inline tree
set_nb_iterations_in_loop (struct loop *loop, 
			   tree res)
{
  /* After the loop copy headers has transformed the code, each loop
     runs at least once.  */
  res = chrec_fold_plus (chrec_type (res), res, integer_one_node);
  /* FIXME HWI: However we want to store one iteration less than the
     count of the loop in order to be compatible with the other
     nb_iter computations in loop-iv.  This also allows the
     representation of nb_iters that are equal to MAX_INT.  */
  if (TREE_CODE (res) == INTEGER_CST
      && TREE_INT_CST_LOW (res) == 0)
    res = chrec_top;
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "  (set_nb_iterations_in_loop = ");
      print_generic_expr (dump_file, res, 0);
      fprintf (dump_file, "))\n");
    }
  
  loop->nb_iterations = res;
  return res;
}



/* This section selects the loops that will be good candidates for the
   scalar evolution analysis.
   
   Note: This section will be rewritten to expose a better interface
   to other client passes.  For the moment, greedily select all the
   loop nests we could analyze.  */

/* Determine whether it is possible to analyze this condition
   expression.  */

static bool
analyzable_condition (tree expr)
{
  tree condition;
  
  if (TREE_CODE (expr) != COND_EXPR)
    return false;
  
  condition = TREE_OPERAND (expr, 0);
  
  switch (TREE_CODE (condition))
    {
    case SSA_NAME:
      /* Volatile expressions are not analyzable.  */
      if (TREE_THIS_VOLATILE (SSA_NAME_VAR (condition)))
	return false;
      return true;
      
    case LT_EXPR:
    case LE_EXPR:
    case GT_EXPR:
    case GE_EXPR:
    case EQ_EXPR:
    case NE_EXPR:
      {
	tree opnd0, opnd1;
	
	opnd0 = TREE_OPERAND (condition, 0);
	opnd1 = TREE_OPERAND (condition, 1);
	
	if (TREE_CODE (opnd0) == SSA_NAME
	    && TREE_THIS_VOLATILE (SSA_NAME_VAR (opnd0)))
	  return false;
	
	if (TREE_CODE (opnd1) == SSA_NAME
	    && TREE_THIS_VOLATILE (SSA_NAME_VAR (opnd1)))
	  return false;
	
	return true;
      }
      
    default:
      return false;
    }
  
  return false;
}

/* For a loop with a single exit edge, determine the COND_EXPR that
   guards the exit edge.  If the expression is too difficult to
   analyze, then give up.  */

tree 
get_loop_exit_condition (struct loop *loop)
{
  tree res = NULL_TREE;
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "(get_loop_exit_condition \n  ");
  
  if (loop_exit_edges (loop))
    {
      edge exit_edge;
      tree expr;
      
      exit_edge = loop_exit_edge (loop, 0);
      expr = last_stmt (edge_source (exit_edge));
      
      if (analyzable_condition (expr))
	res = expr;
    }
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      print_generic_expr (dump_file, res, 0);
      fprintf (dump_file, ")\n");
    }
  
  return res;
}

/* Recursively determine and enqueue the exit conditions for a loop.  */

static void 
get_exit_conditions_rec (struct loop *loop, 
			 varray_type *exit_conditions)
{
  if (!loop)
    return;
  
  /* Recurse on the inner loops, then on the next (sibling) loops.  */
  get_exit_conditions_rec (inner_loop (loop), exit_conditions);
  get_exit_conditions_rec (next_loop (loop), exit_conditions);
  
  flow_loop_scan (loop, LOOP_EXIT_EDGES);
  if (loop_num_exits (loop) == 1)
    {
      tree loop_condition = get_loop_exit_condition (loop);
      
      if (loop_condition)
	VARRAY_PUSH_TREE (*exit_conditions, loop_condition);
    }
}

/* Select the candidate loop nests for the analysis.  This function
   initializes the EXIT_CONDITIONS array.  The vector EXIT_CONDITIONS is
   initialized in a loop-depth-first order, ie. the inner loops
   conditions appear before the outer.  This property of the
   EXIT_CONDITIONS list is exploited by the evolution analyzer.  */

static void
select_loops_exit_conditions (struct loops *loops, 
			      varray_type *exit_conditions)
{
  struct loop *function_body = loops->parray[0];
  
  get_exit_conditions_rec (inner_loop (function_body), exit_conditions);
}



/* Debugging functions section.  */

extern void draw_tree_cfg (void);

/* Draw the flow graph.  */

void
draw_tree_cfg (void)
{
  FILE *dump_file;
  if (n_basic_blocks > 0)
    {
      dump_file = fopen ("tree_cfg.dot", "w");
      if (dump_file)
        {
          tree_cfg2dot (dump_file);
          fclose (dump_file);
          system ("dotty tree_cfg.dot");
        }
    }
}
     


/* Follow the ssa edge into the right hand side of an assignment.  */

static bool
follow_ssa_edge_in_rhs (struct loop *loop,
			tree rhs, 
			tree halting_phi, 
			tree *evolution_of_loop)
{
  bool res = false;
  tree rhs0, rhs1;
  tree type_rhs = TREE_TYPE (rhs);
  
  /* The RHS is one of the following cases:
     - an SSA_NAME, 
     - an INTEGER_CST,
     - a PLUS_EXPR, 
     - a MINUS_EXPR,
     - other cases are not yet handled. 
  */
  switch (TREE_CODE (rhs))
    {
    case INTEGER_CST:
      /* This assignment is under the form "a_1 = 7".  */
      res = false;
      break;
      
    case SSA_NAME:
      /* This assignment is under the form: "a_1 = b_2".  */
      res = follow_ssa_edge 
	(loop, SSA_NAME_DEF_STMT (rhs), halting_phi, evolution_of_loop);
      break;
      
    case PLUS_EXPR:
      /* This case is under the form "rhs0 + rhs1".  */
      rhs0 = TREE_OPERAND (rhs, 0);
      rhs1 = TREE_OPERAND (rhs, 1);
      
      if (TREE_CODE (rhs0) == SSA_NAME)
	{
	  if (TREE_CODE (rhs1) == SSA_NAME)
	    {
	      /* Match an assignment under the form: 
		 "a = b + c".  */
	      res = follow_ssa_edge 
		(loop, SSA_NAME_DEF_STMT (rhs0), halting_phi, 
		 evolution_of_loop);
	      
	      if (res)
		*evolution_of_loop = add_to_evolution 
		  (loop->num, 
		   chrec_convert (type_rhs, *evolution_of_loop), 
		   PLUS_EXPR, rhs1);
	      
	      else
		{
		  res = follow_ssa_edge 
		    (loop, SSA_NAME_DEF_STMT (rhs1), halting_phi, 
		     evolution_of_loop);
		  
		  if (res)
		    *evolution_of_loop = add_to_evolution 
		      (loop->num, 
		       chrec_convert (type_rhs, *evolution_of_loop), 
		       PLUS_EXPR, rhs0);
		}
	    }
	  
	  else
	    {
	      /* Match an assignment under the form: 
		 "a = b + ...".  */
	      res = follow_ssa_edge 
		(loop, SSA_NAME_DEF_STMT (rhs0), halting_phi, 
		 evolution_of_loop);
	      if (res)
		*evolution_of_loop = add_to_evolution 
		  (loop->num, chrec_convert (type_rhs, *evolution_of_loop), 
		   PLUS_EXPR, rhs1);
	    }
	}
      
      else if (TREE_CODE (rhs1) == SSA_NAME)
	{
	  /* Match an assignment under the form: 
	     "a = ... + c".  */
	  res = follow_ssa_edge 
	    (loop, SSA_NAME_DEF_STMT (rhs1), halting_phi, 
	     evolution_of_loop);
	  if (res)
	    *evolution_of_loop = add_to_evolution 
	      (loop->num, chrec_convert (type_rhs, *evolution_of_loop), 
	       PLUS_EXPR, rhs0);
	}

      else
	/* Otherwise, match an assignment under the form: 
	   "a = ... + ...".  */
	/* And there is nothing to do.  */
	res = false;
      
      break;
      
    case MINUS_EXPR:
      /* This case is under the form "opnd0 = rhs0 - rhs1".  */
      rhs0 = TREE_OPERAND (rhs, 0);
      rhs1 = TREE_OPERAND (rhs, 1);
      if (TREE_CODE (rhs0) == SSA_NAME)
	{
	  if (TREE_CODE (rhs1) == SSA_NAME)
	    {
	      /* Match an assignment under the form: 
		 "a = b - c".  */
	      res = follow_ssa_edge 
		(loop, SSA_NAME_DEF_STMT (rhs0), halting_phi, 
		 evolution_of_loop);
	      
	      if (res)
		*evolution_of_loop = add_to_evolution 
		  (loop->num, chrec_convert (type_rhs, *evolution_of_loop), 
		   MINUS_EXPR, rhs1);
	      
	      else
		{
		  res = follow_ssa_edge 
		    (loop, SSA_NAME_DEF_STMT (rhs1), halting_phi, 
		     evolution_of_loop);
		  
		  if (res)
		    *evolution_of_loop = add_to_evolution 
		      (loop->num, 
		       chrec_fold_multiply (type_rhs, 
					    *evolution_of_loop, 
					    convert (type_rhs,
						     integer_minus_one_node)),
		       PLUS_EXPR, rhs0);
		}
	    }
	  
	  else
	    {
	      /* Match an assignment under the form: 
		 "a = b - ...".  */
	      res = follow_ssa_edge 
		(loop, SSA_NAME_DEF_STMT (rhs0), halting_phi, 
		 evolution_of_loop);
	      if (res)
		*evolution_of_loop = add_to_evolution 
		  (loop->num, chrec_convert (type_rhs, *evolution_of_loop), 
		   MINUS_EXPR, rhs1);
	    }
	}
      
      else if (TREE_CODE (rhs1) == SSA_NAME)
	{
	  /* Match an assignment under the form: 
	     "a = ... - c".  */
	  res = follow_ssa_edge 
	    (loop, SSA_NAME_DEF_STMT (rhs1), halting_phi, 
	     evolution_of_loop);
	  if (res)
	    *evolution_of_loop = add_to_evolution 
	      (loop->num, 
	       chrec_fold_multiply (type_rhs, 
				    *evolution_of_loop, 
				    convert (type_rhs, integer_minus_one_node)),
	       PLUS_EXPR, rhs0);
	}
      
      else
	/* Otherwise, match an assignment under the form: 
	   "a = ... - ...".  */
	/* And there is nothing to do.  */
	res = false;
      
      break;
    
    case MULT_EXPR:
      /* This case is under the form "opnd0 = rhs0 * rhs1".  */
      rhs0 = TREE_OPERAND (rhs, 0);
      rhs1 = TREE_OPERAND (rhs, 1);
      if (TREE_CODE (rhs0) == SSA_NAME)
	{
	  if (TREE_CODE (rhs1) == SSA_NAME)
	    {
	      /* Match an assignment under the form: 
		 "a = b * c".  */
	      res = follow_ssa_edge 
		(loop, SSA_NAME_DEF_STMT (rhs0), halting_phi, 
		 evolution_of_loop);
	      
	      if (res)
		*evolution_of_loop = multiply_evolution 
		  (loop->num, *evolution_of_loop, rhs1);
	      
	      else
		{
		  res = follow_ssa_edge 
		    (loop, SSA_NAME_DEF_STMT (rhs1), halting_phi, 
		     evolution_of_loop);
		  
		  if (res)
		    *evolution_of_loop = multiply_evolution 
		      (loop->num, *evolution_of_loop, rhs0);
		}
	    }
	  
	  else
	    {
	      /* Match an assignment under the form: 
		 "a = b * ...".  */
	      res = follow_ssa_edge 
		(loop, SSA_NAME_DEF_STMT (rhs0), halting_phi, 
		 evolution_of_loop);
	      if (res)
		*evolution_of_loop = multiply_evolution 
		  (loop->num, *evolution_of_loop, rhs1);
	    }
	}
      
      else if (TREE_CODE (rhs1) == SSA_NAME)
	{
	  /* Match an assignment under the form: 
	     "a = ... * c".  */
	  res = follow_ssa_edge 
	    (loop, SSA_NAME_DEF_STMT (rhs1), halting_phi, 
	     evolution_of_loop);
	  if (res)
	    *evolution_of_loop = multiply_evolution 
	      (loop->num, *evolution_of_loop, rhs0);
	}
      
      else
	/* Otherwise, match an assignment under the form: 
	   "a = ... * ...".  */
	/* And there is nothing to do.  */
	res = false;
      
      break;

    default:
      res = false;
      break;
    }
  
  return res;
}

/* Checks whether the I-th argument of a PHI comes from a backedge.  */

static bool
backedge_phi_arg_p (tree phi, int i)
{
  edge e = PHI_ARG_EDGE (phi, i);

  /* We would in fact like to test EDGE_DFS_BACK here, but we do not care
     about updating it anywhere, and this should work as well most of the
     time.  */
  if (e->flags & EDGE_IRREDUCIBLE_LOOP)
    return true;

  return false;
}

/* Helper function for one branch of the condition-phi-node.  */

static inline bool
follow_ssa_edge_in_condition_phi_branch (int i,
					 struct loop *loop, 
					 tree condition_phi, 
					 tree halting_phi,
					 tree *evolution_of_branch,
					 tree init_cond)
{
  tree branch = PHI_ARG_DEF (condition_phi, i);
  *evolution_of_branch = chrec_top;

  /* Do not follow back edges (they must belong to an irreducible loop, which
     we really do not want to worry about).  */
  if (backedge_phi_arg_p (condition_phi, i))
    return false;

  if (TREE_CODE (branch) == SSA_NAME)
    {
      *evolution_of_branch = init_cond;
      return follow_ssa_edge (loop, SSA_NAME_DEF_STMT (branch), halting_phi, 
			      evolution_of_branch);
    }

  /* This case occurs when one of the condition branches sets 
     the variable to a constant: ie. a phi-node like
     "a_2 = PHI <a_7(5), 2(6)>;".  
     The testsuite/.../ssa-chrec-17.c exercises this code.  
	 
     FIXME:  This case have to be refined correctly: 
     in some cases it is possible to say something better than
     chrec_top, for example using a wrap-around notation.  */
  return false;
}

/* This function merges the branches of a condition-phi-node in a
   loop.  */

static bool
follow_ssa_edge_in_condition_phi (struct loop *loop,
				  tree condition_phi, 
				  tree halting_phi, 
				  tree *evolution_of_loop)
{
  int i;
  tree init = *evolution_of_loop;
  tree evolution_of_branch;

  if (!follow_ssa_edge_in_condition_phi_branch (0, loop, condition_phi,
						halting_phi,
						&evolution_of_branch,
						init))
    return false;
  *evolution_of_loop = evolution_of_branch;

  for (i = 1; i < PHI_NUM_ARGS (condition_phi); i++)
    {
      if (!follow_ssa_edge_in_condition_phi_branch (i, loop, condition_phi,
						    halting_phi,
						    &evolution_of_branch,
						    init))
	return false;

      *evolution_of_loop = chrec_merge (*evolution_of_loop,
					evolution_of_branch);
    }
  
  return true;
}

/* Follow an SSA edge in an inner loop.  It computes the overall
   effect of the loop, and following the symbolic initial conditions,
   it follows the edges in the parent loop.  The inner loop is
   considered as a single statement.  */

static bool
follow_ssa_edge_inner_loop_phi (struct loop *outer_loop,
				tree loop_phi_node, 
				tree halting_phi,
				tree *evolution_of_loop)
{
  struct loop *loop = loop_of_stmt (loop_phi_node);
  tree ev = compute_overall_effect_of_inner_loop (loop,
						  PHI_RESULT (loop_phi_node));

  return follow_ssa_edge_in_rhs (outer_loop, ev, halting_phi,
				 evolution_of_loop);
}

/* Follow an SSA edge from a loop-phi-node to itself, constructing a
   path that is analyzed on the return walk.  */

static bool
follow_ssa_edge (struct loop *loop, 
		 tree def, 
		 tree halting_phi,
		 tree *evolution_of_loop)
{
  struct loop *def_loop;
  
  if (TREE_CODE (def) == NOP_EXPR)
    return false;
  
  def_loop = loop_of_stmt (def);
  
  switch (TREE_CODE (def))
    {
    case PHI_NODE:
      if (!loop_phi_node_p (def))
	{
	  /* DEF is a condition-phi-node.  Follow the branches, and
	     record their evolutions.  Finally, merge the collected
	     information and set the approximation to the main
	     variable.  */
    	  return follow_ssa_edge_in_condition_phi 
		  (loop, def, halting_phi, evolution_of_loop);
	}

      /* When the analyzed phi is the halting_phi, the
	 depth-first search is over: we have found a path from
	 the halting_phi to itself in the loop.  */
      if (def == halting_phi)
	return true;
	  
      /* Otherwise, the evolution of the HALTING_PHI depends
	 on the evolution of another loop-phi-node, ie. the
	 evolution function is a higher degree polynomial.  */
      if (def_loop == loop)
	return false;
	  
      /* Inner loop.  */
      if (flow_loop_nested_p (loop, def_loop))
	return follow_ssa_edge_inner_loop_phi
		(loop, def, halting_phi, evolution_of_loop);
	  
      /* Outer loop.  */
      return false;

    case MODIFY_EXPR:
      return follow_ssa_edge_in_rhs (loop,
				     TREE_OPERAND (def, 1), 
				     halting_phi, 
				     evolution_of_loop);
      
    default:
      /* At this level of abstraction, the program is just a set
	 of MODIFY_EXPRs and PHI_NODEs.  In principle there is no
	 other node to be handled.  */
      return false;
    }
}

/* Given a LOOP_PHI_NODE, this function determines the evolution
   function from LOOP_PHI_NODE to LOOP_PHI_NODE in the loop.  */

static tree
analyze_evolution_in_loop (tree loop_phi_node, 
			   tree init_cond)
{
  int i;
  tree evolution_function = chrec_not_analyzed_yet;
  struct loop *loop = loop_of_stmt (loop_phi_node);
  basic_block bb;
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "(analyze_evolution_in_loop \n");
      fprintf (dump_file, "  (loop_phi_node = ");
      print_generic_expr (dump_file, loop_phi_node, 0);
      fprintf (dump_file, ")\n");
    }
  
  for (i = 0; i < PHI_NUM_ARGS (loop_phi_node); i++)
    {
      tree arg = PHI_ARG_DEF (loop_phi_node, i);
      tree ssa_chain, ev_fn;
      bool res;

      /* Select the edges that enter the loop body.  */
      bb = PHI_ARG_EDGE (loop_phi_node, i)->src;
      if (!flow_bb_inside_loop_p (loop, bb))
	continue;
      
      if (TREE_CODE (arg) == SSA_NAME)
	{
	  ssa_chain = SSA_NAME_DEF_STMT (arg);

	  /* Pass in the initial condition to the follow edge function.  */
	  ev_fn = init_cond;
	  res = follow_ssa_edge (loop, ssa_chain, loop_phi_node, &ev_fn);
	}
      else
	res = false;
	      
      /* When it is impossible to go back on the same
	 loop_phi_node by following the ssa edges, the
	 evolution is represented by a peeled chrec, ie. the
	 first iteration, EV_FN has the value INIT_COND, then
	 all the other iterations it has the value of ARG.  */
      if (!res)
	{
	  /* FIXME: when dealing with periodic scalars, the
	     analysis of the scalar evolution of ARG would
	     create an infinite recurrence.  Solution: don't
	     try to simplify the peeled chrec at this time,
	     but wait until having more information.   */
	  ev_fn = build_peeled_chrec (loop->num, init_cond, arg);
		  
	  /* Try to simplify the peeled chrec.  */
	  ev_fn = simplify_peeled_chrec (ev_fn);
	}
	      
      /* When there are multiple back edges of the loop (which in fact never
	 happens currently, but nevertheless), merge their evolutions. */
      evolution_function = chrec_merge (evolution_function, ev_fn);
    }
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "  (evolution_function = ");
      print_generic_expr (dump_file, evolution_function, 0);
      fprintf (dump_file, "))\n");
    }
  
  return evolution_function;
}

/* Given a loop-phi-node, this function determines the initial
   conditions of the variable on entry of the loop.  When the CCP has
   propagated constants into the loop-phi-node, the initial condition
   is instantiated, otherwise the initial condition is kept symbolic.
   This analyzer does not analyze the evolution outside the current
   loop, and leaves this task to the on-demand tree reconstructor.  */

static tree 
analyze_initial_condition (tree loop_phi_node)
{
  int i;
  tree init_cond = chrec_not_analyzed_yet;
  struct loop *loop = bb_for_stmt (loop_phi_node)->loop_father;
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "(analyze_initial_condition \n");
      fprintf (dump_file, "  (loop_phi_node = \n");
      print_generic_expr (dump_file, loop_phi_node, 0);
      fprintf (dump_file, ")\n");
    }
  
  for (i = 0; i < PHI_NUM_ARGS (loop_phi_node); i++)
    {
      tree branch = PHI_ARG_DEF (loop_phi_node, i);
      basic_block bb = PHI_ARG_EDGE (loop_phi_node, i)->src;
      
      /* When the branch is oriented to the loop's body, it does
     	 not contribute to the initial condition.  */
      if (flow_bb_inside_loop_p (loop, bb))
       	continue;

      if (init_cond == chrec_not_analyzed_yet)
	{
	  init_cond = branch;
	  continue;
	}

      if (TREE_CODE (branch) == SSA_NAME)
	{
	  init_cond = chrec_top;
      	  break;
	}

      init_cond = chrec_merge (init_cond, branch);
    }

  /* Ooops -- a loop without an entry???  */
  if (init_cond == chrec_not_analyzed_yet)
    init_cond = chrec_top;

  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "  (init_cond = ");
      print_generic_expr (dump_file, init_cond, 0);
      fprintf (dump_file, "))\n");
    }
  
  return init_cond;
}

/* Analyze the scalar evolution for the loop-phi-node DEF.  */

static tree 
interpret_loop_phi (struct loop *loop, tree loop_phi)
{
  tree res = get_scalar_evolution (loop, PHI_RESULT (loop_phi));
  struct loop *phi_loop = loop_of_stmt (loop_phi);
  tree init_cond;
  
  if (res != chrec_not_analyzed_yet)
    return res;

  if (phi_loop != loop)
    {
      struct loop *subloop;

      /* Dive one level deeper.  */
      subloop = superloop_at_depth (phi_loop, loop->depth + 1);

      /* And interpret the subloop.  */
      res = compute_overall_effect_of_inner_loop (subloop,
						  PHI_RESULT (loop_phi));
      return res;
    }

  /* Otherwise really interpret the loop phi.  */
  init_cond = analyze_initial_condition (loop_phi);
  res = analyze_evolution_in_loop (loop_phi, init_cond);
  set_scalar_evolution (loop, PHI_RESULT (loop_phi), res);

  return res;
}

/* This function merges the branches of a condition-phi-node,
   contained in the outermost loop, and whose arguments are already
   analyzed.  */

static tree
interpret_condition_phi (struct loop *loop, tree condition_phi)
{
  int i;
  tree res = chrec_not_analyzed_yet;
  
  for (i = 0; i < PHI_NUM_ARGS (condition_phi); i++)
    {
      tree branch_chrec;
      
      if (backedge_phi_arg_p (condition_phi, i))
	{
	  res = chrec_top;
	  break;
	}

      branch_chrec = analyze_scalar_evolution
	      (loop, PHI_ARG_DEF (condition_phi, i));
      
      res = chrec_merge (res, branch_chrec);
    }

  set_scalar_evolution (loop, PHI_RESULT (condition_phi), res);
  return res;
}

/* Interpret the right hand side of a modify_expr OPND1.  If we didn't
   analyzed this node before, follow the definitions until ending
   either on an analyzed modify_expr, or on a loop-phi-node.  On the
   return path, this function propagates evolutions (ala constant copy
   propagation).  OPND1 is not a GIMPLE expression because we could
   analyze the effect of an inner loop: see interpret_loop_phi.  */

static tree
interpret_rhs_modify_expr (struct loop *loop,
			   tree opnd1, tree type)
{
  tree res, opnd10, opnd11, chrec10, chrec11;
  
  if (is_gimple_min_invariant (opnd1))
    return chrec_convert (type, opnd1);
  
  switch (TREE_CODE (opnd1))
    {
    case PLUS_EXPR:
      opnd10 = TREE_OPERAND (opnd1, 0);
      opnd11 = TREE_OPERAND (opnd1, 1);
      chrec10 = analyze_scalar_evolution (loop, opnd10);
      chrec11 = analyze_scalar_evolution (loop, opnd11);
      chrec10 = chrec_convert (type, chrec10);
      chrec11 = chrec_convert (type, chrec11);
      res = chrec_fold_plus (type, chrec10, chrec11);
      break;
      
    case MINUS_EXPR:
      opnd10 = TREE_OPERAND (opnd1, 0);
      opnd11 = TREE_OPERAND (opnd1, 1);
      chrec10 = analyze_scalar_evolution (loop, opnd10);
      chrec11 = analyze_scalar_evolution (loop, opnd11);
      chrec10 = chrec_convert (type, chrec10);
      chrec11 = chrec_convert (type, chrec11);
      res = chrec_fold_minus (type, chrec10, chrec11);
      break;

    case NEGATE_EXPR:
      opnd10 = TREE_OPERAND (opnd1, 0);
      chrec10 = analyze_scalar_evolution (loop, opnd10);
      chrec10 = chrec_convert (type, chrec10);
      res = chrec_fold_negate (type, chrec10);
      break;

    case MULT_EXPR:
      opnd10 = TREE_OPERAND (opnd1, 0);
      opnd11 = TREE_OPERAND (opnd1, 1);
      chrec10 = analyze_scalar_evolution (loop, opnd10);
      chrec11 = analyze_scalar_evolution (loop, opnd11);
      chrec10 = chrec_convert (type, chrec10);
      chrec11 = chrec_convert (type, chrec11);
      res = chrec_fold_multiply (type, chrec10, chrec11);
      break;
      
    case SSA_NAME:
      res = chrec_convert (type, analyze_scalar_evolution (loop, opnd1));
      break;
      
    case NOP_EXPR:
    case CONVERT_EXPR:
      opnd10 = TREE_OPERAND (opnd1, 0);
      chrec10 = analyze_scalar_evolution (loop, opnd10);
      res = chrec_convert (type, chrec10);
      break;
      
    default:
      res = chrec_top;
      break;
    }
  
  return res;
}



/* This section contains all the entry points: 
   - number_of_iterations_in_loop,
   - analyze_scalar_evolution,
   - instantiate_parameters.
*/

/* Entry point for the scalar evolution analyzer.
   Analyzes and returns the scalar evolution of the ssa_name VERSION.
   LOOP_NB is the identifier number of the loop in which the version
   is used.  
   
   Example of use: having a pointer VERSION to a SSA_NAME node, STMT a
   pointer to the statement that uses this version, in order to
   determine the evolution function of the version, use the following
   calls:
   
   unsigned loop_nb = loop_num (loop_of_stmt (stmt));
   tree chrec_with_symbols = analyze_scalar_evolution (loop_nb, version);
   tree chrec_instantiated = instantiate_parameters 
   (loop_nb, chrec_with_symbols);
*/

tree 
analyze_scalar_evolution (struct loop *loop, tree version)
{
  tree res, def, type = TREE_TYPE (version);
  basic_block bb;
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    {
      fprintf (dump_file, "(analyze_scalar_evolution \n");
      fprintf (dump_file, "  (loop_nb = %d)\n", loop->num);
      fprintf (dump_file, "  (scalar = ");
      print_generic_expr (dump_file, version, 0);
      fprintf (dump_file, ")\n");
    }

  res = get_scalar_evolution (loop, version);

  if (TREE_CODE (version) != SSA_NAME)
    {
      if (res != chrec_top)
	{
	  /* Keep the symbolic form.  */
	  goto end;
	}
	  
      /* Try analyzing the expression.  */
      res = interpret_rhs_modify_expr (loop, version, type);
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "  (res = ");
	  print_generic_expr (dump_file, res, 0);
	  fprintf (dump_file, ")\n");
	}

      goto end;
    }
      
  if (res != chrec_not_analyzed_yet)
    goto end;

  def = SSA_NAME_DEF_STMT (version);
  bb = bb_for_stmt (def);
  if (!bb
      || !flow_bb_inside_loop_p (loop, bb))
    {
      res = version;
      goto end;
    }

  switch (TREE_CODE (def))
    {
    case MODIFY_EXPR:
      res = interpret_rhs_modify_expr (loop, TREE_OPERAND (def, 1), type);
      break;
	      
    case PHI_NODE:
      if (loop_phi_node_p (def))
	res = interpret_loop_phi (loop, def);
      else
	res = interpret_condition_phi (loop, def);
      break;
	      
    default:
      res = chrec_top;
      break;
    }

end:
  set_scalar_evolution (loop, version, res);
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, ")\n");
  
  return res;
}

/* Analyze all the parameters of the chrec that were left under a
   symbolic form.  LOOP is the loop in which symbolic names have to
   be analyzed and instantiated.  */

tree
instantiate_parameters (struct loop *loop,
			tree chrec)
{
  tree res, op0, op1, op2;
  
  if (chrec == NULL_TREE
      || automatically_generated_chrec_p (chrec))
    res = chrec;
  
  else if (TREE_CODE (chrec) == SSA_NAME
	   || TREE_CODE (chrec) == VAR_DECL
	   || TREE_CODE (chrec) == PARM_DECL)
    {
      if (tree_is_in_varray_tree_p (chrec, already_instantiated))
	/* Don't instantiate the SSA_NAME if it is in a mixer
	   structure.  This is used for avoiding the instantiation of
	   recursively defined functions, such as: 

	   | a_2 -> {0, +, 1, +, a_2}_1
	   
	   Note: the size of already_instantiated is proportional to
	   the degree of the evolution function.  This is the number
	   of parameters that have to be instantiated, and is almost
	   all the time less than 2.  */
	res = chrec;
      
      else
	{
	  res = analyze_scalar_evolution (loop, chrec);
	  
	  /* If the analysis yields a parametric chrec, instantiate
	     the result again.  Enqueue the SSA_NAME such that it will
	     never be instantiated twice, avoiding the cyclic
	     instantiation in mixers.  */
	  if (chrec_contains_symbols (res))
	    {
	      VARRAY_PUSH_TREE (already_instantiated, chrec);
	      res = instantiate_parameters (loop, res);
	      VARRAY_POP (already_instantiated);
	    }
	}
    }
  else
    switch (TREE_CODE (chrec))
      {
      case POLYNOMIAL_CHREC:
	op0 = instantiate_parameters (loop, CHREC_LEFT (chrec));
	op1 = instantiate_parameters (loop, CHREC_RIGHT (chrec));
	res = build_polynomial_chrec (CHREC_VARIABLE (chrec), op0, op1);
	break;
	
      case EXPONENTIAL_CHREC:
	op0 = instantiate_parameters (loop, CHREC_LEFT (chrec));
	op1 = instantiate_parameters (loop, CHREC_RIGHT (chrec));
	res = build_exponential_chrec (CHREC_VARIABLE (chrec), op0, op1);
	break;
	
      case PEELED_CHREC:
	op0 = instantiate_parameters (loop, CHREC_LEFT (chrec));
	op1 = instantiate_parameters (loop, CHREC_RIGHT (chrec));
	res = build_peeled_chrec (CHREC_VARIABLE (chrec), op0, op1);
	break;
	
      case INTERVAL_CHREC:
	op0 = instantiate_parameters (loop, CHREC_LOW (chrec));
	op1 = instantiate_parameters (loop, CHREC_UP (chrec));
	res = build_interval_chrec (op0, op1);
	break;
	
      case PLUS_EXPR:
	op0 = instantiate_parameters (loop, TREE_OPERAND (chrec, 0));
	op1 = instantiate_parameters (loop, TREE_OPERAND (chrec, 1));
	res = chrec_fold_plus (TREE_TYPE (chrec), op0, op1);
	break;
	
      case MINUS_EXPR:
	op0 = instantiate_parameters (loop, TREE_OPERAND (chrec, 0));
	op1 = instantiate_parameters (loop, TREE_OPERAND (chrec, 1));
	res = chrec_fold_minus (TREE_TYPE (chrec), op0, op1);
	break;
	
      case MULT_EXPR:
	op0 = instantiate_parameters (loop, TREE_OPERAND (chrec, 0));
	op1 = instantiate_parameters (loop, TREE_OPERAND (chrec, 1));
	res = chrec_fold_multiply (TREE_TYPE (chrec), op0, op1);
	break;
	
      case ABS_EXPR:
	/* In general these nodes come from the symbolic computation
	   of the number of iterations.  These nodes are too difficult
	   to instantiate for the moment.  */
	res = chrec;
	break;

      case NOP_EXPR:
	/* res = build1 (NOP_EXPR, TREE_TYPE (chrec), 
	   instantiate_parameters (loop_nb, 
	   TREE_OPERAND (chrec, 0)));
	*/
	res = instantiate_parameters (loop, TREE_OPERAND (chrec, 0));
	break;
	
      default:
	switch (TREE_CODE_LENGTH (TREE_CODE (chrec)))
	  {
	  case 3:
	    op0 = instantiate_parameters 
	      (loop, TREE_OPERAND (chrec, 0));
	    op1 = instantiate_parameters 
	      (loop, TREE_OPERAND (chrec, 1));
	    op2 = instantiate_parameters 
	      (loop, TREE_OPERAND (chrec, 2));
	    res = build (TREE_CODE (chrec), TREE_TYPE (chrec), op0, op1, op2);
	    break;

	  case 2:
	    op0 = instantiate_parameters 
	      (loop, TREE_OPERAND (chrec, 0));
	    op1 = instantiate_parameters 
	      (loop, TREE_OPERAND (chrec, 1));
	    res = build (TREE_CODE (chrec), TREE_TYPE (chrec), op0, op1);
	    break;
	    
	  case 1:
	    res = instantiate_parameters 
	      (loop, TREE_OPERAND (chrec, 0));
	    if (!automatically_generated_chrec_p (res))
	      res = build1 (TREE_CODE (chrec), TREE_TYPE (chrec), res);
	    break;
	    
	  default:
	    res = chrec;
	    break;
	  }
	break;
      }
  
  return res;
}

/* Entry point for the analysis of the number of iterations pass.  
   This function tries to safely approximate the number of iterations
   the loop will run.  When this property is not decidable at compile
   time, the result is chrec_top: [-oo, +oo].  Otherwise the result is
   a scalar, an interval, or a symbolic parameter.
   
   Example of analysis: suppose that the loop has an exit condition:
   
   "if (b > 49) goto end_loop;"
   
   and that in a previous analysis we have determined that the
   variable 'b' has an evolution function:
   
   "EF = {23, +, 5}_2".  
   
   When we evaluate the function at the point 5, i.e. the value of the
   variable 'b' after 5 iterations in the loop, we have EF (5) = 48,
   and EF (6) = 53.  In this case the value of 'b' on exit is '53' and
   the loop body has been executed 6 times.  */

tree 
number_of_iterations_in_loop (struct loop *loop)
{
  tree res;
  tree cond, test, opnd0, opnd1;
  tree chrec0, chrec1;
  edge exit;
  
  /* Determine whether the number_of_iterations_in_loop has already
     been computed.  */
  res = loop_nb_iterations (loop);
  if (res)
    return res;
  
  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "(number_of_iterations_in_loop \n");
  
  cond = get_loop_exit_condition (loop);
  if (cond == NULL_TREE)
    return set_nb_iterations_in_loop (loop, chrec_top);
  
  test = TREE_OPERAND (cond, 0);
  exit = loop_exit_edge (loop, 0);
  if (exit->flags & EDGE_TRUE_VALUE)
    test = invert_truthvalue (test);

  switch (TREE_CODE (test))
    {
    case SSA_NAME:
      /* "while (opnd0 != 0)".  */
      chrec0 = analyze_scalar_evolution (loop, test);
      chrec1 = integer_zero_node;
      
      if (chrec0 == chrec_top)
	/* KEEP_IT_SYMBOLIC.  */
	chrec0 = test;
      
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "  (loop_nb = %d)\n", loop->num);
	  fprintf (dump_file, "  (loop_while_expr_is_true: ");
	  print_generic_expr (dump_file, test, 0);
	  fprintf (dump_file, ")\n  (chrec0 = ");
	  print_generic_expr (dump_file, chrec0, 0);
	  fprintf (dump_file, ")\n");
	}
      
      if (chrec_contains_undetermined (chrec0))
	return cannot_analyze_loop_nb_iterations_yet ();
      
      else
	return set_nb_iterations_in_loop 
	  (loop, first_iteration_non_satisfying (NE_EXPR, loop->num, 
						 chrec0, chrec1));

    case LT_EXPR:
    case LE_EXPR:
    case GT_EXPR:
    case GE_EXPR:
    case EQ_EXPR:
    case NE_EXPR:
      opnd0 = TREE_OPERAND (test, 0);
      opnd1 = TREE_OPERAND (test, 1);
      chrec0 = analyze_scalar_evolution (loop, opnd0);
      chrec1 = analyze_scalar_evolution (loop, opnd1);
      
      chrec0 = instantiate_parameters (loop, chrec0);
      chrec1 = instantiate_parameters (loop, chrec1);
      
      if (chrec0 == chrec_top)
	/* KEEP_IT_SYMBOLIC.  */
	chrec0 = opnd0;
      
      if (chrec1 == chrec_top)
	/* KEEP_IT_SYMBOLIC.  */
	chrec1 = opnd1;
      
      if (dump_file && (dump_flags & TDF_DETAILS))
	{
	  fprintf (dump_file, "  (loop_nb = %d)\n", loop->num);
	  fprintf (dump_file, "  (loop_while_expr_is_true: ");
	  print_generic_expr (dump_file, test, 0);
	  fprintf (dump_file, ")\n  (chrec0 = ");
	  print_generic_expr (dump_file, chrec0, 0);
	  fprintf (dump_file, ")\n  (chrec1 = ");
	  print_generic_expr (dump_file, chrec1, 0);
	  fprintf (dump_file, ")\n");
	}
      
      if (chrec_contains_undetermined (chrec0)
	  || chrec_contains_undetermined (chrec1))
	return cannot_analyze_loop_nb_iterations_yet ();
      
      return set_nb_iterations_in_loop 
	(loop, first_iteration_non_satisfying (TREE_CODE (test), loop->num, 
					       chrec0, chrec1));
    default:
      return set_nb_iterations_in_loop (loop, chrec_top);
    }
}

/* One of the drivers for testing the scalar evolutions analysis.
   This function computes the number of iterations for all the loops
   from the EXIT_CONDITIONS array.  */

static void 
number_of_iterations_for_all_loops (varray_type exit_conditions)
{
  unsigned int i;
  
  for (i = 0; i < VARRAY_ACTIVE_SIZE (exit_conditions); i++)
    number_of_iterations_in_loop 
      (loop_of_stmt (VARRAY_TREE (exit_conditions, i)));
  
  if (dump_file)
    print_loop_ir (dump_file);
}



/* Reset the counters.  */

static inline void
reset_chrecs_counters (void)
{
  stats_nb_chrecs = 0;
  stats_nb_peeled_affine = 0;
  stats_nb_affine = 0;
  stats_nb_affine_multivar = 0;
  stats_nb_higher_poly = 0;
  stats_nb_expo = 0;
  stats_nb_chrec_top = 0;
  stats_nb_interval_chrec = 0;
  stats_nb_undetermined = 0;
}

/* Gather statistics about CHREC.  */

static inline void
gather_chrec_stats (FILE *file, tree chrec)
{
  stats_nb_chrecs++;
  fprintf (file, "(classify_chrec ");
  print_generic_expr (file, chrec, 0);
  fprintf (file, "\n");

  if (chrec == NULL_TREE)
    return;
  
  switch (TREE_CODE (chrec))
    {
    case POLYNOMIAL_CHREC:
      if (evolution_function_is_affine_p (chrec))
	{
	  fprintf (file, "  affine_univariate\n");
	  stats_nb_affine++;
	}
      
      else if (evolution_function_is_affine_multivariate_p (chrec))
	{
	  fprintf (file, "  affine_multivariate\n");
	  stats_nb_affine_multivar++;
	}
      
      else if (evolution_function_is_peeled_affine_p (chrec))
	{
	  fprintf (file, "  peeled_affine\n");
	  stats_nb_peeled_affine++;
	}
      
      else
	{
	  fprintf (file, "  higher_degree_polynomial\n");
	  stats_nb_higher_poly++;
	}
      
      break;
      
    case EXPONENTIAL_CHREC:
      stats_nb_expo++;
      fprintf (file, "  exponential\n");
      break;

    case INTERVAL_CHREC:
      if (chrec == chrec_top)
	{
	  stats_nb_chrec_top++;
	  fprintf (file, "  chrec_top\n");
	}
      else
	{
	  stats_nb_interval_chrec++;
	  fprintf (file, "  interval chrec\n");
	}
      break;
      
    default:
      break;
    }
  
  if (chrec_contains_undetermined (chrec))
    {
      fprintf (file, "  undetermined\n");
      stats_nb_undetermined++;
    }
  
  fprintf (file, ")\n");
}

/* Dump the stats about the chrecs.  */

static inline void
dump_chrecs_stats (FILE *file)
{
  fprintf (file, "\n(\n");
  fprintf (file, "-----------------------------------------\n");
  fprintf (file, "%d\taffine univariate chrecs\n", stats_nb_affine);
  fprintf (file, "%d\taffine multivariate chrecs\n", 
	   stats_nb_affine_multivar);
  fprintf (file, "%d\tdegree greater than 2 polynomials\n", 
	   stats_nb_higher_poly);
  fprintf (file, "%d\taffine peeled chrecs\n", stats_nb_peeled_affine);
  fprintf (file, "%d\texponential chrecs\n", stats_nb_expo);
  fprintf (file, "%d\tchrec_top chrecs\n", stats_nb_chrec_top);
  fprintf (file, "%d\tinterval chrecs\n", stats_nb_chrec_top);
  fprintf (file, "-----------------------------------------\n");
  fprintf (file, "%d\ttotal chrecs\n", stats_nb_chrecs);
  fprintf (file, "%d\twith undetermined coefficients\n", 
	   stats_nb_undetermined);
  fprintf (file, "-----------------------------------------\n");
  fprintf (file, "%d\tchrecs in the scev database\n", 
	   (int) VARRAY_ACTIVE_SIZE (scalar_evolution_info));
  fprintf (file, "-----------------------------------------\n");
  fprintf (file, ")\n\n");
}

/* One of the drivers for testing the scalar evolutions analysis.
   This function analyzes the scalar evolution of all the scalars
   defined as loop phi nodes in one of the loops from the
   EXIT_CONDITIONS array.  
   
   TODO Optimization: A loop is in canonical form if it contains only
   a single scalar loop phi node.  All the other scalars that have an
   evolution in the loop are rewritten in function of this single
   index.  This allows the parallelization of the loop.  */

static void 
analyze_scalar_evolution_for_all_loop_phi_nodes (varray_type exit_conditions)
{
  unsigned int i;

  reset_chrecs_counters ();
  
  for (i = 0; i < VARRAY_ACTIVE_SIZE (exit_conditions); i++)
    {
      struct loop *loop;
      basic_block bb;
      tree phi, chrec;
      
      loop = loop_of_stmt (VARRAY_TREE (exit_conditions, i));
      bb = loop_header (loop);
      
      for (phi = phi_nodes (bb); phi; phi = TREE_CHAIN (phi))
	if (is_gimple_reg (PHI_RESULT (phi)))
	  {
	    chrec = instantiate_parameters 
	      (loop, 
	       analyze_scalar_evolution (loop, PHI_RESULT (phi)));
	    
	    if (dump_file && (dump_flags & TDF_STATS))
	      gather_chrec_stats (dump_file, chrec);
	  }
    }
  
  if (dump_file && (dump_flags & TDF_STATS))
    dump_chrecs_stats (dump_file);
}

/* Classify the chrecs of the whole database.  */

void 
gather_stats_on_scev_database (void)
{
  unsigned i;
  
  if (!dump_file)
    return;
  
  reset_chrecs_counters ();  
  
  for (i = 0; i < VARRAY_ACTIVE_SIZE (scalar_evolution_info); i++)
    {
      struct scev_info_str *elt = 
	VARRAY_GENERIC_PTR (scalar_evolution_info, i);
      gather_chrec_stats (dump_file, elt->chrec);
    }
  
  dump_chrecs_stats (dump_file);
}



static void initialize_scalar_evolutions_analyzer (void);
static void scev_init (void);
static void scev_analysis (void);
static void scev_depend (void);
static void scev_elim_checks (void);
static void scev_vectorize (void);
static void scev_done (void);
static bool gate_scev (void);
static bool gate_scev_analysis (void);
static bool gate_scev_depend (void);
static bool gate_scev_elim_checks (void);
static bool gate_scev_vectorize (void);

/* Initializer.  */

static void
initialize_scalar_evolutions_analyzer (void)
{
  /* The elements below are unique.  The values contained in these
     intervals are not used.  */
  chrec_not_analyzed_yet = NULL_TREE;
  chrec_top = build_interval_chrec 
    (build_int_2 (2222, 0), build_int_2 (3222, 0));
  chrec_bot = build_interval_chrec 
    (build_int_2 (3333, 0), build_int_2 (4333, 0));
}

/* Initialize the analysis of scalar evolutions for LOOPS.  */

void
scev_initialize (struct loops *loops)
{
  unsigned i;
  current_loops = loops;

  VARRAY_GENERIC_PTR_INIT (scalar_evolution_info, 100, 
			   "scalar_evolution_info");
  VARRAY_TREE_INIT (already_instantiated, 3, 
		    "already_instantiated");
  
  initialize_scalar_evolutions_analyzer ();

  for (i = 1; i < loops->num; i++)
    if (loops->parray[i])
      flow_loop_scan (loops->parray[i], LOOP_EXIT_EDGES);
}

/* Initialize the analysis of scalar evolutions.  */

static void
scev_init (void)
{
  current_loops = tree_loop_optimizer_init (NULL, flag_tree_loop != 0);
  if (!current_loops)
    return;
  scev_initialize (current_loops);
}

/* Runs the analysis of scalar evolutions.  */

static void
scev_analysis (void)
{
  varray_type exit_conditions;
  
  VARRAY_GENERIC_PTR_INIT (exit_conditions, 37, "exit_conditions");
  select_loops_exit_conditions (current_loops, &exit_conditions);

#if 0
  dump_file = stderr;
  dump_flags = 31;
#endif
  
  if (dump_file && (dump_flags & TDF_STATS))
    analyze_scalar_evolution_for_all_loop_phi_nodes (exit_conditions);
  
  number_of_iterations_for_all_loops (exit_conditions);
  VARRAY_CLEAR (exit_conditions);
}

/* Runs the analysis of all the data dependences.  */

static void
scev_depend (void)
{
  analyze_all_data_dependences (current_loops);
  dd_info_available = true;
}

static void
scev_elim_checks (void)
{
  eliminate_redundant_checks ();
}

/* Runs the linear loop transformations.  */

static void
scev_linear_transform (void)
{
  linear_transform_loops (current_loops, scalar_evolution_info);
}

/* Runs the canonical iv creation pass.  */

static void
scev_iv_canon (void)
{
  canonicalize_induction_variables (current_loops);
}

/* Runs the vectorization pass.  */

static void
scev_vectorize (void)
{
  bitmap_clear (vars_to_rename);

  vectorize_loops (current_loops, scalar_evolution_info);
}

/* Finalize the scalar evolution analysis.  */

void
scev_finalize (void)
{
  scalar_evolution_info = NULL;
  already_instantiated = NULL;
  current_loops = NULL;
}

/* Finalize the scalar evolution passes.  */

static void
scev_done (void)
{
  if (current_loops)
    {
      loop_optimizer_finalize (current_loops, NULL);
      scev_finalize ();
      cleanup_tree_cfg ();
    }

  dd_info_available = false;
}

static bool
gate_scev (void)
{
  return (flag_scalar_evolutions != 0
	  || flag_tree_vectorize != 0
	  || flag_all_data_deps != 0
	  || flag_tree_elim_checks != 0
	  || flag_tree_loop_linear != 0);
}

static bool
gate_scev_analysis (void)
{
  return current_loops && flag_scalar_evolutions != 0;
}

static bool
gate_scev_depend (void)
{
  return current_loops && flag_all_data_deps != 0;
}

static bool 
gate_scev_elim_checks (void)
{
  return current_loops && flag_tree_elim_checks != 0;
}

static bool
gate_scev_linear_transform (void)
{
  return current_loops && flag_tree_loop_linear != 0;
}

static bool
gate_scev_iv_canon (void)
{
  return (current_loops
	  /* Only run this pass if we will be able to eliminate the
	     superfluous ivs we create.   */
	  && flag_tree_loop);
}

static bool
gate_scev_vectorize (void)
{
  return current_loops && flag_tree_vectorize != 0;
}

struct tree_opt_pass pass_scev = 
{
  NULL,                                 /* name */
  gate_scev,				/* gate */
  NULL,					/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  0,					/* tv_id */
  PROP_cfg | PROP_ssa,			/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func			/* todo_flags_finish */
};

struct tree_opt_pass pass_scev_init = 
{
  NULL,					/* name */
  NULL,					/* gate */
  scev_init,				/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  0,					/* tv_id */
  PROP_cfg | PROP_ssa,			/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0					/* todo_flags_finish */
};

struct tree_opt_pass pass_scev_anal = 
{
  "scev",				/* name */
  gate_scev_analysis,			/* gate */
  scev_analysis,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_SCALAR_EVOLUTIONS,			/* tv_id */
  PROP_cfg | PROP_ssa,			/* properties_required */
  0,        				/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0					/* todo_flags_finish */
};

struct tree_opt_pass pass_scev_depend = 
{
  "ddall",				/* name */
  gate_scev_depend,			/* gate */
  scev_depend,				/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_ALL_DATA_DEPS,			/* tv_id */
  PROP_cfg | PROP_ssa,			/* properties_required */
  PROP_scev,				/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0					/* todo_flags_finish */
};

struct tree_opt_pass pass_scev_vectorize = 
{
  "vect",				/* name */
  gate_scev_vectorize,			/* gate */
  scev_vectorize,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_VECTORIZATION,		/* tv_id */
  PROP_cfg | PROP_ssa,			/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func | TODO_rename_vars	/* todo_flags_finish */
};

struct tree_opt_pass pass_scev_linear_transform =
{
  "ltrans",				/* name */
  gate_scev_linear_transform,		/* gate */
  scev_linear_transform,       		/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_LINEAR_TRANSFORM,  		/* tv_id */
  PROP_cfg | PROP_ssa,			/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func                	/* todo_flags_finish */
};

struct tree_opt_pass pass_scev_iv_canon =
{
  "ivcan",				/* name */
  gate_scev_iv_canon,			/* gate */
  scev_iv_canon,	       		/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_LOOP_IVCANON,	  		/* tv_id */
  PROP_cfg | PROP_ssa,			/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func                	/* todo_flags_finish */
};

struct tree_opt_pass pass_scev_elim_checks = 
{
  "elck",				/* name */
  gate_scev_elim_checks,		/* gate */
  scev_elim_checks,			/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_TREE_ELIM_CHECKS,  		/* tv_id */
  PROP_cfg | PROP_ssa,			/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  TODO_dump_func                	/* todo_flags_finish */
};

struct tree_opt_pass pass_scev_done = 
{
  NULL,					/* name */
  NULL,					/* gate */
  scev_done,				/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  0,					/* tv_id */
  PROP_cfg | PROP_ssa,			/* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0					/* todo_flags_finish */
};
