/* Read the GIMPLE representation from a file stream.

   Copyright 2009 Free Software Foundation, Inc.
   Contributed by Kenneth Zadeck <zadeck@naturalbridge.com>

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

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "toplev.h"
#include "tree.h"
#include "expr.h"
#include "flags.h"
#include "params.h"
#include "input.h"
#include "varray.h"
#include "hashtab.h"
#include "basic-block.h"
#include "tree-flow.h"
#include "tree-pass.h"
#include "cgraph.h"
#include "function.h"
#include "ggc.h"
#include "diagnostic.h"
#include "libfuncs.h"
#include "except.h"
#include "debug.h"
#include "vec.h"
#include "timevar.h"
#include "output.h"
#include "ipa-utils.h"
#include "lto-tags.h"
#include "lto-streamer.h"

/* Forward reference to break cyclical dependencies.  */
static tree input_tree_operand (struct lto_input_block *, struct data_in *,
                                tree, enum LTO_tags);
static tree input_local_decl (struct lto_input_block *, struct data_in *,
			      struct function *, unsigned int);
static tree input_expr_operand (struct lto_input_block *, struct data_in *, 
				struct function *, enum LTO_tags);

/* Map between LTO tags and tree codes.  */
static enum tree_code tag_to_expr[LTO_tree_last_tag];

/* The number of flags that are defined for each tree code.  */
static int flags_length_for_code[NUM_TREE_CODES];

/* Data structure used to has file names in the source_location field.  */
struct string_slot
{
  const char *s;
  unsigned int slot_num;
};

/* The table to hold the file_names.  */
static htab_t file_name_hash_table;

/* Return a hash code for P.  */

static hashval_t
hash_string_slot_node (const void *p)
{
  const struct string_slot *ds = (const struct string_slot *) p;
  return (hashval_t) htab_hash_string (ds->s);
}


/* Returns nonzero if P1 and P2 are equal.  */

static int
eq_string_slot_node (const void *p1, const void *p2)
{
  const struct string_slot *ds1 = (const struct string_slot *) p1;
  const struct string_slot *ds2 = (const struct string_slot *) p2;
  return strcmp (ds1->s, ds2->s) == 0;
}


/* Read a string from the string table in DATA_IN using input block
   IB.  Write the length to RLEN.  */

static const char *
input_string_internal (struct data_in *data_in, struct lto_input_block *ib,
		       unsigned int *rlen)
{
  struct lto_input_block str_tab;
  unsigned int len;
  unsigned int loc;
  const char *result;
  
  loc = lto_input_uleb128 (ib);
  LTO_INIT_INPUT_BLOCK (str_tab, data_in->strings, loc, data_in->strings_len);
  len = lto_input_uleb128 (&str_tab);
  *rlen = len;
  gcc_assert (str_tab.p + len <= data_in->strings_len);
  
  result = (const char *)(data_in->strings + str_tab.p);

  return result;
}


/* Read a STRING_CST from the string table in DATA_IN using input
   block IB.  */

static tree
input_string_cst (struct data_in *data_in, struct lto_input_block *ib)
{
  unsigned int len;
  const char * ptr;
  unsigned int is_null;

  is_null = lto_input_uleb128 (ib);
  if (is_null)
    return NULL;

  ptr = input_string_internal (data_in, ib, &len);
  return build_string (len, ptr);
}


/* Read a bitmap from input block IB. If GC_P is true, allocate the
   bitmap in GC memory.  Otherwise,  allocate it on OBSTACK.  If
   OBSTACK is NULL, it is allocated in the default bitmap obstack.  */

static bitmap
input_bitmap (struct lto_input_block *ib, bitmap_obstack *obstack, bool gc_p)
{
  unsigned long num_bits, i;
  bitmap b;

  num_bits = lto_input_uleb128 (ib);
  if (num_bits == 0)
    return NULL;

  if (gc_p)
    b = BITMAP_GGC_ALLOC ();
  else
    b = BITMAP_ALLOC (obstack);

  for (i = 0; i < num_bits; i++)
    {
      unsigned long bit = lto_input_uleb128 (ib);
      bitmap_set_bit (b, bit);
    }

  return b;
}


/* Read an IDENTIFIER from the string table in DATA_IN using input
   block IB.  */

static tree
input_identifier (struct data_in *data_in, struct lto_input_block *ib)
{
  unsigned int len;
  const char *ptr;
  unsigned int is_null;

  is_null = lto_input_uleb128 (ib);
  if (is_null)
    return NULL;

  ptr = input_string_internal (data_in, ib, &len);
  return get_identifier_with_length (ptr, len);
}

/* Read a NULL terminated string from the string table in DATA_IN.  */

static const char *
input_string (struct data_in *data_in, struct lto_input_block *ib)
{
  unsigned int len;
  const char *ptr;
  unsigned int is_null;

  is_null = lto_input_uleb128 (ib);
  if (is_null)
    return NULL;

  ptr = input_string_internal (data_in, ib, &len);
  gcc_assert (ptr[len - 1] == '\0');
  return ptr;
}


/* Read a real constant of type TYPE from DATA_IN using input block
   IB.  */

static tree
input_real (struct lto_input_block *ib, struct data_in *data_in, tree type)
{
  const char *str;
  REAL_VALUE_TYPE value;

  str = input_string (data_in, ib);
  real_from_string (&value, str);

  return build_real (type, value);
}


/* Return the next tag in the input block IB.  */

static enum LTO_tags
input_record_start (struct lto_input_block *ib)
{
  enum LTO_tags tag = (enum LTO_tags) lto_input_1_unsigned (ib);
  return tag;
} 


/* Get the label referenced by the next token in DATA_IN using input
   block IB.  */

static tree 
get_label_decl (struct data_in *data_in, struct lto_input_block *ib)
{
  int ix, nlabels;
  tree label;

  /* A negative IX indicates that the label is an unnamed label.
     These are stored at the back of DATA_IN->LABELS.  */
  ix = lto_input_sleb128 (ib);
  ix = (ix >= 0) ? ix : (int) data_in->num_named_labels - ix;
  nlabels = (int) data_in->num_named_labels + data_in->num_unnamed_labels;
  gcc_assert (ix >= 0 && ix < nlabels);

  label = data_in->labels[ix];
  gcc_assert (!emit_label_in_global_context_p (label));

  return label;
}


/* Read the type referenced by the next token in IB and store it in
   the type table in DATA_IN.  */

static tree
input_type_ref (struct data_in *data_in, struct lto_input_block *ib)
{
  int index;
  tree result;
  enum LTO_tags tag;

  tag = input_record_start (ib);
  if (tag == LTO_type_ref)
    {
      index = lto_input_uleb128 (ib);
      result = lto_file_decl_data_get_type (data_in->file_data, index);
    }
  else
    gcc_unreachable ();

  return result;
}


/* Read the tree flags for CODE from IB, if needed.  If FORCE is true,
   the flags are read regardless of CODE's status in lto_flags_needed_for.  */

static lto_flags_type
input_tree_flags (struct lto_input_block *ib, enum tree_code code, bool force)
{
  lto_flags_type flags;

  if (force || TEST_BIT (lto_flags_needed_for, code))
    flags = lto_input_widest_uint_uleb128 (ib);
  else
    flags = 0;

  return flags;
}


/* Set all of the flag bits inside EXPR by unpacking FLAGS.  */

static void
process_tree_flags (tree expr, lto_flags_type flags)
{
  enum tree_code code = TREE_CODE (expr);

  /* Shift the flags up so that the first flag is at the top of the
     flag word.  */
  flags <<= BITS_PER_LTO_FLAGS_TYPE - flags_length_for_code[code];

#define CLEAROUT (BITS_PER_LTO_FLAGS_TYPE - 1)

#define START_CLASS_SWITCH()              \
  {                                       \
    switch (TREE_CODE_CLASS (code))       \
    {

#define START_CLASS_CASE(class)    case class:

#define ADD_CLASS_DECL_FLAG(flag_name)    \
  { expr->decl_common. flag_name = flags >> CLEAROUT; flags <<= 1; }

#define ADD_CLASS_EXPR_FLAG(flag_name)    \
  { expr->base. flag_name = flags >> CLEAROUT; flags <<= 1; }

#define ADD_CLASS_TYPE_FLAG(flag_name)    \
  { expr->type. flag_name = flags >> CLEAROUT; flags <<= 1; }

#define END_CLASS_CASE(class)      break;

#define END_CLASS_SWITCH()                \
    default:                              \
      gcc_unreachable ();                 \
    }


#define START_EXPR_SWITCH()               \
    switch (code)			  \
    {

#define START_EXPR_CASE(code)    case code:

#define ADD_EXPR_FLAG(flag_name) \
  { expr->base. flag_name = (flags >> CLEAROUT); flags <<= 1; }

#define ADD_TYPE_FLAG(flag_name) \
  { expr->type. flag_name = (flags >> CLEAROUT); flags <<= 1; }

#define ADD_DECL_FLAG(flag_name) \
  { expr->decl_common. flag_name = flags >> CLEAROUT; flags <<= 1; }

#define ADD_VIS_FLAG(flag_name)  \
  { expr->decl_with_vis. flag_name = (flags >> CLEAROUT); flags <<= 1; }

#define ADD_VIS_FLAG_SIZE(flag_name,size)				    \
  { expr->decl_with_vis. flag_name = 					    \
      (enum symbol_visibility) (flags >> (BITS_PER_LTO_FLAGS_TYPE - size)); \
    flags <<= size; }

#define ADD_TLS_FLAG(flag_name,size)					\
  { expr->decl_with_vis. flag_name =					\
      (enum tls_model) (flags >> (BITS_PER_LTO_FLAGS_TYPE - size));	\
    flags <<= size; }

#define ADD_FUN_FLAG(flag_name)  \
  { expr->function_decl. flag_name = (flags >> CLEAROUT); flags <<= 1; }

#define END_EXPR_CASE(class)      break;

#define END_EXPR_SWITCH()                 \
    default:                              \
      gcc_unreachable ();                 \
    }                                     \
  }

#include "lto-tree-flags.def"

#undef START_CLASS_SWITCH
#undef START_CLASS_CASE
#undef ADD_CLASS_DECL_FLAG
#undef ADD_CLASS_EXPR_FLAG
#undef ADD_CLASS_TYPE_FLAG
#undef END_CLASS_CASE
#undef END_CLASS_SWITCH
#undef START_EXPR_SWITCH
#undef START_EXPR_CASE
#undef ADD_EXPR_FLAG
#undef ADD_TYPE_FLAG
#undef ADD_DECL_FLAG
#undef ADD_VIS_FLAG
#undef ADD_VIS_FLAG_SIZE
#undef ADD_TLS_FLAG
#undef ADD_FUN_FLAG
#undef END_EXPR_CASE
#undef END_EXPR_SWITCH
}


/* Lookup STRING in file_name_hash_table.  If found, return the existing
   string, otherwise insert STRING as the canonical version.  */

static const char *
canon_file_name (const char *string)
{
  void **slot;
  struct string_slot s_slot;
  s_slot.s = string;

  slot = htab_find_slot (file_name_hash_table, &s_slot, INSERT);
  if (*slot == NULL)
    {
      size_t len;
      char *saved_string;
      struct string_slot *new_slot;

      len = strlen (string);
      saved_string = (char *) xmalloc (len + 1);
      new_slot = XCNEW (struct string_slot);
      strcpy (saved_string, string);
      new_slot->s = saved_string;
      *slot = new_slot;
      return saved_string;
    }
  else
    {
      struct string_slot *old_slot = (struct string_slot *) *slot;
      return old_slot->s;
    }
}


/* Based on FLAGS read in a file, a line and a column into the
   fields in DATA_IN using input block IB.  */

static void
input_line_info (struct lto_input_block *ib, struct data_in *data_in, 
		 lto_flags_type flags)
{
  gcc_assert (flags & LTO_SOURCE_HAS_LOC);

  if (flags & LTO_SOURCE_FILE)
    {
      if (data_in->current_file)
	linemap_add (line_table, LC_LEAVE, false, NULL, 0);

      data_in->current_file = canon_file_name (input_string (data_in, ib));
    }

  if (flags & LTO_SOURCE_LINE)
    {
      data_in->current_line = lto_input_uleb128 (ib);

      if (!(flags & LTO_SOURCE_FILE))
	linemap_line_start (line_table, data_in->current_line, 80);
    }

  if (flags & LTO_SOURCE_FILE)
    linemap_add (line_table, LC_ENTER, false, data_in->current_file,
		 data_in->current_line);

  if (flags & LTO_SOURCE_COL)
    data_in->current_col = lto_input_uleb128 (ib);
}


/* Set the line info stored in DATA_IN for NODE.  */

static void
set_line_info (struct data_in *data_in, tree node)
{
  if (EXPR_P (node))
    LINEMAP_POSITION_FOR_COLUMN (node->exp.locus, line_table, data_in->current_col);
  else if (DECL_P (node))
    LINEMAP_POSITION_FOR_COLUMN (DECL_SOURCE_LOCATION (node), line_table, data_in->current_col);
}


/* Clear the line info stored in DATA_IN.  */

static void
clear_line_info (struct data_in *data_in)
{
  if (data_in->current_file)
    linemap_add (line_table, LC_LEAVE, false, NULL, 0);
  data_in->current_file = NULL;
  data_in->current_line = 0;
  data_in->current_col = 0;
}


/* Read a tree node from DATA_IN using input block IB.  TAG is the
   expected node that should be found in IB.  FN is the function scope
   for the read tree.  */

static tree
input_expr_operand (struct lto_input_block *ib, struct data_in *data_in, 
		    struct function *fn, enum LTO_tags tag)
{
  enum tree_code code = tag_to_expr[tag];
  tree type = NULL_TREE;
  lto_flags_type flags;
  tree result = NULL_TREE;
  bool needs_line_set;

  if (tag == LTO_null)
    return NULL_TREE;

