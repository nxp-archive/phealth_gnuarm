/* Write the GIMPLE representation to a file stream.

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
#include "except.h"
#include "vec.h"
#include "lto-streamer.h"
#include "lto-tags.h"

sbitmap lto_flags_needed_for;
sbitmap lto_types_needed_for;

/* Forward declarations to break cyclical dependencies.  */
static void output_record_start (struct output_block *, tree, tree,
				 unsigned int);
static void output_expr_operand (struct output_block *, tree);

/* The index of the last eh_region seen for an instruction.  The
   eh_region for an instruction is only emitted if it different from
   the last instruction.  */
static int last_eh_region_seen;
static enum LTO_tags expr_to_tag[NUM_TREE_CODES];
static unsigned int stmt_to_tag[LAST_AND_UNUSED_GIMPLE_CODE];

/* Returns nonzero if P1 and P2 are equal.  */

static int
eq_label_slot_node (const void *p1, const void *p2)
{
  const struct lto_decl_slot *ds1 =
    (const struct lto_decl_slot *) p1;
  const struct lto_decl_slot *ds2 =
    (const struct lto_decl_slot *) p2;

  return ds1->t == ds2->t;
}

/* Returns a hash code for P.  */

static hashval_t
hash_label_slot_node (const void *p)
{
  const struct lto_decl_slot *ds = (const struct lto_decl_slot *) p;
  return htab_hash_pointer (ds->t);
}


struct string_slot {
  const char *s;
  int len;
  unsigned int slot_num;
};


/* Returns a hash code for P.  */

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
  const struct string_slot *ds1 =
    (const struct string_slot *) p1;
  const struct string_slot *ds2 =
    (const struct string_slot *) p2;

  if (ds1->len == ds2->len)
    {
      int i;
      for (i=0; i<ds1->len; i++)
	if (ds1->s[i] != ds2->s[i])
	  return 0;
      return 1;
    }
  else return 0;
}


/* Free the string slot.  */

static void 
string_slot_free (void *p)
{
  struct string_slot *slot = (struct string_slot *)p;
  free (CONST_CAST (void *, (const void *) slot->s));
  free (slot);
}



/* Clear the line info stored in DATA_IN.  */

static void
clear_line_info (struct output_block *ob)
{
  ob->current_file = NULL;
  ob->current_line = 0;
  ob->current_col = 0;
}


/* Create the output block and return it.  SECTION_TYPE is
   LTO_section_function_body or lto_static_initializer.  */

struct output_block *
create_output_block (enum lto_section_type section_type)
{
  struct output_block *ob = XCNEW (struct output_block);

  ob->section_type = section_type;
  ob->decl_state = lto_get_out_decl_state ();
  ob->main_stream = XCNEW (struct lto_output_stream);
  ob->string_stream = XCNEW (struct lto_output_stream);

  ob->named_label_stream = XCNEW (struct lto_output_stream);
  if (section_type == LTO_section_function_body)
    {
      ob->local_decl_index_stream = XCNEW (struct lto_output_stream);
      ob->local_decl_stream = XCNEW (struct lto_output_stream);
      ob->ssa_names_stream =XCNEW (struct lto_output_stream);
      ob->cfg_stream = XCNEW (struct lto_output_stream);
    }

  clear_line_info (ob);

  ob->label_hash_table
    = htab_create (37, hash_label_slot_node, eq_label_slot_node, free);

  ob->string_hash_table
    = htab_create (37, hash_string_slot_node, eq_string_slot_node,
		   string_slot_free);

  lto_init_tree_ref_encoder (&ob->local_decl_encoder, lto_hash_decl_slot_node,
			     lto_eq_decl_slot_node);

  /* The unnamed labels must all be negative.  */
  ob->next_unnamed_label_index = -1;
  return ob;
}


/* Destroy the output block OB.  */

void
destroy_output_block (struct output_block *ob)
{
  enum lto_section_type section_type = ob->section_type;

  htab_delete (ob->label_hash_table);
  htab_delete (ob->string_hash_table);

  if (ob->main_hash_table)
    htab_delete (ob->main_hash_table);

  free (ob->main_stream);
  free (ob->string_stream);
  free (ob->named_label_stream);
  if (section_type == LTO_section_function_body)
    {
      free (ob->local_decl_index_stream);
      free (ob->local_decl_stream);
      free (ob->ssa_names_stream);
      free (ob->cfg_stream);
    }

  VEC_free (tree, heap, ob->named_labels);
  if (section_type == LTO_section_function_body)
    {
      VEC_free (int, heap, ob->local_decls_index);
      VEC_free (int, heap, ob->unexpanded_local_decls_index);
    }

  lto_destroy_tree_ref_encoder (&ob->local_decl_encoder);

  free (ob);
}

/* Output STRING of LEN characters to the string
   table in OB. The string might or might not include a trailing '\0'.
   Then put the index onto the INDEX_STREAM.  */

static void
output_string_with_length (struct output_block *ob,
			   struct lto_output_stream *index_stream,
			   const char *s,
			   unsigned int len)
{
  struct string_slot **slot;
  struct string_slot s_slot;
  char *string = (char *) xmalloc (len + 1);
  memcpy (string, s, len);
  string[len] = '\0';

  s_slot.s = string;
  s_slot.len = len;
  s_slot.slot_num = 0;

  slot = (struct string_slot **) htab_find_slot (ob->string_hash_table,
						 &s_slot, INSERT);
  if (*slot == NULL)
    {
      struct lto_output_stream *string_stream = ob->string_stream;
      unsigned int start = string_stream->total_size;
      struct string_slot *new_slot
	= (struct string_slot *) xmalloc (sizeof (struct string_slot));
      unsigned int i;

      new_slot->s = string;
      new_slot->len = len;
      new_slot->slot_num = start;
      *slot = new_slot;
      lto_output_uleb128_stream (index_stream, start);
      lto_output_uleb128_stream (string_stream, len);
      for (i=0; i<len; i++)
	lto_output_1_stream (string_stream, string[i]);
    }
  else
    {
      struct string_slot *old_slot = (struct string_slot *)*slot;
      lto_output_uleb128_stream (index_stream, old_slot->slot_num);
      free (string);
    }
}

/* Output the '\0' terminated STRING to the string
   table in OB.  Then put the index onto the INDEX_STREAM.  */

static void
output_string (struct output_block *ob,
	       struct lto_output_stream *index_stream,
	       const char *string)
{
  if (string)
    {
      lto_output_uleb128_stream (index_stream, 0);
      output_string_with_length (ob, index_stream, string, strlen (string) + 1);
    }
  else
    lto_output_uleb128_stream (index_stream, 1);
}


/* Output the STRING constant to the string
   table in OB.  Then put the index onto the INDEX_STREAM.  */

static void
output_string_cst (struct output_block *ob,
		   struct lto_output_stream *index_stream,
		   tree string)
{
  if (string)
    {
      lto_output_uleb128_stream (index_stream, 0);
      output_string_with_length (ob, index_stream,
				 TREE_STRING_POINTER (string),
				 TREE_STRING_LENGTH (string));
    }
  else
    lto_output_uleb128_stream (index_stream, 1);
}


/* Output the identifier ID to the string
   table in OB.  Then put the index onto the INDEX_STREAM.  */

static void
output_identifier (struct output_block *ob,
		   struct lto_output_stream *index_stream,
		   tree id)
{
  if (id)
    {
      lto_output_uleb128_stream (index_stream, 0);
      output_string_with_length (ob, index_stream,
				 IDENTIFIER_POINTER (id),
				 IDENTIFIER_LENGTH (id));
    }
  else
    lto_output_uleb128_stream (index_stream, 1);
}

/* Put out a real constant.  */

static void
output_real (struct output_block *ob, tree t)
{
  static char real_buffer[1000];
  const REAL_VALUE_TYPE *r = &TREE_REAL_CST (t);

  real_to_hexadecimal (real_buffer, r, 1000, 0, 1);
  output_string (ob, ob->main_stream, real_buffer);
}


/* Write a zero to the output stream.  */

static void
output_zero (struct output_block *ob)
{
  lto_output_1_stream (ob->main_stream, 0);
}


/* Output an unsigned LEB128 quantity to OB->main_stream.  */

static void
output_uleb128 (struct output_block *ob, unsigned HOST_WIDE_INT work)
{
  lto_output_uleb128_stream (ob->main_stream, work);
}


/* Output a signed LEB128 quantity to OB->main_stream.  */

static void
output_sleb128 (struct output_block *ob, HOST_WIDE_INT work)
{
  lto_output_sleb128_stream (ob->main_stream, work);
}

/* HOST_WIDEST_INT version of output_uleb128.  OB and WORK are as in
   output_uleb128. */

static void
output_widest_uint_uleb128 (struct output_block *ob,
			    unsigned HOST_WIDEST_INT work)
{
  lto_output_widest_uint_uleb128_stream (ob->main_stream, work);
}

/* Put out a integer constant.  These are stored as two HOST_WIDE_INTS
   so games may have to be played to shift the data from the high to
   the low value.  */

static void
output_integer (struct output_block *ob, tree t)
{
  lto_output_integer_stream (ob->main_stream, t);
}


/* Output bitmap B to OB.  */

static void
output_bitmap (struct output_block *ob, bitmap b)
{
  bitmap_iterator bi;
  unsigned i;

  if (b == NULL)
    {
      output_zero (ob);
      return;
    }

  /* Indicate how many set bits B has.  */
  output_uleb128 (ob, bitmap_count_bits (b));

  /* FIXME lto.  For now, emit a sequence of all the bit positions
     that are set in B.  This could be compacted by packing multiple
     bits in one word.  */
  EXECUTE_IF_SET_IN_BITMAP (b, 0, i, bi)
    output_uleb128 (ob, i);
}


/* Build a densely packed word that contains only the flags that are
   used for this type of tree EXPR and write the word in uleb128 to
   the OB.  IF CODE is 0 (ERROR_MARK), put the flags anyway.
   FORCE_LOC forces the line number to be serialized regardless of
   the type of tree.  */


