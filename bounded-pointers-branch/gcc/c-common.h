/* Definitions for c-common.c.
   Copyright (C) 1987, 1993, 1994, 1995, 1997, 1998,
   1999, 2000 Free Software Foundation, Inc.

This file is part of GNU CC.

GNU CC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU CC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU CC; see the file COPYING.  If not, write to
the Free Software Foundation, 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

#ifndef GCC_C_COMMON_H
#define GCC_C_COMMON_H

/* Usage of TREE_LANG_FLAG_?:
   0: COMPOUND_STMT_NO_SCOPE (in COMPOUND_STMT).
      TREE_NEGATED_INT (in INTEGER_CST).
      IDENTIFIER_MARKED (used by search routines).
      SCOPE_BEGIN_P (in SCOPE_STMT)
      DECL_PRETTY_FUNCTION_P (in VAR_DECL)
      NEW_FOR_SCOPE_P (in FOR_STMT)
   1: C_DECLARED_LABEL_FLAG (in LABEL_DECL)
      STMT_IS_FULL_EXPR_P (in _STMT)
   2: STMT_LINENO_FOR_FN_P (in _STMT)
   3: SCOPE_NO_CLEANUPS_P (in SCOPE_STMT)
   4: SCOPE_PARTIAL_P (in SCOPE_STMT)
*/

/* Reserved identifiers.  This is the union of all the keywords for C,
   C++, and Objective C.  All the type modifiers have to be in one
   block at the beginning, because they are used as mask bits.  There
   are 27 type modifiers; if we add many more we will have to redesign
   the mask mechanism.  */

enum rid
{
  /* Modifiers: */
  /* C, in empirical order of frequency. */
  RID_STATIC = 0,
  RID_UNSIGNED, RID_LONG,    RID_CONST, RID_EXTERN,
  RID_REGISTER, RID_TYPEDEF, RID_SHORT, RID_INLINE,
  RID_VOLATILE, RID_SIGNED,  RID_AUTO,  RID_RESTRICT,

  /* C extensions */
  RID_BOUNDED, RID_UNBOUNDED, RID_COMPLEX,

  /* C++ */
  RID_FRIEND, RID_VIRTUAL, RID_EXPLICIT, RID_EXPORT, RID_MUTABLE,

  /* ObjC */
  RID_IN, RID_OUT, RID_INOUT, RID_BYCOPY, RID_BYREF, RID_ONEWAY,

  /* C */
  RID_INT,     RID_CHAR,   RID_FLOAT,    RID_DOUBLE, RID_VOID,
  RID_ENUM,    RID_STRUCT, RID_UNION,    RID_IF,     RID_ELSE,
  RID_WHILE,   RID_DO,     RID_FOR,      RID_SWITCH, RID_CASE,
  RID_DEFAULT, RID_BREAK,  RID_CONTINUE, RID_RETURN, RID_GOTO,
  RID_SIZEOF,

  /* C extensions */
  RID_ASM,       RID_TYPEOF,   RID_ALIGNOF,  RID_ATTRIBUTE,  RID_VA_ARG,
  RID_EXTENSION, RID_IMAGPART, RID_REALPART, RID_LABEL,      RID_PTRLOW,
  RID_PTRHIGH,  RID_PTRVALUE,

  /* C++ */
  RID_BOOL,     RID_WCHAR,    RID_CLASS,
  RID_PUBLIC,   RID_PRIVATE,  RID_PROTECTED,
  RID_TEMPLATE, RID_NULL,     RID_CATCH,
  RID_DELETE,   RID_FALSE,    RID_NAMESPACE,
  RID_NEW,      RID_OPERATOR, RID_THIS,
  RID_THROW,    RID_TRUE,     RID_TRY,
  RID_TYPENAME, RID_TYPEID,   RID_USING,

  /* casts */
  RID_CONSTCAST, RID_DYNCAST, RID_REINTCAST, RID_STATCAST,