  if (tag == LTO_type_ref)
    {
      int index = lto_input_uleb128 (ib);
      result = lto_file_decl_data_get_type (data_in->file_data, index);
      return result;
    }
  
  gcc_assert (code);
  if (TEST_BIT (lto_types_needed_for, code))
    type = input_type_ref (data_in, ib);

  flags = input_tree_flags (ib, code, false);

  needs_line_set = (flags & LTO_SOURCE_HAS_LOC);
  if (needs_line_set)
    input_line_info (ib, data_in, flags);

  switch (code)
    {
    case COMPLEX_CST:
      {
	tree elt_type = input_type_ref (data_in, ib);

	result = build0 (code, type);
	if (tag == LTO_complex_cst1)
	  {
	    TREE_REALPART (result) = input_real (ib, data_in, elt_type);
	    TREE_IMAGPART (result) = input_real (ib, data_in, elt_type);
	  }
	else
	  {
	    TREE_REALPART (result) = lto_input_integer (ib, elt_type);
	    TREE_IMAGPART (result) = lto_input_integer (ib, elt_type);
	  }
      }
      break;

    case INTEGER_CST:
      result = lto_input_integer (ib, type);
      break;

    case REAL_CST:
      result = input_real (ib, data_in, type);
      break;

    case STRING_CST:
      result = input_string_cst (data_in, ib);
      TREE_TYPE (result) = type;
      break;

    case IDENTIFIER_NODE:
      result = input_identifier (data_in, ib);
      break;

    case VECTOR_CST:
      {
	tree chain = NULL_TREE;
	int len = lto_input_uleb128 (ib);
	tree elt_type = input_type_ref (data_in, ib);

	if (len && tag == LTO_vector_cst1)
	  {
	    int i;
	    tree last;

	    last = input_real (ib, data_in, elt_type);
	    last = build_tree_list (NULL_TREE, last);
	    chain = last; 
	    for (i = 1; i < len; i++)
	      {
		tree t;
		t = input_real (ib, data_in, elt_type);
		t = build_tree_list (NULL_TREE, t);
		TREE_CHAIN (last) = t;
		last = t;
	      }
	  }
	else
	  {
	    int i;
	    tree t;
	    tree last;

	    t =  lto_input_integer (ib, elt_type);
	    last = build_tree_list (NULL_TREE, t);
	    chain = last; 
	    for (i = 1; i < len; i++)
	      {
		tree t;
		t = lto_input_integer (ib, elt_type);
		t = build_tree_list (NULL_TREE, t);
		TREE_CHAIN (last) = t;
		last = t;
	      }
	  }
	result = build_vector (type, chain);
      }
      break;

    case CASE_LABEL_EXPR:
      {
	int variant = tag - LTO_case_label_expr0;
	tree op0 = NULL_TREE;
	tree op1 = NULL_TREE;
	
	if (variant & 0x1)
	  op0 = input_expr_operand (ib, data_in, fn, 
				    input_record_start (ib));

	if (variant & 0x2)
	  op1 = input_expr_operand (ib, data_in, fn, 
				    input_record_start (ib));

	result = build3 (code, void_type_node, 
			 op0, op1, get_label_decl (data_in, ib));
      }
      break;

    case CONSTRUCTOR:
      {
	VEC(constructor_elt,gc) *vec = NULL;
	unsigned int len = lto_input_uleb128 (ib);
	
	if (len)
	  {
	    unsigned int i = 0;
	    vec = VEC_alloc (constructor_elt, gc, len);
	    for (i = 0; i < len; i++)
	      {
		tree purpose = NULL_TREE;
		tree value;
		constructor_elt *elt; 
		enum LTO_tags ctag;
		
		ctag = input_record_start (ib);
		if (ctag)
		  purpose = input_expr_operand (ib, data_in, fn, ctag);

		ctag = input_record_start (ib);
		value = input_expr_operand (ib, data_in, fn, ctag);
		elt = VEC_quick_push (constructor_elt, vec, NULL);
		elt->index = purpose;
		elt->value = value;
	      }
	  }
	result = build_constructor (type, vec);
      }
      break;

    case SSA_NAME:
      result = VEC_index (tree, SSANAMES (fn), lto_input_uleb128 (ib));
      break;

    case CONST_DECL:
      gcc_unreachable ();

    case FIELD_DECL:
      {
	unsigned index;
	gcc_assert (tag == LTO_field_decl);
	index = lto_input_uleb128 (ib);
        result = lto_file_decl_data_get_field_decl (data_in->file_data, index);
	gcc_assert (result);
      }
      break;

    case FUNCTION_DECL:
      result = lto_file_decl_data_get_fn_decl (data_in->file_data,
					       lto_input_uleb128 (ib));
      gcc_assert (result);
      break;

    case TYPE_DECL:
      gcc_assert (tag == LTO_type_decl);
      result = lto_file_decl_data_get_type_decl (data_in->file_data,
						  lto_input_uleb128 (ib));
      gcc_assert (result);
      break;

    case NAMESPACE_DECL:
      result = lto_file_decl_data_get_namespace_decl (data_in->file_data,
						      lto_input_uleb128 (ib));
      gcc_assert (result);
      break;

    case VAR_DECL:
    case PARM_DECL:
    case RESULT_DECL:
      if (tag == LTO_var_decl1 || tag == LTO_result_decl)
        {
          /* Static or externs are here.  */
          result = lto_file_decl_data_get_var_decl (data_in->file_data,
						    lto_input_uleb128 (ib));
	  if (tag != LTO_result_decl)
	    varpool_mark_needed_node (varpool_node (result));
        }
      else 
	{
	  /* Locals are here.  */
	  int lv_index = lto_input_uleb128 (ib);
	  result = data_in->local_decls[lv_index];
	  if (result == NULL)
	    {
	      /* Create a context to read the local variable so that
		 it does not disturb the position of the code that is
		 calling for the local variable.  This allows locals
		 to refer to other locals.  */
	      struct lto_input_block lib;

	      lib.data = ib->data;
	      lib.len = ib->len;
	      lib.p = data_in->local_decls_index[lv_index];

	      result = input_local_decl (&lib, data_in, fn, lv_index); 
              gcc_assert (TREE_CODE (result) == VAR_DECL
                          || TREE_CODE (result) == PARM_DECL);
	      data_in->local_decls[lv_index] = result;
	    }
	}
      break;

    case LABEL_DECL:
      if (tag == LTO_label_decl1)
	result = lto_file_decl_data_get_label_decl (data_in->file_data,
						    lto_input_uleb128 (ib));
      else
	result = get_label_decl (data_in, ib);
      break;

    case COMPONENT_REF:
      {
	tree op0, op1, op2;
	op0 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	op1 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	op2 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	result = build3 (code, type, op0, op1, op2);
      }
      break;

    case BIT_FIELD_REF:
      {
	tree op0, op1, op2;

	if (tag == LTO_bit_field_ref1)
	  {
	    op1 = build_int_cst_wide (sizetype, lto_input_uleb128 (ib), 0);
	    op2 = build_int_cst_wide (bitsizetype, lto_input_uleb128 (ib), 0);
	    op0 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	  }
	else
	  {
	    op0 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	    op1 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	    op2 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	  }
	result = build3 (code, type, op0, op1, op2);
      }
      break;

    case ARRAY_REF:
    case ARRAY_RANGE_REF:
      {
	tree op0, op1, op2, op3;
	
	op0 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	op1 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	op2 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	op3 = input_expr_operand (ib, data_in, fn, input_record_start (ib));
	result = build4 (code, type, op0, op1, op2, op3);
      }
      break;

    case RANGE_EXPR:
      {
	tree op0 = lto_input_integer (ib, input_type_ref (data_in, ib));
	tree op1 = lto_input_integer (ib, input_type_ref (data_in, ib));
	result = build2 (RANGE_EXPR, sizetype, op0, op1);
      }
      break;

    case TREE_LIST:
      {
	unsigned int count = lto_input_uleb128 (ib);
	tree next = NULL;

	result = NULL_TREE;
	while (count--)
	  {
	    tree value;
	    tree purpose;
	    tree elt;
	    enum LTO_tags tag = input_record_start (ib);

	    if (tag)
	      value = input_expr_operand (ib, data_in, fn, tag);
	    else 
	      value = NULL_TREE;
	    tag = input_record_start (ib);
	    if (tag)
	      purpose = input_expr_operand (ib, data_in, fn, tag);
	    else 
	      purpose = NULL_TREE;

	    elt = build_tree_list (purpose, value);
	    if (result)
	      TREE_CHAIN (next) = elt;
	    else
	      /* Save the first one.  */
	      result = elt;
	    next = elt;
	  }
      }
      break;

      /* This is the default case. All of the cases that can be done
	 completely mechanically are done here.  */
#define SET_NAME(a,b)
#define TREE_SINGLE_MECHANICAL_TRUE
#define MAP_EXPR_TAG(expr,tag) case expr:
#define MAP_STMT_TAG(expr,tag) case expr:
#include "lto-tree-tags.def"
#undef MAP_EXPR_TAG
#undef TREE_SINGLE_MECHANICAL_TRUE
#undef SET_NAME
      {
	tree ops[7];
	int len = TREE_CODE_LENGTH (code);
	int i;
	for (i = 0; i < len; i++)
	  ops[i] = input_expr_operand (ib, data_in, fn, 
				       input_record_start (ib));
	switch (len)
	  {
	  case 0:
	    result = build0 (code, type);
	    break;
	  case 1:
	    result = build1 (code, type, ops[0]);
	    break;
	  case 2:
	    result = build2 (code, type, ops[0], ops[1]);
	    break;
	  case 3:
	    result = build3 (code, type, ops[0], ops[1], ops[2]);
	    break;
	  case 4:
	    result = build4 (code, type, ops[0], ops[1], ops[2], ops[3]);
	    break;
	  case 5:
	    result = build5 (code, type, ops[0], ops[1], ops[2], ops[3], 
			     ops[4]);
	    break;
	  default:
	    gcc_unreachable ();
	  }
      }
      break;

    default:
      /* We cannot have forms that are not explicity handled.  So when
	 this is triggered, there is some form that is not being
	 output.  */
      gcc_unreachable ();
    }

  if (flags)
    {
      /* If we need to set flags on a constant, make a copy to avoid
	 clobbering shared constants.  */
      if (CONSTANT_CLASS_P (result))
	result = copy_node (result);

      process_tree_flags (result, flags);
    }

  if (needs_line_set)
    set_line_info (data_in, result);

  /* It is not enough to just put the flags back as we serialized
     them.  There are side effects to the buildN functions which play
     with the flags to the point that we just have to call this here
     to get it right.  */
  if (code == ADDR_EXPR)
    {
      tree x = get_base_var (result);

      if (TREE_CODE (x) == VAR_DECL || TREE_CODE (x) == PARM_DECL)
	TREE_ADDRESSABLE (x) = 1;

      recompute_tree_invariant_for_addr_expr (result);
    }

  return result;
}


/* Load NAMED_COUNT named labels and constuct UNNAMED_COUNT unnamed
   labels from DATA segment SIZE bytes long using DATA_IN.  IB is the
   input block to read from.  */

static void 
input_labels (struct lto_input_block *ib, struct data_in *data_in, 
	      unsigned int named_count, unsigned int unnamed_count)
{
  unsigned int i;
  tree label;

  clear_line_info (data_in);

  /* The named and unnamed labels share the same array.  In the lto
     code, the unnamed labels have a negative index.  Their position
     in the array can be found by subtracting that index from the
     number of named labels.  */
  data_in->labels = (tree *) xcalloc (named_count + unnamed_count,
				      sizeof (tree));
  data_in->num_named_labels = named_count;
  data_in->num_unnamed_labels = unnamed_count;

  for (i = 0; i < named_count; i++)
    {
      tree name = input_identifier (data_in, ib);
      label = build_decl (UNKNOWN_LOCATION, LABEL_DECL, name, void_type_node);
      DECL_CONTEXT (label) = current_function_decl;
      data_in->labels[i] = label;
    }

  for (i = 0; i < unnamed_count; i++)
    {
      label = build_decl (UNKNOWN_LOCATION, LABEL_DECL, NULL_TREE,
			  void_type_node);
      DECL_CONTEXT (label) = current_function_decl;
      data_in->labels[i + named_count] = label;
    }
}


/* Read the index table for local variables into DATA_IN->LOCAL_DECLS_INDEX
   using input block IB.  COUNT is the number of variables to read.  */

static void
input_local_vars_index (struct lto_input_block *ib, struct data_in *data_in, 
			unsigned int count)
{
  unsigned int i;
  data_in->local_decls_index = (int *) xcalloc (count, sizeof (unsigned int));

  for (i = 0; i < count; i++)
    data_in->local_decls_index[i] = lto_input_uleb128 (ib); 
}


/* Helper for input_local_decl.  Read local variable with index I for
   function FN from DATA_IN using input block IB.  TAG is one of the
   variants of LTO_local_var_decl_body0 or LTO_parm_decl_body0 (see
   lto-tags.h for details on how the variants are encoded).  */

static tree
input_local_var_decl (struct lto_input_block *ib, struct data_in *data_in, 
                      struct function *fn, unsigned int i, enum LTO_tags tag)
{
  unsigned int variant;
  bool is_var;
  tree name = NULL_TREE;
  tree assembler_name = NULL_TREE;
  tree type;
  lto_flags_type flags;
  tree result;

  variant = tag & 0xF;
  is_var = ((tag & 0xFFF0) == LTO_local_var_decl_body0);

  name = input_identifier (data_in, ib);
  assembler_name = input_identifier (data_in, ib);

  type = input_type_ref (data_in, ib);
  gcc_assert (type);

  if (is_var)
    result = build_decl (UNKNOWN_LOCATION, VAR_DECL, name, type);
  else
    result = build_decl (UNKNOWN_LOCATION, PARM_DECL, name, type);