static void
output_tree_flags (struct output_block *ob, enum tree_code code, tree expr, 
		   bool force_loc)
{
  lto_flags_type flags;
  const char *current_file;
  int current_line;
  int current_col;

  flags = 0;

  if (code == 0 || TEST_BIT (lto_flags_needed_for, code))
    {

#define START_CLASS_SWITCH()              \
  {                                       \
    enum tree_code code = TREE_CODE (expr); \
                                          \
    switch (TREE_CODE_CLASS (code))       \
    {

#define START_CLASS_CASE(klass)    case klass:
#define ADD_CLASS_DECL_FLAG(flag_name)    \
      { flags <<= 1; if (expr->decl_common. flag_name ) flags |= 1; }
#define ADD_CLASS_EXPR_FLAG(flag_name)    \
      { flags <<= 1; if (expr->base. flag_name ) flags |= 1; }
#define ADD_CLASS_TYPE_FLAG(flag_name)    \
      { flags <<= 1; if (expr->type. flag_name ) flags |= 1; }
#define END_CLASS_CASE(klass)      break;
#define END_CLASS_SWITCH()                \
    default:                              \
      gcc_unreachable ();                 \
    }


#define START_EXPR_SWITCH()               \
    switch (code)			  \
    {
#define START_EXPR_CASE(code)    case code:
#define ADD_EXPR_FLAG(flag_name) \
      { flags <<= 1; if (expr->base. flag_name ) flags |= 1; }
#define ADD_TYPE_FLAG(flag_name) \
      { flags <<= 1; if (expr->type. flag_name ) flags |= 1; }
#define ADD_DECL_FLAG(flag_name) \
      { flags <<= 1; if (expr->decl_common. flag_name ) flags |= 1; }
#define ADD_VIS_FLAG(flag_name)  \
      { flags <<= 1; if (expr->decl_with_vis. flag_name ) flags |= 1; }
#define ADD_VIS_FLAG_SIZE(flag_name,size)	\
      { flags <<= size; if (expr->decl_with_vis. flag_name ) flags |= expr->decl_with_vis. flag_name; }
#define ADD_TLS_FLAG(flag_name,size)	\
      { flags <<= size; if (expr->decl_with_vis. flag_name ) flags |= expr->decl_with_vis. flag_name; }
#define ADD_FUN_FLAG(flag_name)  \
      { flags <<= 1; if (expr->function_decl. flag_name ) flags |= 1; }
#define END_EXPR_CASE(klass)      break;
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

      /* Make sure that we have room to store the locus bits.  */
      {
	lto_flags_type mask;
	mask = LTO_SOURCE_FILE 
	       | LTO_SOURCE_LINE
	       | LTO_SOURCE_COL
	       | LTO_SOURCE_HAS_LOC;
	mask <<= (HOST_BITS_PER_WIDEST_INT - LTO_SOURCE_LOC_BITS); 
	gcc_assert ((flags & mask) == 0);
      }

      flags <<= LTO_SOURCE_LOC_BITS;

      current_file = NULL;
      current_line = -1;
      current_col = -1;

      if (expr)
	{
	  expanded_location xloc;

	  memset (&xloc, 0, sizeof (xloc));

	  if (EXPR_P (expr) && EXPR_HAS_LOCATION (expr))
	    xloc = expand_location (EXPR_LOCATION (expr));
	  else if (force_loc && DECL_P (expr))
	    {
	      /* We use FORCE_LOC here because we only want to put out
		 the line number when we are writing the top level
		 list of var and parm decls, not when we access them
		 inside a function.  */
	      xloc = expand_location (DECL_SOURCE_LOCATION (expr));
	    }
	  else if (TREE_CODE (expr) == BLOCK)
	    xloc = expand_location (BLOCK_SOURCE_LOCATION (expr));

	  if (xloc.file)
	    {
	      current_file = xloc.file;
	      current_line = xloc.line;
	      current_col = xloc.column;
	      flags |= LTO_SOURCE_HAS_LOC;
	    }

	  if (current_file)
	    flags |= LTO_SOURCE_FILE;

	  if (current_line != -1)
	    flags |= LTO_SOURCE_LINE;

	  if (current_col != -1)
	    flags |= LTO_SOURCE_COL;
	}

      output_widest_uint_uleb128 (ob, flags);

      if (flags & LTO_SOURCE_FILE)
	{
	  ob->current_file = current_file;
	  output_string (ob, ob->main_stream, current_file);
	}

      if (flags & LTO_SOURCE_LINE)
	{
	  ob->current_line = current_line;
	  output_uleb128 (ob, current_line);
	}

      if (flags & LTO_SOURCE_COL)
	{
	  ob->current_col = current_col;
	  output_uleb128 (ob, current_col);
	}
    }
}


/* Like output_type_ref, but no debug information is written.  */

static void
output_type_ref_1 (struct output_block *ob, tree node)
{
  /* FIXME lto.  This is a hack, the use of -funsigned-char should be
     reflected in the IL by changing every reference to char_type_node
     into unsigned_char_type_node in pass_ipa_free_lang_data.  */
  if (flag_signed_char == 0 && node == char_type_node)
    node = unsigned_char_type_node;

  output_record_start (ob, NULL, NULL, LTO_type_ref);
  lto_output_type_ref_index (ob->decl_state, ob->main_stream, node);
}


/* Look up NODE in the type table and write the uleb128 index for it to OB.
   This is a hack and will be replaced with a real reference to the type.  */

static void
output_type_ref (struct output_block *ob, tree node)
{
  output_type_ref_1 (ob, node);
}


/* Look up NAME in the type table and if WRITE, write the uleb128
   index for it to OB.  */

static unsigned int
output_local_decl_ref (struct output_block *ob, tree name, bool write)
{
  bool new_local;
  unsigned int index;

  new_local = lto_output_decl_index (write ? ob->main_stream : NULL, 
                                     &ob->local_decl_encoder,
                                     name, &index);
  /* Push the new local decl onto a vector for later processing.  */
  if (new_local)
    {
      VEC_safe_push (int, heap, ob->local_decls_index, 0);
      VEC_safe_push (int, heap, ob->unexpanded_local_decls_index, -1);
    }
  return index;
}


/* Look up LABEL in the label table and write the uleb128 index for it.  */

static void
output_label_ref (struct output_block *ob, tree label)
{
  void **slot;
  struct lto_decl_slot d_slot;
  d_slot.t = label;

  /* If LABEL is DECL_NONLOCAL or FORCED_LABEL, it may be referenced
     from other functions, so it needs to be streamed out in the
     global context.  */
  if (emit_label_in_global_context_p (label))
    {
      struct lto_out_decl_state *state;
      struct lto_output_stream *obs;
      struct lto_tree_ref_encoder *encoder;
      unsigned index;

      obs = ob->main_stream;
      state = ob->decl_state;
      encoder = &state->streams[LTO_DECL_STREAM_LABEL_DECL];
      lto_output_decl_index (obs, encoder, label, &index);
      return;
    }

  slot = htab_find_slot (ob->label_hash_table, &d_slot, INSERT);
  if (*slot == NULL)
    {
      struct lto_decl_slot *new_slot
	= (struct lto_decl_slot *) xmalloc (sizeof (struct lto_decl_slot));

      /* Named labels are given positive integers and unnamed labels are 
	 given negative indexes.  */
      bool named = (DECL_NAME (label) != NULL_TREE);
      int index = named
	? ob->next_named_label_index++ : ob->next_unnamed_label_index--;

      new_slot->t = label;
      new_slot->slot_num = index;
      *slot = new_slot;
      output_sleb128 (ob, index);
      if (named)
	VEC_safe_push (tree, heap, ob->named_labels, label);
    }
  else
    {
      struct lto_decl_slot *old_slot = (struct lto_decl_slot *)*slot;
      gcc_assert (old_slot->t == label);
      output_sleb128 (ob, old_slot->slot_num);
    }
}


/* Output the start of a record with TAG and possibly flags for EXPR,
   and the TYPE for VALUE to OB.  */

static void
output_record_start (struct output_block *ob, tree expr,
		     tree value, unsigned int tag)
{
  lto_output_1_stream (ob->main_stream, tag);
  if (expr)
    {
      enum tree_code code = TREE_CODE (expr);
      if (value
	  && TEST_BIT (lto_types_needed_for, code)
	  && TREE_TYPE (value))
	output_type_ref (ob, TREE_TYPE (value));
      output_tree_flags (ob, code, expr, false);
    }
}


/* Output EH region R in function FN to OB.  CURR_RN is the slot index
   that is being emitted in FN->EH->REGION_ARRAY.  This is used to
   detect EH region sharing.  */

static void
output_eh_region (struct output_block *ob, struct function *fn,
		  eh_region r, int curr_rn)
{
  enum LTO_tags tag;

  if (r == NULL)
    {
      output_zero (ob);
      return;
    }

  /* If R has a different region number than CURR_RN it means that
     CURR_RN is an alias for the original region R.  In this case,
     instead of wasting space emitting all of R again, only emit the
     integer R->REGION_NUMBER so that we can share the EH array slots
     on the reading side.  */
  if (r->region_number != curr_rn)
    {
      /* Make sure the EH regions are indeed shared.  */
      gcc_assert (VEC_index (eh_region, fn->eh->region_array, r->region_number)
		  == VEC_index (eh_region, fn->eh->region_array, curr_rn));

      output_record_start (ob, NULL, NULL, LTO_eh_table_shared_region);
      return;
    }

  if (r->type == ERT_CLEANUP)
    tag = LTO_eh_table_cleanup0;
  else if (r->type == ERT_TRY)
    tag = LTO_eh_table_try0;
  else if (r->type == ERT_CATCH)
    tag = LTO_eh_table_catch0;
  else if (r->type == ERT_ALLOWED_EXCEPTIONS)
    tag = LTO_eh_table_allowed0;
  else if (r->type == ERT_MUST_NOT_THROW)
    tag = LTO_eh_table_must_not_throw0;
  else if (r->type == ERT_THROW)
    tag = LTO_eh_table_throw0;
  else
    gcc_unreachable ();

  /* If the region may contain a throw, use the '1' variant for TAG.  */
  if (r->may_contain_throw)
    tag = (enum LTO_tags)((int)tag + 1);

  output_record_start (ob, NULL, NULL, tag);
  output_sleb128 (ob, r->region_number);
  output_bitmap (ob, r->aka);
  if (r->outer)
    output_uleb128 (ob, r->outer->region_number);
  else
    output_zero (ob);

  if (r->inner)
    output_uleb128 (ob, r->inner->region_number);
  else
    output_zero (ob);

  if (r->next_peer)
    output_uleb128 (ob, r->next_peer->region_number);
  else
    output_zero (ob);

  if (r->tree_label)
    output_expr_operand (ob, r->tree_label);
  else
    output_zero (ob);

  if (r->type == ERT_TRY)
    {
      eh_region eh_catch = r->u.eh_try.eh_catch;
      eh_region last_catch = r->u.eh_try.last_catch;
      if (eh_catch)
	output_uleb128 (ob, eh_catch->region_number);
      else
	output_zero (ob);
      if (last_catch)
	output_uleb128 (ob, last_catch->region_number);
      else
	output_zero (ob);
    }
  else if (r->type == ERT_CATCH)
    {
      tree list;
      eh_region next_catch = r->u.eh_catch.next_catch;
      eh_region prev_catch = r->u.eh_catch.prev_catch;

      if (next_catch)
	output_uleb128 (ob, next_catch->region_number);
      else
	output_zero (ob);

      if (prev_catch)
	output_uleb128 (ob, prev_catch->region_number);
      else
	output_zero (ob);

      /* FIXME lto: output_expr_operand should handle NULL operands
	 by calling output_zero.  */
      list = r->u.eh_catch.type_list;
      if (list)
	output_expr_operand (ob, list);
      else
	output_zero (ob);

      list = r->u.eh_catch.filter_list;
      if (list)
	output_expr_operand (ob, list);
      else
	output_zero (ob);
    }
  else if (r->type == ERT_ALLOWED_EXCEPTIONS)
    {
      tree list = r->u.allowed.type_list;
      if (list)
	output_expr_operand (ob, list);
      else
	output_zero (ob);
      output_uleb128 (ob, r->u.allowed.filter);
    }
  else if (r->type == ERT_THROW)
    {
      output_type_ref (ob, r->u.eh_throw.type);
    }
}


/* Output the existing eh_table to OB.  */

static void
output_eh_regions (struct output_block *ob, struct function *fn)
{
  eh_region curr;

  if (fn->eh && fn->eh->region_array)
    {
      unsigned i;

      output_record_start (ob, NULL, NULL, LTO_eh_table);
      output_sleb128 (ob, fn->eh->last_region_number);

      /* If the EH regions were optimized, there may not be a region
	 tree.  FIXME, if there is no region tree we should not be
	 removing all statements from the EH tables.  This is a bug in
	 the generic EH code.  */
      if (fn->eh->region_tree)
	output_sleb128 (ob, fn->eh->region_tree->region_number);
      else
	output_sleb128 (ob, -1);

      output_sleb128 (ob, VEC_length (eh_region, fn->eh->region_array));
      for (i = 0; VEC_iterate (eh_region, fn->eh->region_array, i, curr); i++)
	output_eh_region (ob, fn, curr, i);
    }

  /* The 0 either terminates the record or indicates that there are no
     eh_records at all.  */
  output_zero (ob);
}


/* Output constructor CTOR to OB.  */

static void
output_constructor (struct output_block *ob, tree ctor)
{
  tree value;
  tree purpose;
  unsigned HOST_WIDE_INT idx;

  output_record_start (ob, ctor, ctor, LTO_constructor);
  output_uleb128 (ob, VEC_length (constructor_elt, CONSTRUCTOR_ELTS (ctor)));

  FOR_EACH_CONSTRUCTOR_ELT (CONSTRUCTOR_ELTS (ctor), idx, purpose, value)
    {
      if (purpose)
	output_expr_operand (ob, purpose);
      else 
	output_zero (ob);

      if (TREE_CODE (value) == CONSTRUCTOR)
	output_constructor (ob, value);
      else 
	output_expr_operand (ob, value);
    }
}


/* Helper for output_tree_block.  T is either a FUNCTION_DECL or a
   BLOCK.  If T is a FUNCTION_DECL, write a reference to it (to avoid
   duplicate definitions on the reader side).  Otherwise, write it as
   a regular tree node.  OB is as in output_tree_block.

   FIXME lto, this would not be needed if streaming of nodes in the
   global context was unified with streaming of function bodies.  */

static void
output_block_or_decl (struct output_block *ob, tree t)
{
  if (t == NULL_TREE)
    output_zero (ob);
  else if (TREE_CODE (t) == FUNCTION_DECL)
    output_expr_operand (ob, t);
  else if (TREE_CODE (t) == BLOCK)
    output_tree (ob, t);
  else
    gcc_unreachable ();
}


/* Write symbol binding block BLOCK to output block OB.  */

static void
output_tree_block (struct output_block *ob, tree block)
{
  unsigned HOST_WIDEST_INT block_flags;
  unsigned i;
  tree t;
  static struct function *last_cfun = NULL;
  static struct pointer_set_t *local_syms = NULL;
  static unsigned last_block_num = 0;

  BLOCK_NUMBER (block) = last_block_num++;

  block_flags = 0;
  lto_set_flag (&block_flags, BLOCK_ABSTRACT (block));
  lto_set_flags (&block_flags, BLOCK_NUMBER (block), 31);
  output_sleb128 (ob, block_flags);

  /* Note that we are only interested in emitting the symbols that are
     actually referenced in CFUN.  Create a set of local symbols to
     use as a filter for BLOCK_VARS.  */
  if (last_cfun != cfun)
    {
      if (local_syms)
	pointer_set_destroy (local_syms);
      
      local_syms = pointer_set_create ();
      for (t = cfun->local_decls; t; t = TREE_CHAIN (t))
	{
	  tree v = TREE_VALUE (t);
	  if (TREE_CODE (v) != TYPE_DECL)
	    pointer_set_insert (local_syms, v);
	}

      last_cfun = cfun;
    }

  /* FIXME lto.  Disabled for now.  This is causing regressions in
     libstdc++ testsuite
     (testsuite/23_containers/list/check_construct_destroy.cc).  */
  for (t = BLOCK_VARS (block); t && 0; t = TREE_CHAIN (t))
    if (TREE_CODE (t) != TYPE_DECL && pointer_set_contains (local_syms, t))
      output_expr_operand (ob, t);
  output_zero (ob);

  output_sleb128 (ob, VEC_length (tree, BLOCK_NONLOCALIZED_VARS (block)));
  for (i = 0; VEC_iterate (tree, BLOCK_NONLOCALIZED_VARS (block), i, t); i++)
    output_expr_operand (ob, t);

  output_block_or_decl (ob, BLOCK_SUPERCONTEXT (block));
  output_block_or_decl (ob, BLOCK_ABSTRACT_ORIGIN (block));
  output_block_or_decl (ob, BLOCK_FRAGMENT_ORIGIN (block));
  output_block_or_decl (ob, BLOCK_FRAGMENT_CHAIN (block));
  output_tree (ob, BLOCK_CHAIN (block));
  output_tree (ob, BLOCK_SUBBLOCKS (block));
}


/* Output EXPR to the main stream in OB.  */

static void
output_expr_operand (struct output_block *ob, tree expr)
{
  enum tree_code code;
  enum tree_code_class klass;
  unsigned int tag;

  if (expr == NULL_TREE)
    {
      output_zero (ob);
      return;
    }

  code = TREE_CODE (expr);
  klass = TREE_CODE_CLASS (code);
  tag = expr_to_tag [code];

  if (klass == tcc_type)
    {
      output_type_ref (ob, expr);
      return;
    }

  switch (code)
    {
    case COMPLEX_CST:
      if (TREE_CODE (TREE_REALPART (expr)) == REAL_CST)
	{
	  output_record_start (ob, expr, expr, LTO_complex_cst1);
	  output_type_ref (ob, TREE_TYPE (TREE_REALPART (expr)));
	  output_real (ob, TREE_REALPART (expr));
	  output_real (ob, TREE_IMAGPART (expr));
	}
      else
	{
	  output_record_start (ob, expr, expr, LTO_complex_cst0);
	  output_type_ref (ob, TREE_TYPE (TREE_REALPART (expr)));
	  output_integer (ob, TREE_REALPART (expr));
	  output_integer (ob, TREE_IMAGPART (expr));
	}
      break;

    case INTEGER_CST:
      output_record_start (ob, expr, expr, tag);
      output_integer (ob, expr);
      break;

    case REAL_CST:
      output_record_start (ob, expr, expr, tag);
      output_real (ob, expr);
      break;

    case STRING_CST:
      {
	/* Most STRING_CSTs have a type when they get here.  The ones
	   in the string operands of asms do not.  Put something there
	   so that all STRING_CSTs can be handled uniformly.  */
	if (!TREE_TYPE (expr))
	  TREE_TYPE (expr) = void_type_node;

	output_record_start (ob, expr, expr, LTO_string_cst);
	output_string_cst (ob, ob->main_stream, expr);
      }
      break;

    case IDENTIFIER_NODE:
      {
	output_record_start (ob, expr, expr, LTO_identifier_node);
	output_identifier (ob, ob->main_stream, expr);
      }
      break;
      

    case VECTOR_CST:
      {
	tree t = TREE_VECTOR_CST_ELTS (expr);
	int len = 1;

	while ((t = TREE_CHAIN (t)) != NULL)
	  len++;
	t = TREE_VECTOR_CST_ELTS (expr);
	if (TREE_CODE (TREE_VALUE(t)) == REAL_CST)
	  {
	    output_record_start (ob, expr, expr, LTO_vector_cst1);
	    output_uleb128 (ob, len);
	    output_type_ref (ob, TREE_TYPE (TREE_VALUE (t)));
	    output_real (ob, TREE_VALUE (t));
	    while ((t = TREE_CHAIN (t)) != NULL)
	      output_real (ob, TREE_VALUE (t));
	  }
	else
	  {
	    output_record_start (ob, expr, expr, LTO_vector_cst0);
	    output_uleb128 (ob, len);
	    output_type_ref (ob, TREE_TYPE (TREE_VALUE (t)));
	    output_integer (ob, TREE_VALUE (t));
	    while ((t = TREE_CHAIN (t)) != NULL)
	      output_integer (ob, TREE_VALUE (t));
	  }
      }
      break;

    case CASE_LABEL_EXPR:
      {
	int variant = 0;
	if (CASE_LOW (expr) != NULL_TREE)
	  variant |= 0x1;
	if (CASE_HIGH (expr) != NULL_TREE)
	  variant |= 0x2;
	output_record_start (ob, expr, NULL, LTO_case_label_expr0 + variant);

	if (CASE_LOW (expr) != NULL_TREE)
	  output_expr_operand (ob, CASE_LOW (expr));
	if (CASE_HIGH (expr) != NULL_TREE)
	  output_expr_operand (ob, CASE_HIGH (expr));
	output_label_ref (ob, CASE_LABEL (expr));
      }
      break;

    case CONSTRUCTOR:
      output_constructor (ob, expr);
      break;

    case SSA_NAME:
      output_record_start (ob, expr, expr, LTO_ssa_name);
      output_uleb128 (ob, SSA_NAME_VERSION (expr));
      break;

    case CONST_DECL:
      /* We should not see these by the time we get here.  All these
	 have been folded into their DECL_INITIAL values.  */
      gcc_unreachable ();
      break;

    case FIELD_DECL:
      output_record_start (ob, NULL, NULL, LTO_field_decl);
      lto_output_field_decl_index (ob->decl_state, ob->main_stream, expr);
      break;

    case FUNCTION_DECL:
      tag = DECL_IS_BUILTIN (expr) ? LTO_function_decl1 : LTO_function_decl0;
      output_record_start (ob, NULL, NULL, tag);
      lto_output_fn_decl_index (ob->decl_state, ob->main_stream, expr);
      break;

    case VAR_DECL:
      if (decl_function_context (expr) == NULL)
	{
	  output_record_start (ob, NULL, NULL, LTO_var_decl1);
	  lto_output_var_decl_index (ob->decl_state, ob->main_stream, expr);
	}
      else
	{
	  output_record_start (ob, NULL, NULL, LTO_var_decl0);
	  output_local_decl_ref (ob, expr, true);
	}
      break;

    case TYPE_DECL:
      output_record_start (ob, NULL, NULL, LTO_type_decl);
      lto_output_type_decl_index (ob->decl_state, ob->main_stream, expr);
      break;

    case NAMESPACE_DECL:
      output_record_start (ob, NULL, NULL, tag);
      lto_output_namespace_decl_index (ob->decl_state, ob->main_stream, expr);
      break;

    case PARM_DECL:
      gcc_assert (!DECL_RTL_SET_P (expr));
      output_record_start (ob, NULL, NULL, tag);
      output_local_decl_ref (ob, expr, true);
      break;

    case LABEL_DECL:
      tag = emit_label_in_global_context_p (expr)
	    ? LTO_label_decl1
	    : LTO_label_decl0;
      output_record_start (ob, expr, NULL_TREE, tag);
      output_label_ref (ob, expr);
      break;

    case RESULT_DECL:
      output_record_start (ob, expr, expr, tag);
      lto_output_var_decl_index (ob->decl_state, ob->main_stream, expr);
      break;

    case COMPONENT_REF:
      output_record_start (ob, expr, expr, tag);
      output_expr_operand (ob, TREE_OPERAND (expr, 0));
      output_expr_operand (ob, TREE_OPERAND (expr, 1));
      output_expr_operand (ob, TREE_OPERAND (expr, 2));
      break;

    case BIT_FIELD_REF:
      {
	tree op1 = TREE_OPERAND (expr, 1);
	tree op2 = TREE_OPERAND (expr, 2);
	if ((TREE_CODE (op1) == INTEGER_CST)
	    && (TREE_CODE (op2) == INTEGER_CST))
	  {
	    output_record_start (ob, expr, expr,
				 LTO_bit_field_ref1);
	    output_uleb128 (ob, TREE_INT_CST_LOW (op1));
	    output_uleb128 (ob, TREE_INT_CST_LOW (op2));
	    output_expr_operand (ob, TREE_OPERAND (expr, 0));
	  }
	else
	  {
	    output_record_start (ob, expr, expr,
				 LTO_bit_field_ref0);
	    output_expr_operand (ob, TREE_OPERAND (expr, 0));
	    output_expr_operand (ob, op1);
	    output_expr_operand (ob, op2);
	  }
      }
      break;

    case ARRAY_REF:
    case ARRAY_RANGE_REF:
      output_record_start (ob, expr, expr, tag);
      output_expr_operand (ob, TREE_OPERAND (expr, 0));
      output_expr_operand (ob, TREE_OPERAND (expr, 1));
      output_expr_operand (ob, TREE_OPERAND (expr, 2));
      output_expr_operand (ob, TREE_OPERAND (expr, 3));
      break;


    case ASM_EXPR:
      {
	tree string_cst = ASM_STRING (expr);
	output_record_start (ob, expr, NULL, LTO_asm_expr);
	output_string_cst (ob, ob->main_stream, string_cst);
	if (ASM_INPUTS (expr))
	  output_expr_operand (ob, ASM_INPUTS (expr));
	else 
	  output_zero (ob);

	if (ASM_OUTPUTS (expr))
	  output_expr_operand (ob, ASM_OUTPUTS (expr));
	else 
	  output_zero (ob);

	if (ASM_CLOBBERS (expr))
	  output_expr_operand (ob, ASM_CLOBBERS (expr));
	else 
	  output_zero (ob);
      }
      break;

    case RANGE_EXPR:
      {
	output_record_start (ob, NULL, NULL, LTO_range_expr);
	/* Need the types here to reconstruct the ranges.  */
	output_type_ref (ob, TREE_OPERAND (expr, 0));
	output_integer (ob, TREE_OPERAND (expr, 0));
	output_type_ref (ob, TREE_OPERAND (expr, 1));
	output_integer (ob, TREE_OPERAND (expr, 1));
      }
      break; 

    case TREE_LIST:
      {
	tree tl;
	int count = 0;

	output_record_start (ob, expr, NULL, tag);
	for (tl = expr; tl; tl = TREE_CHAIN (tl))
	  count++;

	gcc_assert (count);
	output_uleb128 (ob, count);
	for (tl = expr; tl; tl = TREE_CHAIN (tl))
	  {
	    if (TREE_VALUE (tl) != NULL_TREE)
	      output_expr_operand (ob, TREE_VALUE (tl));
	    else
	      output_zero (ob);
	    
	    if (TREE_PURPOSE (tl))
	      output_expr_operand (ob, TREE_PURPOSE (tl));
	    else
	      output_zero (ob);
	  }
      }
      break;

      /* This is the default case. All of the cases that can be done
	 completely mechanically are done here.  */
      {
	int i;
#define SET_NAME(a,b)
#define MAP_EXPR_TAG(expr, tag) case expr:
#define TREE_SINGLE_MECHANICAL_TRUE

#include "lto-tree-tags.def"
#undef MAP_EXPR_TAG
#undef TREE_SINGLE_MECHANICAL_TRUE
#undef SET_NAME
	output_record_start (ob, expr, expr, tag);
	for (i = 0; i < TREE_CODE_LENGTH (TREE_CODE (expr)); i++)
	  output_expr_operand (ob, TREE_OPERAND (expr, i));
	break;
      }

    default:
      /* We cannot have forms that are not explicity handled.  So when
	 this is triggered, there is some form that is not being
	 output.  */
      gcc_unreachable ();
    }
}


/* Output the local var at INDEX to OB.  */

static void
output_local_var_decl (struct output_block *ob, int index)
{
  tree decl, name;
  unsigned variant = 0;
  bool is_var, needs_backing_var;
  unsigned int tag;
  
  decl = lto_tree_ref_encoder_get_tree (&ob->local_decl_encoder, index);
  is_var = (TREE_CODE (decl) == VAR_DECL);
  needs_backing_var = (DECL_DEBUG_EXPR_IS_FROM (decl) && DECL_DEBUG_EXPR (decl));
  
  variant |= DECL_ATTRIBUTES (decl)      != NULL_TREE ? 0x01 : 0;
  variant |= DECL_SIZE_UNIT (decl)       != NULL_TREE ? 0x02 : 0;
  variant |= needs_backing_var                        ? 0x04 : 0;

  /* This will either be a local var decl or a parm decl. */
  tag = (is_var ? LTO_local_var_decl_body0 : LTO_parm_decl_body0) + variant;

  output_record_start (ob, NULL, NULL, tag);

  /* To facilitate debugging, create a DECL_NAME for compiler temporaries
     so they match the format 'D.<uid>' used by the pretty printer.  This
     will reduce some spurious differences in dump files between the
     original front end and gimple.  Note, however, that this will not
     fix all differences.  Temporaries generated by optimizers in lto1
     will have different DECL_UIDs than those created by the
     optimizers in the original front end.  */
  if (DECL_NAME (decl) == NULL)
    {
      char s[20];
      gcc_assert (DECL_UID (decl) < 1000000);
      sprintf (s, "D.%d", DECL_UID (decl));
      name = get_identifier (s);
    }
  else
    name = DECL_NAME (decl);

  output_identifier (ob, ob->main_stream, name);
  output_identifier (ob, ob->main_stream, decl->decl_with_vis.assembler_name);

  output_type_ref (ob, TREE_TYPE (decl));
  
  if (is_var)
    {
      if (DECL_INITIAL (decl))
	output_expr_operand (ob, DECL_INITIAL (decl));
      else
	output_zero (ob);

      /* Index in unexpanded_vars_list.  */
      output_sleb128 (ob, VEC_index (int, ob->unexpanded_local_decls_index,
				     index));
    }
  else
    {
      output_type_ref (ob, DECL_ARG_TYPE (decl));

      /* The chain is only necessary for parm_decls.  */
      if (TREE_CHAIN (decl))
	output_expr_operand (ob, TREE_CHAIN (decl));
      else
	output_zero (ob);
    }

  clear_line_info (ob);
  output_tree_flags (ob, ERROR_MARK, decl, true);

  gcc_assert (decl_function_context (decl) != NULL_TREE);

  output_uleb128 (ob, DECL_ALIGN (decl));
  
  /* Put out the subtrees.  */
  /* Note that DECL_SIZE might be NULL_TREE for a
     variably-modified type.  See reset_lang_specific
     and the comment above.  */
  if (DECL_SIZE (decl))
    output_expr_operand (ob, DECL_SIZE (decl));
  else
    output_zero (ob);

  if (DECL_ATTRIBUTES (decl)!= NULL_TREE)
    output_expr_operand (ob, DECL_ATTRIBUTES (decl));

  if (DECL_SIZE_UNIT (decl) != NULL_TREE)
    output_expr_operand (ob, DECL_SIZE_UNIT (decl));

  if (needs_backing_var)
    output_expr_operand (ob, DECL_DEBUG_EXPR (decl));

  if (DECL_HAS_VALUE_EXPR_P (decl))
    output_expr_operand (ob, DECL_VALUE_EXPR (decl));
}

/* Output the local declaration or type at INDEX to OB.  */

static void
output_local_decl (struct output_block *ob, int index)
{
  tree decl = lto_tree_ref_encoder_get_tree (&ob->local_decl_encoder, index);

  VEC_replace (int, ob->local_decls_index, index, ob->main_stream->total_size);

  if (TREE_CODE (decl) == VAR_DECL
      || TREE_CODE (decl) == PARM_DECL)
    output_local_var_decl (ob, index);
  else
    gcc_unreachable ();
}


/* Output the local declarations and types to OB.  */

static void
output_local_vars (struct output_block *ob, struct function *fn)
{
  unsigned int index = 0;
  tree t;
  int i = 0;
  struct lto_output_stream *tmp_stream = ob->main_stream;
  bitmap local_statics = lto_bitmap_alloc ();

  ob->main_stream = ob->local_decl_stream;

  /* We have found MOST of the local vars by scanning the function.
     However, many local vars have other local vars inside them.
     Other local vars can be found by walking the unexpanded vars
     list.  */

  /* Need to put out the local statics first, to avoid the pointer
     games used for the regular locals.  */
  for (t = fn->local_decls; t; t = TREE_CHAIN (t))
    {
      tree lv = TREE_VALUE (t);

      if (DECL_CONTEXT (lv) == NULL)
	{
	  /* Do not put the static in the chain more than once, even
	     if it was in the chain more than once to start.  */
	  if (!bitmap_bit_p (local_statics, DECL_UID (lv)))
	    {
	      bitmap_set_bit (local_statics, DECL_UID (lv));
	      output_expr_operand (ob, lv);

	      gcc_assert (!DECL_CONTEXT (lv));

	      if (DECL_INITIAL (lv))
		output_expr_operand (ob, DECL_INITIAL (lv));
	      else 
		output_zero (ob); /* DECL_INITIAL.  */
	    }
	}
      else
	{
	  int j = output_local_decl_ref (ob, lv, false);
	  /* Just for the fun of it, some of the locals are in the
	     local_decls_list more than once.  */
	  if (VEC_index (int, ob->unexpanded_local_decls_index, j) == -1)
	    VEC_replace (int, ob->unexpanded_local_decls_index, j, i++);
	}
    }

  /* End of statics.  */
  output_zero (ob);
  lto_bitmap_free (local_statics);

  /* The easiest way to get all of this stuff generated is to play
     pointer games with the streams and reuse the code for putting out
     the function bodies for putting out the local decls.  It needs to
     go into a separate stream because the LTO reader will want to
     process the local variables first, rather than have to back patch
     them.  */
  while (index < lto_tree_ref_encoder_size (&ob->local_decl_encoder))
    output_local_decl (ob, index++);

  ob->main_stream = tmp_stream;
}


/* Output the local var_decls index and parm_decls index to OB.  */

static void
output_local_vars_index (struct output_block *ob)
{
  unsigned int index = 0;
  unsigned int stop;

  struct lto_output_stream *tmp_stream = ob->main_stream;
  ob->main_stream = ob->local_decl_index_stream;

  stop = VEC_length (int, ob->local_decls_index);
  for (index = 0; index < stop; index++)
    output_uleb128 (ob, VEC_index (int, ob->local_decls_index, index));

  ob->main_stream = tmp_stream;
}


/* Output the names in the named labels to the named_label stream.  */

static void
output_named_labels (struct output_block *ob)
{
  unsigned int index = 0;
  clear_line_info (ob);

  while (index < VEC_length (tree, ob->named_labels))
    {
      tree decl = VEC_index (tree, ob->named_labels, index++);
      tree name = DECL_NAME (decl);
      output_identifier (ob, ob->named_label_stream, name);
   }
}


/* Output all of the active ssa names to the ssa_names stream.  */

static void
output_ssa_names (struct output_block *ob, struct function *fn)
{
  /* Switch streams so we can use output_expr_operand to write the
     SSA_NAME_VAR.  */
  struct lto_output_stream *tmp_stream = ob->main_stream;
  unsigned int i;
  unsigned int len = VEC_length (tree, SSANAMES (fn));

  ob->main_stream = ob->ssa_names_stream;
  output_uleb128 (ob, len);

  for (i = 1; i < len; i++)
    {
      tree ptr = VEC_index (tree, SSANAMES (fn), i);

      if (ptr == NULL_TREE
	  || SSA_NAME_IN_FREE_LIST (ptr)
	  || !is_gimple_reg (ptr))
	continue;

      output_uleb128 (ob, i);
      output_expr_operand (ob, SSA_NAME_VAR (ptr));
      /* Use code ERROR_MARK to force flags to be output.  */
      output_tree_flags (ob, ERROR_MARK, ptr, false);
    }

  output_zero (ob);
  ob->main_stream = tmp_stream;
}


/* Output the cfg.  */

static void
output_cfg (struct output_block *ob, struct function *fn)
{
  struct lto_output_stream *tmp_stream = ob->main_stream;
  basic_block bb;

  ob->main_stream = ob->cfg_stream;

  output_uleb128 (ob, profile_status_for_function (fn));

  /* Output the number of the highest basic block.  */
  output_uleb128 (ob, last_basic_block_for_function (fn));

  FOR_ALL_BB_FN (bb, fn)
    {
      edge_iterator ei;
      edge e;

      output_sleb128 (ob, bb->index);

      /* Output the successors and the edge flags.  */
      output_uleb128 (ob, EDGE_COUNT (bb->succs));
      FOR_EACH_EDGE (e, ei, bb->succs)
	{
	  output_uleb128 (ob, e->dest->index);
	  output_sleb128 (ob, e->probability);
	  output_sleb128 (ob, e->count);
	  output_uleb128 (ob, e->flags);
	}
    }

  output_sleb128 (ob, -1);

  bb = ENTRY_BLOCK_PTR;
  while (bb->next_bb)
    {
      output_sleb128 (ob, bb->next_bb->index);
      bb = bb->next_bb;
    }

  output_sleb128 (ob, -1);

  ob->main_stream = tmp_stream;
}


/* Output PHI function PHI to the main stream in OB.  */

static void
output_phi (struct output_block *ob, gimple phi)
{
  unsigned i, len = gimple_phi_num_args (phi);
  
  lto_output_1_stream (ob->main_stream, LTO_gimple_phi);
  output_uleb128 (ob, SSA_NAME_VERSION (PHI_RESULT (phi)));

  for (i = 0; i < len; i++)
    {
      output_expr_operand (ob, gimple_phi_arg_def (phi, i));
      output_uleb128 (ob, gimple_phi_arg_edge (phi, i)->src->index);
    }
}


/* Emit the location of STMT to outpub block OB.  */

static void
output_stmt_location (struct output_block *ob, gimple stmt)
{
  expanded_location xloc;

  xloc = expand_location (gimple_location (stmt));
  if (xloc.file == NULL)
    {
      output_string (ob, ob->main_stream, xloc.file);
      return;
    }

  output_string (ob, ob->main_stream, xloc.file);
  output_sleb128 (ob, xloc.line);
  output_sleb128 (ob, xloc.column);

  ob->current_file = xloc.file;
  ob->current_line = xloc.line;
  ob->current_col = xloc.column;
}


/* Emit statement STMT on the main stream of output block OB.  */

static void
output_gimple_stmt (struct output_block *ob, gimple stmt)
{
  unsigned i;
  enum gimple_code code = gimple_code (stmt);
  unsigned int tag = stmt_to_tag[code];

  /* Emit identifying tag.  */
  gcc_assert (tag < UCHAR_MAX);
  lto_output_1_stream (ob->main_stream, (char) tag);

  /* Emit the number of operands in the statement.  */
  lto_output_uleb128_stream (ob->main_stream, gimple_num_ops (stmt));

  /* Emit location information for the statement.  */
  output_stmt_location (ob, stmt);

  /* Emit the lexical block holding STMT.  */
  output_tree (ob, gimple_block (stmt));

  /* Emit the tuple header.  FIXME lto.  This is emitting fields that are not
     necessary to emit (e.g., gimple_statement_base.bb,
     gimple_statement_base.block).  */
  lto_output_data_stream (ob->main_stream, stmt, gimple_size (code));

  /* Emit the operands.  */
  switch (gimple_code (stmt))
    {
    case GIMPLE_ASM:
      output_string (ob, ob->main_stream, gimple_asm_string (stmt));
      /* Fallthru  */

    case GIMPLE_ASSIGN:
    case GIMPLE_CALL:
    case GIMPLE_RETURN:
    case GIMPLE_SWITCH:
    case GIMPLE_LABEL:
    case GIMPLE_COND:
    case GIMPLE_GOTO:
    case GIMPLE_PREDICT:
    case GIMPLE_RESX:
      for (i = 0; i < gimple_num_ops (stmt); i++)
	{
	  tree op = gimple_op (stmt, i);
	  if (op)
	    output_expr_operand (ob, op);
	  else
	    output_zero (ob);
	}
      break;

    default:
      gcc_unreachable ();
    }
}


/* Output a basic block BB to the main stream in OB for this FN.  */

static void
output_bb (struct output_block *ob, basic_block bb, struct function *fn)
{
  gimple_stmt_iterator bsi = gsi_start_bb (bb);

  output_record_start (ob, NULL, NULL,
		       (!gsi_end_p (bsi)) || phi_nodes (bb)
		        ? LTO_bb1
			: LTO_bb0);

  output_uleb128 (ob, bb->index);
  output_sleb128 (ob, bb->count);
  output_sleb128 (ob, bb->loop_depth);
  output_sleb128 (ob, bb->frequency);
  output_sleb128 (ob, bb->flags);

  if (!gsi_end_p (bsi) || phi_nodes (bb))
    {
      /* Output the statements.  The list of statements is terminated
	 with a zero.  */
      for (bsi = gsi_start_bb (bb); !gsi_end_p (bsi); gsi_next (&bsi))
	{
	  int region;
	  gimple stmt = gsi_stmt (bsi);

	  output_gimple_stmt (ob, stmt);
	
	  /* Emit the EH region holding STMT.  If the EH region is the
	     same as the previous statement, emit a 0 for brevity.  */
	  region = lookup_stmt_eh_region_fn (fn, stmt);
	  if (region != last_eh_region_seen)
	    {
	      output_record_start (ob, NULL, NULL,
				   LTO_set_eh0 + (region ? 1 : 0));
	      if (region)
		output_sleb128 (ob, region);

	      last_eh_region_seen = region;
	    }
	  else
	    output_zero (ob);
	}

      output_zero (ob);

      for (bsi = gsi_start_phis (bb); !gsi_end_p (bsi); gsi_next (&bsi))
	{
	  gimple phi = gsi_stmt (bsi);

	  /* Only emit PHIs for gimple registers.  PHI nodes for .MEM
	     will be filled in on reading when the SSA form is
	     updated.  */
	  if (is_gimple_reg (gimple_phi_result (phi)))
	    output_phi (ob, phi);
	}

      output_zero (ob);
    }
}

/* Create the header in the file using OB.  If the section type is for
   a function, set FN to the decl for that function.  */

static void
produce_asm (struct output_block *ob, tree fn)
{
  enum lto_section_type section_type = ob->section_type;
  struct lto_function_header header;
  char *section_name;
  struct lto_output_stream *header_stream;

  if (section_type == LTO_section_function_body)
    {
      const char *name = IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (fn));
      section_name = lto_get_section_name (section_type, name);
    }
  else
    section_name = lto_get_section_name (section_type, NULL);

  lto_begin_section (section_name);
  free (section_name);

  /* The entire header is stream computed here.  */
  memset (&header, 0, sizeof (struct lto_function_header));
  
  /* Write the header.  */
  header.lto_header.major_version = LTO_major_version;
  header.lto_header.minor_version = LTO_minor_version;
  header.lto_header.section_type = section_type;
  
  header.num_local_decls =
    lto_tree_ref_encoder_size (&ob->local_decl_encoder);
  header.num_named_labels = ob->next_named_label_index;
  header.num_unnamed_labels = -ob->next_unnamed_label_index;
  header.compressed_size = 0;
  
  header.named_label_size = ob->named_label_stream->total_size;
  if (section_type == LTO_section_function_body)
    {
      header.ssa_names_size = ob->ssa_names_stream->total_size;
      header.cfg_size = ob->cfg_stream->total_size;
      header.local_decls_index_size = ob->local_decl_index_stream->total_size;
      header.local_decls_size = ob->local_decl_stream->total_size;
    }
  header.main_size = ob->main_stream->total_size;
  header.string_size = ob->string_stream->total_size;

  header_stream = XCNEW (struct lto_output_stream);
  lto_output_data_stream (header_stream, &header, sizeof header);
  lto_write_stream (header_stream);
  free (header_stream);

  /* Put all of the gimple and the string table out the asm file as a
     block of text.  */
  lto_write_stream (ob->named_label_stream);
  if (section_type == LTO_section_function_body)
    {
      lto_write_stream (ob->ssa_names_stream);
      lto_write_stream (ob->cfg_stream);
      lto_write_stream (ob->local_decl_index_stream);
      lto_write_stream (ob->local_decl_stream);
    }
  lto_write_stream (ob->main_stream);
  lto_write_stream (ob->string_stream);

  lto_end_section ();
}


static bool initialized = false;
static bool initialized_local = false;


/* Static initialization for both the lto reader and the lto
   writer.  */

void
lto_static_init (void)
{
  if (initialized)
    return;

  initialized = true;

  lto_flags_needed_for = sbitmap_alloc (NUM_TREE_CODES);
  sbitmap_ones (lto_flags_needed_for);
  RESET_BIT (lto_flags_needed_for, FIELD_DECL);
  RESET_BIT (lto_flags_needed_for, FUNCTION_DECL);
  RESET_BIT (lto_flags_needed_for, IDENTIFIER_NODE);
  RESET_BIT (lto_flags_needed_for, PARM_DECL);
  RESET_BIT (lto_flags_needed_for, SSA_NAME);
  RESET_BIT (lto_flags_needed_for, VAR_DECL);
  RESET_BIT (lto_flags_needed_for, TREE_LIST);
  RESET_BIT (lto_flags_needed_for, TREE_VEC);
  RESET_BIT (lto_flags_needed_for, TYPE_DECL);
  RESET_BIT (lto_flags_needed_for, TRANSLATION_UNIT_DECL);
  RESET_BIT (lto_flags_needed_for, NAMESPACE_DECL);

  lto_types_needed_for = sbitmap_alloc (NUM_TREE_CODES);

  /* Global declarations and types will handle the
     type field by other means, so lto_types_needed_for
     should not be set for them.  */
#if REDUNDANT_TYPE_SYSTEM
  /* These forms never need types.  */
  sbitmap_ones (lto_types_needed_for);
  RESET_BIT (lto_types_needed_for, ASM_EXPR);
  RESET_BIT (lto_types_needed_for, BLOCK);
  RESET_BIT (lto_types_needed_for, CASE_LABEL_EXPR);
  RESET_BIT (lto_types_needed_for, FIELD_DECL);
  RESET_BIT (lto_types_needed_for, FUNCTION_DECL);
  RESET_BIT (lto_types_needed_for, IDENTIFIER_NODE);
  RESET_BIT (lto_types_needed_for, LABEL_DECL);
  RESET_BIT (lto_types_needed_for, LABEL_EXPR);
  RESET_BIT (lto_types_needed_for, MODIFY_EXPR);
  RESET_BIT (lto_types_needed_for, PARM_DECL);
  RESET_BIT (lto_types_needed_for, RESX_EXPR);
  RESET_BIT (lto_types_needed_for, SSA_NAME);
  RESET_BIT (lto_types_needed_for, VAR_DECL);
  RESET_BIT (lto_types_needed_for, TREE_LIST);
  RESET_BIT (lto_types_needed_for, TREE_VEC);
  RESET_BIT (lto_types_needed_for, TYPE_DECL);
  RESET_BIT (lto_types_needed_for, NAMESPACE_DECL);
  RESET_BIT (lto_types_needed_for, TRANSLATION_UNIT_DECL);
  /* These forms *are* the types.  */
  RESET_BIT (lto_types_needed_for, VOID_TYPE);
  RESET_BIT (lto_types_needed_for, INTEGER_TYPE);
  RESET_BIT (lto_types_needed_for, REAL_TYPE);
  RESET_BIT (lto_types_needed_for, FIXED_POINT_TYPE);
  RESET_BIT (lto_types_needed_for, COMPLEX_TYPE);
  RESET_BIT (lto_types_needed_for, BOOLEAN_TYPE);
  RESET_BIT (lto_types_needed_for, OFFSET_TYPE);
  RESET_BIT (lto_types_needed_for, ENUMERAL_TYPE);
  RESET_BIT (lto_types_needed_for, POINTER_TYPE);
  RESET_BIT (lto_types_needed_for, REFERENCE_TYPE);
  RESET_BIT (lto_types_needed_for, VECTOR_TYPE);
  RESET_BIT (lto_types_needed_for, ARRAY_TYPE);
  RESET_BIT (lto_types_needed_for, RECORD_TYPE);
  RESET_BIT (lto_types_needed_for, UNION_TYPE);
  RESET_BIT (lto_types_needed_for, QUAL_UNION_TYPE);
  RESET_BIT (lto_types_needed_for, FUNCTION_TYPE);
  RESET_BIT (lto_types_needed_for, METHOD_TYPE);
#else
  /* These forms will need types, even when the type system is fixed.  */
  SET_BIT (lto_types_needed_for, COMPLEX_CST);
  SET_BIT (lto_types_needed_for, CONSTRUCTOR);
  SET_BIT (lto_types_needed_for, CONVERT_EXPR);
  SET_BIT (lto_types_needed_for, FIXED_CONVERT_EXPR);
  SET_BIT (lto_types_needed_for, FIXED_CST);
  SET_BIT (lto_types_needed_for, INTEGER_CST);
  SET_BIT (lto_types_needed_for, NOP_EXPR);
  SET_BIT (lto_types_needed_for, REAL_CST);
  SET_BIT (lto_types_needed_for, STRING_CST);
  SET_BIT (lto_types_needed_for, VECTOR_CST );
  SET_BIT (lto_types_needed_for, VIEW_CONVERT_EXPR);
#endif
}


/* Static initialization for the lto writer.  */

static void
lto_init_writer (void)
{
  if (initialized_local)
    return;

  initialized_local = true;

  /* Initialize the expression and statement to tag mappings.  */
#define MAP_EXPR_TAG(expr,tag)   expr_to_tag[expr] = tag;
#define MAP_STMT_TAG(stmt,tag)   stmt_to_tag[stmt] = tag;
#define TREE_SINGLE_MECHANICAL_TRUE
#define TREE_SINGLE_MECHANICAL_FALSE
#define GIMPLE_CODES
#include "lto-tree-tags.def"

#undef MAP_EXPR_TAG
#undef TREE_SINGLE_MECHANICAL_TRUE
#undef TREE_SINGLE_MECHANICAL_FALSE
#undef GIMPLE_CODES

  lto_static_init ();
}


#ifdef FILE_PER_FUNCTION
/* The once per compilation unit initialization flag.  */
static int function_num;
#endif


/* Output the body of function NODE->DECL.  */

static void
output_function (struct cgraph_node *node)
{
  unsigned HOST_WIDEST_INT flags;
  tree function = node->decl;
  struct function *fn = DECL_STRUCT_FUNCTION (function);
  basic_block bb;
  struct output_block *ob = create_output_block (LTO_section_function_body);

  clear_line_info (ob);
  ob->cgraph_node = node;
  ob->main_hash_table = htab_create (37, lto_hash_global_slot_node,
				     lto_eq_global_slot_node, free);

  gcc_assert (!current_function_decl && !cfun);

  /* Set current_function_decl and cfun.  */
  current_function_decl = function;
  push_cfun (fn);

  /* Make string 0 be a NULL string.  */
  lto_output_1_stream (ob->string_stream, 0);

  last_eh_region_seen = 0;

  output_record_start (ob, NULL, NULL, LTO_function);

  /* Write all the attributes for FN.  Note that flags must be
     encoded in opposite order as they are decoded in
     input_function.  */
  flags = 0;
  lto_set_flag (&flags, fn->is_thunk);
  lto_set_flag (&flags, fn->has_local_explicit_reg_vars);
  lto_set_flag (&flags, fn->after_tree_profile);
  lto_set_flag (&flags, fn->returns_pcc_struct);
  lto_set_flag (&flags, fn->returns_struct);
  lto_set_flag (&flags, fn->always_inline_functions_inlined);
  lto_set_flag (&flags, fn->after_inlining);
  lto_set_flag (&flags, fn->dont_save_pending_sizes_p);
  lto_set_flag (&flags, fn->stdarg);
  lto_set_flag (&flags, fn->has_nonlocal_label);
  lto_set_flag (&flags, fn->calls_alloca);
  lto_set_flag (&flags, fn->calls_setjmp);
  lto_set_flags (&flags, fn->function_frequency, 2);
  lto_set_flags (&flags, fn->va_list_fpr_size, 8);
  lto_set_flags (&flags, fn->va_list_gpr_size, 8);

  lto_output_widest_uint_uleb128_stream (ob->main_stream, flags);

  /* Output the static chain and non-local goto save area.  */
  if (fn->static_chain_decl)
    output_expr_operand (ob, fn->static_chain_decl);
  else
    output_zero (ob);

  if (fn->nonlocal_goto_save_area)
    output_expr_operand (ob, fn->nonlocal_goto_save_area);
  else
    output_zero (ob);

  /* Output any exception handling regions.  */
  output_eh_regions (ob, fn);

  /* Output DECL_INITIAL for the function, which contains the tree of
     lexical scopes.  */
  output_tree (ob, DECL_INITIAL (function));

  /* Output the head of the arguments list.  */
  if (DECL_ARGUMENTS (function))
    output_expr_operand (ob, DECL_ARGUMENTS (function));
  else
    output_zero (ob);

  /* We will renumber the statements.  The code that does this uses
     the same ordering that we use for serializing them so we can use
     the same code on the other end and not have to write out the
     statement numbers.  */
  renumber_gimple_stmt_uids ();

  /* Output the code for the function.  */
  FOR_ALL_BB_FN (bb, fn)
    output_bb (ob, bb, fn);

  /* The terminator for this function.  */
  output_zero (ob);

  output_ssa_names (ob, fn);
  output_cfg (ob, fn);
  output_local_vars (ob, fn);
  output_local_vars_index (ob);
  output_named_labels (ob);

  /* Create a section to hold the pickled output of this function.   */
  produce_asm (ob, function);

  destroy_output_block (ob);

  current_function_decl = NULL;

  pop_cfun ();
}

/* Output initializer of VAR in output block OB.  */

static void
output_var_init (struct output_block *ob, tree var)
{
  output_expr_operand (ob, var);
  if (DECL_INITIAL (var))
    output_expr_operand (ob, DECL_INITIAL (var));
  else
    output_zero (ob);
}

/* Output all global vars reachable from STATE to output block OB.
   SEEN is a bitmap indexed by DECL_UID of vars to avoid multiple
   outputs in the same file.  */

static void
output_inits_in_decl_state (struct output_block *ob,
			    struct lto_out_decl_state *state,
			    bitmap seen)
{
  struct lto_tree_ref_encoder *encoder =
    &state->streams[LTO_DECL_STREAM_VAR_DECL];
  unsigned num_vars, i;

  num_vars = lto_tree_ref_encoder_size (encoder);
  for (i = 0; i < num_vars; i++)
    {
      tree var = lto_tree_ref_encoder_get_tree (encoder, i);
      tree context = DECL_CONTEXT (var);
      gcc_assert (!context || TREE_CODE (context) == FUNCTION_DECL);
      if (TREE_STATIC (var)
	  && !context
	  && !bitmap_bit_p (seen, DECL_UID (var)))
	{
	  bitmap_set_bit (seen, DECL_UID (var));
	  output_var_init (ob, var);
	}
    }
}

/* Output used constructors for static or external vars to OB.  */

static void
output_used_constructors_and_inits (struct output_block *ob)
{
  bitmap seen;
  struct lto_out_decl_state *out_state = lto_get_out_decl_state ();
  struct lto_out_decl_state *fn_out_state;
  unsigned i, num_fns;

  /* Go through all out-state to find out variable used. */
  seen = lto_bitmap_alloc ();
  num_fns = VEC_length (lto_out_decl_state_ptr, lto_function_decl_states);
  output_inits_in_decl_state (ob, out_state, seen);
  for (i = 0; i < num_fns; i++)
    {
      fn_out_state =
	VEC_index (lto_out_decl_state_ptr, lto_function_decl_states, i);
      output_inits_in_decl_state (ob, fn_out_state, seen);
    }
  lto_bitmap_free (seen);
}

/* Output constructors and inits of all vars in varpool that have not been
   output so far.  This is done typically in the last LTRANS input.  */

static void
output_remaining_constructors_and_inits (struct output_block *ob)
{
  struct varpool_node *vnode;

  FOR_EACH_STATIC_VARIABLE (vnode)
    {
      tree var = vnode->decl;
      tree context = DECL_CONTEXT (var);
      gcc_assert (!context || TREE_CODE (context) == FUNCTION_DECL);
      if (TREE_STATIC (var) && TREE_PUBLIC (var)
	  && !context
	  && !(lto_get_decl_flags (var) & LTO_DECL_FLAG_DEFINED))
	output_var_init (ob, var);
    }
}

/* Output constructors and inits of all vars in varpool to output block OB.  */

static void
output_all_constructors_and_inits (struct output_block *ob)
{
  struct varpool_node *vnode;

  FOR_EACH_STATIC_VARIABLE (vnode)
    {
      tree var = vnode->decl;
      tree context = DECL_CONTEXT (var);
      gcc_assert (!context || TREE_CODE (context) == FUNCTION_DECL);
      if (!context)
	output_var_init (ob, var);
    }
}


/* Return true if alias pair P belongs to the set of cgraph nodes in
   SET.  If P is a an alias for a VAR_DECL, it can always be emitted.
   However, for FUNCTION_DECL aliases, we should only output the pair
   if it belongs to a function whose cgraph node is in SET.
   Otherwise, the LTRANS phase will get into trouble when finalizing
   aliases because the alias will refer to a function not defined in
   the file processed by LTRANS.  */

static bool
output_alias_pair_p (alias_pair *p, cgraph_node_set set)
{
  cgraph_node_set_iterator csi;
  struct cgraph_node *target_node;

  /* Always emit VAR_DECLs.  FIXME lto, we should probably only emit
     those VAR_DECLs that are instantiated in this file partition, but
     we have no easy way of knowing this based on SET.  */
  if (TREE_CODE (p->decl) == VAR_DECL)
    return true;

  /* Check if the assembler name for P->TARGET has its cgraph node in SET.  */
  gcc_assert (TREE_CODE (p->decl) == FUNCTION_DECL);
  target_node = cgraph_node_for_asm (p->target);
  csi = cgraph_node_set_find (set, target_node);
  return (!csi_end_p (csi));
}


/* Output constructors and inits of all vars.  SET is the current
   cgraph node set being output.  */

void
output_constructors_and_inits (cgraph_node_set set)
{
  struct output_block *ob =
    create_output_block (LTO_section_static_initializer);
  unsigned i;
  alias_pair *p;

  ob->cgraph_node = NULL;

  clear_line_info (ob);

  /* Make string 0 be a NULL string.  */
  lto_output_1_stream (ob->string_stream, 0);

  /* Output inits and constructors of variables.  */
  if (flag_wpa)
    {
      /* In WPA mode, only output the inits and constructors of reachable
         variables from functions in the cgrah node set being output.  */
      output_used_constructors_and_inits (ob);

      /* Output all remaining vars into last LTRANS file.  */
      if (set->aux)
	output_remaining_constructors_and_inits (ob);
    }
  else
    output_all_constructors_and_inits (ob);

  /* The terminator for the constructor.  */
  output_zero (ob);

  /* Emit the alias pairs for the nodes in SET.  */
  for (i = 0; VEC_iterate (alias_pair, alias_pairs, i, p); i++)
    {
      if (output_alias_pair_p (p, set))
	{
	  output_expr_operand (ob, p->decl);
	  output_expr_operand (ob, p->target);
	}
    }

  output_zero (ob);
  output_named_labels (ob);

  produce_asm (ob, NULL);
  destroy_output_block (ob);
}

/* Copy the function body of NODE without deserializing. */

static void
copy_function (struct cgraph_node *node)
{
  tree function = node->decl;
  struct lto_file_decl_data *file_data = node->local.lto_file_data;
  struct lto_output_stream *output_stream = XCNEW (struct lto_output_stream);
  const char *data;
  size_t len;
  const char *name = IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (function));
  char *section_name =
    lto_get_section_name (LTO_section_function_body, name);
  size_t i, j;
  struct lto_in_decl_state *in_state;
  struct lto_out_decl_state *out_state = lto_get_out_decl_state ();

  lto_begin_section (section_name);
  free (section_name);

  /* We may have renamed the declaration, e.g., a static function.  */
  name = lto_get_decl_name_mapping (file_data, name);

  data = lto_get_section_data (file_data, LTO_section_function_body,
                               name, &len);
  gcc_assert (data);

  /* Do a bit copy of the function body.  */
  lto_output_data_stream (output_stream, data, len);
  lto_write_stream (output_stream);

  /* Copy decls. */
  in_state =
    lto_get_function_in_decl_state (node->local.lto_file_data, function);
  gcc_assert (in_state);

  for (i = 0; i < LTO_N_DECL_STREAMS; i++)
    {
      size_t n = in_state->streams[i].size;
      tree *trees = in_state->streams[i].trees;
      struct lto_tree_ref_encoder *encoder = &(out_state->streams[i]);

      /* The out state must have the same indices and the in state.
	 So just copy the vector.  All the encoders in the in state
	 must be empty where we reach here. */
      gcc_assert (lto_tree_ref_encoder_size (encoder) == 0);
      for (j = 0; j < n; j++)
	VEC_safe_push (tree, heap, encoder->trees, trees[j]);
      encoder->next_index = n;
    }
  
  lto_free_section_data (file_data, LTO_section_function_body, name,
			 data, len);
  free (output_stream);
  lto_end_section ();
}

