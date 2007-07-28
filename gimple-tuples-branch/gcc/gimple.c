/* Gimple IR support functions.

   Copyright 2007 Free Software Foundation, Inc.
   Contributed by Aldy Hernandez <aldyh@redhat.com>

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

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tree.h"
#include "ggc.h"
#include "errors.h"
#include "tree-gimple.h"
#include "gimple.h"
#include "diagnostic.h"

#define DEFGSCODE(SYM, NAME)	NAME,
const char *const gimple_code_name[] = {
#include "gimple.def"
};
#undef DEFGSCODE

/* Pointer map to store the sequence of GIMPLE statements
   corresponding to each function.  For every FUNCTION_DECL FN, the
   sequence of GIMPLE statements corresponding to FN are stored in
   gimple_body (FN).  */
static struct pointer_map_t *gimple_bodies = NULL;

/* Gimple tuple constructors.
   Note: Any constructor taking a ``gimple_seq'' as a parameter, can
   be passed a NULL to start with an empty sequence.  */

/* Set the code for statement G to CODE.  */

static inline void
set_gimple_code (gimple g, enum gimple_code code)
{
  g->base.code = code;
}


/* Set PREV to be the previous statement to G.  */

static inline void
set_gimple_prev (gimple g, gimple prev)
{
  g->base.prev = prev;
}


/* Set NEXT to be the next statement to G.  */

static inline void
set_gimple_next (gimple g, gimple next)
{
  g->base.next = next;
}


/* Return the GSS_* identifier for the given GIMPLE statement CODE.  */

static enum gimple_statement_structure_enum
gss_for_code (enum gimple_code code)
{
  switch (code)
    {
    case GIMPLE_ASSIGN:
    case GIMPLE_CALL:
    case GIMPLE_RETURN:		return GSS_WITH_MEM_OPS;
    case GIMPLE_COND:
    case GIMPLE_GOTO:
    case GIMPLE_LABEL:
    case GIMPLE_SWITCH:		return GSS_WITH_OPS;
    case GIMPLE_ASM:		return GSS_ASM;
    case GIMPLE_BIND:		return GSS_BIND;
    case GIMPLE_CATCH:		return GSS_CATCH;
    case GIMPLE_EH_FILTER:	return GSS_EH_FILTER;
    case GIMPLE_NOP:		return GSS_BASE;
    case GIMPLE_PHI:		return GSS_PHI;
    case GIMPLE_RESX:		return GSS_RESX;
    case GIMPLE_TRY:		return GSS_TRY;
    case GIMPLE_OMP_CRITICAL:	return GSS_OMP_CRITICAL;
    case GIMPLE_OMP_FOR:	return GSS_OMP_FOR;
    case GIMPLE_OMP_CONTINUE:
    case GIMPLE_OMP_MASTER:		
    case GIMPLE_OMP_ORDERED:
    case GIMPLE_OMP_RETURN:
    case GIMPLE_OMP_SECTION:	return GSS_OMP;
    case GIMPLE_OMP_PARALLEL:	return GSS_OMP_PARALLEL;
    case GIMPLE_OMP_SECTIONS:	return GSS_OMP_SECTIONS;
    case GIMPLE_OMP_SINGLE:	return GSS_OMP_SINGLE;
    default:			gcc_unreachable ();
    }
}


/* Build a tuple with operands.  CODE is the statement to build (which
   must be one of the GIMPLE_WITH_OPS tuples).  SUBCODE is the sub-code
   for the new tuple.  NUM_OPS is the number of operands to allocate.  */ 

static gimple
build_gimple_with_ops (enum gimple_code code, unsigned subcode, size_t num_ops)
{
  gimple s;
  enum gimple_statement_structure_enum gss = gss_for_code (code);
  
  if (gss == GSS_WITH_OPS)
    s = ggc_alloc_cleared (sizeof (struct gimple_statement_with_ops));
  else if (gss == GSS_WITH_MEM_OPS)
    s = ggc_alloc_cleared (sizeof (struct gimple_statement_with_memory_ops));
  else
    gcc_unreachable ();

  set_gimple_code (s, code);
  set_gimple_flags (s, subcode);
  s->with_ops.num_ops = num_ops;
  s->with_ops.op = ggc_alloc_cleared (sizeof (tree) * num_ops);

  return s;
}