  if (assembler_name != NULL_TREE)
    SET_DECL_ASSEMBLER_NAME (result, assembler_name);

  data_in->local_decls[i] = result;
  
  if (is_var)
    {
      int index;

      tag = input_record_start (ib);
      if (tag)
	DECL_INITIAL (result) = input_expr_operand (ib, data_in, fn, tag);

      index = lto_input_sleb128 (ib);
      if (index != -1)
	data_in->local_decl_indexes[index] = i;
    }
  else
    {
      DECL_ARG_TYPE (result) = input_type_ref (data_in, ib);
      tag = input_record_start (ib);
      if (tag)
	TREE_CHAIN (result) = input_expr_operand (ib, data_in, fn, tag);
      else 
	TREE_CHAIN (result) = NULL_TREE;
    }

  flags = input_tree_flags (ib, ERROR_MARK, true);

  if (flags & LTO_SOURCE_HAS_LOC)
    {
      input_line_info (ib, data_in, flags);
      set_line_info (data_in, result);
    }

  DECL_CONTEXT (result) = fn->decl;

  DECL_ALIGN (result) = lto_input_uleb128 (ib);

  tag = input_record_start (ib);
  if (tag)
    DECL_SIZE (result) = input_expr_operand (ib, data_in, fn, tag);
  else
    DECL_SIZE (result) = NULL_TREE;

  if (variant & 0x1)
    DECL_ATTRIBUTES (result) = input_expr_operand (ib, data_in, fn,
						   input_record_start (ib));

  if (variant & 0x2)
    DECL_SIZE_UNIT (result) 
      = input_expr_operand (ib, data_in, fn, input_record_start (ib));

  if (variant & 0x4)
    {
      tag = input_record_start (ib);
      gcc_assert (tag);
      SET_DECL_DEBUG_EXPR (result, input_expr_operand (ib, data_in, fn, tag));
    }

  process_tree_flags (result, flags);

  if (DECL_HAS_VALUE_EXPR_P (result))
    {
      tag = input_record_start (ib);
      gcc_assert (tag);
      SET_DECL_VALUE_EXPR (result, input_expr_operand (ib, data_in, fn, tag));
    }

  return result;
}


/* Read local symbol with index I for function FN from DATA_IN using
   input block IB.  */

static tree
input_local_decl (struct lto_input_block *ib, struct data_in *data_in, 
                  struct function *fn, unsigned int i)
{
  enum LTO_tags tag;
  tree result;

  /* The line number info needs to be reset for each local decl since
     they are read in random order.  */
  clear_line_info (data_in);

  tag = input_record_start (ib);

  /* FIXME: Use LTO_*_body nomenclature for fields and types?
     Since we are reading from a separate local_decls stream,
     re-use of the tags for a different purpose doesn't break
     anything, but is perhaps ugly.  */
  if ((tag & 0xFFF0) == LTO_parm_decl_body0
      || (tag & 0xFFF0) == LTO_local_var_decl_body0)
    result = input_local_var_decl (ib, data_in, fn, i, tag);
  else
    gcc_unreachable ();

  return result;
}


/* Read COUNT local variables and parameters in function FN from
   DATA_IN using input block IB.  */

static void 
input_local_vars (struct lto_input_block *ib, struct data_in *data_in, 
		  struct function *fn, unsigned int count)
{
  int i;
  enum LTO_tags tag;

  data_in->local_decl_indexes = (int *) xcalloc (count, sizeof (int));
  data_in->local_decls = (tree *) xcalloc (count, sizeof (tree*));

  memset (data_in->local_decl_indexes, -1, count * sizeof (int));

  /* Recreate the local_var.  Put the statics at the end.*/
  fn->local_decls = NULL;
  tag = input_record_start (ib);
  
  while (tag)
    {
      tree var;
      
      var = input_expr_operand (ib, data_in, fn, tag);
      fn->local_decls = tree_cons (NULL_TREE, var, fn->local_decls);
      DECL_CONTEXT (var) = NULL_TREE;

      tag = input_record_start (ib);
      if (tag)
	DECL_INITIAL (var) = input_expr_operand (ib, data_in, fn, tag);

      /* Statics never have external visibility.  */
      DECL_EXTERNAL (var) = 0;

      /* Next static.  */
      tag = input_record_start (ib);
    }

  for (i = 0; i < (int) count; i++)
    if (!data_in->local_decls[i])
      {
	/* Some local decls may have already been read in if they are
	   used as part of a previous local decl.  */
	ib->p = data_in->local_decls_index[i];
	input_local_decl (ib, data_in, fn, i);
      }

  /* Add the regular locals in the proper order.  */
  for (i = count - 1; i >= 0; i--)
    if (data_in->local_decl_indexes[i] != -1)
      fn->local_decls 
	= tree_cons (NULL_TREE, 
		     data_in->local_decls[data_in->local_decl_indexes[i]],
		     fn->local_decls);

  free (data_in->local_decl_indexes);
  data_in->local_decl_indexes = NULL;
}


/* Read and return EH region REGION_NUMBER from DATA_IN using input
   block IB.  FN is the function being processed.  */

static eh_region
input_eh_region (struct lto_input_block *ib, struct data_in *data_in,
		 struct function *fn, int region_number)
{
  enum LTO_tags tag, label_tag;
  eh_region r;

  /* Read the region header.  */
  tag = input_record_start (ib);
  if (tag == 0)
    return NULL;

  /* If TAG indicates that this is a shared region, then return a NULL
     region.  The caller is responsible for sharing EH regions in the
     EH table using the AKA bitmaps.  */
  if (tag == LTO_eh_table_shared_region)
    return NULL;

  r = GGC_CNEW (struct eh_region_d);
  r->region_number = lto_input_sleb128 (ib);
  r->aka = input_bitmap (ib, NULL, true);

  gcc_assert (r->region_number == region_number);

  /* Read all the region pointers as region numbers.  We'll fix up
     the pointers once the whole array has been read.  */
  r->outer = (eh_region) (intptr_t) lto_input_uleb128 (ib);
  r->inner = (eh_region) (intptr_t) lto_input_uleb128 (ib);
  r->next_peer = (eh_region) (intptr_t) lto_input_uleb128 (ib);
  label_tag = input_record_start (ib);
  if (label_tag)
    r->tree_label = input_expr_operand (ib, data_in, fn, label_tag);

  if (tag == LTO_eh_table_cleanup1
      || tag == LTO_eh_table_try1
      || tag == LTO_eh_table_catch1
      || tag == LTO_eh_table_allowed1
      || tag == LTO_eh_table_must_not_throw1
      || tag == LTO_eh_table_throw1)
    r->may_contain_throw = 1;

  switch (tag)
    {
      case LTO_eh_table_cleanup0:
      case LTO_eh_table_cleanup1:
	r->type = ERT_CLEANUP;
	break;

      case LTO_eh_table_try0:
      case LTO_eh_table_try1:
	r->type = ERT_TRY;
	r->u.eh_try.eh_catch = (eh_region) (intptr_t) lto_input_uleb128 (ib);
	r->u.eh_try.last_catch = (eh_region) (intptr_t) lto_input_uleb128 (ib);
	break;

      case LTO_eh_table_catch0:
      case LTO_eh_table_catch1:
	r->type = ERT_CATCH;
	r->u.eh_catch.next_catch 
	  = (eh_region) (intptr_t) lto_input_uleb128 (ib);
	r->u.eh_catch.prev_catch 
	  = (eh_region) (intptr_t) lto_input_uleb128 (ib);
	if (input_record_start (ib))
	  {
	    tree list = input_expr_operand (ib, data_in, fn, LTO_tree_list);
	    r->u.eh_catch.type_list = list;
	    for (; list; list = TREE_CHAIN (list))
	      add_type_for_runtime (TREE_VALUE (list));
	  }

	if (input_record_start (ib))
	  r->u.eh_catch.filter_list = input_expr_operand (ib, data_in, fn,
							  LTO_tree_list);
	break;

      case LTO_eh_table_allowed0:
      case LTO_eh_table_allowed1:
	r->type = ERT_ALLOWED_EXCEPTIONS;
	if (input_record_start (ib))
	  {
	    tree list = input_expr_operand (ib, data_in, fn, LTO_tree_list);
	    r->u.allowed.type_list = list;
	    for (; list ; list = TREE_CHAIN (list))
	      add_type_for_runtime (TREE_VALUE (list));
	  }
	r->u.allowed.filter = lto_input_uleb128 (ib);
	break;

      case LTO_eh_table_must_not_throw0:
      case LTO_eh_table_must_not_throw1:
	r->type = ERT_MUST_NOT_THROW;
	break;

      case LTO_eh_table_throw0:
      case LTO_eh_table_throw1:
	r->type = ERT_THROW;
	r->u.eh_throw.type = input_type_ref (data_in, ib);
	break;

      default:
	gcc_unreachable ();
    }

  return r;
}


/* After reading the EH regions, pointers to peer and children regions
   are region numbers.  This converts all these region numbers into
   real pointers into the rematerialized regions for FN.  ROOT_REGION
   is the region number for the root EH region in FN.  */

static void
fixup_eh_region_pointers (struct function *fn, HOST_WIDE_INT root_region)
{
  unsigned i;
  VEC(eh_region,gc) *array = fn->eh->region_array;
  eh_region r;

#define fixup_region(r) (r) = VEC_index (eh_region, array, \
					 (HOST_WIDE_INT) (intptr_t) (r))

  gcc_assert (array);

  /* A root region with value -1 means that there is not a region tree
     for this function.  However, we may still have an EH table with
     statements in it.  FIXME, this is a bug in the generic EH code.  */
  if (root_region >= 0)
    fn->eh->region_tree = VEC_index (eh_region, array, root_region);

  for (i = 0; VEC_iterate (eh_region, array, i, r); i++)
    {
      if (r == NULL)
	continue;

      /* If R is a shared EH region, then its region number will be
	 that of its original EH region.  Skip these, since they only
	 need to be fixed up when processing the original region.  */
      if (i != (unsigned) r->region_number)
	continue;

      fixup_region (r->outer);
      fixup_region (r->inner);
      fixup_region (r->next_peer);

      if (r->type == ERT_TRY)
	{
	  fixup_region (r->u.eh_try.eh_catch);
	  fixup_region (r->u.eh_try.last_catch);
	}
      else if (r->type == ERT_CATCH)
	{
	  fixup_region (r->u.eh_catch.next_catch);
	  fixup_region (r->u.eh_catch.prev_catch);
	}

      /* If R has an AKA set, all the table slot for the regions
	 mentioned in AKA must point to R.  */
      if (r->aka)
	{
	  bitmap_iterator bi;
	  unsigned i;

	  EXECUTE_IF_SET_IN_BITMAP (r->aka, 0, i, bi)
	    VEC_replace (eh_region, array, i, r);
	}
    }

#undef fixup_region
}


/* Return the runtime type for type T.  For LTO, we assume that each
   front end has generated the appropriate runtime types (see
   output_eh_region), so there is nothing for us to do here.  */

static tree
lto_eh_runtime_type (tree t)
{
  return t;
}


/* Initialize EH support.  */

static void
lto_init_eh (void)
{
  /* Contrary to most other FEs, we only initialize EH support when at
     least one of the files in the set contains exception regions in
     it.  Since this happens much later than the call to init_eh in
     lang_dependent_init, we have to set flag_exceptions and call
     init_eh again to initialize the EH tables.  */
  flag_exceptions = 1;
  init_eh ();

  /* Initialize dwarf2 tables.  Since dwarf2out_do_frame() returns
     true only when exceptions are enabled, this initialization is
     never done during lang_dependent_init.  */
#if defined DWARF2_DEBUGGING_INFO || defined DWARF2_UNWIND_INFO
  if (dwarf2out_do_frame ())
    dwarf2out_frame_init ();
#endif

  default_init_unwind_resume_libfunc ();
  lang_eh_runtime_type = lto_eh_runtime_type;
}


/* Read the exception table for FN from IB using the data descriptors
   in DATA_IN.  */

static void
input_eh_regions (struct lto_input_block *ib, struct data_in *data_in,
		  struct function *fn)
{
  HOST_WIDE_INT i, last_region, root_region, len;
  enum LTO_tags tag;
  
  tag = input_record_start (ib);
  if (tag == LTO_eh_table)
    {
      static bool eh_initialized_p = false;

      /* If the file contains EH regions, then it was compiled with
	 -fexceptions.  In that case, initialize the backend EH
	 machinery.  */
      if (!eh_initialized_p)
	{
	  lto_init_eh ();
	  eh_initialized_p = true;
	}

      gcc_assert (fn->eh);

      last_region = lto_input_sleb128 (ib);
      fn->eh->last_region_number = last_region;

      root_region = lto_input_sleb128 (ib);

      /* Fill in the EH region array.  */
      len = lto_input_sleb128 (ib);
      if (len > 0)
	{
	  VEC_safe_grow (eh_region, gc, fn->eh->region_array, len);
	  for (i = 0; i < len; i++)
	    {
	      eh_region r = input_eh_region (ib, data_in, fn, i);
	      VEC_replace (eh_region, fn->eh->region_array, i, r);
	    }

	  /* Reconstruct the EH region tree by fixing up the
	     peer/children pointers.  */
	  fixup_eh_region_pointers (fn, root_region);
	}

      tag = input_record_start (ib);
      gcc_assert (tag == LTO_null);
    }
}


/* Make a new basic block with index INDEX in function FN.  */

static basic_block
make_new_block (struct function *fn, unsigned int index)
{
  basic_block bb = alloc_block ();
  bb->index = index;
  SET_BASIC_BLOCK_FOR_FUNCTION (fn, index, bb);
  bb->il.gimple = GGC_CNEW (struct gimple_bb_info);
  n_basic_blocks_for_function (fn)++;
  bb->flags = 0;
  set_bb_seq (bb, gimple_seq_alloc ());
  return bb;
}


/* Read the CFG for function FN from input block IB.  */