  /* alternate spellings */
  RID_AND, RID_AND_EQ, RID_NOT, RID_NOT_EQ,
  RID_OR,  RID_OR_EQ,  RID_XOR, RID_XOR_EQ,
  RID_BITAND, RID_BITOR, RID_COMPL,

  /* Objective C */
  RID_ID,          RID_AT_ENCODE,    RID_AT_END,
  RID_AT_CLASS,    RID_AT_ALIAS,     RID_AT_DEFS,
  RID_AT_PRIVATE,  RID_AT_PROTECTED, RID_AT_PUBLIC,
  RID_AT_PROTOCOL, RID_AT_SELECTOR,  RID_AT_INTERFACE,
  RID_AT_IMPLEMENTATION,

  RID_MAX,

  RID_FIRST_MODIFIER = RID_STATIC,
  RID_LAST_MODIFIER = RID_ONEWAY
};

/* The elements of `ridpointers' are identifier nodes for the reserved
   type names and storage classes.  It is indexed by a RID_... value.  */
extern tree *ridpointers;

/* Standard named or nameless data types of the C compiler.  */

enum c_tree_index
{
    CTI_WCHAR_TYPE,
    CTI_SIGNED_WCHAR_TYPE,
    CTI_UNSIGNED_WCHAR_TYPE,
    CTI_WINT_TYPE,
    CTI_C_SIZE_TYPE, /* For format checking only.  */
    CTI_SIGNED_SIZE_TYPE, /* For format checking only.  */
    CTI_UNSIGNED_PTRDIFF_TYPE, /* For format checking only.  */
    CTI_WIDEST_INT_LIT_TYPE,
    CTI_WIDEST_UINT_LIT_TYPE,

    CTI_CHAR_ARRAY_TYPE,
    CTI_WCHAR_ARRAY_TYPE,
    CTI_INT_ARRAY_TYPE,
    CTI_STRING_TYPE,
    CTI_CONST_STRING_TYPE,

    CTI_BOOLEAN_TYPE,
    CTI_BOOLEAN_TRUE,
    CTI_BOOLEAN_FALSE,
    CTI_DEFAULT_FUNCTION_TYPE,
    CTI_VOID_LIST,

    CTI_VOID_FTYPE,
    CTI_VOID_FTYPE_PTR,
    CTI_INT_FTYPE_INT,
    CTI_PTR_FTYPE_SIZETYPE,

    CTI_G77_INTEGER_TYPE,
    CTI_G77_UINTEGER_TYPE,
    CTI_G77_LONGINT_TYPE,
    CTI_G77_ULONGINT_TYPE,

    /* These are not types, but we have to look them up all the time.  */
    CTI_FUNCTION_ID,
    CTI_PRETTY_FUNCTION_ID,
    CTI_FUNC_ID,

    CTI_VOID_ZERO,

    CTI_MAX
};

#define wchar_type_node			c_global_trees[CTI_WCHAR_TYPE]
#define signed_wchar_type_node		c_global_trees[CTI_SIGNED_WCHAR_TYPE]
#define unsigned_wchar_type_node	c_global_trees[CTI_UNSIGNED_WCHAR_TYPE]
#define wint_type_node			c_global_trees[CTI_WINT_TYPE]
#define c_size_type_node		c_global_trees[CTI_C_SIZE_TYPE]
#define signed_size_type_node		c_global_trees[CTI_SIGNED_SIZE_TYPE]
#define unsigned_ptrdiff_type_node	c_global_trees[CTI_UNSIGNED_PTRDIFF_TYPE]
#define widest_integer_literal_type_node c_global_trees[CTI_WIDEST_INT_LIT_TYPE]
#define widest_unsigned_literal_type_node c_global_trees[CTI_WIDEST_UINT_LIT_TYPE]