/* Main entry point from the pass manager.  */

static void
lto_output (cgraph_node_set set)
{
  struct cgraph_node *node;
  struct lto_out_decl_state *decl_state;
  cgraph_node_set_iterator csi;
  bitmap output = lto_bitmap_alloc ();

  lto_init_writer ();

  /* Process only the functions with bodies.  */
  for (csi = csi_start (set); !csi_end_p (csi); csi_next (&csi))
    {
      node = csi_node (csi);
      if (node->analyzed && !bitmap_bit_p (output, DECL_UID (node->decl)))
	{
	  bitmap_set_bit (output, DECL_UID (node->decl));
	  decl_state = lto_new_out_decl_state ();
	  lto_push_out_decl_state (decl_state);
	  if (!flag_wpa)
	    output_function (node);
	  else
	    copy_function (node);
	  gcc_assert (lto_get_out_decl_state () == decl_state);
	  lto_pop_out_decl_state ();
	  lto_record_function_out_decl_state (node->decl, decl_state);
	}
    }

  /* Emit the callgraph after emitting function bodies.  This needs to
     be done now to make sure that all the statements in every function
     have been renumbered so that edges can be associated with call
     statements using the statement UIDs.  */
  output_cgraph (set);

  lto_bitmap_free (output);
}

struct ipa_opt_pass_d pass_ipa_lto_gimple_out =
{
 {
  IPA_PASS,
  "lto_gimple_out",	                /* name */
  gate_lto_out,			        /* gate */
  NULL,		                	/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_IPA_LTO_GIMPLE_IO,		        /* tv_id */
  0,	                                /* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,            			/* todo_flags_start */
  TODO_dump_func                        /* todo_flags_finish */
 },
 NULL,		                        /* generate_summary */
 lto_output,           			/* write_summary */
 NULL,		         		/* read_summary */
 NULL,					/* function_read_summary */
 0,					/* TODOs */
 NULL,			                /* function_transform */
 NULL					/* variable_transform */
};