static void 
input_cfg (struct lto_input_block *ib, struct function *fn)
{
  unsigned int bb_count;
  basic_block p_bb;
  unsigned int i;
  int index;

  init_empty_tree_cfg_for_function (fn);
  init_ssa_operands ();

  profile_status_for_function (fn) = 
    (enum profile_status_d) lto_input_uleb128 (ib);

  bb_count = lto_input_uleb128 (ib);

  last_basic_block_for_function (fn) = bb_count;
  if (bb_count > VEC_length (basic_block, basic_block_info_for_function (fn)))
    VEC_safe_grow_cleared (basic_block, gc,
			   basic_block_info_for_function (fn), bb_count);

  if (bb_count > VEC_length (basic_block, label_to_block_map_for_function (fn)))
    VEC_safe_grow_cleared (basic_block, gc, 
			   label_to_block_map_for_function (fn), bb_count);

  index = lto_input_sleb128 (ib);
  while (index != -1)
    {
      basic_block bb = BASIC_BLOCK_FOR_FUNCTION (fn, index);
      unsigned int edge_count;

      if (bb == NULL)
	bb = make_new_block (fn, index);

      edge_count = lto_input_uleb128 (ib);

      /* Connect up the CFG.  */
      for (i = 0; i < edge_count; i++)
	{
	  unsigned int dest_index;
	  unsigned int edge_flags;
	  basic_block dest;
	  int probability;
	  gcov_type count;
	  edge e;

	  dest_index = lto_input_uleb128 (ib);
	  probability = (int) lto_input_sleb128 (ib);
	  count = (gcov_type) lto_input_sleb128 (ib);
	  edge_flags = lto_input_uleb128 (ib);

	  dest = BASIC_BLOCK_FOR_FUNCTION (fn, dest_index);

	  if (dest == NULL) 
	    dest = make_new_block (fn, dest_index);

	  e = make_edge (bb, dest, edge_flags);
	  e->probability = probability;
	  e->count = count;
	}

      index = lto_input_sleb128 (ib);
    }

  p_bb = ENTRY_BLOCK_PTR_FOR_FUNCTION(fn);
  index = lto_input_sleb128 (ib);
  while (index != -1)
    {
      basic_block bb = BASIC_BLOCK_FOR_FUNCTION (fn, index);
      bb->prev_bb = p_bb;
      p_bb->next_bb = bb;
      p_bb = bb;
      index = lto_input_sleb128 (ib);
    }
}


/* Read a PHI function for basic block BB in function FN.  DATA_IN is
   the file being read.  IB is the input block to use for reading.  */

static gimple
input_phi (struct lto_input_block *ib, basic_block bb, struct data_in *data_in,
	   struct function *fn)
{
  unsigned HOST_WIDE_INT ix;
  tree phi_result;
  int i, len;
  gimple result;

  ix = lto_input_uleb128 (ib);
  phi_result = VEC_index (tree, SSANAMES (fn), ix);
  len = EDGE_COUNT (bb->preds);
  result = create_phi_node (phi_result, bb);
  SSA_NAME_DEF_STMT (phi_result) = result;

  /* We have to go through a lookup process here because the preds in the
     reconstructed graph are generally in a different order than they
     were in the original program.  */
  for (i = 0; i < len; i++)
    {
      tree def = input_expr_operand (ib, data_in, fn, input_record_start (ib));
      int src_index = lto_input_uleb128 (ib);
      basic_block sbb = BASIC_BLOCK_FOR_FUNCTION (fn, src_index);
      
      edge e = NULL;
      int j;
      
      for (j = 0; j < len; j++)
	if (EDGE_PRED (bb, j)->src == sbb)
	  {
	    e = EDGE_PRED (bb, j);
	    break;
	  }

      add_phi_arg (result, def, e); 
    }

  return result;
}


/* Read the SSA names array for function FN from DATA_IN using input
   block IB.  */

static void
input_ssa_names (struct lto_input_block *ib, struct data_in *data_in,
		 struct function *fn)
{
  unsigned int i;
  int size;

  size = lto_input_uleb128 (ib);
  init_ssanames (fn, size);
  i = lto_input_uleb128 (ib);

  while (i)
    {
      tree ssa_name;
      tree name;
      lto_flags_type flags;

      /* Skip over the elements that had been freed.  */
      while (VEC_length (tree, SSANAMES (fn)) < i)
	VEC_quick_push (tree, SSANAMES (fn), NULL_TREE);

      name = input_expr_operand (ib, data_in, fn, input_record_start (ib));
      ssa_name = make_ssa_name_fn (fn, name, gimple_build_nop ());

      flags = input_tree_flags (ib, ERROR_MARK, true);

      /* Bug fix for handling debug info previously omitted.
         See comment in output_tree_flags, which failed to emit
         the flags debug info in some cases.  */
      process_tree_flags (ssa_name, flags);
      if (SSA_NAME_IS_DEFAULT_DEF (ssa_name))
	set_default_def (SSA_NAME_VAR (ssa_name), ssa_name);
      i = lto_input_uleb128 (ib);
    } 
}


/* Read location information from input block IB using the descriptors
   in DATA_IN.  */

static location_t
input_stmt_location (struct lto_input_block *ib, struct data_in *data_in)
{
  location_t loc;
  const char *file;
  HOST_WIDE_INT line, column;

  file = input_string (data_in, ib);
  if (file == NULL)
    return UNKNOWN_LOCATION;

  file = canon_file_name (file);
  line = lto_input_sleb128 (ib);
  column = lto_input_sleb128 (ib);

  if (file != data_in->current_file)
    {
      data_in->current_file = file;
      linemap_add (line_table, LC_LEAVE, false, NULL, 0);
    }

  if (line != data_in->current_line)
    {
      data_in->current_line = line;
      if (!file)
	linemap_line_start (line_table, data_in->current_line, 80);
    }

  linemap_add (line_table, LC_ENTER, false, data_in->current_file,
	       data_in->current_line);

  if (column != data_in->current_col)
    data_in->current_col = column;

  LINEMAP_POSITION_FOR_COLUMN (loc, line_table, data_in->current_col);

  return loc;
}


/* Read a statement with tag TAG in function FN from block IB using
   descriptors in DATA_IN.  */

static gimple
input_gimple_stmt (struct lto_input_block *ib, struct data_in *data_in,
		   struct function *fn, enum LTO_tags tag)
{
  gimple stmt;
  enum gimple_code code;
  unsigned HOST_WIDE_INT num_ops;
  size_t i, nbytes;
  char *buf;
  location_t location;
  tree block;

  if (tag == LTO_gimple_asm)
    code = GIMPLE_ASM;
  else if (tag == LTO_gimple_assign)
    code = GIMPLE_ASSIGN;
  else if (tag == LTO_gimple_call)
    code = GIMPLE_CALL;
  else if (tag == LTO_gimple_cond)
    code = GIMPLE_COND;
  else if (tag == LTO_gimple_goto)
    code = GIMPLE_GOTO;
  else if (tag == LTO_gimple_label)
    code = GIMPLE_LABEL;
  else if (tag == LTO_gimple_return)
    code = GIMPLE_RETURN;
  else if (tag == LTO_gimple_switch)
    code = GIMPLE_SWITCH;
  else if (tag == LTO_gimple_resx)
    code = GIMPLE_RESX;
  else if (tag == LTO_gimple_predict)
    code = GIMPLE_PREDICT;
  else
    gcc_unreachable ();

  /* Read the number of operands in the statement.  */
  num_ops = lto_input_uleb128 (ib);

  /* Read location information.  */
  location = input_stmt_location (ib, data_in);

  /* Read lexical block reference.  */
  block = input_tree (ib, data_in);

  /* Read the tuple header.  FIXME lto.  This seems unnecessarily slow
     and it is reading pointers in the tuple that need to be re-built
     locally (e.g, basic block, lexical block, operand vectors, etc).  */
  nbytes = gimple_size (code);
  stmt = gimple_alloc (code, num_ops);
  buf = (char *) stmt;
  for (i = 0; i < nbytes; i++)
    buf[i] = lto_input_1_unsigned (ib);

  /* Read in all the operands.  */
  if (code == GIMPLE_ASM)
    {
      /* FIXME lto.  Move most of this into a new gimple_asm_set_string().  */
      tree str = input_string_cst (data_in, ib);
      stmt->gimple_asm.string = TREE_STRING_POINTER (str);
    }

  for (i = 0; i < num_ops; i++)
    {
      enum LTO_tags tag = input_record_start (ib);
      if (tag)
	{
	  /* FIXME lto.  We shouldn't be writing NULL operands.  Use
	     alternate tags to identify tuple variants (e.g.,
	     GIMPLE_CALLs without a return value).  */
	  tree op = input_expr_operand (ib, data_in, fn, tag);
	  gimple_set_op (stmt, i, op);
	}
    }

  /* Update the properties of symbols, SSA names and labels associated
     with STMT.  */
  if (code == GIMPLE_ASSIGN || code == GIMPLE_CALL)
    {
      tree lhs = gimple_get_lhs (stmt);
      if (lhs && TREE_CODE (lhs) == SSA_NAME)
	SSA_NAME_DEF_STMT (lhs) = stmt;
    }
  else if (code == GIMPLE_LABEL)
    gcc_assert (emit_label_in_global_context_p (gimple_label_label (stmt))
	        || DECL_CONTEXT (gimple_label_label (stmt)) == fn->decl);
  else if (code == GIMPLE_ASM)
    {
      unsigned i;

      for (i = 0; i < gimple_asm_noutputs (stmt); i++)
	{
	  tree op = TREE_VALUE (gimple_asm_output_op (stmt, i));
	  if (TREE_CODE (op) == SSA_NAME)
	    SSA_NAME_DEF_STMT (op) = stmt;
	}
    }

  /* Clear out invalid pointer values read above.  FIXME lto, this
     should disappear after we fix the unnecessary fields that are
     written for every tuple.  */
  gimple_set_bb (stmt, NULL);
  gimple_set_block (stmt, block);
  if (gimple_has_ops (stmt))
    {
      gimple_set_def_ops (stmt, NULL);
      gimple_set_use_ops (stmt, NULL);
    }

  if (gimple_has_mem_ops (stmt))
    {
      gimple_set_vdef (stmt, NULL);
      gimple_set_vuse (stmt, NULL);
    }

  /* Mark the statement modified so its operand vectors can be filled in.  */
  gimple_set_modified (stmt, true);

  /* Set location information for STMT.  */
  gimple_set_location (stmt, location);

  return stmt;
}

 
/* Read a basic block with tag TAG from DATA_IN using input block IB.
   FN is the function being processed.  */

static void
input_bb (struct lto_input_block *ib, enum LTO_tags tag, 
	  struct data_in *data_in, struct function *fn)
{
  unsigned int index;
  basic_block bb;
  gimple_stmt_iterator bsi;
  HOST_WIDE_INT curr_eh_region;

  /* This routine assumes that CFUN is set to FN, as it needs to call
     basic GIMPLE routines that use CFUN.  */
  gcc_assert (cfun == fn);

  index = lto_input_uleb128 (ib);
  bb = BASIC_BLOCK_FOR_FUNCTION (fn, index);

  bb->count = lto_input_sleb128 (ib);
  bb->loop_depth = lto_input_sleb128 (ib);
  bb->frequency = lto_input_sleb128 (ib);
  bb->flags = lto_input_sleb128 (ib);

  /* LTO_bb1 has statements.  LTO_bb0 does not.  */
  if (tag == LTO_bb0)
    return;

  curr_eh_region = -1;
  bsi = gsi_start_bb (bb);
  tag = input_record_start (ib);
  while (tag)
    {
      gimple stmt = input_gimple_stmt (ib, data_in, fn, tag);
      find_referenced_vars_in (stmt);
      gimple_set_block (stmt, DECL_INITIAL (fn->decl));
      gsi_insert_after (&bsi, stmt, GSI_NEW_STMT);

      /* After the statement, expect a 0 delimiter or the EH region
	 that the previous statement belongs to.  */
      tag = input_record_start (ib);
      gcc_assert (tag == LTO_set_eh1 || tag == LTO_set_eh0 || tag == LTO_null);

      if (tag == LTO_set_eh1 || tag == LTO_set_eh0)
	{
	  HOST_WIDE_INT region = 0;

	  if (tag == LTO_set_eh1)
	    region = lto_input_sleb128 (ib);

	  if (region != curr_eh_region)
	    curr_eh_region = region;
	}

      if (curr_eh_region >= 0)
	{
	  gcc_assert (curr_eh_region <= num_eh_regions ());
	  add_stmt_to_eh_region (stmt, curr_eh_region);
	}

      tag = input_record_start (ib);
    }

  tag = input_record_start (ib);
  while (tag)
    {
      gimple phi = input_phi (ib, bb, data_in, fn);
      find_referenced_vars_in (phi);
      tag = input_record_start (ib);
    }
}

/* Go through all NODE edges and fixup call_stmt pointers
   so they point to STMTS.  */

static void
fixup_call_stmt_edges_1 (struct cgraph_node *node, gimple *stmts)
{
  struct cgraph_edge *cedge;
  for (cedge = node->callees; cedge; cedge = cedge->next_callee)
    cedge->call_stmt = stmts[cedge->lto_stmt_uid];
}

/* Fixup call_stmt pointers in NODE and all clones.  */

static void
fixup_call_stmt_edges (struct cgraph_node *orig, gimple *stmts)
{
  struct cgraph_node *node;

  while (orig->clone_of)
    orig = orig->clone_of;

  fixup_call_stmt_edges_1 (orig, stmts);
  if (orig->clones)
    for (node = orig->clones; node != orig;)
      {
	fixup_call_stmt_edges_1 (node, stmts);
	if (node->clones)
	  node = node->clones;
	else if (node->next_sibling_clone)
	  node = node->next_sibling_clone;
	else
	  {
	    while (node != orig && !node->next_sibling_clone)
	      node = node->clone_of;
	    if (node != orig)
	      node = node->next_sibling_clone;
	  }
      }
}