#define boolean_type_node		c_global_trees[CTI_BOOLEAN_TYPE]
#define boolean_true_node		c_global_trees[CTI_BOOLEAN_TRUE]
#define boolean_false_node		c_global_trees[CTI_BOOLEAN_FALSE]

#define char_array_type_node		c_global_trees[CTI_CHAR_ARRAY_TYPE]
#define wchar_array_type_node		c_global_trees[CTI_WCHAR_ARRAY_TYPE]
#define int_array_type_node		c_global_trees[CTI_INT_ARRAY_TYPE]
#define string_type_node		c_global_trees[CTI_STRING_TYPE]
#define const_string_type_node		c_global_trees[CTI_CONST_STRING_TYPE]

#define default_function_type		c_global_trees[CTI_DEFAULT_FUNCTION_TYPE]
#define void_list_node			c_global_trees[CTI_VOID_LIST]
#define void_ftype			c_global_trees[CTI_VOID_FTYPE]
#define void_ftype_ptr			c_global_trees[CTI_VOID_FTYPE_PTR]
#define int_ftype_int			c_global_trees[CTI_INT_FTYPE_INT]
#define ptr_ftype_sizetype		c_global_trees[CTI_PTR_FTYPE_SIZETYPE]

/* g77 integer types, which which must be kept in sync with f/com.h */
#define g77_integer_type_node		c_global_trees[CTI_G77_INTEGER_TYPE]
#define g77_uinteger_type_node		c_global_trees[CTI_G77_UINTEGER_TYPE]
#define g77_longint_type_node		c_global_trees[CTI_G77_LONGINT_TYPE]
#define g77_ulongint_type_node		c_global_trees[CTI_G77_ULONGINT_TYPE]

#define function_id_node		c_global_trees[CTI_FUNCTION_ID]
#define pretty_function_id_node		c_global_trees[CTI_PRETTY_FUNCTION_ID]
#define func_id_node			c_global_trees[CTI_FUNC_ID]

/* A node for `((void) 0)'.  */
#define void_zero_node                  c_global_trees[CTI_VOID_ZERO]

extern tree c_global_trees[CTI_MAX];

typedef enum c_language_kind
{
  clk_c,           /* A dialect of C: K&R C, ANSI/ISO C89, C2000,
		       etc. */
  clk_cplusplus,   /* ANSI/ISO C++ */
  clk_objective_c  /* Objective C */
} 
c_language_kind;

/* Information about a statement tree.  */

struct stmt_tree_s {
  /* The last statement added to the tree.  */
  tree x_last_stmt;
  /* The type of the last expression statement.  (This information is
     needed to implement the statement-expression extension.)  */
  tree x_last_expr_type;
  /* In C++, Non-zero if we should treat statements as full
     expressions.  In particular, this variable is no-zero if at the
     end of a statement we should destroy any temporaries created
     during that statement.  Similarly, if, at the end of a block, we
     should destroy any local variables in this block.  Normally, this
     variable is non-zero, since those are the normal semantics of
     C++.

     However, in order to represent aggregate initialization code as
     tree structure, we use statement-expressions.  The statements
     within the statement expression should not result in cleanups
     being run until the entire enclosing statement is complete.  

     This flag has no effect in C.  */
  int stmts_are_full_exprs_p; 
};

typedef struct stmt_tree_s *stmt_tree;

/* Global state pertinent to the current function.  Some C dialects
   extend this structure with additional fields.  */

struct language_function {
  /* While we are parsing the function, this contains information
     about the statement-tree that we are building.  */
  struct stmt_tree_s x_stmt_tree;
};

/* When building a statement-tree, this is the last statement added to
   the tree.  */

#define last_tree (current_stmt_tree ()->x_last_stmt)

/* The type of the last expression-statement we have seen.  */

#define last_expr_type (current_stmt_tree ()->x_last_expr_type)

/* The type of a function that walks over tree structure.  */

