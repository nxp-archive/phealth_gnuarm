/* LTO symbol table.
   Copyright 2006 Free Software Foundation, Inc.
   Contributed by CodeSourcery, Inc.

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
the Free Software Foundation, 51 Franklin Street, Fifth Floor,
Boston, MA 02110-1301, USA.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "toplev.h"
#include "tree.h"
#include "gimple.h"
#include "lto-tree-in.h"
#include "ggc.h"	/* lambda.h needs this */
#include "lambda.h"	/* gcd */
#include "hashtab.h"

/* Vector to keep track of external variables we've seen so far.  */
VEC(tree,gc) *lto_global_var_decls;

/* Base type for resolution map. It maps NODE to resolution.  */

struct lto_symtab_base_def GTY(())
{
  /* Key is either an IDENTIFIER or a DECL.  */
  tree node;
};
typedef struct lto_symtab_base_def *lto_symtab_base_t;

struct lto_symtab_identifier_def GTY (())
{
  struct lto_symtab_base_def base;
  tree decl;
};
typedef struct lto_symtab_identifier_def *lto_symtab_identifier_t;

struct lto_symtab_decl_def GTY (())
{
  struct lto_symtab_base_def base;
  enum ld_plugin_symbol_resolution resolution;
  struct lto_file_decl_data * GTY((skip (""))) file_data;
};
typedef struct lto_symtab_decl_def *lto_symtab_decl_t;

/* A poor man's symbol table. This hashes identifier to prevailing DECL
   if there is one. */

static GTY ((if_marked ("lto_symtab_identifier_marked_p"),
	     param_is (struct lto_symtab_identifier_def)))
  htab_t lto_symtab_identifiers;

static GTY ((if_marked ("lto_symtab_decl_marked_p"),
	     param_is (struct lto_symtab_decl_def)))
  htab_t lto_symtab_decls;

/* Return the hash value of an lto_symtab_base_t object pointed to by P.  */

static hashval_t
lto_symtab_base_hash (const void *p)
{
  const struct lto_symtab_base_def *base =
    (const struct lto_symtab_base_def*) p;
  return htab_hash_pointer (base->node);
}

/* Return non-zero if P1 and P2 points to lto_symtab_base_def structs
   corresponding to the same tree node.  */

static int
lto_symtab_base_eq (const void *p1, const void *p2)
{
  const struct lto_symtab_base_def *base1 =
     (const struct lto_symtab_base_def *) p1;
  const struct lto_symtab_base_def *base2 =
     (const struct lto_symtab_base_def *) p2;
  return (base1->node == base2->node);
}

/* Returns non-zero if P points to an lto_symtab_base_def struct that needs
   to be marked for GC.  */ 

static int
lto_symtab_base_marked_p (const void *p)
{
  const struct lto_symtab_base_def *base =
     (const struct lto_symtab_base_def *) p;

  /* Keep this only if the key node is marked.  */
  return ggc_marked_p (base->node);
}

/* Returns non-zero if P points to an lto_symtab_identifier_def struct that
   needs to be marked for GC.  */ 

static int
lto_symtab_identifier_marked_p (const void *p)
{
  return lto_symtab_base_marked_p (p);
}

/* Returns non-zero if P points to an lto_symtab_decl_def struct that needs
   to be marked for GC.  */ 

static int
lto_symtab_decl_marked_p (const void *p)
{
  return lto_symtab_base_marked_p (p);
}

#define lto_symtab_identifier_eq	lto_symtab_base_eq
#define lto_symtab_identifier_hash	lto_symtab_base_hash
#define lto_symtab_decl_eq		lto_symtab_base_eq
#define lto_symtab_decl_hash		lto_symtab_base_hash

/* Lazily initialize resolution hash tables.  */