/* Read the body of function FN_DECL from DATA_IN using input block IB.  */

static void
input_function (tree fn_decl, struct data_in *data_in, 
		struct lto_input_block *ib)
{
  struct function *fn;
  enum LTO_tags tag;
  gimple *stmts;
  basic_block bb;
  unsigned HOST_WIDEST_INT flags;

  fn = DECL_STRUCT_FUNCTION (fn_decl);
  tag = input_record_start (ib);
  clear_line_info (data_in);

  gimple_register_cfg_hooks ();
  gcc_assert (tag == LTO_function);

  /* Read all the attributes for FN.  Note that flags are decoded in
     the opposite order that they were encoded by output_function.  */
  flags = lto_input_widest_uint_uleb128 (ib);

  fn->va_list_gpr_size = lto_get_flags (&flags, 8);
  fn->va_list_fpr_size = lto_get_flags (&flags, 8);
  fn->function_frequency = (enum function_frequency) lto_get_flags (&flags, 2);
  fn->calls_setjmp = lto_get_flag (&flags);
  fn->calls_alloca = lto_get_flag (&flags);
  fn->has_nonlocal_label = lto_get_flag (&flags);
  fn->stdarg = lto_get_flag (&flags);
  fn->dont_save_pending_sizes_p = lto_get_flag (&flags);
  fn->after_inlining = lto_get_flag (&flags);
  fn->always_inline_functions_inlined = lto_get_flag (&flags);
  fn->returns_struct = lto_get_flag (&flags);
  fn->returns_pcc_struct = lto_get_flag (&flags);
  fn->after_tree_profile = lto_get_flag (&flags);
  fn->has_local_explicit_reg_vars = lto_get_flag (&flags);
  fn->is_thunk = lto_get_flag (&flags);

  /* Read the static chain and non-local goto save area.  */
  tag = input_record_start (ib);
  if (tag)
    fn->static_chain_decl = input_expr_operand (ib, data_in, fn, tag);

  tag = input_record_start (ib);
  if (tag)
    fn->nonlocal_goto_save_area = input_expr_operand (ib, data_in, fn, tag);

  /* Read the exception handling regions in the function.  */
  input_eh_regions (ib, data_in, fn);

  /* Read the tree of lexical scopes for the function.  */
  DECL_INITIAL (fn_decl) = input_tree (ib, data_in);
  if (DECL_INITIAL (fn_decl) == NULL_TREE)
    {
      DECL_INITIAL (fn_decl) = make_node (BLOCK);
      BLOCK_ABSTRACT_ORIGIN (DECL_SAVED_TREE (fn_decl)) = fn_decl;
    }
  DECL_SAVED_TREE (fn_decl) = DECL_INITIAL (fn_decl);

  tag = input_record_start (ib);
  if (tag)
    DECL_ARGUMENTS (fn_decl) = input_expr_operand (ib, data_in, fn, tag); 

  /* Read all the basic blocks.  */
  tag = input_record_start (ib);
  while (tag)
    {
      input_bb (ib, tag, data_in, fn);
      tag = input_record_start (ib);
    }

  /* Fix up the call statements that are mentioned in the callgraph
     edges.  */
  renumber_gimple_stmt_uids ();
  stmts = (gimple *) xcalloc (gimple_stmt_max_uid (fn), sizeof (gimple));
  FOR_ALL_BB (bb)
    {
      gimple_stmt_iterator bsi;
      for (bsi = gsi_start_bb (bb); !gsi_end_p (bsi); gsi_next (&bsi))
	{
	  gimple stmt = gsi_stmt (bsi);
	  stmts[gimple_uid (stmt)] = stmt;
	}
    }

  /* Set the gimple body to the statement sequence in the entry
     basic block.  FIXME lto, this is fairly hacky.  The existence
     of a gimple body is used by the cgraph routines, but we should
     really use the presence of the CFG.  */
  {
    edge_iterator ei = ei_start (ENTRY_BLOCK_PTR->succs);
    gimple_set_body (fn_decl, bb_seq (ei_edge (ei)->dest));
  }

  fixup_call_stmt_edges (cgraph_node (fn_decl), stmts);

  update_ssa (TODO_update_ssa_only_virtuals); 
  free (stmts);
}


/* Read initializer expressions for public statics.  DATA_IN is the
   file being read.  IB is the input block used for reading.  */

static void
input_constructors_or_inits (struct data_in *data_in, 
			     struct lto_input_block *ib)
{
  enum LTO_tags tag;

  clear_line_info (data_in);
  tag = input_record_start (ib);
  while (tag)
    {
      tree var;
      var = input_expr_operand (ib, data_in, NULL, tag);
      tag = input_record_start (ib);
      if (tag)
	DECL_INITIAL (var) = input_expr_operand (ib, data_in, NULL, tag);
      tag = input_record_start (ib);
    }

  tag = input_record_start (ib);
  while (tag)
    {
      const char *orig_name, *new_name;
      alias_pair *p = VEC_safe_push (alias_pair, gc, alias_pairs, NULL);
      p->decl = input_expr_operand (ib, data_in, NULL, tag);
      tag = input_record_start (ib);
      p->target = input_expr_operand (ib, data_in, NULL, tag);

      /* If the target is a static object, we may have registered a
	 new name for it to avoid clashes between statics coming from
	 different files.  In that case, use the new name.  */
      orig_name = IDENTIFIER_POINTER (p->target);
      new_name = lto_get_decl_name_mapping (data_in->file_data, orig_name);
      if (strcmp (orig_name, new_name) != 0)
	p->target = get_identifier (new_name);

      tag = input_record_start (ib);
    }
}


/* Static initialization for the LTO reader.  */

void
lto_init_reader (void)
{
  static bool initialized_local = false;

  if (initialized_local)
    return;

  initialized_local = true;

  /* Initialize the expression to tag mapping.  */
#define MAP_EXPR_TAG(expr,tag)   tag_to_expr [tag] = expr;
#define MAP_EXPR_TAGS(expr,tag,count)	\
    {					\
      int i;				\
      for (i = 0; i < count; i++)	\
	tag_to_expr[tag + i] = expr;	\
    }
#define MAP_STMT_TAGS(stmt,tag,count)	\
    {					\
      int i;				\
      for (i = 0; i < count; i++)	\
	tag_to_stmt[tag + i] = stmt;	\
    }
#define TREE_MULTIPLE
#define TREE_SINGLE_MECHANICAL_TRUE
#define TREE_SINGLE_MECHANICAL_FALSE
#define SET_NAME(a,b)
#include "lto-tree-tags.def"

#undef MAP_EXPR_TAG
#undef MAP_EXPR_TAGS
#undef TREE_MULTIPLE
#undef TREE_SINGLE_MECHANICAL_TRUE
#undef TREE_SINGLE_MECHANICAL_FALSE
#undef SET_NAME
  /* Initialize flags_length_for_code.  */


#define START_CLASS_SWITCH()                  			\
  {                                           			\
    int code;				      			\
    for (code = 0; code < NUM_TREE_CODES; code++) 		\
      {                                       			\
	/* The LTO_SOURCE_LOC_BITS leaves room for file and	\
	   line number for exprs.  */ 				\
        flags_length_for_code[code] = LTO_SOURCE_LOC_BITS;	\
                                              			\
        switch (TREE_CODE_CLASS (code))       			\
          {

#define START_CLASS_CASE(class)    case class:
#define ADD_CLASS_DECL_FLAG(flag_name)    flags_length_for_code[code]++;
#define ADD_CLASS_EXPR_FLAG(flag_name)    flags_length_for_code[code]++;
#define ADD_CLASS_TYPE_FLAG(flag_name)    flags_length_for_code[code]++;
#define END_CLASS_CASE(class)      break;
#define END_CLASS_SWITCH()				\
          default:					\
	    fprintf (stderr, "no declaration for "	\
		     "TREE_CODE_CLASS for = %s(%d)\n",  \
                     tree_code_name[code], code);	\
            gcc_unreachable ();				\
          }


#define START_EXPR_SWITCH()                   \
        switch (code)			      \
          {
#define START_EXPR_CASE(code)    case code:
#define ADD_EXPR_FLAG(flag_name)           flags_length_for_code[code]++;
#define ADD_TYPE_FLAG(flag_name)           flags_length_for_code[code]++;
#define ADD_DECL_FLAG(flag_name)           flags_length_for_code[code]++;
#define ADD_VIS_FLAG(flag_name)            flags_length_for_code[code]++;
#define ADD_VIS_FLAG_SIZE(flag_name,size)  flags_length_for_code[code] += size;
#define ADD_TLS_FLAG(flag_name,size)       flags_length_for_code[code] += size;
#define ADD_FUN_FLAG(flag_name)            flags_length_for_code[code]++;
#define END_EXPR_CASE(class)      break;
#define END_EXPR_SWITCH()                     \
          default:                            \
	    fprintf (stderr, "no declaration for TREE CODE = %s(%d)\n", \
                     tree_code_name[code], code);		        \
            gcc_unreachable ();               \
          }                                   \
      }					      \
  }

#include "lto-tree-flags.def"

#undef START_CLASS_SWITCH
#undef START_CLASS_CASE
#undef ADD_CLASS_DECL_FLAG
#undef ADD_CLASS_EXPR_FLAG
#undef ADD_CLASS_TYPE_FLAG
#undef END_CLASS_CASE
#undef END_CLASS_SWITCH
#undef START_EXPR_SWITCH
#undef START_EXPR_CASE
#undef ADD_EXPR_FLAG
#undef ADD_TYPE_FLAG
#undef ADD_DECL_FLAG
#undef ADD_VIS_FLAG
#undef ADD_VIS_FLAG_SIZE
#undef ADD_TLS_FLAG
#undef ADD_FUN_FLAG
#undef END_EXPR_CASE
#undef END_EXPR_SWITCH

  /* Verify that lto_flags_type is wide enough.  */
  {
    int code;
    for (code = 0; code < NUM_TREE_CODES; code++)
      gcc_assert (flags_length_for_code[code] <= BITS_PER_LTO_FLAGS_TYPE);
  }

  lto_static_init ();
  gimple_register_cfg_hooks ();

  file_name_hash_table
    = htab_create (37, hash_string_slot_node, eq_string_slot_node, free);
}


/* Read the body from DATA for function FN_DECL and fill it in.
   FILE_DATA are the global decls and types.  SECTION_TYPE is either
   LTO_section_function_body or LTO_section_static_initializer.  If
   section type is LTO_section_function_body, FN must be the decl for
   that function.  */

static void 
lto_read_body (struct lto_file_decl_data *file_data, tree fn_decl,
	       const char *data, enum lto_section_type section_type)
{
  const struct lto_function_header *header 
    = (const struct lto_function_header *) data;
  struct data_in data_in;
  int32_t named_label_offset = sizeof (struct lto_function_header); 
  int32_t ssa_names_offset = named_label_offset + header->named_label_size;
  int32_t cfg_offset = ssa_names_offset + header->ssa_names_size;
  int32_t local_decls_index_offset = cfg_offset + header->cfg_size;
  int32_t local_decls_offset = local_decls_index_offset
			       + header->local_decls_index_size;
  int32_t main_offset = local_decls_offset + header->local_decls_size;
  int32_t string_offset = main_offset + header->main_size;

  struct lto_input_block ib_named_labels;
  struct lto_input_block ib_ssa_names;
  struct lto_input_block ib_cfg;
  struct lto_input_block ib_local_decls_index;
  struct lto_input_block ib_local_decls;
  struct lto_input_block ib_main;

  LTO_INIT_INPUT_BLOCK (ib_named_labels, data + named_label_offset, 0, 
			header->named_label_size);
  LTO_INIT_INPUT_BLOCK (ib_ssa_names, data + ssa_names_offset, 0, 
			header->ssa_names_size);
  LTO_INIT_INPUT_BLOCK (ib_cfg, data + cfg_offset, 0, header->cfg_size);
  LTO_INIT_INPUT_BLOCK (ib_local_decls_index, data + local_decls_index_offset,
			0, header->local_decls_index_size);
  LTO_INIT_INPUT_BLOCK (ib_local_decls, data + local_decls_offset, 0, 
			header->local_decls_size);
  LTO_INIT_INPUT_BLOCK (ib_main, data + main_offset, 0, header->main_size);
  
  memset (&data_in, 0, sizeof (struct data_in));
  data_in.file_data = file_data;
  data_in.strings = data + string_offset;
  data_in.strings_len = header->string_size;

  lto_init_reader ();

  /* Make sure the file was generated by the exact same compiler.  */
  gcc_assert (header->lto_header.major_version == LTO_major_version);
  gcc_assert (header->lto_header.minor_version == LTO_minor_version);

  if (section_type == LTO_section_function_body)
    {
      struct function *fn = DECL_STRUCT_FUNCTION (fn_decl);
      struct lto_in_decl_state *decl_state;

      push_cfun (fn);
      init_tree_ssa (fn);

      /* Use the function's decl state. */
      decl_state = lto_get_function_in_decl_state (file_data, fn_decl);
      gcc_assert (decl_state);
      file_data->current_decl_state = decl_state;

      input_labels (&ib_named_labels, &data_in, header->num_named_labels,
		    header->num_unnamed_labels);
      
      input_local_vars_index (&ib_local_decls_index, &data_in,
			      header->num_local_decls);
      
      input_local_vars (&ib_local_decls, &data_in, fn, header->num_local_decls);
      
      input_ssa_names (&ib_ssa_names, &data_in, fn);
      
      input_cfg (&ib_cfg, fn);

      /* Set up the struct function.  */
      input_function (fn_decl, &data_in, &ib_main);

      /* We should now be in SSA.  */
      cfun->gimple_df->in_ssa_p = true;

      /* Fill in properties we know hold for the rebuilt CFG.  */
      cfun->curr_properties = PROP_ssa
			      | PROP_cfg
			      | PROP_gimple_any
			      | PROP_gimple_lcf
			      | PROP_gimple_leh
			      | PROP_referenced_vars;

      /* Restore decl state */
      file_data->current_decl_state = file_data->global_decl_state;

      pop_cfun ();
    }
  else 
    {
      input_labels (&ib_named_labels, &data_in, 
		    header->num_named_labels, header->num_unnamed_labels);

      input_constructors_or_inits (&data_in, &ib_main);
    }

  clear_line_info (&data_in);
  if (section_type == LTO_section_function_body)
    {
      free (data_in.labels);
      free (data_in.local_decls_index);
    }
}