/* Serialization of global types and declarations.  */

static void output_tree_with_context (struct output_block *, tree, tree);
void output_type_tree (struct output_block *, tree);

/* Output the start of a record with TAG and possibly flags for EXPR,
   and the TYPE for VALUE to OB.   Unlike output_record_start, use
   output_type_tree instead of output_type_ref.  */

static void
output_global_record_start (struct output_block *ob, tree expr,
                            tree value, unsigned int tag)
{
  lto_output_1_stream (ob->main_stream, tag);
  if (expr)
    {
      enum tree_code code = TREE_CODE (expr);
      if (value
	  && TEST_BIT (lto_types_needed_for, code)
	  && TREE_TYPE (value))
	output_type_tree (ob, TREE_TYPE (value));
      output_tree_flags (ob, code, expr, false);
    }
}


static void
output_const_decl (struct output_block *ob, tree decl)
{
  /* tag and flags */
  output_global_record_start (ob, NULL, NULL, LTO_const_decl);
  output_tree_flags (ob, ERROR_MARK, decl, true);

  output_tree (ob, decl->decl_minimal.name);
  gcc_assert (decl->decl_minimal.context == NULL_TREE);
  output_tree (ob, decl->common.type);
  output_tree (ob, decl->decl_common.abstract_origin);
  output_uleb128 (ob, decl->decl_common.mode);
  output_uleb128 (ob, decl->decl_common.align);
  gcc_assert (decl->decl_common.off_align == 0);
  output_tree (ob, decl->decl_common.initial);
}