static void
lto_symtab_maybe_init_hash_tables (void)
{
  if (!lto_symtab_identifiers)
    {
      lto_symtab_identifiers =
	htab_create_ggc (1021, lto_symtab_identifier_hash,
			 lto_symtab_identifier_eq, ggc_free);
      lto_symtab_decls =
	htab_create_ggc (1021, lto_symtab_decl_hash,
			 lto_symtab_decl_eq, ggc_free);
    }
}

/* Returns true iff TYPE_1 and TYPE_2 are the same type.  */

static bool
lto_same_type_p (tree type_1, tree type_2)
{
  unsigned int code;

  /* Check first for the obvious case of pointer identity.  */
  if (type_1 == type_2)
    return true;

  /* Check that we have two types to compare.  */
  if (type_1 == NULL_TREE || type_2 == NULL_TREE)
    return false;

  /* Can't be the same type if the types don't have the same code.  */
  code = TREE_CODE (type_1);
  if (code != TREE_CODE (type_2))
    return false;

  /* "If GNU attributes are present, types which could be the same be it not
     for their GNU attributes may in fact be different due to the use of GNU
     attributes."  Hmmm.  Punt on this for now and assume they're different
     if we see attributes on either type.  */
  if (TYPE_ATTRIBUTES (type_1) || TYPE_ATTRIBUTES (type_2))
    return false;

  switch (code)
    {
    case VOID_TYPE:
      /* Void types are the same in all translation units.  */
      return true;

    case INTEGER_TYPE:
    case BOOLEAN_TYPE:
      /* Corresponding integral types are the same.  */
      return (TYPE_PRECISION (type_1) == TYPE_PRECISION (type_2)
	      && TYPE_UNSIGNED (type_1) == TYPE_UNSIGNED (type_2)
	      && tree_int_cst_equal (TYPE_SIZE (type_1), TYPE_SIZE (type_2))
	      && TYPE_ALIGN (type_1) == TYPE_ALIGN (type_2)
	      && TYPE_STRING_FLAG (type_1) == TYPE_STRING_FLAG (type_2));
      
    case REAL_TYPE:
      /* Corresponding float types are the same.  */
      return (TYPE_PRECISION (type_1) == TYPE_PRECISION (type_2)
	      && tree_int_cst_equal (TYPE_SIZE (type_1), TYPE_SIZE (type_2))
	      && TYPE_ALIGN (type_1) == TYPE_ALIGN (type_2));

    case ARRAY_TYPE:
      /* Array types are the same if the element types are the same and
	 the number of elements are the same.  */
      if (!lto_same_type_p (TREE_TYPE (type_1), TREE_TYPE (type_2))
	  || TYPE_STRING_FLAG (type_1) != TYPE_STRING_FLAG (type_2))
	return false;
      else
	{
	  tree index_1 = TYPE_DOMAIN (type_1);
	  tree index_2 = TYPE_DOMAIN (type_2);
	  /* For an incomplete external array, the type domain can be
 	     NULL_TREE.  Check this condition also.  */
	  if (!index_1 || !index_2)
	    return (!index_1 && !index_2);
	  else
	    {
	      tree min_1 = TYPE_MIN_VALUE (index_1);
	      tree min_2 = TYPE_MIN_VALUE (index_2);
	      tree max_1 = TYPE_MAX_VALUE (index_1);
	      tree max_2 = TYPE_MAX_VALUE (index_2);
	      /* If the array types both have unspecified bounds, then
		 MAX_{1,2} will be NULL_TREE.  */
	      if (min_1 && min_2 && !max_1 && !max_2)
		return (integer_zerop (min_1)
			&& integer_zerop (min_2));
	      /* Otherwise, we need the bounds to be fully
		 specified.  */
	      if (!min_1 || !min_2 || !max_1 || !max_2)
		return false;
	      if (TREE_CODE (min_1) != INTEGER_CST
		  || TREE_CODE (min_2) != INTEGER_CST
		  || TREE_CODE (max_1) != INTEGER_CST
		  || TREE_CODE (max_2) != INTEGER_CST)
		return false;
	      if (tree_int_cst_equal (min_1, min_2))
		return tree_int_cst_equal (max_1, max_2);
	      else
		{
		  tree nelts_1 = array_type_nelts (type_1);
		  tree nelts_2 = array_type_nelts (type_2);
		  if (! nelts_1 || ! nelts_2)
		    return false;
		  if (TREE_CODE (nelts_1) != INTEGER_CST
		      || TREE_CODE (nelts_2) != INTEGER_CST)
		    return false;
		  return tree_int_cst_equal (nelts_1, nelts_2);
		}
	    }
	}

    case FUNCTION_TYPE:
      /* Function types are the same if the return type and arguments types
	 are the same.  */
      if (!lto_same_type_p (TREE_TYPE (type_1), TREE_TYPE (type_2)))
	return false;
      else
	{
	  tree parms_1 = TYPE_ARG_TYPES (type_1);
	  tree parms_2 = TYPE_ARG_TYPES (type_2);
	  if (parms_1 == parms_2)
	    return true;
	  else
	    {
	      while (parms_1 && parms_2)
		{
		  if (!lto_same_type_p (TREE_VALUE (parms_1),
					TREE_VALUE (parms_2)))
		    return false;
		  parms_1 = TREE_CHAIN (parms_1);
		  parms_2 = TREE_CHAIN (parms_2);
		}
	      return !parms_1 && !parms_2;
	    }
	}

    case POINTER_TYPE:
    case REFERENCE_TYPE:
      /* Pointer and reference types are the same if the pointed-to types are
	 the same.  */
      return lto_same_type_p (TREE_TYPE (type_1), TREE_TYPE (type_2));

    case ENUMERAL_TYPE:
    case RECORD_TYPE:
    case UNION_TYPE:
    case QUAL_UNION_TYPE:
      /* Enumeration and class types are the same if they have the same
	 name.  */
      {
	tree variant_1 = TYPE_MAIN_VARIANT (type_1);
	tree variant_2 = TYPE_MAIN_VARIANT (type_2);
	tree name_1 = TYPE_NAME (type_1);
	tree name_2 = TYPE_NAME (type_2);
	if (!name_1 || !name_2)
	  /* Presumably, anonymous types are all unique.  */
	  return false;

	if (TREE_CODE (name_1) == TYPE_DECL)
	  {
	    name_1 = DECL_NAME (name_1);
	    if (! name_1)
	      return false;
	  }
	gcc_assert (TREE_CODE (name_1) == IDENTIFIER_NODE);

	if (TREE_CODE (name_2) == TYPE_DECL)
	  {
	    name_2 = DECL_NAME (name_2);
	    if (! name_2)
	      return false;
	  }
	gcc_assert (TREE_CODE (name_2) == IDENTIFIER_NODE);

	/* Identifiers can be compared with pointer equality rather
	   than a string comparison.  */
	if (name_1 == name_2)
	  return true;

	/* If either type has a variant type, compare that.  This finds
	   the case where a struct is typedef'ed in one module but referred
	   to as 'struct foo' in the other; here, the main type for one is
	   'foo', and for the other 'foo_t', but the variants have the same
	   name 'foo'.  */
	if (variant_1 != type_1 || variant_2 != type_2)
	  return lto_same_type_p (variant_1, variant_2);
	else
	  return false;
      }

      /* FIXME:  add pointer to member types.  */
    default:
      return false;
    }
}