/* Construct a GIMPLE_RETURN statement.

   RESULT_DECL_P is non-zero if using RESULT_DECL.
   RETVAL is the return value.  */

gimple
build_gimple_return (bool result_decl_p, tree retval)
{
  gimple s = build_gimple_with_ops (GIMPLE_RETURN, (int) result_decl_p, 1);
  gcc_assert (is_gimple_val (retval));
  gimple_return_set_retval (s, retval);
  return s;
}

/* Helper for build_gimple_call and build_gimple_call_vec.  Build the basic
   components of a GIMPLE_CALL statement to function FN with NARGS
   arguments.  */

static inline gimple
build_gimple_call_1 (tree fn, size_t nargs)
{
  gimple s = build_gimple_with_ops (GIMPLE_CALL, 0, nargs + 3);
  s->with_ops.op[1] = fn;
  return s;
}


/* Build a GIMPLE_CALL statement to function FN with the arguments
   specified in vector ARGS.  */

gimple
build_gimple_call_vec (tree fn, VEC(tree, gc) *args)
{
  size_t i;
  size_t nargs = VEC_length (tree, args);
  gimple call = build_gimple_call_1 (fn, nargs);

  for (i = 0; i < nargs; i++)
    gimple_call_set_arg (call, i, VEC_index (tree, args, i));

  return call;
}


/* Build a GIMPLE_CALL statement to function FN.  NARGS is the number of
   arguments.  The ... are the arguments.  */

gimple
build_gimple_call (tree fn, size_t nargs, ...)
{
  va_list ap;
  gimple call;
  size_t i;

  call = build_gimple_call_1 (fn, nargs);

  va_start (ap, nargs);
  for (i = 0; i < nargs; i++)
    gimple_call_set_arg (call, i, va_arg (ap, tree));
  va_end (ap);

  return call;
}


/* Construct a GIMPLE_ASSIGN statement.

   LHS of the assignment.
   RHS of the assignment which can be unary or binary.  */

gimple
build_gimple_assign (tree lhs, tree rhs)
{
  gimple p;
  size_t num_ops;
  enum tree_code subcode = TREE_CODE (rhs);
  enum gimple_rhs_class class = get_gimple_rhs_class (rhs);

  /* Make sure the RHS is a valid GIMPLE RHS.  */
  gcc_assert (is_gimple_formal_tmp_rhs (rhs));

  /* Need 1 operand for LHS and 1 or 2 for the RHS (depending on the
     code).  */
  if (class == GIMPLE_UNARY_RHS || class == GIMPLE_SINGLE_RHS)
    num_ops = 2;
  else if (class == GIMPLE_BINARY_RHS)
    num_ops = 3;
  else
    gcc_unreachable ();
  
  p = build_gimple_with_ops (GIMPLE_ASSIGN, subcode, num_ops);
  gimple_assign_set_lhs (p, lhs);

  if (class == GIMPLE_BINARY_RHS)
    {
      gimple_assign_set_rhs1 (p, TREE_OPERAND (rhs, 0));
      gimple_assign_set_rhs2 (p, TREE_OPERAND (rhs, 1));
    }
  else if (class == GIMPLE_UNARY_RHS)
    gimple_assign_set_rhs1 (p, TREE_OPERAND (rhs, 0));
  else if (class == GIMPLE_SINGLE_RHS)
    gimple_assign_set_rhs1 (p, rhs);
  else
    gcc_unreachable ();

  return p;
}


/* Construct a GIMPLE_COND statement.

   PRED is the condition used to compare LHS and the RHS.
   T_LABEL is the label to jump to if the condition is true.
   F_LABEL is teh label to jump to otherwise.  */

gimple
build_gimple_cond (enum gimple_cond pred, tree lhs, tree rhs,
		   tree t_label, tree f_label)
{
  gimple p = build_gimple_with_ops (GIMPLE_COND, pred, 4);
  gimple_cond_set_lhs (p, lhs);
  gimple_cond_set_rhs (p, rhs);
  gimple_cond_set_true_label (p, t_label);
  gimple_cond_set_false_label (p, f_label);
  return p;
}

/* Invert the condition of a GIMPLE_COND by swapping its labels.  */

void
gimple_cond_invert (gimple g)
{
  tree tmp;

  tmp = gimple_cond_true_label (g);
  gimple_cond_set_true_label (g, gimple_cond_false_label (g));
  gimple_cond_set_false_label (g, tmp);
}

