/* Java(TM) language-specific gimplification routines.

   Copyright (C) 2003 Free Software Foundation, Inc.

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
Boston, MA 02111-1307, USA. 

Java and all Java-based marks are trademarks or registered trademarks
of Sun Microsystems, Inc. in the United States and other countries.
The Free Software Foundation is independent of Sun Microsystems, Inc.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "tree.h"
#include "java-tree.h"
#include "tree-simple.h"
#include "toplev.h"

static tree java_gimplify_case_expr (tree);
static tree java_gimplify_default_expr (tree);
static tree java_gimplify_block (tree);
static tree java_gimplify_new_array_init (tree);
static tree java_gimplify_try_expr (tree);

static void cleanup_compound_expr (tree *);
static void cleanup_try_finally_expr (tree *);

/* Gimplify a Java tree.  */

int
java_gimplify_expr (tree *expr_p, tree *pre_p ATTRIBUTE_UNUSED,
		    tree *post_p ATTRIBUTE_UNUSED)
{
  switch (TREE_CODE (*expr_p))
    {
    case BLOCK:
      *expr_p = java_gimplify_block (*expr_p);
      return 1;

    case EXPR_WITH_FILE_LOCATION:
      {
	tree wfl = *expr_p;
	*expr_p = EXPR_WFL_NODE (wfl);
	annotate_with_file_line (*expr_p, EXPR_WFL_FILENAME (wfl),
				 EXPR_WFL_LINENO (wfl));
      }
      return 1;

    case CASE_EXPR:
      *expr_p = java_gimplify_case_expr (*expr_p);
      return 1;

    case DEFAULT_EXPR:
      *expr_p = java_gimplify_default_expr (*expr_p);
      return 1;

    case NEW_ARRAY_INIT:
      *expr_p = java_gimplify_new_array_init (*expr_p);
      return 1;

    case TRY_EXPR:
      *expr_p = java_gimplify_try_expr (*expr_p);
      return 1;

    case JAVA_CATCH_EXPR:
      *expr_p = TREE_OPERAND (*expr_p, 0);
      return 1;

    case JAVA_EXC_OBJ_EXPR:
      *expr_p = build_exception_object_ref (TREE_TYPE (*expr_p));
      return 1;

    /* These should already be lowered before we get here.  */
    case URSHIFT_EXPR:
    case COMPARE_EXPR:
    case COMPARE_L_EXPR:
    case COMPARE_G_EXPR:
    case UNARY_PLUS_EXPR:
    case NEW_ARRAY_EXPR:
    case NEW_ANONYMOUS_ARRAY_EXPR:
    case NEW_CLASS_EXPR:
    case THIS_EXPR:
    case SYNCHRONIZED_EXPR:
    case CONDITIONAL_EXPR:
    case INSTANCEOF_EXPR:
    case CLASS_LITERAL:
      abort ();

    case COMPOUND_EXPR:
      cleanup_compound_expr (expr_p);
      return 0;

    case TRY_FINALLY_EXPR:
      cleanup_try_finally_expr (expr_p);
      return 0;

    default:
      return 0;
    }
}

static tree
java_gimplify_case_expr (tree expr)
{
  tree label = build_decl (LABEL_DECL, NULL_TREE, NULL_TREE);
  DECL_CONTEXT (label) = current_function_decl;
  return build (CASE_LABEL_EXPR, void_type_node,
		TREE_OPERAND (expr, 0), NULL_TREE, label);
}

static tree
java_gimplify_default_expr (tree expr ATTRIBUTE_UNUSED)
{
  tree label = build_decl (LABEL_DECL, NULL_TREE, NULL_TREE);
  DECL_CONTEXT (label) = current_function_decl;
  return build (CASE_LABEL_EXPR, void_type_node, NULL_TREE, NULL_TREE, label);
}

/* Gimplify BLOCK into a BIND_EXPR.  */

static tree
java_gimplify_block (tree java_block)
{
  tree decls = BLOCK_VARS (java_block);
  tree body = BLOCK_EXPR_BODY (java_block);
  tree outer = gimple_current_bind_expr ();
  tree block;

  /* Don't bother with empty blocks.  */
  if (! body || IS_EMPTY_STMT (body))
    return body;

  /* Make a proper block.  Java blocks are unsuitable for BIND_EXPR
     because they use BLOCK_SUBBLOCKS for another purpose.  */
  block = make_node (BLOCK);
  BLOCK_VARS (block) = decls;
  if (outer != NULL_TREE)
    {
      outer = BIND_EXPR_BLOCK (outer);
      BLOCK_SUBBLOCKS (outer) = chainon (BLOCK_SUBBLOCKS (outer), block);
    }

  return build (BIND_EXPR, TREE_TYPE (java_block), decls, body, block);
}

/* Gimplify a NEW_ARRAY_INIT node into array/element assignments.  */