/* Read the body of FN_DECL using DATA.  FILE_DATA holds the global
   decls and types.  */

void 
lto_input_function_body (struct lto_file_decl_data *file_data,
			 tree fn_decl, const char *data)
{
  current_function_decl = fn_decl;
  lto_read_body (file_data, fn_decl, data, LTO_section_function_body);
}


/* Read in VAR_DECL using DATA.  FILE_DATA holds the global decls and
   types.  */

void 
lto_input_constructors_and_inits (struct lto_file_decl_data *file_data,
				  const char *data)
{
  lto_read_body (file_data, NULL, data, LTO_section_static_initializer);
}


/* Push NODE as the next sequential entry in the globals index vector
   obtained from DATA_IN.  */

static unsigned
global_vector_enter (struct data_in *data_in, tree node)
{
  unsigned index = VEC_length (tree, data_in->globals_index);

#ifdef LTO_GLOBAL_VECTOR_TRACE
  fprintf (stderr, "ENTER %06u -> %p %s\n", index, (void *) node,
	   tree_code_name[node ? ((int) TREE_CODE (node)) : 0]);
#endif

  VEC_safe_push (tree, heap, data_in->globals_index, node);
  gcc_assert (TREE_CODE (node) < NUM_TREE_CODES);
  lto_stats.num_trees[TREE_CODE (node)]++;

  return index;
}


/* Read and return a tree from input block IB in file DATA_IN.  FN is
   the function context holding the read tree.  If FN is NULL, the
   tree belongs to the global scope.  */

static tree
input_tree_with_context (struct lto_input_block *ib, struct data_in *data_in,
			 tree fn)
{
  enum LTO_tags tag = input_record_start (ib);

  if (!tag)
    return NULL_TREE;
  else if (tag == LTO_tree_pickle_reference)
    {
      /* If TAG is a tree reference, resolve to a previously read node.  */
      tree result;
      unsigned int index;
      
      index = lto_input_uleb128 (ib);
      gcc_assert (data_in->globals_index);

      gcc_assert (index < VEC_length (tree, data_in->globals_index));

      result = VEC_index (tree, data_in->globals_index, index);
      gcc_assert (result);

      return result;
    }
  else
    return input_tree_operand (ib, data_in, fn, tag);
}


/* Read a FIELD_DECL from input block IB using the descriptors in
   DATA_IN.  */

static tree
input_field_decl (struct lto_input_block *ib, struct data_in *data_in)
{
  tree decl;
  lto_flags_type flags;
  
  decl = make_node (FIELD_DECL);
  
  flags = input_tree_flags (ib, FIELD_DECL, true);
  if (flags & LTO_SOURCE_HAS_LOC)
    {
      input_line_info (ib, data_in, flags);
      set_line_info (data_in, decl);
    }

  process_tree_flags (decl, flags);

  global_vector_enter (data_in, decl);

  decl->decl_minimal.name = input_tree (ib, data_in);
  decl->decl_minimal.context = input_tree (ib, data_in);
  decl->common.type = input_tree (ib, data_in);
  decl->decl_common.attributes = input_tree (ib, data_in);
  decl->decl_common.abstract_origin = input_tree (ib, data_in);
  decl->decl_common.mode = (enum machine_mode) lto_input_uleb128 (ib);
  decl->decl_common.align = lto_input_uleb128 (ib);
  decl->decl_common.off_align = lto_input_uleb128 (ib);
  decl->decl_common.size = input_tree (ib, data_in);
  decl->decl_common.size_unit = input_tree (ib, data_in);
  decl->field_decl.offset = input_tree (ib, data_in);
  decl->field_decl.bit_field_type = input_tree (ib, data_in);
  decl->field_decl.qualifier = input_tree (ib, data_in);
  decl->field_decl.bit_offset = input_tree (ib, data_in);
  decl->field_decl.fcontext = input_tree (ib, data_in);
  decl->decl_common.initial = input_tree (ib, data_in);
  decl->common.chain = input_tree (ib, data_in);

  return decl;
}


/* Read a CONST_DECL tree from input block IB using descriptors in
   DATA_IN.  */

static tree
input_const_decl (struct lto_input_block *ib, struct data_in *data_in)
{
  tree decl;
  lto_flags_type flags;

  decl = make_node (CONST_DECL);

  flags = input_tree_flags (ib, CONST_DECL, true);
  if (flags & LTO_SOURCE_HAS_LOC)
    {
      input_line_info (ib, data_in, flags);
      set_line_info (data_in, decl);
    }

  process_tree_flags (decl, flags);

  global_vector_enter (data_in, decl);

  decl->decl_minimal.name = input_tree (ib, data_in);
  decl->decl_minimal.context = NULL_TREE;
  decl->common.type = input_tree (ib, data_in);
  decl->decl_common.abstract_origin = input_tree (ib, data_in);
  decl->decl_common.mode = (enum machine_mode) lto_input_uleb128 (ib);
  decl->decl_common.align = lto_input_uleb128 (ib);
  decl->decl_common.initial = input_tree (ib, data_in);

  return decl;
}


/* Return the resolution for the decl with index INDEX from DATA_IN. */

static enum ld_plugin_symbol_resolution
get_resolution (struct data_in *data_in, unsigned index)
{
  if (data_in->globals_resolution)
    {
      ld_plugin_symbol_resolution_t ret;
      gcc_assert (index < VEC_length (ld_plugin_symbol_resolution_t,
				      data_in->globals_resolution));
      ret = VEC_index (ld_plugin_symbol_resolution_t,
		       data_in->globals_resolution,
		       index);
      gcc_assert (ret != LDPR_UNKNOWN);
      return ret;
    }
  else
    {
      /* Fake symbol resolution if no resolution file was provided.  */
      tree t = VEC_index (tree, data_in->globals_index, index);

      gcc_assert (TREE_PUBLIC (t));

      /* There should be no DECL_ABSTRACT in the middle end.  */
      gcc_assert (!DECL_ABSTRACT (t));

      /* If T is a weak definition, we select the first one we see to
	 be the prevailing definition.  */
      if (DECL_WEAK (t))
	{
	  tree prevailing_decl;
	  if (DECL_EXTERNAL (t))
	    return LDPR_RESOLVED_IR;

	  /* If this is the first time we see T, it won't have a
	     prevailing definition yet.  */
	  prevailing_decl = lto_symtab_prevailing_decl (t);
	  if (prevailing_decl == t
	      || prevailing_decl == NULL_TREE
	      || DECL_EXTERNAL (prevailing_decl))
	    return LDPR_PREVAILING_DEF;
	  else
	    return LDPR_PREEMPTED_IR;
	}
      else
	{
	  /* For non-weak definitions, extern declarations are assumed
	     to be resolved elsewhere (LDPR_RESOLVED_IR), otherwise T
	     is a prevailing definition.  */
	  if (DECL_EXTERNAL (t))
	    return LDPR_RESOLVED_IR;
	  else
	    return LDPR_PREVAILING_DEF;
	}
    }
}


/* Read a FUNCTION_DECL tree from input block IB using descriptors in
   DATA_IN.  TAG is one of LTO_function_decl0 or LTO_function_decl1.  */

static tree
input_function_decl (struct lto_input_block *ib, struct data_in *data_in,
		     enum LTO_tags tag)
{
  unsigned index;
  unsigned has_personality;
  tree decl;
  lto_flags_type flags;

  if (tag == LTO_function_decl1)
    {
      /* If we are going to read a built-in function, all we need is
	 the code and class.  */
      enum built_in_class fclass;
      enum built_in_function fcode;
      const char *asmname;

      fclass = (enum built_in_class) lto_input_uleb128 (ib);
      gcc_assert (fclass == BUILT_IN_NORMAL || fclass == BUILT_IN_MD);

      fcode = (enum built_in_function) lto_input_uleb128 (ib);
      gcc_assert (fcode < END_BUILTINS);

      decl = built_in_decls[(size_t) fcode];
      gcc_assert (decl);

      asmname = input_string (data_in, ib);
      if (asmname)
	set_builtin_user_assembler_name (decl, asmname);

      global_vector_enter (data_in, decl);

      return decl;
    }

  decl = make_node (FUNCTION_DECL);

  flags = input_tree_flags (ib, FUNCTION_DECL, true);
  if (flags & LTO_SOURCE_HAS_LOC)
    {
      input_line_info (ib, data_in, flags);
      set_line_info (data_in, decl);
    }

  process_tree_flags (decl, flags);

  index = global_vector_enter (data_in, decl);

  decl->decl_minimal.name = input_tree (ib, data_in);
  decl->decl_minimal.context = input_tree (ib, data_in);
  decl->decl_with_vis.assembler_name = input_tree (ib, data_in);
  decl->decl_with_vis.section_name = input_tree (ib, data_in);
  decl->decl_with_vis.comdat_group = input_tree (ib, data_in);
  decl->common.type = input_tree (ib, data_in);
  decl->decl_common.attributes = input_tree (ib, data_in);
  decl->decl_common.abstract_origin = input_tree (ib, data_in);
  decl->decl_common.mode = (enum machine_mode) lto_input_uleb128 (ib);
  decl->decl_common.align = lto_input_uleb128 (ib);
  decl->decl_common.size = input_tree (ib, data_in);
  decl->decl_common.size_unit = input_tree (ib, data_in);
  decl->decl_non_common.arguments = input_tree_with_context (ib, data_in, decl);
  decl->decl_non_common.result = input_tree_with_context (ib, data_in, decl);
  decl->decl_non_common.vindex = input_tree (ib, data_in);

  has_personality = lto_input_uleb128 (ib);
  if (has_personality)
    {
      decl->function_decl.personality = input_tree (ib, data_in);
      gcc_assert (TREE_CODE (decl->function_decl.personality) == FUNCTION_DECL);
      lto_input_uleb128 (ib);
    }
  else
    decl->function_decl.personality = NULL ;


  DECL_BUILT_IN_CLASS (decl) = (enum built_in_class) lto_input_uleb128 (ib);
  gcc_assert (!DECL_IS_BUILTIN (decl)
	      || DECL_BUILT_IN_CLASS (decl) == NOT_BUILT_IN
	      || DECL_BUILT_IN_CLASS (decl) == BUILT_IN_FRONTEND);

  DECL_FUNCTION_CODE (decl) = (enum built_in_function) lto_input_uleb128 (ib);

  /* Need to ensure static entities between different files
     don't clash unexpectedly.  */
  if (!TREE_PUBLIC (decl))
    {
      /* We must not use the DECL_ASSEMBLER_NAME macro here, as it
	 may set the assembler name where it was previously empty.  */
      tree old_assembler_name = decl->decl_with_vis.assembler_name;

      /* FIXME lto:  We normally pre-mangle names before we serialize
	 them out.  Here, in lto1, we do not know the language, and
	 thus cannot do the mangling again. Instead, we just append a
	 suffix to the mangled name.  The resulting name, however, is
	 not a properly-formed mangled name, and will confuse any
	 attempt to unmangle it.  */
      const char *name = IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (decl));
      char *label;
      
      ASM_FORMAT_PRIVATE_NAME (label, name, DECL_UID (decl));
      SET_DECL_ASSEMBLER_NAME (decl, get_identifier (label));

      /* We may arrive here with the old assembler name not set
	 if the function body is not needed, e.g., it has been
	 inlined away and does not appear in the cgraph.  */
      if (old_assembler_name)
	{
	  tree new_assembler_name = decl->decl_with_vis.assembler_name;

	  /* Make the original assembler name available for later use.
	     We may have used it to indicate the section within its
	     object file where the function body may be found.
	     FIXME lto: Find a better way to maintain the function decl
	     to body section mapping so we don't need this hack.  */
	  lto_record_renamed_decl (data_in->file_data,
				   IDENTIFIER_POINTER (old_assembler_name),
				   IDENTIFIER_POINTER (new_assembler_name));

	  /* Also register the reverse mapping so that we can find the
	     new name given to an existing assembler name (used when
	     restoring alias pairs in input_constructors_or_inits.  */ 
	  lto_record_renamed_decl (data_in->file_data,
				   IDENTIFIER_POINTER (new_assembler_name),
				   IDENTIFIER_POINTER (old_assembler_name));
	}				   
    }

  /* If the function has already been declared, merge the
     declarations.  */
  if (TREE_PUBLIC (decl) && !DECL_ABSTRACT (decl))
    {
      enum ld_plugin_symbol_resolution resolution;
      resolution = get_resolution (data_in, index);
      lto_symtab_merge_fn (decl, resolution, data_in->file_data);
    }

  return decl;
}


/* Read a VAR_DECL tree from input block IB using descriptors in
   DATA_IN.  */