/* Construct a GIMPLE_LABEL statement for LABEL.  */

gimple
build_gimple_label (tree label)
{
  gimple p = build_gimple_with_ops (GIMPLE_LABEL, 0, 1);
  gimple_label_set_label (p, label);
  return p;
}

/* Construct a GIMPLE_GOTO statement to DEST.  */

gimple
build_gimple_goto (tree dest)
{
  gimple p = build_gimple_with_ops (GIMPLE_GOTO, 0, 1);
  gimple_goto_set_dest (p, dest);
  return p;
}

/* Construct a GIMPLE_NOP statement.  */

gimple 
build_gimple_nop (void)
{
  gimple p = ggc_alloc_cleared (sizeof (struct gimple_statement_base));
  set_gimple_code (p, GIMPLE_NOP);
  return p;
}

/* Construct a GIMPLE_BIND statement.

   VARS are the variables in BODY.  */

gimple
build_gimple_bind (tree vars, gimple_seq body)
{
  gimple p = ggc_alloc_cleared (sizeof (struct gimple_statement_bind));
  set_gimple_code (p, GIMPLE_BIND);
  gimple_bind_set_vars (p, vars);
  if (body)
    gimple_bind_set_body (p, body);
  return p;
}

/* Construct a GIMPLE_ASM statement.

   STRING is the assembly code.
   NINPUT is the number of register inputs.
   NOUTPUT is the number of register outputs.
   NCLOBBERS is the number of clobbered registers.
   ... are trees for each input, output and clobbered register.  */

gimple
build_gimple_asm (const char *string, unsigned ninputs, unsigned noutputs, 
              unsigned nclobbers, ...)
{
  gimple p;
  unsigned i;
  va_list ap;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_asm)
                         + sizeof (tree) * (ninputs + noutputs + nclobbers - 1));
  set_gimple_code (p, GIMPLE_ASM);
  p->gimple_asm.ni = ninputs;
  p->gimple_asm.no = noutputs;
  p->gimple_asm.nc = nclobbers;
  p->gimple_asm.string = string;
  
  va_start (ap, nclobbers);

  for (i = 0; i < ninputs; i++)
    gimple_asm_set_input_op (p, i, va_arg (ap, tree));

  for (i = 0; i < noutputs; i++)
    gimple_asm_set_output_op (p, i, va_arg (ap, tree));

  for (i = 0; i < nclobbers; i++)
    gimple_asm_set_clobber_op (p, i, va_arg (ap, tree));

  va_end (ap);
  
  return p;
}

/* Construct a GIMPLE_CATCH statement.

  TYPES are the catch types.
  HANDLER is the exception handler.  */

gimple
build_gimple_catch (tree types, gimple_seq handler)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_catch));
  set_gimple_code (p, GIMPLE_CATCH);
  gimple_catch_set_types (p, types);
  if (handler)
    gimple_catch_set_handler (p, handler);

  return p;
}

/* Construct a GIMPLE_EH_FILTER statement.

   TYPES are the filter's types.
   FAILURE is the filter's failure action.  */

gimple
build_gimple_eh_filter (tree types, gimple_seq failure)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_eh_filter));
  set_gimple_code (p, GIMPLE_EH_FILTER);
  gimple_eh_filter_set_types (p, types);
  if (failure)
    gimple_eh_filter_set_failure (p, failure);

  return p;
}

/* Construct a GIMPLE_TRY statement.

   EVAL is the expression to evaluate.
   CLEANUP is the cleanup expression.
   CATCH_FINALLY is either GIMPLE_TRY_CATCH or GIMPLE_TRY_FINALLY depending on
   whether this is a try/catch or a try/finally respectively.  */

gimple
build_gimple_try (gimple_seq eval, gimple_seq cleanup,
    		  unsigned int catch_finally)
{
  gimple p;

  gcc_assert (catch_finally == GIMPLE_TRY_CATCH
	      || catch_finally == GIMPLE_TRY_FINALLY);
  p = ggc_alloc_cleared (sizeof (struct gimple_statement_try));
  set_gimple_code (p, GIMPLE_TRY);
  if (eval)
    gimple_try_set_eval (p, eval);
  if (cleanup)
    gimple_try_set_cleanup (p, cleanup);
  set_gimple_flags (p, catch_finally);

  return p;
}