static void
output_field_decl (struct output_block *ob, tree decl)
{
  /* tag and flags */
  output_global_record_start (ob, NULL, NULL, LTO_field_decl);
  output_tree_flags (ob, ERROR_MARK, decl, true);

  /* uid and locus are handled specially */
  output_tree (ob, decl->decl_minimal.name);
  output_tree (ob, decl->decl_minimal.context);
  output_tree (ob, decl->common.type);
  output_tree (ob, decl->decl_common.attributes);
  output_tree (ob, decl->decl_common.abstract_origin);
  output_uleb128 (ob, decl->decl_common.mode);
  output_uleb128 (ob, decl->decl_common.align);
  output_uleb128 (ob, decl->decl_common.off_align);
  output_tree (ob, decl->decl_common.size);
  output_tree (ob, decl->decl_common.size_unit);
  output_tree (ob, decl->field_decl.offset);
  output_tree (ob, decl->field_decl.bit_field_type);
  output_tree (ob, decl->field_decl.qualifier);
  output_tree (ob, decl->field_decl.bit_offset);
  output_tree (ob, decl->field_decl.fcontext);

  /* lang_specific */
  output_tree (ob, decl->decl_common.initial);

  /* Write out current field before its siblings,
     so follow the chain last.  */
  output_tree (ob, decl->common.chain);
}