/* Transfer TYPE_2 qualifiers to TYPE_1 so that TYPE_1's qualifiers are
   conservatively correct with respect to optimization done before the
   merge.  */

static void
lto_merge_qualifiers (tree type_1, tree type_2)
{
  if (TYPE_VOLATILE (type_2))
    TYPE_VOLATILE (type_1) = TYPE_VOLATILE (type_2);
  if (! TYPE_READONLY (type_2))
    TYPE_READONLY (type_1) = TYPE_READONLY (type_2);
  if (! TYPE_RESTRICT (type_2))
    TYPE_RESTRICT (type_1) = TYPE_RESTRICT (type_2);
}

/* If TYPE_1 and TYPE_2 can be merged to form a common type, do it.
   Specifically, if they are both array types that have the same element
   type and one of them is a complete array type and the other isn't,
   return the complete array type.  Otherwise return NULL_TREE. */

static tree
lto_merge_types (tree type_1, tree type_2)
{
  if (TREE_CODE (type_1) == ARRAY_TYPE
      && (TREE_CODE (type_2) == ARRAY_TYPE)
      && ! TYPE_ATTRIBUTES (type_1) && ! TYPE_ATTRIBUTES (type_2)
      && (lto_same_type_p (TREE_TYPE (type_1), TREE_TYPE (type_2))))
    {
      if (COMPLETE_TYPE_P (type_1) && !COMPLETE_TYPE_P (type_2))
        {
	  lto_merge_qualifiers (type_1, type_2);
	  return type_1;
	}
      else if (COMPLETE_TYPE_P (type_2) && !COMPLETE_TYPE_P (type_1))
        {
	  lto_merge_qualifiers (type_2, type_1);
	  return type_2;
	}
      else
	return NULL_TREE;
    }
  return NULL_TREE;
}