/* Construct a GIMPLE_PHI statement.

   CAPACITY is the max number of args this node can have (for later reuse).
   RESULT the 
   NARGS is the number of phi_arg_d's in ..., representing the number of
   incomming edges in this phi node.
   ... phi_arg_d* for the incomming edges to this node.  */

gimple
build_gimple_phi (unsigned capacity, unsigned nargs, tree result, ...)
{
  gimple p;
  unsigned int i;
  va_list va;
  p = ggc_alloc_cleared (sizeof (struct gimple_statement_phi)
			 + (sizeof (struct phi_arg_d) * (nargs - 1)) );
  
  set_gimple_code (p, GIMPLE_PHI);
  gimple_phi_set_capacity (p, capacity);
  gimple_phi_set_nargs (p, nargs);
  gimple_phi_set_result (p, result);
  
  va_start (va, result);
  for (i = 0; i < nargs; i++)
    {
      struct phi_arg_d* phid = va_arg (va, struct phi_arg_d*);
      gimple_phi_set_arg (p, i, phid);
    }
  va_end (va);
  
  return p;
}

/* Construct a GIMPLE_RESX statement.

   REGION is the region number from which this resx causes control flow to 
   leave.  */

gimple
build_gimple_resx (int region)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_resx));
  set_gimple_code (p, GIMPLE_RESX);
  gimple_resx_set_region (p, region);
  return p;
}


/* The helper for constructing a gimple switch statement.
   INDEX is the switch's index.
   NLABELS is the number of labels in the switch excluding the default.
   DEFAULT_LABEL is the default label for the switch statement.  */

static inline gimple 
build_gimple_switch_1 (unsigned int nlabels, tree index, tree default_label)
{
  /* nlabels + 1 default label + 1 index.  */
  gimple p = build_gimple_with_ops (GIMPLE_SWITCH, 0, nlabels + 1 + 1);
  gimple_switch_set_index (p, index);
  gimple_switch_set_default_label (p, default_label);
  return p;
}


/* Construct a GIMPLE_SWITCH statement.

   INDEX is the switch's index.
   NLABELS is the number of labels in the switch excluding the DEFAULT_LABEL. 
   ... are the labels excluding the default.  */

gimple 
build_gimple_switch (unsigned int nlabels, tree index, tree default_label, ...)
{
  va_list al;
  unsigned int i;
  gimple p;
  
  p = build_gimple_switch_1 (nlabels, index, default_label);

  /* Store the rest of the labels.  */
  va_start (al, default_label);
  for (i = 1; i <= nlabels; i++)
    gimple_switch_set_label (p, i, va_arg (al, tree));
  va_end (al);

  return p;
}


/* Construct a GIMPLE_SWITCH statement.

   INDEX is the switch's index.
   NLABELS is the number of labels in the switch excluding the default. 
   ARGS is a vector of labels excluding the default.  */

gimple
build_gimple_switch_vec (tree index, tree default_label, VEC(tree, heap) *args)
{
  size_t i;
  size_t nlabels = VEC_length (tree, args);
  gimple p = build_gimple_switch_1 (nlabels, index, default_label);

  /*  Put labels in labels[1 - (nlabels + 1)].
     Default label is in labels[0].  */
  for (i = 1; i <= nlabels; i++)
    gimple_switch_set_label (p, i, VEC_index (tree, args, i - 1));

  return p;
}


/* Construct a GIMPLE_OMP_CRITICAL statement.

   BODY is the sequence of statements for which only one thread can execute.
   NAME is optional identifier for this critical block.  */

gimple 
build_gimple_omp_critical (gimple_seq body, tree name)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_omp_critical));
  set_gimple_code (p, GIMPLE_OMP_CRITICAL);
  gimple_omp_critical_set_name (p, name);
  if (body)
    gimple_omp_set_body (p, body);

  return p;
}

/* Construct a GIMPLE_OMP_FOR statement.

   BODY is sequence of statements inside the for loop.
   CLAUSES, are any of the OMP loop construct's clauses: private, firstprivate, 
   lastprivate, reductions, ordered, schedule, and nowait.
   PRE_BODY is the sequence of statements that are loop invariant.
   INDEX is the index variable.
   INITIAL is the initial value of INDEX.
   FINAL is final value of INDEX.
   OMP_FOR_COND  the predicate used to compare INDEX and FINAL.
   INCR is the increment expression.  */