/* Write FUNCTION_DECL DECL to the output block OB.  */

static void
output_function_decl (struct output_block *ob, tree decl)
{
  bool saved_external, saved_public;

  /* If DECL is a builtin of class BUILT_IN_MD or BUILT_IN_NORMAL, we
     only need to write its code and class.  If DECL is BUILT_IN_FRONTEND
     we have to write it out as a regular function.  */
  if (DECL_IS_BUILTIN (decl)
      && (DECL_BUILT_IN_CLASS (decl) == BUILT_IN_NORMAL
	  || DECL_BUILT_IN_CLASS (decl) == BUILT_IN_MD))
    {
      output_global_record_start (ob, NULL, NULL, LTO_function_decl1);
      output_uleb128 (ob, DECL_BUILT_IN_CLASS (decl));
      output_uleb128 (ob, DECL_FUNCTION_CODE (decl));
      if (DECL_ASSEMBLER_NAME_SET_P (decl))
	{
	  /* When the assembler name of a builtin gets a user name,
	     the new name is always prefixed with '*' by
	     set_builtin_user_assembler_name.  So, to prevent the
	     reader side from adding a second '*', we omit it here.  */
	  const char *str = IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (decl));
	  if (strlen (str) > 1 && str[0] == '*')
	    output_string (ob, ob->main_stream, &str[1]);
	  else
	    output_string (ob, ob->main_stream, NULL);
	}
      else
	output_string (ob, ob->main_stream, NULL);

      return;
    }

  output_global_record_start (ob, NULL, NULL, LTO_function_decl0);

  /* This function is a cherry-picked inlined function.  To avoid
     multiple definition in the final link, we fake the function decl
     so that it is written out as extern inline. */
  if (lto_forced_extern_inline_p (decl))
    {
      saved_external = DECL_EXTERNAL (decl);
      saved_public = TREE_PUBLIC (decl);
      DECL_EXTERNAL (decl) = true;
      TREE_PUBLIC (decl) = true;
      output_tree_flags (ob, ERROR_MARK, decl, true);
      DECL_EXTERNAL (decl) = saved_external;
    }
  else
    output_tree_flags (ob, ERROR_MARK, decl, true);

  /* uid and locus are handled specially */
  output_tree (ob, decl->decl_minimal.name);
  output_tree (ob, decl->decl_minimal.context);

  output_tree (ob, decl->decl_with_vis.assembler_name);
  output_tree (ob, decl->decl_with_vis.section_name);
  if (decl->decl_with_vis.comdat_group)
    output_tree (ob, decl->decl_with_vis.comdat_group);
  else
    output_zero (ob);

  /* omit chain, which would result in writing all functions  */
  output_tree (ob, decl->common.type);

  output_tree (ob, decl->decl_common.attributes);
  output_tree (ob, decl->decl_common.abstract_origin);

  output_uleb128 (ob, decl->decl_common.mode);
  output_uleb128 (ob, decl->decl_common.align);
  gcc_assert (decl->decl_common.off_align == 0);

  output_tree (ob, decl->decl_common.size);
  output_tree (ob, decl->decl_common.size_unit);

  /* lang_specific */

  /* omit rtl */

  /* saved_tree -- this is a function body, so omit it here */
  output_tree_with_context (ob, decl->decl_non_common.arguments, decl);
  output_tree_with_context (ob, decl->decl_non_common.result, decl);
  output_tree (ob, decl->decl_non_common.vindex);

  if (decl->function_decl.personality)
    {
      /* FIXME lto: We have to output the index since the symbol table
	 is composed of all decls we emit a index for. Since this might
	 be the only place we see this decl, we also to write it to disk. */
      gcc_assert (TREE_CODE (decl->function_decl.personality) == FUNCTION_DECL);
      output_uleb128 (ob, 1);
      output_tree (ob, decl->function_decl.personality);
      lto_output_fn_decl_index (ob->decl_state, ob->main_stream,
				decl->function_decl.personality);
    }
  else
    output_uleb128 (ob, 0);

  gcc_assert (!DECL_IS_BUILTIN (decl)
	      || DECL_BUILT_IN_CLASS (decl) == NOT_BUILT_IN
	      || DECL_BUILT_IN_CLASS (decl) == BUILT_IN_FRONTEND);
  output_uleb128 (ob, DECL_BUILT_IN_CLASS (decl));
  output_uleb128 (ob, DECL_FUNCTION_CODE (decl));
}