typedef tree (*walk_tree_fn)                    PARAMS ((tree *, 
							 int *, 
							 void *));

extern stmt_tree current_stmt_tree              PARAMS ((void));
extern void begin_stmt_tree                     PARAMS ((tree *));
extern tree add_stmt				PARAMS ((tree));
extern void finish_stmt_tree                    PARAMS ((tree *));

extern int statement_code_p                     PARAMS ((enum tree_code));
extern int (*lang_statement_code_p)             PARAMS ((enum tree_code));
extern tree walk_stmt_tree			PARAMS ((tree *,
							 walk_tree_fn,
							 void *));
extern void prep_stmt                           PARAMS ((tree));
extern void (*lang_expand_stmt)                 PARAMS ((tree));
extern void expand_stmt                         PARAMS ((tree));

/* LAST_TREE contains the last statement parsed.  These are chained
   together through the TREE_CHAIN field, but often need to be
   re-organized since the parse is performed bottom-up.  This macro
   makes LAST_TREE the indicated SUBSTMT of STMT.  */

#define RECHAIN_STMTS(stmt, substmt)		\
  do {						\
    substmt = TREE_CHAIN (stmt);		\
    TREE_CHAIN (stmt) = NULL_TREE;		\
    last_tree = stmt;				\
  } while (0)

/* The variant of the C language being processed.  Each C language
   front-end defines this variable.  */

extern c_language_kind c_language;

/* Nonzero means give string constants the type `const char *', rather
   than `char *'.  */

extern int flag_const_strings;

/* Warn about *printf or *scanf format/argument anomalies. */

extern int warn_format;

/* Nonzero means do some things the same way PCC does.  */

extern int flag_traditional;

/* Nonzero means enable C89 Amendment 1 features, other than digraphs.  */

extern int flag_isoc94;

/* Nonzero means use the ISO C99 dialect of C.  */

extern int flag_isoc99;

/* Nonzero means accept digraphs.  */

extern int flag_digraphs;

/* Nonzero means environment is hosted (i.e., not freestanding) */

extern int flag_hosted;

/* Nonzero means add default format_arg attributes for functions not
   in ISO C.  */

extern int flag_noniso_default_format_attributes;

/* Nonzero means warn about suggesting putting in ()'s.  */

extern int warn_parentheses;

/* Warn if a type conversion is done that might have confusing results.  */

extern int warn_conversion;

/* C types are partitioned into three subsets: object, function, and
   incomplete types.  */
#define C_TYPE_OBJECT_P(type) \
  (TREE_CODE (type) != FUNCTION_TYPE && TYPE_SIZE (type))

#define C_TYPE_INCOMPLETE_P(type) \
  (TREE_CODE (type) != FUNCTION_TYPE && TYPE_SIZE (type) == 0)

#define C_TYPE_FUNCTION_P(type) \
  (TREE_CODE (type) == FUNCTION_TYPE)

/* For convenience we define a single macro to identify the class of
   object or incomplete types.  */
#define C_TYPE_OBJECT_OR_INCOMPLETE_P(type) \
  (!C_TYPE_FUNCTION_P (type))

/* Record in each node resulting from a binary operator
   what operator was specified for it.  */
#define C_EXP_ORIGINAL_CODE(exp) ((enum tree_code) TREE_COMPLEXITY (exp))

/* Pointer to function to generate the VAR_DECL for __FUNCTION__ etc.
   ID is the identifier to use, NAME is the string.
   TYPE_DEP indicates whether it depends on type of the function or not
   (i.e. __PRETTY_FUNCTION__).  */

extern tree (*make_fname_decl)                  PARAMS ((tree, const char *, int));

extern void declare_function_name		PARAMS ((void));
extern void decl_attributes			PARAMS ((tree, tree, tree));
extern void init_function_format_info		PARAMS ((void));
extern void check_function_format		PARAMS ((tree, tree, tree));
extern void c_apply_type_quals_to_decl		PARAMS ((int, tree));
/* Print an error message for invalid operands to arith operation CODE.
   NOP_EXPR is used as a special case (see truthvalue_conversion).  */