gimple
build_gimple_omp_for (gimple_seq body, tree clauses, tree index, 
                  tree initial, tree final, tree incr, 
                  gimple_seq pre_body, enum gimple_cond omp_for_cond)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_omp_for));
  set_gimple_code (p, GIMPLE_OMP_FOR);
  if (body)
    gimple_omp_set_body (p, body);
  gimple_omp_for_set_clauses (p, clauses);
  gimple_omp_for_set_index (p, index);
  gimple_omp_for_set_initial (p, initial);
  gimple_omp_for_set_final (p, final);
  gimple_omp_for_set_incr (p, incr);
  if (pre_body)
    gimple_omp_for_set_pre_body (p, pre_body);
  gimple_assign_omp_for_cond (p, omp_for_cond);

  return p;
}

/* Construct a GIMPLE_OMP_PARALLEL statement.

   BODY is sequence of statements which are executed in parallel.
   CLAUSES, are the OMP parallel construct's clauses.
   CHILD_FN is the function created for the parallel threads to execute.
   DATA_ARG are the shared data argument(s).  */

gimple 
build_gimple_omp_parallel (gimple_seq body, tree clauses, tree child_fn, 
                       tree data_arg)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_omp_parallel));
  set_gimple_code (p, GIMPLE_OMP_PARALLEL);
  if (body)
    gimple_omp_set_body (p, body);
  gimple_omp_parallel_set_clauses (p, clauses);
  gimple_omp_parallel_set_child_fn (p, child_fn);
  gimple_omp_parallel_set_data_arg (p, data_arg);

  return p;
}

/* Construct a GIMPLE_OMP_SECTION statement for a sections statement.

   BODY is the sequence of statements in the section.  */

gimple
build_gimple_omp_section (gimple_seq body)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_omp));
  set_gimple_code (p, GIMPLE_OMP_SECTION);
  if (body)
    gimple_omp_set_body (p, body);

  return p;
}
/* Construct a GIMPLE_OMP_MASTER statement.

   BODY is the sequence of statements to be executed by just the master.  */

gimple 
build_gimple_omp_master (gimple_seq body)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_omp));
  set_gimple_code (p, GIMPLE_OMP_MASTER);
  if (body)
    gimple_omp_set_body (p, body);

  return p;
}
/* Construct a GIMPLE_OMP_CONTINUE statement.
   FIXME tuples: BODY.  */

gimple 
build_gimple_omp_continue (gimple_seq body)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_omp));
  set_gimple_code (p, GIMPLE_OMP_CONTINUE);
  if (body)
    gimple_omp_set_body (p, body);

  return p;
}

/* Construct a GIMPLE_OMP_ORDERED statement.

   BODY is the sequence of statements inside a loop that will executed in
   sequence.  */

gimple 
build_gimple_omp_ordered (gimple_seq body)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_omp));
  set_gimple_code (p, GIMPLE_OMP_ORDERED);
  if (body)
    gimple_omp_set_body (p, body);

  return p;
}

/* Construct a GIMPLE_OMP_RETURN statement.
   WAIT_P is true if this is a non-waiting return.  */

gimple 
build_gimple_omp_return (bool wait_p)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_omp));
  set_gimple_code (p, GIMPLE_OMP_RETURN);
  if (wait_p)
    set_gimple_flags (p, OMP_RETURN_NOWAIT_FLAG);

  return p;
}

/* Construct a GIMPLE_OMP_SECTIONS statement.

   BODY is a sequence of section statements.
   CLAUSES are any of the OMP sections contsruct's clauses: private,
   firstprivate, lastprivate, reduction, and nowait.  */

gimple 
build_gimple_omp_sections (gimple_seq body, tree clauses)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_omp_sections));
  set_gimple_code (p, GIMPLE_OMP_SECTIONS);
  if (body)
    gimple_omp_set_body (p, body);
  gimple_omp_sections_set_clauses (p, clauses);

  return p;
}

/* Construct a GIMPLE_OMP_SINGLE statement.

   BODY is the sequence of statements that will be executed once.
   CLAUSES are any of the OMP single construct's clauses: private, firstprivate,
   copyprivate, nowait.  */