static tree
java_gimplify_new_array_init (tree exp)
{
  tree array_type = TREE_TYPE (TREE_TYPE (exp));
  tree data_field = lookup_field (&array_type, get_identifier ("data"));
  tree element_type = TYPE_ARRAY_ELEMENT (array_type);
  HOST_WIDE_INT ilength = java_array_type_length (array_type);
  tree length = build_int_2 (ilength, 0);
  tree init = TREE_OPERAND (exp, 0);
  tree values = CONSTRUCTOR_ELTS (init);

  tree array_ptr_type = build_pointer_type (array_type);
  tree block = build (BLOCK, array_ptr_type, NULL_TREE);
  tree tmp = build_decl (VAR_DECL, get_identifier ("<tmp>"), array_ptr_type);
  tree array = build_decl (VAR_DECL, get_identifier ("<array>"), array_ptr_type);
  tree body = build (MODIFY_EXPR, array_ptr_type, tmp,
		     build_new_array (element_type, length));

  int index = 0;

  /* FIXME: try to allocate array statically?  */
  while (values != NULL_TREE)
    {
      /* FIXME: Should use build_java_arrayaccess here, but avoid
	 bounds checking.  */
      tree lhs = build (COMPONENT_REF, TREE_TYPE (data_field),    
			build_java_indirect_ref (array_type, tmp, 0),
			data_field);
      tree assignment = build (MODIFY_EXPR, element_type,
  			       build (ARRAY_REF, element_type, lhs,
				      build_int_2 (index++, 0)),
			       TREE_VALUE (values));
      body = build (COMPOUND_EXPR, element_type, body, assignment);
      values = TREE_CHAIN (values);
    }

  body = build (COMPOUND_EXPR, array_ptr_type, body,
		build (MODIFY_EXPR, array_ptr_type, array, tmp));
  TREE_CHAIN (tmp) = array;
  BLOCK_VARS (block) = tmp;
  BLOCK_EXPR_BODY (block) = body;
  return java_gimplify_block (block);
}

static tree
java_gimplify_try_expr (tree try_expr)
{
  tree body = TREE_OPERAND (try_expr, 0);
  tree handler = TREE_OPERAND (try_expr, 1);
  tree catch = NULL_TREE;

  /* Build a CATCH_EXPR for each handler.  */
  while (handler)
    {
      tree java_catch = TREE_OPERAND (handler, 0);
      tree expr = build (CATCH_EXPR, void_type_node,
			 TREE_TYPE (TREE_TYPE (BLOCK_EXPR_DECLS (java_catch))),
			 handler);
      if (catch)
	catch = build (COMPOUND_EXPR, void_type_node, catch, expr);
      else
	catch = expr;
      handler = TREE_CHAIN (handler);
    }
  return build (TRY_CATCH_EXPR, void_type_node, body, catch);
}

/* Ensure that every COMPOUND_EXPR has a type.  Also purge any
   COMPOUND_EXPR with one or more empty statements.  */

static void
cleanup_compound_expr (tree *expr_p)
{
  if (TREE_CODE (TREE_OPERAND (*expr_p, 0)) == COMPOUND_EXPR)
    cleanup_compound_expr (&TREE_OPERAND (*expr_p, 0));
  if (TREE_CODE (TREE_OPERAND (*expr_p, 1)) == COMPOUND_EXPR)
    cleanup_compound_expr (&TREE_OPERAND (*expr_p, 1));

  if (TREE_OPERAND (*expr_p, 0) == NULL_TREE
      || IS_EMPTY_STMT (TREE_OPERAND (*expr_p, 0)))
    {
      *expr_p = TREE_OPERAND (*expr_p, 1);
      return;
    }
  if (TREE_OPERAND (*expr_p, 1) == NULL_TREE
      || IS_EMPTY_STMT (TREE_OPERAND (*expr_p, 1)))
    {
      *expr_p = TREE_OPERAND (*expr_p, 0);
      return;
    }

  if (TREE_TYPE (*expr_p) == NULL_TREE)
    {
      tree last = TREE_OPERAND (*expr_p, 1);
      TREE_TYPE (*expr_p) = TREE_TYPE (last);
    }
}

/* Ensure that every TRY_FINALLY_EXPR has at least one non-empty
   statement in both its try and finally blocks.  */

static void
cleanup_try_finally_expr (tree *expr_p)
{
  if (TREE_OPERAND (*expr_p, 0) == NULL_TREE
      || IS_EMPTY_STMT (TREE_OPERAND (*expr_p, 0)))
    {
      *expr_p = TREE_OPERAND (*expr_p, 1);
      return;
    }
  if (TREE_OPERAND (*expr_p, 1) == NULL_TREE
      || IS_EMPTY_STMT (TREE_OPERAND (*expr_p, 1)))
    {
      *expr_p = TREE_OPERAND (*expr_p, 0);
      return;
    }
}