extern void binary_op_error			PARAMS ((enum tree_code));
extern void c_expand_expr_stmt			PARAMS ((tree));
extern void c_expand_start_cond			PARAMS ((tree, int, int));
extern void c_expand_start_else			PARAMS ((void));
extern void c_expand_end_cond			PARAMS ((void));
/* Validate the expression after `case' and apply default promotions.  */
extern tree check_case_value			PARAMS ((tree));
/* Concatenate a list of STRING_CST nodes into one STRING_CST.  */
extern tree combine_strings			PARAMS ((tree));
extern void constant_expression_warning		PARAMS ((tree));
extern tree convert_and_check			PARAMS ((tree, tree));
extern void overflow_warning			PARAMS ((tree));
extern void unsigned_conversion_warning		PARAMS ((tree, tree));

/* Read the rest of the current #-directive line.  */
#if USE_CPPLIB
extern char *get_directive_line			PARAMS ((void));
#define GET_DIRECTIVE_LINE() get_directive_line ()
#else
extern char *get_directive_line			PARAMS ((FILE *));
#define GET_DIRECTIVE_LINE() get_directive_line (finput)
#endif

/* Subroutine of build_binary_op, used for comparison operations.
   See if the operands have both been converted from subword integer types
   and, if so, perhaps change them both back to their original type.  */
extern tree shorten_compare			PARAMS ((tree *, tree *, tree *, enum tree_code *));
/* Prepare expr to be an argument of a TRUTH_NOT_EXPR,
   or validate its data type for an `if' or `while' statement or ?..: exp. */
extern tree truthvalue_conversion		PARAMS ((tree));
extern tree type_for_mode			PARAMS ((enum machine_mode, int));
extern tree type_for_size			PARAMS ((unsigned, int));

extern unsigned int min_precision		PARAMS ((tree, int));

/* Add qualifiers to a type, in the fashion for C.  */
extern tree c_build_qualified_type              PARAMS ((tree, int));

/* Build tree nodes and builtin functions common to both C and C++ language
   frontends.  */
extern void c_common_nodes_and_builtins		PARAMS ((int, int, int));

extern tree build_va_arg			PARAMS ((tree, tree));

/* Nonzero if the type T promotes to itself.
   ANSI C states explicitly the list of types that promote;
   in particular, short promotes to int even if they have the same width.  */
#define C_PROMOTING_INTEGER_TYPE_P(t)				\
  (TREE_CODE ((t)) == INTEGER_TYPE				\
   && (TYPE_MAIN_VARIANT (t) == char_type_node			\
       || TYPE_MAIN_VARIANT (t) == signed_char_type_node	\
       || TYPE_MAIN_VARIANT (t) == unsigned_char_type_node	\
       || TYPE_MAIN_VARIANT (t) == short_integer_type_node	\
       || TYPE_MAIN_VARIANT (t) == short_unsigned_type_node))

extern int self_promoting_args_p		PARAMS ((tree));
extern tree simple_type_promotes_to		PARAMS ((tree));

/* These macros provide convenient access to the various _STMT nodes.  */

/* Nonzero if this statement should be considered a full-expression,
   i.e., if temporaries created during this statement should have
   their destructors run at the end of this statement.  (In C, this
   will always be false, since there are no destructors.)  */
#define STMT_IS_FULL_EXPR_P(NODE) TREE_LANG_FLAG_1 ((NODE))

/* IF_STMT accessors. These give access to the condtion of the if
   statement, the then block of the if statement, and the else block
   of the if stsatement if it exists. */
#define IF_COND(NODE)           TREE_OPERAND (IF_STMT_CHECK (NODE), 0)
#define THEN_CLAUSE(NODE)       TREE_OPERAND (IF_STMT_CHECK (NODE), 1)
#define ELSE_CLAUSE(NODE)       TREE_OPERAND (IF_STMT_CHECK (NODE), 2)