gimple 
build_gimple_omp_single (gimple_seq body, tree clauses)
{
  gimple p;

  p = ggc_alloc_cleared (sizeof (struct gimple_statement_omp_single));
  set_gimple_code (p, GIMPLE_OMP_SINGLE);
  if (body)
    gimple_omp_set_body (p, body);
  gimple_omp_single_set_clauses (p, clauses);

  return p;
}

/* Return which gimple structure is used by T.  The enums here are defined
   in gsstruct.def.  */

enum gimple_statement_structure_enum
gimple_statement_structure (gimple gs)
{
  return gss_for_code (gimple_code (gs));
}

#if defined ENABLE_GIMPLE_CHECKING && (GCC_VERSION >= 2007)
/* Complain of a gimple type mismatch and die.  */

void
gimple_check_failed (const gimple gs, const char *file, int line,
	         const char *function, enum gimple_code code,
		 unsigned int subcode)
{
  internal_error ("gimple check: expected %s(%u), have %s(%u) in %s, at %s:%d",
      		  gimple_code_name[code],
		  subcode,
		  gimple_code_name[gimple_code (gs)],
		  gimple_flags (gs),
		  function, trim_filename (file), line);
}

/* Similar to gimple_check_failed, except that instead of specifying a
   dozen codes, use the knowledge that they're all sequential.  */

void
gimple_range_check_failed (const gimple gs, const char *file, int line,
		       const char *function, enum gimple_code c1,
		       enum gimple_code c2)
{
  char *buffer;
  unsigned length = 0;
  enum gimple_code c;

  for (c = c1; c <= c2; ++c)
    length += 4 + strlen (gimple_code_name[c]);

  length += strlen ("expected ");
  buffer = alloca (length);
  length = 0;

  for (c = c1; c <= c2; ++c)
    {
      const char *prefix = length ? " or " : "expected ";

      strcpy (buffer + length, prefix);
      length += strlen (prefix);
      strcpy (buffer + length, gimple_code_name[c]);
      length += strlen (gimple_code_name[c]);
    }

  internal_error ("gimple check: %s, have %s in %s, at %s:%d",
		  buffer, gimple_code_name[gimple_code (gs)],
		  function, trim_filename (file), line);
}
#endif /* ENABLE_GIMPLE_CHECKING */


/* Link a gimple statement(s) to the end of the sequence SEQ.  */

void
gimple_add (gimple_seq seq, gimple gs)
{
  /* Make sure this stmt is not part of another chain.  */
  gcc_assert (gimple_prev (gs) == NULL && gimple_next (gs) == NULL);

  if (gimple_seq_first (seq) == NULL)
    {
      /* Sequence SEQ is empty.  Make GS its only member.  */
      gimple_seq_set_first (seq, gs);
      gimple_seq_set_last (seq, gs);
    }
  else
    {
      /* Otherwise, link GS to the end of SEQ.  */
      set_gimple_prev (gs, gimple_seq_last (seq));
      set_gimple_next (gimple_seq_last (seq), gs);
      gimple_seq_set_last (seq, gs);
    }
}


/* Append sequence SRC to the end of sequence DST.  */

void
gimple_seq_append (gimple_seq dst, gimple_seq src)
{
  if (gimple_seq_empty_p (src))
    return;

  /* Make sure SRC is not linked somewhere else.  */
  gcc_assert (gimple_prev (src->first) == NULL
              && gimple_next (src->last) == NULL);

  if (gimple_seq_empty_p (dst))
    gimple_seq_copy (dst, src);
  else
    {
      set_gimple_next (gimple_seq_last (dst), gimple_seq_first (src));
      set_gimple_prev (gimple_seq_first (src), gimple_seq_last (dst));
      gimple_seq_set_last (dst, gimple_seq_last (src));
    }
}


/* Visit all the tuples in sequence SEQ, and apply FUNC to all the tree leaves
   in the tuples.  The trees in the tuples encountered will be walked with
   walk_tree().  FUNC, DATA, and PSET are as in walk_tree.

   You cannot use this function to rewrite trees, as the address of
   thre trees passed to walk_tree are local to this function.
   Besides, you shouldn't be rewriting trees this late in the
   game.  */

void
walk_seq_ops (gimple_seq seq, walk_tree_fn func, void *data,
	      struct pointer_set_t *pset)
{
  gimple_stmt_iterator gsi;

  for (gsi = gsi_start (seq); !gsi_end_p (gsi); gsi_next (&gsi))
    walk_tuple_ops (gsi_stmt (gsi), func, data, pset);
}