static tree
input_var_decl (struct lto_input_block *ib, struct data_in *data_in)
{
  unsigned index;
  lto_decl_flags_t decl_flags;
  tree decl;
  lto_flags_type flags;

  decl = make_node (VAR_DECL);

  flags = input_tree_flags (ib, VAR_DECL, true);
  if (flags & LTO_SOURCE_HAS_LOC)
    {
      input_line_info (ib, data_in, flags);
      set_line_info (data_in, decl);
    }

  process_tree_flags (decl, flags);

  /* Additional LTO decl flags. */
  decl_flags = lto_input_uleb128 (ib);
  if (decl_flags)
    lto_set_decl_flags (decl, decl_flags);

  /* Even though we cannot actually generate a reference
     to this node until we have done the lto_symtab_merge_var,
     we must reserve the slot in the globals vector here,
     because the writer allocates the indices before writing
     out the type, etc.  */
  index = global_vector_enter (data_in, decl);

  /* omit locus, uid */
  decl->decl_minimal.name = input_tree (ib, data_in);
  decl->decl_minimal.context = NULL_TREE;

  decl->decl_with_vis.assembler_name = input_tree (ib, data_in);
  decl->decl_with_vis.section_name = input_tree (ib, data_in);
  decl->decl_with_vis.comdat_group = input_tree (ib, data_in);
  decl->common.type = input_tree (ib, data_in);
  decl->decl_common.attributes = input_tree (ib, data_in);
  decl->decl_common.abstract_origin = input_tree (ib, data_in);
  decl->decl_common.mode = (enum machine_mode) lto_input_uleb128 (ib);
  decl->decl_common.align = lto_input_uleb128 (ib);
  decl->decl_common.size = input_tree (ib, data_in);
  decl->decl_common.size_unit = input_tree (ib, data_in);

  /* DECL_DEBUG_EXPR is stored in a table on the side,
     not in the VAR_DECL node itself.  */
  {
    tree debug_expr = NULL_TREE;
    enum LTO_tags tag = input_record_start (ib);

    if (tag)
      debug_expr = input_tree_operand (ib, data_in, NULL, tag);

    if (debug_expr)
      SET_DECL_DEBUG_EXPR (decl, debug_expr);
  }

  /* Register symbols with file or global scope to mark what input
     file has their definition.  */
  if (decl_function_context (decl) == NULL_TREE)
    {
      /* Variable has file scope, not local. Need to ensure static variables
	 between different files don't clash unexpectedly.  */
      if (!TREE_PUBLIC (decl))
        {
	  /* FIXME lto:  We normally pre-mangle names before we serialize them
	     out.  Here, in lto1, we do not know the language, and thus cannot
	     do the mangling again. Instead, we just append a suffix to the
	     mangled name.  The resulting name, however, is not a
	     properly-formed mangled name, and will confuse any attempt to
	     unmangle it.  */
	  const char *name = IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (decl));
	  char *label;
      
	  ASM_FORMAT_PRIVATE_NAME (label, name, DECL_UID (decl));
	  SET_DECL_ASSEMBLER_NAME (decl, get_identifier (label));
          rest_of_decl_compilation (decl, 1, 0);
        }
    }

  /* If this variable has already been declared, merge the
     declarations.  */
  if (TREE_PUBLIC (decl))
    {
      enum ld_plugin_symbol_resolution resolution;
      resolution = get_resolution (data_in, index);
      lto_symtab_merge_var (decl, resolution);
    }

  decl->decl_common.initial = input_tree (ib, data_in);

  return decl;
}


/* Read a PARM_DECL tree for function FN from input block IB using the
   descriptors in DATA_IN.  */

static tree
input_parm_decl (struct lto_input_block *ib, struct data_in *data_in, tree fn)
{
  tree decl;
  lto_flags_type flags;

  decl = make_node (PARM_DECL);

  flags = input_tree_flags (ib, PARM_DECL, true);
  if (flags & LTO_SOURCE_HAS_LOC)
    {
      input_line_info (ib, data_in, flags);
      set_line_info (data_in, decl);
    }

  process_tree_flags (decl, flags);

  global_vector_enter (data_in, decl);

  decl->decl_minimal.name = input_tree (ib, data_in);
  decl->decl_minimal.context = fn;
  decl->common.type = input_tree (ib, data_in);
  decl->decl_common.attributes = input_tree (ib, data_in);
  decl->decl_common.abstract_origin = NULL_TREE;
  decl->decl_common.mode = (enum machine_mode) lto_input_uleb128 (ib);
  decl->decl_common.align = lto_input_uleb128 (ib);
  decl->decl_common.size = input_tree (ib, data_in);
  decl->decl_common.size_unit = input_tree (ib, data_in);
  decl->decl_common.initial = input_tree (ib, data_in);
  decl->common.chain = input_tree_with_context (ib, data_in, fn);

  return decl;
}


/* Read a RESULT_DECL tree for function FN from input block IB using
   the descriptors in DATA_IN.  */

static tree
input_result_decl (struct lto_input_block *ib, struct data_in *data_in,
		   tree fn)
{
  tree decl;
  lto_flags_type flags;

  decl = make_node (RESULT_DECL);

  flags = input_tree_flags (ib, RESULT_DECL, true);
  if (flags & LTO_SOURCE_HAS_LOC)
    {
      input_line_info (ib, data_in, flags);
      set_line_info (data_in, decl);
    }

  process_tree_flags (decl, flags);

  global_vector_enter (data_in, decl);

  decl->decl_minimal.name = input_tree (ib, data_in);
  decl->decl_minimal.context = fn;
  decl->common.type = input_tree (ib, data_in);
  decl->decl_common.attributes = input_tree (ib, data_in);
  decl->decl_common.abstract_origin = input_tree (ib, data_in);
  decl->decl_common.mode = (enum machine_mode) lto_input_uleb128 (ib);
  decl->decl_common.align = lto_input_uleb128 (ib);
  decl->decl_common.size = input_tree (ib, data_in);
  decl->decl_common.size_unit = input_tree (ib, data_in);

  return decl;
}


/* Read a TYPE_DECL tree from input block IB using the descriptors in
   DATA_IN.  */

static tree
input_type_decl (struct lto_input_block *ib, struct data_in *data_in)
{
  tree decl;
  lto_flags_type flags;

  decl = make_node (TYPE_DECL);

  flags = input_tree_flags (ib, TYPE_DECL, true);
  if (flags & LTO_SOURCE_HAS_LOC)
    {
      input_line_info (ib, data_in, flags);
      set_line_info (data_in, decl);
    }

  process_tree_flags (decl, flags);

  global_vector_enter (data_in, decl);

  decl->decl_minimal.name = input_tree (ib, data_in);
  decl->decl_with_vis.assembler_name = input_tree (ib, data_in);
  decl->decl_with_vis.section_name = input_tree (ib, data_in);
  decl->common.type = input_tree (ib, data_in);
  decl->decl_common.attributes = input_tree (ib, data_in);
  decl->decl_common.abstract_origin = input_tree (ib, data_in);
  decl->decl_common.mode = (enum machine_mode) lto_input_uleb128 (ib);
  decl->decl_common.align = lto_input_uleb128 (ib);
  decl->decl_common.size = input_tree (ib, data_in);
  decl->decl_common.size_unit = input_tree (ib, data_in);
  decl->decl_non_common.saved_tree = input_tree (ib, data_in);
  decl->decl_non_common.arguments = input_tree (ib, data_in);
  decl->decl_non_common.result = input_tree (ib, data_in);
  decl->decl_non_common.vindex = input_tree (ib, data_in);

  return decl;
}


/* Read and return a LABEL_DECL from IB using descriptors in DATA_IN.  */

static tree
input_label_decl (struct lto_input_block *ib, struct data_in *data_in)
{
  tree decl;
  lto_flags_type flags;

  decl = make_node (LABEL_DECL);

  flags = input_tree_flags (ib, LABEL_DECL, true);
  if (flags & LTO_SOURCE_HAS_LOC)
    {
      input_line_info (ib, data_in, flags);
      set_line_info (data_in, decl);
    }

  process_tree_flags (decl, flags);

  global_vector_enter (data_in, decl);

  decl->decl_minimal.name = input_tree (ib, data_in);
  decl->decl_minimal.context = input_tree (ib, data_in);
  decl->common.type = input_tree (ib, data_in);
  decl->decl_common.attributes = input_tree (ib, data_in);
  decl->decl_common.abstract_origin = input_tree (ib, data_in);
  decl->decl_common.mode = (enum machine_mode) lto_input_uleb128 (ib);
  decl->decl_common.align = lto_input_uleb128 (ib);
  decl->decl_common.initial = input_tree (ib, data_in);

  return decl;
}


/* Read an IMPORTED_DECL node from IB using descriptors in DATA_IN.  */

static tree
input_imported_decl (struct lto_input_block *ib, struct data_in *data_in)
{
  tree decl;
  lto_flags_type flags;

  decl = make_node (IMPORTED_DECL);

  flags = input_tree_flags (ib, IMPORTED_DECL, true);
  if (flags & LTO_SOURCE_HAS_LOC)
    {
      input_line_info (ib, data_in, flags);
      set_line_info (data_in, decl);
    }

  process_tree_flags (decl, flags);

  global_vector_enter (data_in, decl);

  IMPORTED_DECL_ASSOCIATED_DECL (decl) = input_tree (ib, data_in);
  DECL_NAME (decl) = input_tree (ib, data_in);
  TREE_TYPE (decl) = void_type_node;

  return decl;
}


/* Read a BINFO tree from IB using descriptors in DATA_IN.  */

static tree
input_binfo (struct lto_input_block *ib, struct data_in *data_in)
{
  size_t i;
  tree binfo;
  size_t num_base_accesses;
  size_t num_base_binfos;
  lto_flags_type flags;

  flags = input_tree_flags (ib, TREE_BINFO, true);

  num_base_accesses = lto_input_uleb128 (ib);
  num_base_binfos = lto_input_uleb128 (ib);

  binfo = make_tree_binfo (num_base_binfos);

  gcc_assert (!(flags & LTO_SOURCE_HAS_LOC));
  process_tree_flags (binfo, flags);

  global_vector_enter (data_in, binfo);

  binfo->common.type = input_tree (ib, data_in);
  binfo->binfo.offset = input_tree (ib, data_in);
  binfo->binfo.vtable = input_tree (ib, data_in);
  binfo->binfo.virtuals = input_tree (ib, data_in);
  binfo->binfo.vptr_field = input_tree (ib, data_in);
  binfo->binfo.inheritance = input_tree (ib, data_in);
  binfo->binfo.vtt_subvtt = input_tree (ib, data_in);
  binfo->binfo.vtt_vptr = input_tree (ib, data_in);

  binfo->binfo.base_accesses = VEC_alloc (tree, gc, num_base_accesses);
  for (i = 0; i < num_base_accesses; ++i)
    VEC_quick_push (tree, binfo->binfo.base_accesses,
		    input_tree_operand (ib, data_in, NULL,
					input_record_start (ib)));

  for (i = 0; i < num_base_binfos; ++i)
    VEC_quick_push (tree, &binfo->binfo.base_binfos,
		    input_tree_operand (ib, data_in, NULL,
					input_record_start (ib)));

  binfo->common.chain = input_tree (ib, data_in);

  return binfo;
}


/* Read a type tree node with code CODE from IB using the descriptors
   in DATA_IN.  */

static tree
input_type (struct lto_input_block *ib, struct data_in *data_in,
	    enum tree_code code)
{
  tree type = make_node (code);

  process_tree_flags (type, input_tree_flags (ib, code, true));

  /* Clear this flag, since we didn't stream the values cache.  */
  TYPE_CACHED_VALUES_P (type) = 0;

  global_vector_enter (data_in, type);
    
  type->common.type = input_tree (ib, data_in);
  type->type.size = input_tree (ib, data_in);
  type->type.size_unit = input_tree (ib, data_in);
  type->type.attributes = input_tree (ib, data_in);
  type->type.precision = lto_input_uleb128 (ib);
  type->type.mode = (enum machine_mode) lto_input_uleb128 (ib);
  type->type.align = lto_input_uleb128 (ib);
  type->type.pointer_to = input_tree (ib, data_in);
  type->type.reference_to = input_tree (ib, data_in);
  type->type.name = input_tree (ib, data_in);
  type->type.minval = input_tree (ib, data_in);
  type->type.maxval = input_tree (ib, data_in);
  type->type.next_variant = input_tree (ib, data_in);
  type->type.main_variant = input_tree (ib, data_in);
  type->type.binfo = input_tree (ib, data_in);
  type->type.canonical = input_tree (ib, data_in);

  if (code == RECORD_TYPE || code == UNION_TYPE)
    type->type.values = input_tree (ib, data_in);
  else
    {
      gcc_assert (TYPE_CACHED_VALUES_P (type) || !type->type.values);
      if (type->type.values)
	{
	  /* We have constructed a new values cache while reading the
	     type, presumably due to literal creation above.  Don't
	     clobber it.  */
	  enum LTO_tags tag = input_record_start (ib);

	  /* A values cache is streamed out as NULL_TREE, so check
	     that the input stream agrees with our assumption.  */
	  gcc_assert (!tag);
	}
      else
	type->type.values = input_tree (ib, data_in);
    }

  type->common.chain = input_tree (ib, data_in);

  return type;
}


/* Read a reference to a type node from input block IB using
   descriptors in DATA_IN.  */

static tree
input_type_tree (struct lto_input_block *ib, struct data_in *data_in)
{
  enum LTO_tags tag;
  tree type;

  tag = input_record_start (ib);
  if (tag)
    {
      type = input_tree_operand (ib, data_in, NULL, tag);
      gcc_assert (type && TYPE_P (type));
      return type;
    }
  else
    return NULL_TREE;
}


/* Helper for input_tree_block.  Read a FUNCTION_DECL reference or a
   BLOCK from IB using descriptors in DATA_IN.  */

static tree
input_block_or_decl (struct lto_input_block *ib, struct data_in *data_in)
{
  enum LTO_tags tag;

  /* FIXME lto, this would not be needed if streaming of trees in
     global context was unified with trees in function bodies.  */
  tag = input_record_start (ib);

  if (tag == LTO_null)
    return NULL_TREE;
  else if (tag == LTO_function_decl0)
    return lto_file_decl_data_get_fn_decl (data_in->file_data,
					   lto_input_uleb128 (ib));
  else if (tag == LTO_block || tag == LTO_tree_pickle_reference)
    return input_tree_operand (ib, data_in, NULL, tag);
  else
    gcc_unreachable ();
}