/* WHILE_STMT accessors. These give access to the condtion of the
   while statement and the body of the while statement, respectively. */
#define WHILE_COND(NODE)        TREE_OPERAND (WHILE_STMT_CHECK (NODE), 0)
#define WHILE_BODY(NODE)        TREE_OPERAND (WHILE_STMT_CHECK (NODE), 1)

/* DO_STMT accessors. These give access to the condition of the do
   statement and the body of the do statement, respectively. */
#define DO_COND(NODE)           TREE_OPERAND (DO_STMT_CHECK (NODE), 0)
#define DO_BODY(NODE)           TREE_OPERAND (DO_STMT_CHECK (NODE), 1)

/* RETURN_STMT accessor. This gives the expression associated with a
   return statement. */
#define RETURN_EXPR(NODE)       TREE_OPERAND (RETURN_STMT_CHECK (NODE), 0)

/* EXPR_STMT accessor. This gives the expression associated with an
   expression statement. */
#define EXPR_STMT_EXPR(NODE)    TREE_OPERAND (EXPR_STMT_CHECK (NODE), 0)

/* FOR_STMT accessors. These give access to the init statement,
   condition, update expression, and body of the for statement,
   respectively. */
#define FOR_INIT_STMT(NODE)     TREE_OPERAND (FOR_STMT_CHECK (NODE), 0)
#define FOR_COND(NODE)          TREE_OPERAND (FOR_STMT_CHECK (NODE), 1)
#define FOR_EXPR(NODE)          TREE_OPERAND (FOR_STMT_CHECK (NODE), 2)
#define FOR_BODY(NODE)          TREE_OPERAND (FOR_STMT_CHECK (NODE), 3)

/* SWITCH_STMT accessors. These give access to the condition and body
   of the switch statement, respectively. */
#define SWITCH_COND(NODE)       TREE_OPERAND (SWITCH_STMT_CHECK (NODE), 0)
#define SWITCH_BODY(NODE)       TREE_OPERAND (SWITCH_STMT_CHECK (NODE), 1)

/* CASE_LABEL accessors. These give access to the high and low values
   of a case label, respectively. */
#define CASE_LOW(NODE)          TREE_OPERAND (CASE_LABEL_CHECK (NODE), 0)
#define CASE_HIGH(NODE)         TREE_OPERAND (CASE_LABEL_CHECK (NODE), 1)
#define CASE_LABEL_DECL(NODE)   TREE_OPERAND (CASE_LABEL_CHECK (NODE), 2)

/* GOTO_STMT accessor. This gives access to the label associated with
   a goto statement. */
#define GOTO_DESTINATION(NODE)  TREE_OPERAND (GOTO_STMT_CHECK (NODE), 0)

/* COMPOUND_STMT accessor. This gives access to the TREE_LIST of
   statements assocated with a compound statement. The result is the
   first statement in the list. Succeeding nodes can be acccessed by
   calling TREE_CHAIN on a node in the list. */
#define COMPOUND_BODY(NODE)     TREE_OPERAND (COMPOUND_STMT_CHECK (NODE), 0)

/* ASM_STMT accessors. ASM_STRING returns a STRING_CST for the
   instruction (e.g., "mov x, y"). ASM_OUTPUTS, ASM_INPUTS, and
   ASM_CLOBBERS represent the outputs, inputs, and clobbers for the
   statement. */
#define ASM_CV_QUAL(NODE)       TREE_OPERAND (ASM_STMT_CHECK (NODE), 0)
#define ASM_STRING(NODE)        TREE_OPERAND (ASM_STMT_CHECK (NODE), 1)
#define ASM_OUTPUTS(NODE)       TREE_OPERAND (ASM_STMT_CHECK (NODE), 2)
#define ASM_INPUTS(NODE)        TREE_OPERAND (ASM_STMT_CHECK (NODE), 3)
#define ASM_CLOBBERS(NODE)      TREE_OPERAND (ASM_STMT_CHECK (NODE), 4)