static void
output_var_decl (struct output_block *ob, tree decl)
{
  /* tag and flags */
  /* Assume static or external variable.  */
  output_global_record_start (ob, NULL, NULL, LTO_var_decl1);
  output_tree_flags (ob, ERROR_MARK, decl, true);

  /* Additional LTO decl flags. */
  if (flag_wpa)
    {
      lto_decl_flags_t flags = lto_get_decl_flags (decl);

      /* Make sure we only output a global from one LTRANS file. */
      if (TREE_PUBLIC (decl))
	{
	  if (flags & LTO_DECL_FLAG_DEFINED)
	    flags |= LTO_DECL_FLAG_SUPPRESS_OUTPUT;
	  else
	    flags |= LTO_DECL_FLAG_DEFINED;
	  lto_set_decl_flags (decl, flags);
	}

      output_uleb128 (ob, flags);
    }
  else
    output_zero (ob);

  /* uid and locus are handled specially */
  output_tree (ob, decl->decl_minimal.name);
  gcc_assert (decl->decl_minimal.context == NULL_TREE);

  output_tree (ob, decl->decl_with_vis.assembler_name);
  output_tree (ob, decl->decl_with_vis.section_name);
  if (decl->decl_with_vis.comdat_group)
    output_tree (ob, decl->decl_with_vis.comdat_group);
  else
    output_zero (ob);

  /* omit chain */
  output_tree (ob, decl->common.type);
  output_tree (ob, decl->decl_common.attributes);
  output_tree (ob, decl->decl_common.abstract_origin);
  output_uleb128 (ob, decl->decl_common.mode);
  output_uleb128 (ob, decl->decl_common.align);
  gcc_assert (decl->decl_common.off_align == 0);
  output_tree (ob, decl->decl_common.size);
  output_tree (ob, decl->decl_common.size_unit);

  /* lang_specific */

  /* omit rtl */

  /* DECL_DEBUG_EXPR is stored in a table on the side,
     not in the VAR_DECL node itself.  */
  output_tree (ob, DECL_DEBUG_EXPR (decl));
  
  /* Write initial expression last.  */
  output_tree (ob, decl->decl_common.initial);
}

static void
output_parm_decl (struct output_block *ob, tree decl, tree fn)
{
  /* tag and flags */
  output_global_record_start (ob, NULL, NULL, LTO_parm_decl);
  output_tree_flags (ob, ERROR_MARK, decl, true);

  /* uid and locus are handled specially */
  output_tree (ob, decl->decl_minimal.name);

  /* If FN has a gimple body, DECL's context must bu FN.  Otherwise,
     it doesn't really matter, as we will not be emitting any code for
     FN.  In general, there may be other instances of FN created by
     the front end and since PARM_DECLs are generally shared, their
     DECL_CONTEXT changes as the replicas of FN are created.  The only
     time where DECL_CONTEXT is important is for the FNs that have a
     gimple body (since the PARM_DECL will be used in the function's
     body).  */
  if (gimple_has_body_p (fn))
    gcc_assert (DECL_CONTEXT (decl) == fn);

  output_tree (ob, decl->common.type);

  output_tree (ob, decl->decl_common.attributes);

  output_uleb128 (ob, decl->decl_common.mode);
  output_uleb128 (ob, decl->decl_common.align);
  gcc_assert (decl->decl_common.off_align == 0);

  output_tree (ob, decl->decl_common.size);
  output_tree (ob, decl->decl_common.size_unit);

  output_tree (ob, decl->decl_common.initial);

  /* lang_specific */
  /* omit rtl, incoming_rtl */

  output_tree_with_context (ob, decl->common.chain, fn);
}

static void
output_result_decl (struct output_block *ob, tree decl, tree fn)
{
  /* tag and flags */
  output_global_record_start (ob, NULL, NULL, LTO_result_decl);
  output_tree_flags (ob, ERROR_MARK, decl, true);

  /* uid and locus are handled specially */
  output_tree (ob, decl->decl_minimal.name);

  /* FIXME lto: We should probably set this to NULL in reset_lang_specifics. */
  gcc_assert (decl->decl_minimal.context == fn);

  output_tree (ob, decl->common.type);

  output_tree (ob, decl->decl_common.attributes);
  output_tree (ob, decl->decl_common.abstract_origin);

  output_uleb128 (ob, decl->decl_common.mode);
  output_uleb128 (ob, decl->decl_common.align);
  gcc_assert (decl->decl_common.off_align == 0);

  output_tree (ob, decl->decl_common.size);
  output_tree (ob, decl->decl_common.size_unit);

  /* lang_specific */
  /* omit rtl */

  gcc_assert (decl->common.chain == NULL_TREE);
}

static void
output_type_decl (struct output_block *ob, tree decl)
{
  /* tag and flags */
  output_global_record_start (ob, NULL, NULL, LTO_type_decl);
  output_tree_flags (ob, ERROR_MARK, decl, true);

  /* uid and locus are handled specially */
  /* Must output name before type.  */
  output_tree (ob, decl->decl_minimal.name);

  /* Should be cleared by pass_ipa_free_lang_data.  */
  gcc_assert (decl->decl_minimal.context == NULL_TREE);

  output_tree (ob, decl->decl_with_vis.assembler_name);
  output_tree (ob, decl->decl_with_vis.section_name);

  output_tree (ob, decl->common.type);

  output_tree (ob, decl->decl_common.attributes);
  output_tree (ob, decl->decl_common.abstract_origin);

  output_uleb128 (ob, decl->decl_common.mode);
  output_uleb128 (ob, decl->decl_common.align);

  output_tree (ob, decl->decl_common.size);
  output_tree (ob, decl->decl_common.size_unit);

  /* We expect pass_ipa_free_lang_data to clear the INITIAL field.  */
  gcc_assert (decl->decl_common.initial == NULL_TREE);

  /* lang_specific */

  gcc_assert (decl->decl_with_rtl.rtl == NULL);

  output_tree (ob, decl->decl_non_common.saved_tree);		/* ??? */
  output_tree (ob, decl->decl_non_common.arguments);
  output_tree (ob, decl->decl_non_common.result);		/* ??? */
  output_tree (ob, decl->decl_non_common.vindex);		/* ??? */
}

static void
output_label_decl (struct output_block *ob, tree decl)
{
  enum LTO_tags tag;
  
  tag = emit_label_in_global_context_p (decl)
	? LTO_label_decl1
	: LTO_label_decl0;

  /* tag and flags */
  output_global_record_start (ob, decl, NULL, tag);

  /* uid and locus are handled specially */
  output_tree (ob, decl->decl_minimal.name);
  output_tree (ob, decl->decl_minimal.context);

  output_tree (ob, decl->common.type);

  output_tree (ob, decl->decl_common.attributes);		/* ??? */
  output_tree (ob, decl->decl_common.abstract_origin);		/* ??? */

  output_uleb128 (ob, decl->decl_common.mode);			/* ??? */
  output_uleb128 (ob, decl->decl_common.align);			/* ??? */
  gcc_assert (decl->decl_common.off_align == 0);

  gcc_assert (decl->decl_common.size == NULL_TREE);
  gcc_assert (decl->decl_common.size_unit == NULL_TREE);

  output_tree (ob, decl->decl_common.initial);

  /* lang_specific */
  /* omit rtl, incoming_rtl */
  /* omit chain */
}

/* Emit IMPORTED_DECL DECL to output block OB.  */

static void
output_imported_decl (struct output_block *ob, tree decl)
{
  output_global_record_start (ob, NULL, NULL, LTO_imported_decl);
  output_tree_flags (ob, ERROR_MARK, decl, true);
  output_tree (ob, IMPORTED_DECL_ASSOCIATED_DECL (decl));
  output_tree (ob, DECL_NAME (decl));
  gcc_assert (TREE_TYPE (decl) == void_type_node);
}

static void
output_binfo (struct output_block *ob, tree binfo)
{
  size_t i;
  size_t num_base_accesses = VEC_length (tree, binfo->binfo.base_accesses);
  size_t num_base_binfos = VEC_length (tree, &binfo->binfo.base_binfos);

  output_global_record_start (ob, NULL, NULL, LTO_tree_binfo);
  output_tree_flags (ob, ERROR_MARK, binfo, false);

  output_uleb128 (ob, num_base_accesses);
  output_uleb128 (ob, num_base_binfos);

  output_tree (ob, binfo->common.type);

  output_tree (ob, binfo->binfo.offset);
  output_tree (ob, binfo->binfo.vtable);
  output_tree (ob, binfo->binfo.virtuals);
  output_tree (ob, binfo->binfo.vptr_field);
  output_tree (ob, binfo->binfo.inheritance);
  output_tree (ob, binfo->binfo.vtt_subvtt);
  output_tree (ob, binfo->binfo.vtt_vptr);

  for (i = 0; i < num_base_accesses; ++i)
    output_tree (ob, VEC_index (tree, binfo->binfo.base_accesses, i));

  for (i = 0; i < num_base_binfos; ++i)
    output_tree (ob, VEC_index (tree, &binfo->binfo.base_binfos, i));

  output_tree (ob, binfo->common.chain);
}

static void
output_type (struct output_block *ob, tree type, enum LTO_tags tag)
{
  /* tag and flags */
  output_global_record_start (ob, NULL, NULL, tag);
  output_tree_flags (ob, ERROR_MARK, type, false);

  output_tree (ob, type->common.type);
  output_tree (ob, type->type.size);
  output_tree (ob, type->type.size_unit);
  output_tree (ob, type->type.attributes);
  /* Do not write UID.  Assign a new one on input.  */
  output_uleb128 (ob, type->type.precision);
  output_uleb128 (ob, type->type.mode);
  output_uleb128 (ob, type->type.align);
  output_tree (ob, type->type.pointer_to);
  output_tree (ob, type->type.reference_to);
  /* FIXME: Output symtab here.  Do we need it?  */
  output_tree (ob, type->type.name);	/* may be a TYPE_DECL */
  output_tree (ob, type->type.minval);
  output_tree (ob, type->type.maxval);
  output_tree (ob, type->type.next_variant);
  output_tree (ob, type->type.main_variant);
  gcc_assert (type->type.binfo == NULL_TREE
	      || TREE_CODE (type) == RECORD_TYPE
	      || TREE_CODE (type) == UNION_TYPE);
  output_tree (ob, type->type.binfo);

  /* Should be cleared by pass_ipa_free_lang_data.  */
  gcc_assert (type->type.context == NULL_TREE);

  output_tree (ob, type->type.canonical);

  /* Slot 'values' may be the structures fields, so do them last,
     after other slots of the structure type have been filled in.  */
  if (tag == LTO_record_type || tag == LTO_union_type)
    output_tree (ob, TYPE_FIELDS (type));
  else
    {
      if (TYPE_CACHED_VALUES_P (type))
	{
	  gcc_assert (TREE_CODE (type) != RECORD_TYPE
		      && TREE_CODE (type) != UNION_TYPE
		      && TREE_CODE (type) != ARRAY_TYPE);
	  /* Don't stream the values cache.  We must clear flag
	     TYPE_CACHED_VALUES_P on input.  We don't do it here
	     because we don't want to clobber the tree as we write
	     it, and there is no infrastructure for modifying
	     flags as we serialize them.  */
	  output_zero (ob);
	}
      else
	output_tree (ob, type->type.values);
    }

  output_tree (ob, type->common.chain);	   /* overloaded as TYPE_STUB_DECL */
}

/* Output the start of a record with TAG and possibly flags for EXPR,
   and the TYPE for VALUE to OB.   Unlike output_record_start, use
   output_type_tree instead of output_type_ref.  */