/* Helper function of walk_seq_ops.  Walks one tuple's trees.  The
   arguments are as in walk_seq_ops, except GS is the tuple to
   walk.  */
#define WALKIT(__this) leaf = (__this), walk_tree (&leaf, func, data, pset)
void
walk_tuple_ops (gimple gs, walk_tree_fn func, void *data,
		struct pointer_set_t *pset)
{
  unsigned int i;
  tree leaf;
  enum gimple_statement_structure_enum gss;

  gss = gimple_statement_structure (gs);
  if (gss == GSS_WITH_OPS || gss == GSS_WITH_MEM_OPS)
    for (i = 0; i < gimple_num_ops (gs); i++)
      WALKIT (gimple_op (gs, i));
  else
    switch (gimple_code (gs))
      {
      case GIMPLE_BIND:
	WALKIT (gimple_bind_vars (gs));
	walk_seq_ops (gimple_bind_body (gs), func, data, pset);
	break;

      case GIMPLE_CATCH:
	WALKIT (gimple_catch_types (gs));
	walk_seq_ops (gimple_catch_handler (gs), func, data, pset);
	break;

      case GIMPLE_EH_FILTER:
	WALKIT (gimple_eh_filter_types (gs));
	walk_seq_ops (gimple_eh_filter_failure (gs), func, data, pset);
	break;

      case GIMPLE_PHI:
	WALKIT (gimple_phi_result (gs));
	break;

      case GIMPLE_TRY:
	walk_seq_ops (gimple_try_eval (gs), func, data, pset);
	walk_seq_ops (gimple_try_cleanup (gs), func, data, pset);
	break;

      case GIMPLE_OMP_CRITICAL:
	walk_seq_ops (gimple_omp_body (gs), func, data, pset);
	WALKIT (gimple_omp_critical_name (gs));
	break;

	/* Just a body.  */
      case GIMPLE_OMP_CONTINUE:
      case GIMPLE_OMP_MASTER:
      case GIMPLE_OMP_ORDERED:
      case GIMPLE_OMP_SECTION:
	walk_seq_ops (gimple_omp_body (gs), func, data, pset);
	break;

      case GIMPLE_OMP_FOR:
	walk_seq_ops (gimple_omp_body (gs), func, data, pset);
	WALKIT (gimple_omp_for_clauses (gs));
	WALKIT (gimple_omp_for_index (gs));
	WALKIT (gimple_omp_for_initial (gs));
	WALKIT (gimple_omp_for_final (gs));
	WALKIT (gimple_omp_for_incr (gs));
	walk_seq_ops (gimple_omp_for_pre_body (gs), func, data, pset);
	break;

      case GIMPLE_OMP_PARALLEL:
	walk_seq_ops (gimple_omp_body (gs), func, data, pset);
	WALKIT (gimple_omp_parallel_clauses (gs));
	WALKIT (gimple_omp_parallel_child_fn (gs));
	WALKIT (gimple_omp_parallel_data_arg (gs));
	break;

      case GIMPLE_OMP_SECTIONS:
	walk_seq_ops (gimple_omp_body (gs), func, data, pset);
	WALKIT (gimple_omp_sections_clauses (gs));
	break;

      case GIMPLE_OMP_SINGLE:
	walk_seq_ops (gimple_omp_body (gs), func, data, pset);
	WALKIT (gimple_omp_single_clauses (gs));
	break;

	/* Tuples that do not have trees.  */
      case GIMPLE_NOP:
      case GIMPLE_RESX:
      case GIMPLE_OMP_RETURN:
	break;

      default:
	debug_gimple_stmt (gs);
	gcc_unreachable ();
	break;
      }
}


/* Set sequence SEQ to be the GIMPLE body for function FN.  */

void
set_gimple_body (tree fn, gimple_seq seq)
{
  void **slot;

  if (gimple_bodies == NULL)
    gimple_bodies = pointer_map_create ();

  slot = pointer_map_insert (gimple_bodies, fn);
  *slot = (void *) seq;
}
  

/* Return the body of GIMPLE statements for function FN.  */

gimple_seq
gimple_body (tree fn)
{
  void **slot;
  
  if (gimple_bodies && (slot = pointer_map_contains (gimple_bodies, fn)))
    return (gimple_seq) *slot;
  
  return NULL;
}