/* DECL_STMT accessor. This gives access to the DECL associated with
   the given declaration statement. */ 
#define DECL_STMT_DECL(NODE)    TREE_OPERAND (DECL_STMT_CHECK (NODE), 0)

/* STMT_EXPR accessor. */
#define STMT_EXPR_STMT(NODE)    TREE_OPERAND (STMT_EXPR_CHECK (NODE), 0)

/* LABEL_STMT accessor. This gives access to the label associated with
   the given label statement. */
#define LABEL_STMT_LABEL(NODE)  TREE_OPERAND (LABEL_STMT_CHECK (NODE), 0)

/* Nonzero if this SCOPE_STMT is for the beginning of a scope.  */
#define SCOPE_BEGIN_P(NODE) \
  (TREE_LANG_FLAG_0 (SCOPE_STMT_CHECK (NODE))) 

/* Nonzero if this SCOPE_STMT is for the end of a scope.  */
#define SCOPE_END_P(NODE) \
  (!SCOPE_BEGIN_P (SCOPE_STMT_CHECK (NODE)))

/* The BLOCK containing the declarations contained in this scope.  */
#define SCOPE_STMT_BLOCK(NODE) \
  (TREE_OPERAND (SCOPE_STMT_CHECK (NODE), 0))

/* Nonzero for a SCOPE_STMT if there were no variables in this scope.  */
#define SCOPE_NULLIFIED_P(NODE) \
  (SCOPE_STMT_BLOCK ((NODE)) == NULL_TREE)

/* Nonzero for a SCOPE_STMT which represents a lexical scope, but
   which should be treated as non-existant from the point of view of
   running cleanup actions.  */
#define SCOPE_NO_CLEANUPS_P(NODE) \
  (TREE_LANG_FLAG_3 (SCOPE_STMT_CHECK (NODE)))

/* Nonzero for a SCOPE_STMT if this statement is for a partial scope.
   For example, in:
  
     S s;
     l:
     S s2;
     goto l;

   there is (implicitly) a new scope after `l', even though there are
   no curly braces.  In particular, when we hit the goto, we must
   destroy s2 and then re-construct it.  For the implicit scope,
   SCOPE_PARTIAL_P will be set.  */
#define SCOPE_PARTIAL_P(NODE) \
  (TREE_LANG_FLAG_4 (SCOPE_STMT_CHECK (NODE)))

/* Nonzero for an ASM_STMT if the assembly statement is volatile.  */
#define ASM_VOLATILE_P(NODE)			\
  (ASM_CV_QUAL (ASM_STMT_CHECK (NODE)) != NULL_TREE)

/* The line-number at which a statement began.  But if
   STMT_LINENO_FOR_FN_P does holds, then this macro gives the
   line number for the end of the current function instead.  */
#define STMT_LINENO(NODE)			\
  (TREE_COMPLEXITY ((NODE)))

/* If non-zero, the STMT_LINENO for NODE is the line at which the
   function ended.  */
#define STMT_LINENO_FOR_FN_P(NODE) 		\
  (TREE_LANG_FLAG_2 ((NODE)))

/* Nonzero if we want the new ISO rules for pushing a new scope for `for'
   initialization variables. */
#define NEW_FOR_SCOPE_P(NODE) (TREE_LANG_FLAG_0 (NODE)) 

#define DEFTREECODE(SYM, NAME, TYPE, LENGTH) SYM,

enum c_tree_code {
  C_DUMMY_TREE_CODE = LAST_AND_UNUSED_TREE_CODE,
#include "c-common.def"
  LAST_C_TREE_CODE
};

#undef DEFTREECODE