/* Returns true iff the union of ATTRIBUTES_1 and ATTRIBUTES_2 can be
   applied to DECL.  */
static bool
lto_compatible_attributes_p (tree decl ATTRIBUTE_UNUSED, 
			     tree attributes_1, 
			     tree attributes_2)
{
#if 0
  /* ??? For now, assume two attribute sets are compatible only if they
     are both empty.  */
  return !attributes_1 && !attributes_2;
#else
  /* FIXME.  For the moment, live dangerously, and assume the user knows
     what he's doing. I don't think the linker would distinguish these cases.  */
  return true || (!attributes_1 && !attributes_2);
#endif
}

/* Helper for lto_symtab_compatible. Return TRUE if DECL is an external
   variable declaration of an aggregate type. */

static bool
external_aggregate_decl_p (tree decl)
{
  return (TREE_CODE (decl) == VAR_DECL
	  && DECL_EXTERNAL (decl)
	  && AGGREGATE_TYPE_P (TREE_TYPE (decl)));
}

/* Check if OLD_DECL and NEW_DECL are compatible. */

static bool
lto_symtab_compatible (tree old_decl, tree new_decl)
{
  tree merged_type = NULL_TREE;
  tree merged_result = NULL_TREE;

  if (TREE_CODE (old_decl) != TREE_CODE (new_decl))
    {
      switch (TREE_CODE (new_decl))
	{
	case VAR_DECL:
	  gcc_assert (TREE_CODE (old_decl) == FUNCTION_DECL);
	  error ("%Jfunction %qD redeclared as variable", new_decl, new_decl);
	  error ("%Jpreviously declared here", old_decl);
	  return false;

	case FUNCTION_DECL:
	  gcc_assert (TREE_CODE (old_decl) == VAR_DECL);
	  error ("%Jvariable %qD redeclared as function", new_decl, new_decl);
	  error ("%Jpreviously declared here", old_decl);
	  return false;

	default:
	  gcc_unreachable ();
	}
    }

  if (!lto_same_type_p (TREE_TYPE (old_decl), TREE_TYPE (new_decl)))
    {
      /* Allow an array type with unspecified bounds to
	 be merged with an array type whose bounds are specified, so
	 as to allow "extern int i[];" in one file to be combined with
	 "int i[3];" in another.  */
      if (TREE_CODE (new_decl) == VAR_DECL)
	merged_type = lto_merge_types (TREE_TYPE (old_decl),
				       TREE_TYPE (new_decl));
      else if (TREE_CODE (new_decl) == FUNCTION_DECL)
	{
	  if (DECL_IS_BUILTIN (old_decl) || DECL_IS_BUILTIN (new_decl))
	    {
	      tree candidate;
	      
	      candidate = match_builtin_function_types (TREE_TYPE (new_decl),
							TREE_TYPE (old_decl));

	      /* We don't really have source location information at this
		 point, so the above matching was a bit of a gamble.  */
	      if (candidate)
		merged_type = candidate;
	    }

	  if (!merged_type
	      /* We want either of the types to have argument types,
		 but not both.  */
	      && ((TYPE_ARG_TYPES (TREE_TYPE (old_decl)) != NULL)
		  ^ (TYPE_ARG_TYPES (TREE_TYPE (new_decl)) != NULL)))
	    {
	      /* The situation here is that (in C) somebody was smart
		 enough to use proper declarations in a header file, but
		 the actual definition of the function uses
		 non-ANSI-style argument lists.  Or we have a situation
		 where declarations weren't used anywhere and we're
		 merging the actual definition with a use.  One of the
		 decls will then have a complete function type, whereas
		 the other will only have a result type.  Assume that
		 the more complete type is the right one and don't
		 complain.  */
	      if (TYPE_ARG_TYPES (TREE_TYPE (old_decl)))
		{
		  merged_type = TREE_TYPE (old_decl);
		  merged_result = DECL_RESULT (old_decl);
		}
	      else
		{
		  merged_type = TREE_TYPE (new_decl);
		  merged_result = DECL_RESULT (new_decl);
		}
	    }

	  /* If we don't have a merged type yet...sigh.  The linker
	     wouldn't complain if the types were mismatched, so we
	     probably shouldn't either.  Just use the type from
	     whichever decl appears to be associated with the
	     definition.  If for some odd reason neither decl is, the
	     older one wins.  */
	  if (!merged_type)
	    {
	      if (!DECL_EXTERNAL (new_decl))
		{
		  merged_type = TREE_TYPE (new_decl);
		  merged_result = DECL_RESULT (new_decl);
		}
	      else
		{
		  merged_type = TREE_TYPE (old_decl);
		  merged_result = DECL_RESULT (old_decl);
		}
	    }
	}

      if (!merged_type)
	{
	  error ("%Jtype of %qD does not match original declaration",
		 new_decl, new_decl);
	  error ("%Jpreviously declared here", old_decl);
	  return false;
	}
    }

  if (DECL_UNSIGNED (old_decl) != DECL_UNSIGNED (new_decl))
    {
      error ("%Jsignedness of %qD does not match original declaration",
	     new_decl, new_decl);
      error ("%Jpreviously declared here", old_decl);
      return false;
    }

  if (!tree_int_cst_equal (DECL_SIZE (old_decl),
			   DECL_SIZE (new_decl))
      || !tree_int_cst_equal (DECL_SIZE_UNIT (old_decl),
			      DECL_SIZE_UNIT (new_decl)))
    {
      /* Permit cases where we are declaring aggregates and at least one
	 of the decls is external and one of the decls has a size whereas
	 the other one does not.  This is perfectly legal in C:

         struct s;
	 extern struct s x;

	 void*
	 f (void)
	 {
	   return &x;
	 }

	 There is no way a compiler can tell the size of x.  So we cannot
	 assume that external aggreates have complete types.  */

      if (!((TREE_CODE (TREE_TYPE (old_decl))
	     == TREE_CODE (TREE_TYPE (new_decl)))
	    && ((external_aggregate_decl_p (old_decl)
		 && DECL_SIZE (old_decl) == NULL_TREE)
		|| (external_aggregate_decl_p (new_decl)
		    && DECL_SIZE (new_decl) == NULL_TREE))))
	{
	  error ("%Jsize of %qD does not match original declaration",
		 new_decl, new_decl);
	  error ("%Jpreviously declared here", old_decl);
	  return false;
	}
    }

  /* Report an error if user-specified alignments do not match.  */
  if ((DECL_USER_ALIGN (old_decl) && DECL_USER_ALIGN (new_decl))
      && DECL_ALIGN (old_decl) != DECL_ALIGN (new_decl))
    {
      error ("%Jalignment of %qD does not match original declaration",
	     new_decl, new_decl);
      error ("%Jpreviously declared here", old_decl);
      return false;
    }

  if (DECL_MODE (old_decl) != DECL_MODE (new_decl))
    {
      /* We can arrive here when we are merging 'extern char foo[]' and
	 'char foo[SMALLNUM]'; the former is probably BLKmode and the
	 latter is not.  In such a case, we should have merged the types
	 already; detect it and don't complain.  We also need to handle
	 external aggregate declaration specially.  */
      if ((TREE_CODE (TREE_TYPE (old_decl))
	   == TREE_CODE (TREE_TYPE (new_decl)))
	  && (((TREE_CODE (TREE_TYPE (old_decl)) != ARRAY_TYPE)
	       && ((external_aggregate_decl_p (old_decl)
		    && DECL_MODE (old_decl) == VOIDmode)
		   || (external_aggregate_decl_p (new_decl)
		       && DECL_MODE (new_decl) == VOIDmode)))
	      || ((TREE_CODE (TREE_TYPE (old_decl)) == ARRAY_TYPE)
		  && merged_type)))
	;
      else
	{
	  error ("%Jmachine mode of %qD does not match original declaration",
	         new_decl, new_decl);
	  error ("%Jpreviously declared here", old_decl);
	  return false;
	}
    }

  if (!lto_compatible_attributes_p (old_decl,
				    DECL_ATTRIBUTES (old_decl),
				    DECL_ATTRIBUTES (new_decl)))
    {
      error ("%Jattributes applied to %qD are incompatible with original "
	     "declaration", new_decl, new_decl);
      error ("%Jpreviously declared here", old_decl);
      return false;
    }

  /* We do not require matches for:

     - DECL_NAME

       Only the name used in object files matters.

     - DECL_CONTEXT  

       An entity might be declared in a C++ namespace in one file and
       with a C identifier in another file.  

     - TREE_PRIVATE, TREE_PROTECTED

       Access control is the problem of the front end that created the
       object file.  
       
     Therefore, at this point we have decided to merge the declarations.  */
  return true;
}