/* Read a BLOCK tree from input block IB using descriptors in DATA_IN.  */

static tree
input_tree_block (struct lto_input_block *ib, struct data_in *data_in)
{
  unsigned HOST_WIDEST_INT block_flags;
  tree block;
  unsigned i, vlen;
  enum LTO_tags tag;
  tree first, curr, prev;

  block = make_node (BLOCK);

  global_vector_enter (data_in, block);

  block_flags = lto_input_sleb128 (ib);
  BLOCK_NUMBER (block) = lto_get_flags (&block_flags, 31);
  BLOCK_ABSTRACT (block) = lto_get_flag (&block_flags);

  first = prev = NULL_TREE;
  tag = input_record_start (ib);
  while (tag)
    {
      curr = input_expr_operand (ib, data_in, cfun, tag);
      if (prev)
	TREE_CHAIN (prev) = curr;
      else
	first = curr;

      TREE_CHAIN (curr) = NULL_TREE;
      prev = curr;
      tag = input_record_start (ib);
    }
  BLOCK_VARS (block) = first;


  vlen = lto_input_sleb128 (ib);
  for (i = 0; i < vlen; i++)
    {
      tree var;

      tag = input_record_start (ib);
      var = input_expr_operand (ib, data_in, cfun, tag);
      VEC_safe_push (tree, gc, BLOCK_NONLOCALIZED_VARS (block), var);
    }

  BLOCK_SUPERCONTEXT (block) = input_block_or_decl (ib, data_in);
  BLOCK_ABSTRACT_ORIGIN (block) = input_block_or_decl (ib, data_in);
  BLOCK_FRAGMENT_ORIGIN (block) = input_block_or_decl (ib, data_in);
  BLOCK_FRAGMENT_CHAIN (block) = input_block_or_decl (ib, data_in);
  BLOCK_CHAIN (block) = input_tree (ib, data_in);
  BLOCK_SUBBLOCKS (block) = input_tree (ib, data_in);

  return block;
}


/* Read a node in the body of function FN from input block IB using
   descriptors in DATA_IN.  TAG indicates the kind of tree that is
   expected to be read.  */

static tree
input_tree_operand (struct lto_input_block *ib, struct data_in *data_in, 
		    tree fn, enum LTO_tags tag)
{
  enum tree_code code;
  tree type = NULL_TREE;
  lto_flags_type flags;
  tree result = NULL_TREE;
  bool needs_line_set = false;

  /* If TAG is a reference to a previously read tree, look it up in
     DATA_IN->GLOBALS_INDEX.  */
  if (tag == LTO_tree_pickle_reference)
    {
      tree result;
      unsigned int index;
      
      gcc_assert (data_in->globals_index);

      index = lto_input_uleb128 (ib);
      gcc_assert (index < VEC_length (tree, data_in->globals_index));

      result = VEC_index (tree, data_in->globals_index, index);
      gcc_assert (result);

      return result;
    }

  code = tag_to_expr[tag];
  gcc_assert (code);

  if (TREE_CODE_CLASS (code) != tcc_type
      && TREE_CODE_CLASS (code) != tcc_declaration
      && code != TREE_BINFO)
    {
      if (TEST_BIT (lto_types_needed_for, code))
        type = input_type_tree (ib, data_in);

      flags = input_tree_flags (ib, code, false);
    }
  else
    {
      /* Inhibit the usual flag processing.  Handlers for types and
	 declarations will deal with flags and TREE_TYPE themselves.  */
      flags = 0;
    }


  /* Handlers for declarations currently handle line info themselves.  */
  needs_line_set = flags & LTO_SOURCE_HAS_LOC;
  if (needs_line_set)
    input_line_info (ib, data_in, flags);

  switch (code)
    {
    case BLOCK:
      result = input_tree_block (ib, data_in);
      break;

    case COMPLEX_CST:
      {
	tree elt_type = input_type_tree (ib, data_in);

	result = build0 (code, type);
	if (tag == LTO_complex_cst1)
	  {
	    TREE_REALPART (result) = input_real (ib, data_in, elt_type);
	    TREE_IMAGPART (result) = input_real (ib, data_in, elt_type);
	  }
	else
	  {
	    TREE_REALPART (result) = lto_input_integer (ib, elt_type);
	    TREE_IMAGPART (result) = lto_input_integer (ib, elt_type);
	  }
      }
      break;

    case INTEGER_CST:
      result = lto_input_integer (ib, type);
      break;

    case REAL_CST:
      result = input_real (ib, data_in, type);
      break;

    case STRING_CST:
      result = input_string_cst (data_in, ib);
      TREE_TYPE (result) = type;
      break;

    case IDENTIFIER_NODE:
      result = input_identifier (data_in, ib);
      break;

    case VECTOR_CST:
      {
	tree chain = NULL_TREE;
	int len = lto_input_uleb128 (ib);
	tree elt_type = input_type_tree (ib, data_in);

	if (len && tag == LTO_vector_cst1)
	  {
	    int i;
	    tree last;

	    last = input_real (ib, data_in, elt_type);
	    last = build_tree_list (NULL_TREE, last);
	    chain = last; 
	    for (i = 1; i < len; i++)
	      {
		tree t;

		t = input_real (ib, data_in, elt_type);
		t = build_tree_list (NULL_TREE, t);
		TREE_CHAIN (last) = t;
		last = t;
	      }
	  }
	else
	  {
	    int i;
	    tree last;

	    last = lto_input_integer (ib, elt_type);
	    last = build_tree_list (NULL_TREE, last);
	    chain = last; 
	    for (i = 1; i < len; i++)
	      {
		tree t;

		t = lto_input_integer (ib, elt_type);
		t = build_tree_list (NULL_TREE, t);
		TREE_CHAIN (last) = t;
		last = t;
	      }
	  }
	result = build_vector (type, chain);
      }
      break;

    case CASE_LABEL_EXPR:
      gcc_unreachable ();

    case CONSTRUCTOR:
      {
	VEC(constructor_elt,gc) *vec = NULL;
	unsigned int len = lto_input_uleb128 (ib);
	
	if (len)
	  {
	    unsigned int i = 0;
	    vec = VEC_alloc (constructor_elt, gc, len);
	    for (i = 0; i < len; i++)
	      {
		tree purpose = NULL_TREE;
		tree value;
		constructor_elt *elt; 
		enum LTO_tags ctag = input_record_start (ib);
		
		if (ctag)
		  purpose = input_tree_operand (ib, data_in, fn, ctag);
		
		value = input_tree_operand (ib, data_in, fn,
					    input_record_start (ib));
		elt = VEC_quick_push (constructor_elt, vec, NULL);
		elt->index = purpose;
		elt->value = value;
	      }
	  }
	result = build_constructor (type, vec);
      }
      break;

    case SSA_NAME:
      gcc_unreachable ();
      
    case CONST_DECL:
      result = input_const_decl (ib, data_in);
      break;

    case FIELD_DECL:
      result = input_field_decl (ib, data_in);
      break;

    case FUNCTION_DECL:
      result = input_function_decl (ib, data_in, tag);
      break;

    case IMPORTED_DECL:
      result = input_imported_decl (ib, data_in);
      break;

    case VAR_DECL:
      /* There should be no references to locals in this context.  */
      gcc_assert (tag == LTO_var_decl1);
      result = input_var_decl (ib, data_in);
      break;

    case PARM_DECL:
      result = input_parm_decl (ib, data_in, fn);
      break;

    case RESULT_DECL:
      /* Note that when we reach this point, were are declaring a result
         decl, not referencing one.  In some sense, the actual result
         variable is a local, and should be declared in the function body,
         but these are apparently treated similarly to parameters, for
         which dummy instances are created for extern declarations, etc.
         Actual references should occur only within a function body.  */
      result = input_result_decl (ib, data_in, fn);
      break;

    case TYPE_DECL:
      result = input_type_decl (ib, data_in);
      break;

    case LABEL_DECL:
      result = input_label_decl (ib, data_in);
      break;

    case LABEL_EXPR:
      {
        tree label;
        label = input_tree_operand (ib, data_in, fn, input_record_start (ib));
        gcc_assert (label && TREE_CODE (label) == LABEL_DECL);
        result = build1 (code, void_type_node, label);
        gcc_assert (DECL_CONTEXT (LABEL_EXPR_LABEL (result)));
      }
      break;

    case COMPONENT_REF:
      {
	tree op0;
	tree op1;
	op0 = input_tree_operand (ib, data_in, fn, input_record_start (ib));
	op1 = input_tree_operand (ib, data_in, fn, input_record_start (ib));
	result = build3 (code, type, op0, op1, NULL_TREE);
      }
      break;

    case CALL_EXPR:
      gcc_unreachable ();

    case BIT_FIELD_REF:
      {
	tree op0, op1, op2;

	if (tag == LTO_bit_field_ref1)
	  {
	    op1 = build_int_cst_wide (sizetype, lto_input_uleb128 (ib), 0);
	    op2 = build_int_cst_wide (bitsizetype, lto_input_uleb128 (ib), 0);
	    op0 = input_tree_operand (ib, data_in, fn,
				      input_record_start (ib));
	  }
	else
	  {
	    op0 = input_tree_operand (ib, data_in, fn, input_record_start (ib));
	    op1 = input_tree_operand (ib, data_in, fn, input_record_start (ib));
	    op2 = input_tree_operand (ib, data_in, fn, input_record_start (ib));
	  }
	result = build3 (code, type, op0, op1, op2);
      }
      break;

    case ARRAY_REF:
    case ARRAY_RANGE_REF:
      {
	/* Ignore operands 2 and 3 for ARRAY_REF and ARRAY_RANGE REF
	   because they can be recomputed.  */
	tree op0, op1;
	
	op0 = input_tree_operand (ib, data_in, fn, input_record_start (ib));
	op1 = input_tree_operand (ib, data_in, fn, input_record_start (ib));
	result = build4 (code, type, op0, op1, NULL_TREE, NULL_TREE);
      }
      break;

    case RANGE_EXPR:
      {
	tree op0 = lto_input_integer (ib, input_type_tree (ib, data_in));
	tree op1 = lto_input_integer (ib, input_type_tree (ib, data_in));
	result = build2 (RANGE_EXPR, sizetype, op0, op1);
      }
      break;

    case TREE_LIST:
      {
	unsigned int count = lto_input_uleb128 (ib);
	tree next = NULL;

	result = NULL_TREE;
	while (count--)
	  {
	    tree elt = make_node (TREE_LIST);

	    TREE_VALUE (elt) = input_tree (ib, data_in);
	    TREE_PURPOSE (elt) = input_tree ( ib, data_in);

	    if (result)
	      TREE_CHAIN (next) = elt;
	    else
	      /* Save the first one.  */
	      result = elt;
	    next = elt;
	  }
      }
      break;

    case TREE_VEC:
      {
	unsigned int i;
	unsigned int len = lto_input_uleb128 (ib);
	tree result = make_tree_vec (len);
	
	for (i = 0; i < len; ++i)
	  TREE_VEC_ELT (result, i) = input_tree (ib, data_in);
      }
      break;

    case ERROR_MARK:
      /* The canonical error node is preloaded, so we should never see
	 another one here.  */
      gcc_unreachable ();

    case VOID_TYPE:
    case INTEGER_TYPE:
    case REAL_TYPE:
    case FIXED_POINT_TYPE:
    case COMPLEX_TYPE:
    case BOOLEAN_TYPE:
    case OFFSET_TYPE:
    case ENUMERAL_TYPE:
    case POINTER_TYPE:
    case REFERENCE_TYPE:
    case VECTOR_TYPE:
    case ARRAY_TYPE:
    case RECORD_TYPE:
    case UNION_TYPE:
    case QUAL_UNION_TYPE:
    case FUNCTION_TYPE:
    case METHOD_TYPE:
      result = input_type (ib, data_in, code);
      break;

    case LANG_TYPE:
      gcc_unreachable ();

    case TREE_BINFO:
      result = input_binfo (ib, data_in);
      break;

      /* This is the default case.  All of the cases that can be done
	 completely mechanically are done here.  */
#define SET_NAME(a,b)
#define TREE_SINGLE_MECHANICAL_TRUE
#define MAP_EXPR_TAG(expr,tag) case expr:
#include "lto-tree-tags.def"
#undef MAP_EXPR_TAG
#undef TREE_SINGLE_MECHANICAL_TRUE
#undef SET_NAME

      {
	int len = TREE_CODE_LENGTH (code);
	int i;

	result = make_node (code);
	TREE_TYPE (result) = type;

	/* Calling input_tree here results in NULL being passed as the
	   FN argument to recursive calls to input_tree_operand.  This
	   is only correct because no one actually examines FN at
	   present.  See the LABEL_EXPR case above.  */
	for (i = 0; i < len; i++)
	  TREE_OPERAND (result, i) = input_tree (ib, data_in);
      }
      break;

    default:
      /* We cannot have forms that are not explicity handled.  So when
	 this is triggered, there is some form that is not being
	 output.  */
      gcc_unreachable ();
    }

  if (flags)
    process_tree_flags (result, flags);

  if (needs_line_set)
    set_line_info (data_in, result);

  /* It is not enough to just put the flags back as we serialized
     them.  There are side effects to the buildN functions which play
     with the flags to the point that we just have to call this here
     to get it right.  */
  if (code == ADDR_EXPR)
    {
      tree x = get_base_var (result);

      if (TREE_CODE (x) == VAR_DECL || TREE_CODE (x) == PARM_DECL)
	TREE_ADDRESSABLE (x) = 1;

      recompute_tree_invariant_for_addr_expr (result);
    }

  return result;
}


/* Input a generic tree from the LTO IR input stream IB using the per-file
   context in DATA_IN.  This context is used, for example, to resolve
   references to previously input nodes. */

tree
input_tree (struct lto_input_block *ib, struct data_in *data_in)
{
  return input_tree_with_context (ib, data_in, NULL_TREE);
}