static void
output_global_record_start_1 (struct output_block *ob, tree expr,
			      tree value, unsigned int tag)
{
  lto_output_1_stream (ob->main_stream, tag);
  if (expr)
    {
      enum tree_code code = TREE_CODE (expr);
      if (value && TEST_BIT (lto_types_needed_for, code))
	{
	  if (TREE_TYPE (value))
	    output_type_tree (ob, TREE_TYPE (value));
	  else
	    {
	      /* Allow for null tree type */
	      output_zero (ob);
	    }
	}
      output_tree_flags (ob, code, expr, false);
    }
}


/* Output constructor CTOR to OB.  */

static void
output_global_constructor (struct output_block *ob, tree ctor)
{
  tree value;
  tree purpose;
  unsigned HOST_WIDE_INT idx;

  output_global_record_start_1 (ob, ctor, ctor, LTO_constructor);
  output_uleb128 (ob, VEC_length (constructor_elt, CONSTRUCTOR_ELTS (ctor)));

  FOR_EACH_CONSTRUCTOR_ELT (CONSTRUCTOR_ELTS (ctor), idx, purpose, value)
    {
      if (purpose)
	output_tree (ob, purpose);
      else 
	output_zero (ob);

      if (TREE_CODE (value) == CONSTRUCTOR)
	output_global_constructor (ob, value);
      else 
	output_tree (ob, value);
    }
}

/* Emit tree node EXPR to output block OB. If relevant, the DECL_CONTEXT
   is asserted to be FN. */

static void
output_tree_with_context (struct output_block *ob, tree expr, tree fn)
{
  enum tree_code code;
  enum tree_code_class klass;
  enum LTO_tags tag;
  void **slot;
  struct lto_decl_slot d_slot;

  if (expr == NULL_TREE)
    {
      output_zero (ob);
      return;
    }

  if (TYPE_P (expr)
      || DECL_P (expr)
      || TREE_CODE (expr) == TREE_BINFO
      || TREE_CODE (expr) == BLOCK)
    {
      unsigned int global_index;
      /* FIXME lto:  There are decls that pass the predicate above, but which
	 we do not handle.  We must avoid assigning a global index to such a node,
 	 as we will not emit it, and the indices will get out of sync with the
 	 global vector on the reading side.  We shouldn't be seeing these nodes,
 	 and, ideally, we should abort on them.  This is an interim measure
 	 for the sake of making forward progress.  */
      {
 	enum tree_code code = TREE_CODE (expr);
 
 	switch (code)
 	  {
 	  case CONST_DECL:
 	  case FIELD_DECL:
 	  case FUNCTION_DECL:
 	  case VAR_DECL:
 	  case PARM_DECL:
 	  case RESULT_DECL:
 	  case TYPE_DECL:
 	  case NAMESPACE_DECL:
 	  case TRANSLATION_UNIT_DECL:
	  case LABEL_DECL:
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
 	  case TREE_BINFO:
	  case BLOCK:
 	    break;
 
 	  default:
	    error ("Unhandled type or decl: %s", tree_code_name[code]);
	    gcc_unreachable ();
 	  }
      }
 
      /* If we've already pickled this node, emit a reference.
         Otherwise, assign an index for the node we are about to emit.  */
      if (get_ref_idx_for (expr, ob->main_hash_table, NULL, &global_index))
        {
          output_global_record_start (ob, NULL, NULL,
				      LTO_tree_pickle_reference);
          output_uleb128 (ob, global_index);
          return;
        }
    }
  else
    {
      /* We don't share new instances of other classes of tree nodes,
         but we always want to share the preloaded "well-known" nodes.  */
      d_slot.t = expr;
      slot = htab_find_slot (ob->main_hash_table, &d_slot, NO_INSERT);
      if (slot != NULL)
        {
          struct lto_decl_slot *old_slot = (struct lto_decl_slot *)*slot;
          output_global_record_start (ob, NULL, NULL, LTO_tree_pickle_reference);
          output_uleb128 (ob, old_slot->slot_num);
          return;
        }
    }

  code = TREE_CODE (expr);
  klass = TREE_CODE_CLASS (code);
  tag = expr_to_tag [code];

  switch (code)
    {
    case BLOCK:
      output_global_record_start (ob, expr, NULL, LTO_block);
      output_tree_block (ob, expr);
      break;

    case COMPLEX_CST:
      if (TREE_CODE (TREE_REALPART (expr)) == REAL_CST)
	{
	  output_global_record_start (ob, expr, expr, LTO_complex_cst1);
	  output_type_tree (ob, TREE_TYPE (TREE_REALPART (expr)));
	  output_real (ob, TREE_REALPART (expr));
	  output_real (ob, TREE_IMAGPART (expr));
	}
      else
	{
	  output_global_record_start (ob, expr, expr, LTO_complex_cst0);
	  output_type_tree (ob, TREE_TYPE (TREE_REALPART (expr)));
	  output_integer (ob, TREE_REALPART (expr));
	  output_integer (ob, TREE_IMAGPART (expr));
	}
      break;

    case INTEGER_CST:
      output_global_record_start (ob, expr, expr, tag);
      output_integer (ob, expr);
      break;

    case REAL_CST:
      output_global_record_start (ob, expr, expr, tag);
      output_real (ob, expr);
      break;

    case STRING_CST:
      {
	/* Most STRING_CSTs have a type when they get here.  The ones
	   in the string operands of asms do not.  Put something there
	   so that all STRING_CSTs can be handled uniformly.  */
	if (!TREE_TYPE (expr))
	  TREE_TYPE (expr) = void_type_node;

	output_global_record_start (ob, expr, expr, LTO_string_cst);
	output_string_cst (ob, ob->main_stream, expr);
      }
      break;

    case IDENTIFIER_NODE:
      {
	output_global_record_start (ob, expr, expr, LTO_identifier_node);
	output_identifier (ob, ob->main_stream, expr);
      }
      break;
      

    case VECTOR_CST:
      {
	tree t = TREE_VECTOR_CST_ELTS (expr);
	int len = 1;

	while ((t = TREE_CHAIN (t)) != NULL)
	  len++;
	t = TREE_VECTOR_CST_ELTS (expr);
	if (TREE_CODE (TREE_VALUE(t)) == REAL_CST)
	  {
	    output_global_record_start (ob, expr, expr, LTO_vector_cst1);
	    output_uleb128 (ob, len);
	    output_type_tree (ob, TREE_TYPE (TREE_VALUE (t)));
	    output_real (ob, TREE_VALUE (t));
	    while ((t = TREE_CHAIN (t)) != NULL)
	      output_real (ob, TREE_VALUE (t));
	  }
	else
	  {
	    output_global_record_start (ob, expr, expr, LTO_vector_cst0);
	    output_uleb128 (ob, len);
	    output_type_tree (ob, TREE_TYPE (TREE_VALUE (t)));
	    output_integer (ob, TREE_VALUE (t));
	    while ((t = TREE_CHAIN (t)) != NULL)
	      output_integer (ob, TREE_VALUE (t));
	  }
      }
      break;

    case CONSTRUCTOR:
      output_global_constructor (ob, expr);
      break;

    case SSA_NAME:
      /* FIXME: I don't think SSA_NAME nodes make sense here.  */
      gcc_unreachable ();
      /*
      output_global_record_start (ob, expr, expr, LTO_ssa_name);
      output_uleb128 (ob, SSA_NAME_VERSION (expr));
      */
      break;

    case CONST_DECL:
      output_const_decl (ob, expr);
      break;

    case FIELD_DECL:
      output_field_decl (ob, expr);
      break;

    case FUNCTION_DECL:
      output_function_decl (ob, expr);
      break;

    case IMPORTED_DECL:
      output_imported_decl (ob, expr);
      break;

    case VAR_DECL:
      if (decl_function_context (expr) == NULL_TREE)
        output_var_decl (ob, expr);
      else
        /* We should not be seeing local variables here.  */
        gcc_unreachable ();
      break;

    case PARM_DECL:
      output_parm_decl (ob, expr, fn);
      break;

    case RESULT_DECL:
      output_result_decl (ob, expr, fn);
      break;

    case TYPE_DECL:
      output_type_decl (ob, expr);
      break;

    case LABEL_DECL:
      output_label_decl (ob, expr);
      break;

    case LABEL_EXPR:
      output_global_record_start (ob, expr, NULL, tag);
      output_tree (ob, TREE_OPERAND (expr, 0));
      break;

    case COMPONENT_REF:
      output_global_record_start (ob, expr, expr, tag);
      output_tree (ob, TREE_OPERAND (expr, 0));
      output_tree (ob, TREE_OPERAND (expr, 1));
      /* Ignore 3 because it can be recomputed.  */
      break;

    case BIT_FIELD_REF:
      {
	tree op1 = TREE_OPERAND (expr, 1);
	tree op2 = TREE_OPERAND (expr, 2);
	if ((TREE_CODE (op1) == INTEGER_CST)
	    && (TREE_CODE (op2) == INTEGER_CST))
	  {
	    output_global_record_start (ob, expr, expr,
				 LTO_bit_field_ref1);
	    output_uleb128 (ob, TREE_INT_CST_LOW (op1));
	    output_uleb128 (ob, TREE_INT_CST_LOW (op2));
	    output_tree (ob, TREE_OPERAND (expr, 0));
	  }
	else
	  {
	    output_global_record_start (ob, expr, expr,
				 LTO_bit_field_ref0);
	    output_tree (ob, TREE_OPERAND (expr, 0));
	    output_tree (ob, op1);
	    output_tree (ob, op2);
	  }
      }
      break;

    case ARRAY_REF:
    case ARRAY_RANGE_REF:
      /* Ignore operands 2 and 3 for ARRAY_REF and ARRAY_RANGE REF
	 because they can be recomputed.  */
      output_global_record_start (ob, expr, expr, tag);
      output_tree (ob, TREE_OPERAND (expr, 0));
      output_tree (ob, TREE_OPERAND (expr, 1));
      break;


    case RANGE_EXPR:
      {
	output_global_record_start (ob, NULL, NULL, LTO_range_expr);
	/* Need the types here to reconstruct the ranges.  */
	output_type_tree (ob, TREE_OPERAND (expr, 0));
	output_integer (ob, TREE_OPERAND (expr, 0));
	output_type_tree (ob, TREE_OPERAND (expr, 1));
	output_integer (ob, TREE_OPERAND (expr, 1));
      }
      break; 

    case RESX_EXPR:
      output_global_record_start (ob, expr, NULL, tag);
      output_uleb128 (ob, TREE_INT_CST_LOW (TREE_OPERAND (expr, 0)));
      break;

    case TREE_LIST:
      {
	tree tl;
	int count = 0;

	output_global_record_start (ob, expr, NULL, tag);
	for (tl = expr; tl; tl = TREE_CHAIN (tl))
	  count++;

	gcc_assert (count);
	output_uleb128 (ob, count);
	for (tl = expr; tl; tl = TREE_CHAIN (tl))
	  {
	    if (TREE_VALUE (tl) != NULL_TREE)
	      output_tree (ob, TREE_VALUE (tl));
	    else
	      output_zero (ob);
	    
	    if (TREE_PURPOSE (tl))
	      output_tree (ob, TREE_PURPOSE (tl));
	    else
	      output_zero (ob);
	  }
      }
      break;

    case TREE_VEC:
      {
	size_t i;
	size_t len = TREE_VEC_LENGTH (expr);

	output_global_record_start (ob, NULL, NULL, tag);
	output_uleb128 (ob, len);
	for (i = 0; i < len; ++i)
	  output_tree (ob, TREE_VEC_ELT (expr, i));
      }
      break;

    case ERROR_MARK:
      /* The canonical error node is preloaded,
         so we should never see another one here.  */
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
      output_type (ob, expr, tag);
      break;

    case LANG_TYPE:
      /* FIXME */
      gcc_unreachable ();

    case TREE_BINFO:
      output_binfo (ob, expr);
      break;

      /* This is the default case. All of the cases that can be done
	 completely mechanically are done here.  */
      {
	int i;
#define SET_NAME(a,b)
#define MAP_EXPR_TAG(expr, tag) case expr:
#define TREE_SINGLE_MECHANICAL_TRUE

#include "lto-tree-tags.def"
#undef MAP_EXPR_TAG
#undef TREE_SINGLE_MECHANICAL_TRUE
#undef SET_NAME
	output_global_record_start (ob, expr, expr, tag);
	for (i = 0; i < TREE_CODE_LENGTH (TREE_CODE (expr)); i++)
	  output_tree (ob, TREE_OPERAND (expr, i));
	break;
      }

    default:
      if (TREE_CODE (expr) >= NUM_TREE_CODES)
	{
	  /* EXPR is a language-specific tree node, which has no meaning
	     outside of the front end.  These nodes should have been
	     cleaned up by pass_ipa_free_lang_data.  */
	  error ("Invalid FE-specific tree code: %d", (int) code);
	  gcc_unreachable ();
	}
      else
	{
	  /* All forms must be explicitly handled.  */
	  error ("Unimplemented code: %s", tree_code_name[code]);
	  gcc_unreachable ();
	}
    }
}

/* Emit tree node EXPR to output block OB.  */

void
output_tree (struct output_block *ob, tree expr)
{
  output_tree_with_context (ob, expr, NULL_TREE);
}


/* Replacement for output_type_ref when serializing globals.  */

void
output_type_tree (struct output_block *ob, tree type)
{
  gcc_assert (type && TYPE_P (type));
  output_tree (ob, type);
}