/* Marks decl DECL as having resolution RESOLUTION. */

static void
lto_symtab_set_resolution_and_file_data (tree decl,
					 ld_plugin_symbol_resolution_t
					 resolution,
					 struct lto_file_decl_data *file_data)
{
  lto_symtab_decl_t new_entry;
  void **slot;

  gcc_assert (decl);

  gcc_assert (TREE_PUBLIC (decl));
  gcc_assert (TREE_CODE (decl) != FUNCTION_DECL || !DECL_ABSTRACT (decl));

  new_entry = GGC_CNEW (struct lto_symtab_decl_def);
  new_entry->base.node = decl;
  new_entry->resolution = resolution;
  new_entry->file_data = file_data;
  
  lto_symtab_maybe_init_hash_tables ();
  slot = htab_find_slot (lto_symtab_decls, new_entry, INSERT);
  gcc_assert (!*slot);
  *slot = new_entry;
}

/* Get the lto_symtab_identifier_def struct associated with ID
   if there is one.  If there is none and INSERT_P is true, create
   a new one.  */

static lto_symtab_identifier_t
lto_symtab_get_identifier (tree id, bool insert_p)
{
  struct lto_symtab_identifier_def temp;
  lto_symtab_identifier_t symtab_id;
  void **slot;

  lto_symtab_maybe_init_hash_tables ();
  temp.base.node = id;
  slot = htab_find_slot (lto_symtab_identifiers, &temp,
			 insert_p ? INSERT : NO_INSERT);
  if (insert_p)
    {
      if (*slot)
	return (lto_symtab_identifier_t) *slot;
      else
	{
	  symtab_id = GGC_CNEW (struct lto_symtab_identifier_def);
	  symtab_id->base.node = id;
	  *slot = symtab_id;
	  return symtab_id;
	}
    }
  else
    return slot ? (lto_symtab_identifier_t) *slot : NULL;
}