extern void add_c_tree_codes		        PARAMS ((void));
extern void genrtl_do_pushlevel                 PARAMS ((void));
extern void genrtl_goto_stmt                    PARAMS ((tree));
extern void genrtl_expr_stmt                    PARAMS ((tree));
extern void genrtl_decl_stmt                    PARAMS ((tree));
extern void genrtl_if_stmt                      PARAMS ((tree));
extern void genrtl_while_stmt                   PARAMS ((tree));
extern void genrtl_do_stmt                      PARAMS ((tree));
extern void genrtl_return_stmt                  PARAMS ((tree));
extern void genrtl_for_stmt                     PARAMS ((tree));
extern void genrtl_break_stmt                   PARAMS ((void));
extern void genrtl_continue_stmt                PARAMS ((void));
extern void genrtl_scope_stmt                   PARAMS ((tree));
extern void genrtl_switch_stmt                  PARAMS ((tree));
extern void genrtl_case_label                   PARAMS ((tree));
extern void genrtl_compound_stmt                PARAMS ((tree));
extern void genrtl_asm_stmt                     PARAMS ((tree, tree,
							 tree, tree,
							 tree));
extern void genrtl_decl_cleanup                 PARAMS ((tree, tree));
extern int stmts_are_full_exprs_p               PARAMS ((void));
typedef void (*expand_expr_stmt_fn)             PARAMS ((tree));
extern expand_expr_stmt_fn lang_expand_expr_stmt;
extern int anon_aggr_type_p                     PARAMS ((tree));

/* For a VAR_DECL that is an anonymous union, these are the various
   sub-variables that make up the anonymous union.  */
#define DECL_ANON_UNION_ELEMS(NODE) DECL_ARGUMENTS ((NODE))

extern void emit_local_var                      PARAMS ((tree));
extern void make_rtl_for_local_static           PARAMS ((tree));
extern tree expand_cond                         PARAMS ((tree));
extern void c_expand_return			PARAMS ((tree));
extern void do_case				PARAMS ((tree, tree));
extern tree build_stmt                          PARAMS ((enum tree_code, ...));
extern tree build_case_label                    PARAMS ((tree, tree, tree));
extern tree build_continue_stmt                 PARAMS ((void));
extern tree build_break_stmt                    PARAMS ((void));
extern tree build_return_stmt                   PARAMS ((tree));

#define COMPOUND_STMT_NO_SCOPE(NODE)	TREE_LANG_FLAG_0 (NODE)

extern void c_expand_asm_operands		PARAMS ((tree, tree, tree, tree, int, const char *, int));
extern int current_function_name_declared       PARAMS ((void));
extern void set_current_function_name_declared  PARAMS ((int));

/* These functions must be defined by each front-end which implements
   a variant of the C language.  They are used in c-common.c.  */

extern tree build_unary_op                      PARAMS ((enum tree_code,
							 tree, int));
extern tree build_binary_op                     PARAMS ((enum tree_code,
							 tree, tree, int));
extern int lvalue_p				PARAMS ((tree));
extern tree default_conversion                  PARAMS ((tree));

/* Given two integer or real types, return the type for their sum.
   Given two compatible ANSI C types, returns the merged type.  */

extern tree common_type                         PARAMS ((tree, tree));

extern tree expand_tree_builtin                 PARAMS ((tree, tree, tree));

extern tree decl_constant_value		PARAMS ((tree));

/* Hook currently used only by the C++ front end to reset internal state
   after entering or leaving a header file.  */
extern void extract_interface_info		PARAMS ((void));

/* Information recorded about each file examined during compilation.  */

struct c_fileinfo
{
  int time;	/* Time spent in the file.  */
  short interface_only;		/* Flags - used only by C++ */
  short interface_unknown;
};

struct c_fileinfo *get_fileinfo			PARAMS ((const char *));
extern void dump_time_statistics		PARAMS ((void));

#endif