/* Return the DECL associated with an IDENTIFIER ID or return NULL_TREE
   if there is none.  */

static tree
lto_symtab_get_identifier_decl (tree id)
{
  lto_symtab_identifier_t symtab_id = lto_symtab_get_identifier (id, false);
  return symtab_id ? symtab_id->decl : NULL_TREE;
}

/* SET the associated DECL of an IDENTIFIER ID to be DECL.  */

static void
lto_symtab_set_identifier_decl (tree id, tree decl)
{
  lto_symtab_identifier_t symtab_id = lto_symtab_get_identifier (id, true);
  symtab_id->decl = decl;
}

/* Common helper function for merging variable and function declarations.
   NEW_DECL is the newly found decl. RESOLUTION is the decl's resolution
   provided by the linker. */

static void
lto_symtab_merge_decl (tree new_decl,
		       enum ld_plugin_symbol_resolution resolution,
		       struct lto_file_decl_data *file_data)
{
  tree old_decl;
  tree name;
  ld_plugin_symbol_resolution_t old_resolution;

  gcc_assert (TREE_CODE (new_decl) == VAR_DECL
	      || TREE_CODE (new_decl) == FUNCTION_DECL);

  gcc_assert (TREE_PUBLIC (new_decl));

  /* Check that declarations reaching this function do not have
     properties inconsistent with having external linkage.  If any of
     these asertions fail, then the object file reader has failed to
     detect these cases and issue appropriate error messages.  */
  /* FIXME lto: The assertion below may fail incorrectly on a static
     class member.  The problem seems to be the (documented) fact
     that DECL_NONLOCAL may be set for class instance variables as
     well as for variables referenced from inner functions.  */
  /*gcc_assert (!DECL_NONLOCAL (new_decl));*/
  if (TREE_CODE (new_decl) == VAR_DECL)
    gcc_assert (!(DECL_EXTERNAL (new_decl) && DECL_INITIAL (new_decl)));

  /* Remember the resolution of this symbol. */
  lto_symtab_set_resolution_and_file_data (new_decl, resolution, file_data);

  /* Retrieve the previous declaration.  */
  name = DECL_ASSEMBLER_NAME (new_decl);
  old_decl = lto_symtab_get_identifier_decl (name);

  /* If there was no previous declaration, then there is nothing to
     merge.  */
  if (!old_decl)
    {
      lto_symtab_set_identifier_decl (name, new_decl);
      VEC_safe_push (tree, gc, lto_global_var_decls, new_decl);
      return;
    }

  /* The linker may ask us to combine two incompatible symbols. */
  if (!lto_symtab_compatible (old_decl, new_decl))
    return;

  old_resolution = lto_symtab_get_resolution (old_decl);
  gcc_assert (resolution != LDPR_UNKNOWN
	      && resolution != LDPR_UNDEF
	      && old_resolution != LDPR_UNKNOWN
	      && old_resolution != LDPR_UNDEF);

  if (resolution == LDPR_PREVAILING_DEF
      || resolution == LDPR_PREVAILING_DEF_IRONLY)
    {
      if (old_resolution == LDPR_PREVAILING_DEF
	  || old_resolution == LDPR_PREVAILING_DEF_IRONLY)
	{
	  error ("%J%qD has already been defined", new_decl, new_decl);
	  error ("%Jpreviously defined here", old_decl);
	  return;
	}
      gcc_assert (old_resolution == LDPR_PREEMPTED_IR
		  || old_resolution ==  LDPR_RESOLVED_IR);
      lto_symtab_set_identifier_decl (name, new_decl);
      return;
    }

  if (resolution == LDPR_PREEMPTED_REG
      || resolution == LDPR_RESOLVED_EXEC
      || resolution == LDPR_RESOLVED_DYN)
    gcc_assert (old_resolution == LDPR_PREEMPTED_REG
		|| old_resolution == LDPR_RESOLVED_EXEC
		|| old_resolution == LDPR_RESOLVED_DYN);

  if (resolution == LDPR_PREEMPTED_IR
      || resolution == LDPR_RESOLVED_IR)
    gcc_assert (old_resolution == LDPR_PREVAILING_DEF
		|| old_resolution == LDPR_PREVAILING_DEF_IRONLY
		|| old_resolution == LDPR_PREEMPTED_IR
		|| old_resolution == LDPR_RESOLVED_IR);

  return;
}


/* Merge the VAR_DECL NEW_VAR with resolution RESOLUTION with any previous
   declaration with the same name. */

void
lto_symtab_merge_var (tree new_var, enum ld_plugin_symbol_resolution resolution)
{
  lto_symtab_merge_decl (new_var, resolution, NULL);
}

/* Merge the FUNCTION_DECL NEW_FN with resolution RESOLUTION with any previous
   declaration with the same name. */

void
lto_symtab_merge_fn (tree new_fn, enum ld_plugin_symbol_resolution resolution,
		     struct lto_file_decl_data *file_data)
{
  lto_symtab_merge_decl (new_fn, resolution, file_data);
}

/* Given the decl DECL, return the prevailing decl with the same name. */

tree
lto_symtab_prevailing_decl (tree decl)
{
  tree ret;
  gcc_assert (decl);

  if (!TREE_PUBLIC (decl))
    return decl;

  /* FIXME lto. There should be no DECL_ABSTRACT in the middle end. */
  if (TREE_CODE (decl) == FUNCTION_DECL && DECL_ABSTRACT (decl))
    return decl;

  ret = lto_symtab_get_identifier_decl (DECL_ASSEMBLER_NAME (decl));

  return ret;
}

/* Return the hash table entry of DECL. */

static struct lto_symtab_decl_def *
lto_symtab_get_symtab_def (tree decl)
{
  struct lto_symtab_decl_def temp, *symtab_decl;
  void **slot;

  gcc_assert (decl);

  lto_symtab_maybe_init_hash_tables ();
  temp.base.node = decl;
  slot = htab_find_slot (lto_symtab_decls, &temp, NO_INSERT);
  gcc_assert (slot && *slot);
  symtab_decl = (struct lto_symtab_decl_def*) *slot;
  return symtab_decl;
}

/* Return the resolution of DECL. */

enum ld_plugin_symbol_resolution
lto_symtab_get_resolution (tree decl)
{
  gcc_assert (decl);

  if (!TREE_PUBLIC (decl))
    return LDPR_PREVAILING_DEF_IRONLY;

  /* FIXME lto: There should be no DECL_ABSTRACT in the middle end. */
  if (TREE_CODE (decl) == FUNCTION_DECL && DECL_ABSTRACT (decl))
    return LDPR_PREVAILING_DEF_IRONLY;

  return lto_symtab_get_symtab_def (decl)->resolution;
}

/* Return the file of DECL. */

struct lto_file_decl_data *
lto_symtab_get_file_data (tree decl)
{
  return lto_symtab_get_symtab_def (decl)->file_data;
}

/* Remove any storage used to store resolution of DECL.  */

void
lto_symtab_clear_resolution (tree decl)
{
  struct lto_symtab_decl_def temp;
  gcc_assert (decl);

  if (!TREE_PUBLIC (decl))
    return;

  /* LTO FIXME: There should be no DECL_ABSTRACT in the middle end. */
 if (TREE_CODE (decl) == FUNCTION_DECL && DECL_ABSTRACT (decl))
    return;

 lto_symtab_maybe_init_hash_tables ();
 temp.base.node = decl;
 htab_remove_elt (lto_symtab_decls, &temp);
}

#include "gt-lto-symtab.h"
