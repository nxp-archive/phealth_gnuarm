/* Write the gimple representation of a function and it's local
   variables to a .o file.

   Copyright 2006 Free Software Foundation, Inc.
   Contributed by Kenneth Zadeck <zadeck@naturalbridge.com>

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
#include "tm.h"
#include "toplev.h"
#include "tree.h"
#include "expr.h"
#include "flags.h"
#include "params.h"
#include "input.h"
#include "varray.h"
#include "hashtab.h"
#include "langhooks.h"
#include "basic-block.h"
#include "tree-iterator.h"
#include "tree-pass.h"
#include "tree-flow.h"
#include "cgraph.h"
#include "function.h"
#include "ggc.h"
#include "diagnostic.h"
#include "except.h"
#include "debug.h"
#include "vec.h"
#include "tree-vectorizer.h"
#include "timevar.h"
#include "dwarf2asm.h"
#include "dwarf2out.h"
#include "output.h"
#include "lto-tags.h"
#include <ctype.h>


sbitmap lto_flags_needed_for;
sbitmap lto_types_needed_for;

#ifdef LTO_STREAM_DEBUGGING
const char *LTO_tag_names[LTO_last_tag];

static struct lto_debug_context lto_debug_context;
static void debug_out_fun (struct lto_debug_context *, char);
#endif


/* The index of the last eh_region seen for an instruction.  The
   eh_region for an instruction is only emitted if it different from
   the last instruction.  */

static int last_eh_region_seen;
static unsigned int expr_to_tag[NUM_TREE_CODES];

struct decl_slot {
  tree t;
  int slot_num;
};

/* Returns a hash code for P.  */

static hashval_t
hash_decl_slot_node (const void *p)
{
  const struct decl_slot *ds = (const struct decl_slot *) p;
  return (hashval_t) DECL_UID (ds->t);
}


/* Returns nonzero if P1 and P2 are equal.  */

static int
eq_decl_slot_node (const void *p1, const void *p2)
{
  const struct decl_slot *ds1 =
    (const struct decl_slot *) p1;
  const struct decl_slot *ds2 =
    (const struct decl_slot *) p2;

  return DECL_UID (ds1->t) == DECL_UID (ds2->t);
}


/* Returns a hash code for P.  */

static hashval_t
hash_type_slot_node (const void *p)
{
  const struct decl_slot *ds = (const struct decl_slot *) p;
  return (hashval_t) TYPE_UID (ds->t);
}


/* Returns nonzero if P1 and P2 are equal.  */

static int
eq_type_slot_node (const void *p1, const void *p2)
{
  const struct decl_slot *ds1 =
    (const struct decl_slot *) p1;
  const struct decl_slot *ds2 =
    (const struct decl_slot *) p2;

  return TYPE_UID (ds1->t) == TYPE_UID (ds2->t);
}


/* Returns a hash code for P.  */

static hashval_t
hash_label_slot_node (const void *p)
{
  const struct decl_slot *ds = (const struct decl_slot *) p;
  return (hashval_t) LABEL_DECL_UID (ds->t);
}


/* Returns nonzero if P1 and P2 are equal.  */

static int
eq_label_slot_node (const void *p1, const void *p2)
{
  const struct decl_slot *ds1 =
    (const struct decl_slot *) p1;
  const struct decl_slot *ds2 =
    (const struct decl_slot *) p2;

  return LABEL_DECL_UID (ds1->t) == LABEL_DECL_UID (ds2->t);
}


struct string_slot {
  const char *s;
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

  return strcmp (ds1->s, ds2->s);
}


struct char_ptr_base
{
  char *ptr;
};


/* An incore byte stream to buffer the various parts of the
function. The entire structure should be zeroed when created.  The
record consists of a set of blocks.  The first sizeof (ptr) bytes are
used as a chain, and the rest store the bytes to be written.  */

struct output_stream
{
  /* The pointer to the first block in the stream.  */
  struct char_ptr_base * first_block;
  /* The pointer to the last and current block in the stream.  */
  struct char_ptr_base * current_block;
  /* The pointer to where the next char should be written.  */
  char * current_pointer;
  /* The number of characters left in the current block.  */
  unsigned int left_in_block;
  /* The block size of the last block allocated.  */
  unsigned int block_size;
  /* The total number of characters written.  */
  unsigned int total_size;
};

struct output_block
{
  /* The stream that the main tree codes are written to.  */
  struct output_stream *main_stream;
  /* The stream that contains the local name table.  */
  struct output_stream *local_decl_stream;
  /* The stream that contains the names for the named_labels.  */
  struct output_stream *named_label_stream;
  /* The stream that contains the string table.  */
  struct output_stream *string_stream;
#ifdef LTO_STREAM_DEBUGGING
  /* The stream that contains the stream debugging information.  */
  struct output_stream *debug_decl_stream;
  /* The stream that contains the stream debugging information.  */
  struct output_stream *debug_label_stream;
  /* The stream that contains the stream debugging information.  */
  struct output_stream *debug_main_stream;
#endif

  /* The hash table that contains the set of labels we have seen so
     far and the indexes assigned to them.  */
  htab_t label_hash_table;
  int next_named_label_index;
  int next_unnamed_label_index;
  VEC(tree,heap) *named_labels;

  /* The hash table that contains the set of local parm and var decls
     we have seen so far and the indexes assigned to them.  */
  htab_t local_decl_hash_table;
  unsigned int next_local_decl_index;
  VEC(tree,heap) *local_decls;

  /* The hash table that contains the set of field_decls we have
     seen so far and the indexes assigned to them.  */
  htab_t field_decl_hash_table;
  unsigned int next_field_decl_index;
  VEC(tree,heap) *field_decls;

  /* The hash table that contains the set of function_decls we have
     seen so far and the indexes assigned to them.  */
  htab_t fn_decl_hash_table;
  unsigned int next_fn_decl_index;
  VEC(tree,heap) *fn_decls;

  /* The hash table that contains the set of var_decls we have
     seen so far and the indexes assigned to them.  */
  htab_t var_decl_hash_table;
  unsigned int next_var_decl_index;
  VEC(tree,heap) *var_decls;

  /* The hash table that contains the set of strings we have seen so
     far and the indexes assigned to them.  */
  htab_t string_hash_table;
  unsigned int next_string_index;

  /* The hash table that contains the set of type we have seen so far
     and the indexes assigned to them.  */
  htab_t type_hash_table;
  unsigned int next_type_index;
  VEC(tree,heap) *types;

  /* These are the last file and line that were seen in the stream.
     If the current node differs from these, it needs to insert
     something into the stream and fix these up.  */
  const char *last_file;
  int last_line;
};


/* The output stream that contains the abbrev table for all of the
   functions in this compilation unit.  */
static void output_expr_operand (struct output_block *, tree);


/* Write all of the chars in OBS to the assembler.  Recycle the blocks
   in obs as this is being done.  */

static void
write_stream (struct output_stream *obs)
{
  unsigned int block_size = 1024;
  unsigned int num_chars;
  struct char_ptr_base *block;
  if (!obs->first_block)
    return;

  block = obs->first_block;
  while (block)
    {
      const char *base = ((char *)block) + sizeof (struct char_ptr_base);
      struct char_ptr_base *old_block = block;
      block = (struct char_ptr_base *)block->ptr;
      /* If there is a next block, then this one is full, if there is
	 not a next block, then the left_in_block field says how many
	 chars there are in this block.  */
      num_chars = block_size - sizeof (struct char_ptr_base);
      if (!block)
	num_chars = num_chars - obs->left_in_block;

      assemble_string (base, num_chars);
      free (old_block);
      block_size *= 2;
    }
}


/* Write a character to the output block.  */

static void
output_1_stream (struct output_stream *obs, char c)
{
  /* No space left.  */
  if (obs->left_in_block == 0)
    {
      struct char_ptr_base *new_block;

      if (obs->first_block == NULL)
	{
	  /* This is the first time the stream has been written
	     into.  */
	  obs->block_size = 1024;
	  new_block = (struct char_ptr_base*) xmalloc (obs->block_size);
	  obs->first_block = new_block;
	}
      else
	{
	  struct char_ptr_base *tptr;
	  /* Get a new block that is twice as big as the last block
	     and link it into the list.  */
	  obs->block_size *= 2;
	  new_block = (struct char_ptr_base*) xmalloc (obs->block_size);
	  /* The first bytes of the block are reserved as a pointer to
	     the next block.  Set the chain of the full block to the
	     pointer to the new block.  */
	  tptr = obs->current_block;
	  tptr->ptr = (char *)new_block;
	}

      /* Set the place for the next char at the first position after the
	 chain to the next block.  */
      obs->current_pointer
	= ((char *)new_block) + sizeof (struct char_ptr_base);
      obs->current_block = new_block;
      /* Null out the newly allocated block's pointer to the next block.  */
      new_block->ptr = NULL;
      obs->left_in_block = obs->block_size - sizeof (struct char_ptr_base);
    }

  /* Write the actual character.  */
  *obs->current_pointer = c;
  obs->current_pointer++;
  obs->total_size++;
  obs->left_in_block--;
}


/* Write a zero to the output stream.  */

static void
output_zero (struct output_block *ob)
{
  LTO_DEBUG_WIDE ("U", 0)
  output_1_stream (ob->main_stream, 0);
}


/* Output an unsigned LEB128 quantity to OBS.  */

static void
output_uleb128_stream (struct output_stream *obs, unsigned HOST_WIDE_INT work)
{
  LTO_DEBUG_WIDE ("U", work)
  do
    {
      unsigned int byte = (work & 0x7f);
      work >>= 7;
      if (work != 0)
	/* More bytes to follow.  */
	byte |= 0x80;

      output_1_stream (obs, byte);
    }
  while (work != 0);

}


/* Output an unsigned LEB128 quantity to OB->main_stream.  */

static void
output_uleb128 (struct output_block *ob, unsigned HOST_WIDE_INT work)
{
  output_uleb128_stream (ob->main_stream, work);
}


/* Output a signed LEB128 quantity.  */

static void
output_sleb128_stream (struct output_stream *obs, HOST_WIDE_INT work)
{
  int more, byte;
  LTO_DEBUG_WIDE ("S", work)
  do
    {
      byte = (work & 0x7f);
      /* arithmetic shift */
      work >>= 7;
      more = !((work == 0 && (byte & 0x40) == 0)
	       || (work == -1 && (byte & 0x40) != 0));
      if (more)
	byte |= 0x80;

      output_1_stream (obs, byte);
    }
  while (more);
}


/* Output a signed LEB128 quantity to OB->main_stream.  */

static void
output_sleb128 (struct output_block *ob, HOST_WIDE_INT work)
{
  output_sleb128_stream (ob->main_stream, work);
}


/* Output STRING of LEN to the string table in OB.  Then put the index 
   onto the INDEX_STREAM.  */

static void
output_string (struct output_block *ob, 
	       struct output_stream *index_stream,
	       const char *string,
	       unsigned int len)
{
  void **slot;
  struct string_slot s_slot;
  s_slot.s = string;

  slot = htab_find_slot (ob->string_hash_table, &s_slot, INSERT);
  if (*slot == NULL)
    {
      struct output_stream *string_stream = ob->string_stream;
      unsigned int start = string_stream->total_size;
      struct string_slot *new_slot
	= xmalloc (sizeof (struct string_slot));
      unsigned int i;

      new_slot->s = string;
      new_slot->slot_num = start;
      *slot = new_slot;
      output_uleb128_stream (index_stream, start);
      output_uleb128_stream (string_stream, len);
      for (i=0; i<len; i++)
	output_1_stream (string_stream, string[i]);

    }
  else
    {
      struct decl_slot *old_slot = (struct decl_slot *)*slot;
      output_uleb128_stream (index_stream, old_slot->slot_num);

      /* From the debugging protocol's point of view, the entry needs
	 to look the same reguardless of whether this is the first
	 occurence of this string or not.  Thus, we simulate the same
	 debugging info as would be output as if this was a new
	 string.  */
      LTO_DEBUG_WIDE ("U", old_slot->slot_num)
    }
  LTO_DEBUG_STRING (string, len)
}


/* Put out a real constant.  */

static void
output_real (struct output_block *ob, tree t)
{
  static char real_buffer[1000];
  const REAL_VALUE_TYPE *r = &TREE_REAL_CST (t);

  LTO_DEBUG_TOKEN ("real")
  real_to_hexadecimal (real_buffer, r, 1000, 0, 1);
  output_string (ob, ob->main_stream, real_buffer, strlen (real_buffer));
}


/* Put out a integer constant.  These are stored as two HOST_WIDE_INTS
   so games may have to be played to shift the data from the high to
   the low value.  */

static void
output_integer (struct output_block *ob, tree t)
{
  struct output_stream *obs = ob->main_stream;
  HOST_WIDE_INT low = TREE_INT_CST_LOW (t);
  HOST_WIDE_INT high = TREE_INT_CST_HIGH (t);
  int more, byte;

  /* Of course if the high value is just sign bits for the signed low
     value, we can just punt and call output_sleb128 and be done with
     it.  */
  if (((high == -1) && (low < 0))
      || ((high == 0) && (low >= 0)))
    {
      output_sleb128_stream (obs, low);
      return;
    }

  LTO_DEBUG_INTEGER ("SS", high, low)

  /* This is just a copy of the output_sleb128 code with extra
     operations to transfer the low 7 bits of the high value to the
     top 7 bits of the low value, shift the high down by 7 and then do
     a slightly more complex exit test.  */
  do
    {
      unsigned HOST_WIDE_INT transfer = (high & 0x7f);
      high >>= 7;
      transfer <<= (HOST_BITS_PER_WIDE_INT - 7);

      byte = (low & 0x7f);

      /* Logical shift.  */
      low = ((unsigned HOST_WIDE_INT)low) >> 7;
      low |= transfer;
      more = !((high == 0 && low == 0 && (byte & 0x40) == 0)
	       || (high == -1 && low == -1 && (byte & 0x40) != 0));
      if (more)
	byte |= 0x80;

      output_1_stream (obs, byte);
    }
  while (more);
}


/* Lookup NAME in TABLE.  If NAME is not found, create a new entry in
   TABLE for NAME with NEXT_INDEX and increment NEXT_INDEX.  Then
   print the index to OBS.  True is returned if NAME was added to the
   table.  */

static bool
output_decl_index (struct output_stream * obs, htab_t table,
		   unsigned int *next_index, tree name)
{
  void **slot;
  struct decl_slot d_slot;
  d_slot.t = name;

  slot = htab_find_slot (table, &d_slot, INSERT);
  if (*slot == NULL)
    {
      struct decl_slot *new_slot = xmalloc (sizeof (struct decl_slot));
      int index = (*next_index)++;

      new_slot->t = name;
      new_slot->slot_num = index;
      *slot = new_slot;
      output_uleb128_stream (obs, index);
      return true;
    }
  else
    {
      struct decl_slot *old_slot = (struct decl_slot *)*slot;
      output_uleb128_stream (obs, old_slot->slot_num);
      return false;
    }
}


/* Build a densely packed word that contains only the flags that are
   used for this type of tree EXPR and write the word in uleb128 to
   the OB.  */


static void
output_tree_flags (struct output_block *ob, tree expr)
{
  int flags = 0;
  const char *file_to_write = NULL;
  int line_to_write = -1;
  const char *current_file;

  LTO_DEBUG_TOKEN ("flags")

#define START_CLASS_SWITCH()              \
  {                                       \
    enum tree_code code = TREE_CODE (expr); \
                                          \
    switch (TREE_CODE_CLASS (code))       \
    {

#define START_CLASS_CASE(class)    case class:
#define ADD_CLASS_FLAG(flag_name) { flags <<= 1; if (expr->common. flag_name ) flags |= 1; }
#define END_CLASS_CASE(class)      break;
#define END_CLASS_SWITCH()                \
    default:                              \
      gcc_unreachable ();                 \
    }


#define START_EXPR_SWITCH()               \
    switch (code)			  \
    {
#define START_EXPR_CASE(code)    case code:
#define ADD_EXPR_FLAG(flag_name) { flags <<= 1; if (expr->common. flag_name ) flags |= 1; }
#define ADD_DECL_FLAG(flag_name) { flags <<= 1; if (expr->decl_common. flag_name ) flags |= 1; }
#define END_EXPR_CASE(class)      break;
#define END_EXPR_SWITCH()                 \
    default:                              \
      gcc_unreachable ();                 \
    }                                     \
  }

#include "lto-tree-flags.def"

#undef START_CLASS_SWITCH
#undef START_CLASS_CASE
#undef ADD_CLASS_FLAG
#undef END_CLASS_CASE
#undef END_CLASS_SWITCH
#undef START_EXPR_SWITCH
#undef START_EXPR_CASE
#undef ADD_EXPR_FLAG
#undef ADD_DECL_FLAG
#undef END_EXPR_CASE
#undef END_EXPR_SWITCH

  /* Add two more bits onto the flag if this is a tree node that can
     have a line number.  The first bit is true if this node changes
     files and the second is set if this node changes lines.  */
  if (expr && EXPR_P (expr))
    {
      flags <<= 2;
      if (EXPR_HAS_LOCATION (expr))
	{
	  LOC current_loc = EXPR_LOC (expr);
	  const int current_line = LOC_LINE (current_loc);
	  current_file = LOC_FILE (current_loc);
	  if (ob->last_file != current_file)
	    {
	      file_to_write = current_file;
	      ob->last_file = current_file;
	      flags |= 0x2;
	    }
	  if (ob->last_line != current_line)
	    {
	      line_to_write = current_line;
	      ob->last_line = current_line;
	      flags |= 0x1;
	    }
	}
    }

  output_uleb128 (ob, flags);
  if (file_to_write)
    {
      LTO_DEBUG_TOKEN ("file")
      output_string (ob, ob->main_stream, 
		     file_to_write, strlen (file_to_write));
    }
  if (line_to_write != -1)
    {
      LTO_DEBUG_TOKEN ("line")
      output_uleb128 (ob, line_to_write);
    }
}


/* Look up TYPE in the type table and write the uleb128 index for it.
   This is a hack and will be replaced with a real reference to the
   type.  */

static void
output_type_ref (struct output_block *ob, tree node)
{
  bool new;

  LTO_DEBUG_TOKEN ("type")
  new = output_decl_index (ob->main_stream, ob->type_hash_table,
			   &ob->next_type_index, node);

  if (new)
    VEC_safe_push (tree, heap, ob->types, node);
}


/* Look up NAME in the type table and write the uleb128 index for it
   to OB.  */

static void
output_local_decl_ref (struct output_block *ob, tree name)
{
  bool new = output_decl_index (ob->main_stream, ob->local_decl_hash_table,
				&ob->next_local_decl_index, name);
  /* Push the new local var or param onto a vector for later
     processing.  */
  if (new)
    VEC_safe_push (tree, heap, ob->local_decls, name);

}

/* Look up LABEL in the type table and write the uleb128 index for it.  */

static void
output_label_ref (struct output_block *ob, tree label)
{
  void **slot;
  struct decl_slot d_slot;
  d_slot.t = label;

  slot = htab_find_slot (ob->label_hash_table, &d_slot, INSERT);
  if (*slot == NULL)
    {
      struct decl_slot *new_slot = xmalloc (sizeof (struct decl_slot));

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
      struct decl_slot *old_slot = (struct decl_slot *)*slot;
      output_sleb128 (ob, old_slot->slot_num);
    }
}


/* Output the start of a record with TAG and possibly flags for EXPR,
   and the TYPE for VALUE to OB.  */

static void
output_record_start (struct output_block *ob, tree expr,
		     tree value, unsigned int tag)
{
  output_1_stream (ob->main_stream, tag);
  LTO_DEBUG_INDENT (tag)
  if (expr)
    {
      enum tree_code code = TREE_CODE (expr);
      if (value && TEST_BIT (lto_types_needed_for, code))
	output_type_ref (ob, TREE_TYPE (value));
      output_tree_flags (ob, expr);
    }
}


/* Output a LIST of TYPE_DECLS.  */

static void
output_type_list (struct output_block *ob, tree list)
{
  tree tl;
  int count = 0;
  if (list)
    {
      gcc_assert (TREE_CODE (list) == TREE_LIST);
      for (tl = list; tl; tl = TREE_CHAIN (tl))
	if (TREE_VALUE (tl) != NULL_TREE)
	  count++;

      output_uleb128 (ob, count);
      for (tl = list; tl; tl = TREE_CHAIN (tl))
	if (TREE_VALUE (tl) != NULL_TREE)
	  output_type_ref (ob, TREE_VALUE (tl));
    }
  else
    output_zero (ob);
}


/* Output a LIST of TAG.  */

static void
output_tree_list (struct output_block *ob, tree list, unsigned int tag)
{
  tree tl;
  int count = 0;
  if (list)
    {
      gcc_assert (TREE_CODE (list) == TREE_LIST);
      output_record_start (ob, NULL, NULL, tag);
      for (tl = list; tl; tl = TREE_CHAIN (tl))
	if (TREE_VALUE (tl) != NULL_TREE)
	  count++;

      output_uleb128 (ob, count);
      for (tl = list; tl; tl = TREE_CHAIN (tl))
	if (TREE_VALUE (tl) != NULL_TREE)
	  output_expr_operand (ob, TREE_VALUE (tl));
    }
  else
    output_zero (ob);
}


/* Output an eh_cleanup region with REGION_NUMBER.  HAS_INNER is true if
   there are children of this node and HAS_PEER is true if there are
   siblings of this node.  MAY_CONTAIN_THROW and PREV_TRY are the
   fields of the eh_region.  */

static void
output_eh_cleanup (void *obv,
		   int region_number,
		   bool has_inner, bool has_peer,
		   bool may_contain_throw, int prev_try)
{
  struct output_block *ob = (struct output_block*)obv;
  output_record_start (ob, NULL, NULL,
		       LTO_eh_table_cleanup0 +
		     (has_inner ? 1 : 0) + (may_contain_throw ? 2 : 0));
  output_sleb128 (ob, region_number);
  output_sleb128 (ob, prev_try);
  if (!has_peer)
    output_zero (ob);
}


/* Output an eh_try region with REGION_NUMBER.  HAS_INNER is true if
   there are children of this node and HAS_PEER is true if there are
   siblings of this node.  MAY_CONTAIN_THROW, CATCH and LAST_CATCH are
   the fields of the eh_region.  */

static void
output_eh_try (void *obv,
	       int region_number,
	       bool has_inner, bool has_peer,
	       bool may_contain_throw, int catch,
	       int last_catch)
{
  struct output_block *ob = (struct output_block*)obv;
  output_record_start (ob, NULL, NULL,
		       LTO_eh_table_try0 +
		     (has_inner ? 1 : 0) + (may_contain_throw ? 2 : 0));
  output_sleb128 (ob, region_number);
  output_sleb128 (ob, catch);
  output_sleb128 (ob, last_catch);
  if (!has_peer)
    output_zero (ob);
}


/* Output an eh_catch region with REGION_NUMBER.  HAS_INNER is true if
   there are children of this node and HAS_PEER is true if there are
   siblings of this node.  MAY_CONTAIN_THROW, NEXT_CATCH and
   PREV_CATCH, and TYPE_LIST are the fields of the eh_region.  */

static void
output_eh_catch (void *obv,
		 int region_number,
		 bool has_inner, bool has_peer,
		 bool may_contain_throw, int next_catch,
		 int prev_catch, tree type_list)
{
  struct output_block *ob = (struct output_block*)obv;
  output_record_start (ob, NULL, NULL,
		       LTO_eh_table_catch0 +
		     (has_inner ? 1 : 0) + (may_contain_throw ? 2 : 0));
  output_sleb128 (ob, region_number);
  output_sleb128 (ob, next_catch);
  output_sleb128 (ob, prev_catch);
  output_type_list (ob, type_list);
  if (!has_peer)
    output_zero (ob);
}

/* Output an eh_allowed_exceptions region with REGION_NUMBER.
   HAS_INNER is true if there are children of this node and HAS_PEER
   is true if there are siblings of this node.  MAY_CONTAIN_THROW, and
   TYPE_LIST are the fields of the eh_region.  */

static void
output_eh_allowed (void *obv,
		   int region_number,
		   bool has_inner, bool has_peer,
		   bool may_contain_throw, tree type_list)
{
  struct output_block *ob = (struct output_block*)obv;
  output_record_start (ob, NULL, NULL,
		       LTO_eh_table_allowed0 +
		     (has_inner ? 1 : 0) + (may_contain_throw ? 2 : 0));
  output_sleb128 (ob, region_number);
  output_type_list (ob, type_list);
  if (!has_peer)
    output_zero (ob);
}


/* Output an eh_must_not_throw region with REGION_NUMBER.  HAS_INNER
   is true if there are children of this node and HAS_PEER is true if
   there are siblings of this node.  MAY_CONTAIN_THROW is the field of
   the eh_region.  */

static void
output_eh_must_not_throw (void *obv,
			  int region_number,
			  bool has_inner, bool has_peer,
			  bool may_contain_throw)
{
  struct output_block *ob = (struct output_block*)obv;
  output_record_start (ob, NULL, NULL,
		       LTO_eh_table_must_not_throw0 +
		     (has_inner ? 1 : 0) + (may_contain_throw ? 2 : 0));
  output_sleb128 (ob, region_number);
  if (!has_peer)
    output_zero (ob);
}


/* Output the existing eh_table to OB.  */

static void
output_eh_regions (struct output_block *ob, struct function *cfun)
{
  if (0 && cfun->eh)
    {
      output_record_start (ob, NULL, NULL, LTO_eh_table);
      output_eh_records (ob, cfun,
			 output_eh_cleanup,
			 output_eh_try,
			 output_eh_catch,
			 output_eh_allowed,
			 output_eh_must_not_throw);
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
      if (TREE_CODE (purpose) == RANGE_EXPR)
	{
	  output_record_start (ob, NULL, NULL, LTO_constructor_range);
	  /* Need the types here to reconstruct the ranges.  */
	  output_type_ref (ob, TREE_OPERAND (purpose, 0));
	  output_integer (ob, TREE_OPERAND (purpose, 0));
	  output_type_ref (ob, TREE_OPERAND (purpose, 1));
	  output_integer (ob, TREE_OPERAND (purpose, 1));
	  LTO_DEBUG_UNDENT ()
	}

      switch (TREE_CODE (value))
	{
	case CONSTRUCTOR:
	  output_constructor (ob, value);
	  LTO_DEBUG_UNDENT ()
	  break;
	case INTEGER_CST:
	case REAL_CST:
	case STRING_CST:
	case COMPLEX_CST:
	case VECTOR_CST:
	  output_expr_operand (ob, value);
	  break;

	default:
	  gcc_unreachable ();
	  break;
	}
    }
}

/* Output EXPR to the main stream in OB.  */

static void
output_expr_operand (struct output_block *ob, tree expr)
{
  enum tree_code code;
  enum tree_code_class class;
  unsigned int tag;

  gcc_assert (expr);

  code = TREE_CODE (expr);
  class = TREE_CODE_CLASS (code);
  tag = expr_to_tag [code];

  gcc_assert (class != tcc_type);

  switch (code)
    {
    case COMPLEX_CST:
      if (TREE_CODE (TREE_REALPART (expr)) == REAL_CST)
	{
	  output_record_start (ob, expr, TREE_REALPART (expr),
			       LTO_complex_cst1);
	  output_real (ob, TREE_REALPART (expr));
	  output_real (ob, TREE_IMAGPART (expr));
	}
      else
	{
	  output_record_start (ob, expr, TREE_REALPART (expr),
			       LTO_complex_cst0);
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
	output_record_start (ob, expr, expr, LTO_string_cst);
	output_string (ob, ob->main_stream, 
		       TREE_STRING_POINTER (expr),
		       TREE_STRING_LENGTH (expr));
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
	    output_record_start (ob, expr, TREE_VALUE (t),
				 LTO_vector_cst1);
	    output_uleb128 (ob, len);
	    output_real (ob, TREE_VALUE (t));
	    while ((t = TREE_CHAIN (t)) != NULL)
	      output_real (ob, TREE_VALUE (t));
	  }
	else
	  {
	    output_record_start (ob, expr, TREE_VALUE (t),
				 LTO_vector_cst0);
	    output_uleb128 (ob, len);
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
	output_record_start (ob, expr, NULL,
			     LTO_case_label_expr0 + variant);

	if (CASE_LOW (expr) != NULL_TREE)
	  output_integer (ob, CASE_LOW (expr));
	if (CASE_HIGH (expr) != NULL_TREE)
	  output_integer (ob, CASE_HIGH (expr));
	output_expr_operand (ob, CASE_LABEL (expr));
      }
      break;

    case CONSTRUCTOR:
      output_constructor (ob, expr);
      break;

    case SSA_NAME:
      /* FIXME: I am dead sure that this code is wrong for SSA
	 names. But lto output is currently being done before SSA form
	 is built so we can just fix it later.  */
      output_record_start (ob, expr, expr, LTO_ssa_name);
      output_local_decl_ref (ob, SSA_NAME_VAR (expr));
      output_uleb128 (ob, SSA_NAME_VERSION (expr));

      /* Just to make sure that the comment above is headed.  */
      gcc_unreachable ();
      break;

    case CONST_DECL:
      /* Just ignore these, Mark will make them disappear.  */
      break;

    case FIELD_DECL:
      {
	bool new;
	output_record_start (ob, NULL, NULL, tag);
	
	new = output_decl_index (ob->main_stream, ob->field_decl_hash_table,
				 &ob->next_field_decl_index, expr);
	if (new)
	  VEC_safe_push (tree, heap, ob->field_decls, expr);
      }
      break;

    case FUNCTION_DECL:
      {
	bool new;
	output_record_start (ob, NULL, NULL, tag);
	
	new = output_decl_index (ob->main_stream, ob->fn_decl_hash_table,
				 &ob->next_fn_decl_index, expr);
	if (new)
	  VEC_safe_push (tree, heap, ob->fn_decls, expr);
      }
      break;

    case VAR_DECL:
      if (TREE_STATIC (expr) || DECL_EXTERNAL (expr))
	{
	  bool new;
	  output_record_start (ob, NULL, NULL, LTO_var_decl1);

	  new = output_decl_index (ob->main_stream, ob->var_decl_hash_table,
				   &ob->next_var_decl_index, expr);
	  if (new)
	    VEC_safe_push (tree, heap, ob->var_decls, expr);
	}
      else
	{
	  output_record_start (ob, NULL, NULL, LTO_var_decl0);
	  output_local_decl_ref (ob, expr);
	}
      break;

    case PARM_DECL:
      output_record_start (ob, NULL, NULL, tag);
      output_local_decl_ref (ob, expr);
      break;

    case LABEL_DECL:
      output_record_start (ob, expr, NULL, tag);
      output_label_ref (ob, expr);
      break;

    case LABEL_EXPR:
      output_record_start (ob, expr, NULL, tag);
      output_label_ref (ob, TREE_OPERAND (expr, 0));
      break;

    case RESULT_DECL:
      output_record_start (ob, expr, NULL, tag);
      break;

    case COMPONENT_REF:
      output_record_start (ob, expr, expr, tag);
      output_expr_operand (ob, TREE_OPERAND (expr, 0));
      output_expr_operand (ob, TREE_OPERAND (expr, 1));
      /* Ignore 3 because it can be recomputed.  */
      break;

    case CALL_EXPR:
      {
	unsigned int count = TREE_INT_CST_LOW (TREE_OPERAND (expr, 0));
	unsigned int i;
	output_uleb128 (ob, count);

	/* Operand 2 is the call chain.  */
	if (TREE_OPERAND (expr, 2))
	  {
	    output_record_start (ob, expr, expr, LTO_call_expr1);
	    output_expr_operand (ob, TREE_OPERAND (expr, 2));
	  }
	else
	  output_record_start (ob, expr, expr, LTO_call_expr0);

	output_expr_operand (ob, TREE_OPERAND (expr, 1));
	for (i = 3; i < count; i++)
	  output_expr_operand (ob, TREE_OPERAND (expr, i));
      }
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
      /* Ignore operands 2 and 3 for ARRAY_REF and ARRAY_RANGE REF
	 because they can be recomputed.  */
      output_record_start (ob, expr, expr, tag);
      output_expr_operand (ob, TREE_OPERAND (expr, 0));
      output_expr_operand (ob, TREE_OPERAND (expr, 1));
      break;


    case ASM_EXPR:
      {
	tree string_cst = ASM_STRING (expr);
	output_record_start (ob, expr, NULL, LTO_asm_expr);
	output_string (ob, ob->main_stream, 
		       TREE_STRING_POINTER (string_cst),
		       TREE_STRING_LENGTH (string_cst));

	/* Each of the operand sets is a list of trees terminated by a
	   zero.  The problem is that the operands are not all
	   variables.  They could be constants or strings so you need
	   the general tree mechanism to put them out.  */
	if (ASM_INPUTS (expr) != NULL_TREE)
	  {
	    tree tl;
	    output_record_start (ob, NULL, NULL,
				 LTO_asm_inputs);
	    for (tl = ASM_INPUTS (expr); tl; tl = TREE_CHAIN (tl))
	      output_expr_operand (ob, TREE_VALUE (tl));
	    output_zero (ob);
	    LTO_DEBUG_UNDENT ()
	  }
	if (ASM_OUTPUTS (expr) != NULL_TREE)
	  {
	    tree tl;
	    output_record_start (ob, NULL, NULL,
				 LTO_asm_outputs);
	    for (tl = ASM_OUTPUTS (expr); tl; tl = TREE_CHAIN (tl))
	      output_expr_operand (ob, TREE_VALUE (tl));
	    output_zero (ob);
	    LTO_DEBUG_UNDENT ()
	  }
	if (ASM_CLOBBERS (expr) != NULL_TREE)
	  {
	    tree tl;
	    output_record_start (ob, NULL, NULL,
				 LTO_asm_clobbers);
	    for (tl = ASM_CLOBBERS (expr); tl; tl = TREE_CHAIN (tl))
	      output_expr_operand (ob, TREE_VALUE (tl));
	    output_zero (ob);
	    LTO_DEBUG_UNDENT ()
	  }
      }
      break;

    case RESX_EXPR:
      output_record_start (ob, expr, NULL, tag);
      output_uleb128 (ob, TREE_INT_CST_LOW (TREE_OPERAND (expr, 0)));
      break;

    case RETURN_EXPR:
      {
	tree t = TREE_OPERAND (expr, 0);
	if (t == NULL)
	  {
	    /* Form return.  */
	    output_record_start (ob, expr, NULL,
				 LTO_return_expr0);
	  }
	else if (TREE_CODE (t) == MODIFY_EXPR)
	  {
	    /* Form return a = b;  */
	    output_record_start (ob, expr, NULL,
				 LTO_return_expr2);
	    output_expr_operand (ob, TREE_OPERAND (t, 0));
	    output_expr_operand (ob, TREE_OPERAND (t, 1));
	  }
	else
	  {
	    /* Form return a; */
	    output_record_start (ob, expr, NULL,
				 LTO_return_expr1);
	    output_expr_operand (ob, t);
	  }
      }
      break;

    case SWITCH_EXPR:
      {
	tree label_vec = TREE_OPERAND (expr, 2);
	size_t len = TREE_VEC_LENGTH (label_vec);
	size_t i;
	output_record_start (ob, expr, NULL, tag);
	output_uleb128 (ob, len);
	output_expr_operand (ob, TREE_OPERAND (expr, 0));
	gcc_assert (TREE_OPERAND (expr, 1) == NULL);

	for (i = 0; i < len; ++i)
	  output_expr_operand (ob, TREE_VEC_ELT (label_vec, i));
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
	for (i = 0; i< TREE_CODE_LENGTH (TREE_CODE (expr)); i++)
	  output_expr_operand (ob, TREE_OPERAND (expr, i));
	break;
      }
      /* This is the error case, these are type codes that will either
	 never happen or that we have not gotten around to dealing
	 with are here.  */
    case BIND_EXPR:
    case BLOCK:
    case CATCH_EXPR:
    case EH_FILTER_EXPR:
    case NAME_MEMORY_TAG:
    case OMP_CONTINUE:
    case OMP_CRITICAL:
    case OMP_FOR:
    case OMP_MASTER:
    case OMP_ORDERED:
    case OMP_PARALLEL:
    case OMP_RETURN:
    case OMP_SECTIONS:
    case OMP_SINGLE:
    case STRUCT_FIELD_TAG:
    case SYMBOL_MEMORY_TAG:
    case TARGET_MEM_REF:
    case TRY_CATCH_EXPR:
    case TRY_FINALLY_EXPR:
    default:
      /* We cannot have forms that are not explicity handled.  So when
	 this is triggered, there is some form that is not being
	 output.  */
      gcc_unreachable ();
    }

  LTO_DEBUG_UNDENT ()
}


/* Output the local var_decls and parm_decls to OB.  */

static void
output_local_vars (struct output_block *ob)
{
  unsigned int index = 0;

#ifdef LTO_STREAM_DEBUGGING
  lto_debug_context.current_data = ob->debug_decl_stream;
#endif

  while (index < VEC_length (tree, ob->local_decls))
    {
      tree decl = VEC_index (tree, ob->local_decls, index++);
      unsigned int variant = 0;
      bool is_var = (TREE_CODE (decl) == VAR_DECL);
      bool needs_backing_var
	= (DECL_DEBUG_EXPR_IS_FROM (decl) && DECL_DEBUG_EXPR (decl));

      /* This will either be a local var decl or a parm decl. */
      unsigned int tag;

      gcc_assert (DECL_SIZE (decl));
      variant |= DECL_ATTRIBUTES (decl)      != NULL_TREE ? 0x01 : 0;
      variant |= DECL_SIZE_UNIT (decl)       != NULL_TREE ? 0x02 : 0;
      variant |= needs_backing_var                        ? 0x04 : 0;
      variant |= DECL_ABSTRACT_ORIGIN (decl) != NULL_TREE ? 0x08 : 0;

      tag = (is_var
	     ? LTO_local_var_decl_body0
	     : LTO_parm_decl_body0)
	+ variant;

      output_record_start (ob, NULL, NULL, tag);

      /* Put out the name if there is one.  */
      if (DECL_NAME (decl))
	{
	  tree name = DECL_NAME (decl);
	  output_string (ob, ob->main_stream, 
			 IDENTIFIER_POINTER (name), 
			 IDENTIFIER_LENGTH (name));
	}
      else
	output_uleb128 (ob, 0);

      output_type_ref (ob, TREE_TYPE (decl));

      if (!is_var)
	output_type_ref (ob, DECL_ARG_TYPE (decl));
      
      output_tree_flags (ob, decl);
      LTO_DEBUG_TOKEN ("align")
      output_uleb128 (ob, DECL_ALIGN (decl));

      /* Put out the subtrees.  */
      LTO_DEBUG_TOKEN ("size")
      output_expr_operand (ob, DECL_SIZE (decl));
      if (DECL_ATTRIBUTES (decl)!= NULL_TREE)
	{
	  LTO_DEBUG_TOKEN ("attributes")
	  output_tree_list (ob, DECL_ATTRIBUTES (decl),
			    LTO_attribute_list);
	}
      if (DECL_SIZE_UNIT (decl) != NULL_TREE)
	output_expr_operand (ob, DECL_SIZE_UNIT (decl));
      if (needs_backing_var)
	output_expr_operand (ob, DECL_DEBUG_EXPR (decl));
      if (DECL_ABSTRACT_ORIGIN (decl) != NULL_TREE)
	output_expr_operand (ob, DECL_ABSTRACT_ORIGIN (decl));

      LTO_DEBUG_UNDENT()
    }
}


/* Output the names in the named labels to the named_label stream.  */

static void
output_named_labels (struct output_block *ob)
{
  unsigned int index = 0;

#ifdef LTO_STREAM_DEBUGGING
  lto_debug_context.current_data = ob->debug_label_stream;
#endif

  while (index < VEC_length (tree, ob->named_labels))
    {
      tree decl = VEC_index (tree, ob->named_labels, index++);
      tree name = DECL_NAME (decl);
      output_string (ob, ob->named_label_stream, 
		     IDENTIFIER_POINTER (name), 
		     IDENTIFIER_LENGTH (name));
   }
}


/* Output a basic block BB to the main stream in OB for this CFUN.  */

static void
output_bb (struct output_block *ob, basic_block bb, struct function *cfun)
{
  edge e;
  edge_iterator ei;
  block_stmt_iterator bsi = bsi_start (bb);

  output_record_start (ob, NULL, NULL,
		       bsi_end_p (bsi) ? LTO_bb0 : LTO_bb1);

  /* The index of the basic block.  */
  LTO_DEBUG_TOKEN ("bbindex")
  output_uleb128 (ob, bb->index);

  /* Output the successors and the edge flags.  */
  LTO_DEBUG_TOKEN ("edgecount")
  output_uleb128 (ob, EDGE_COUNT (bb->succs));
  FOR_EACH_EDGE (e, ei, bb->succs)
    {
      LTO_DEBUG_TOKEN ("dest")
      output_uleb128 (ob, e->dest->index);
      LTO_DEBUG_TOKEN ("eflags")
      output_uleb128 (ob, e->flags);
    }

  if (!bsi_end_p (bsi))
    {
      /* Output the statements.  The list of statements is terminated
	 with a zero.  */
      for (bsi = bsi_start (bb);
	   !bsi_end_p (bsi); bsi_next (&bsi))
	{
	  tree stmt = bsi_stmt (bsi);
	
	  LTO_DEBUG_INDENT_TOKEN ("stmt")
	  output_expr_operand (ob, stmt);
	
	  /* We only need to set the region number of the tree that
	     could throw if the region number is different from the
	     last region number we set.  */
	  if (0 && tree_could_throw_p (stmt))
	    {
	      int region = lookup_stmt_eh_region_fn (cfun, stmt);
	      if (region != last_eh_region_seen)
		{
		  output_record_start (ob, NULL, NULL,
				       LTO_set_eh0 + region ? 1 : 0);
		  if (region)
		    output_sleb128 (ob, region);
		
		  last_eh_region_seen = region;
		}
	    }
	}

      LTO_DEBUG_INDENT_TOKEN ("stmt")
      output_zero (ob);
    }
  LTO_DEBUG_UNDENT()
}


/* Create the header in the file.  */

static void
produce_asm (struct output_block *ob, tree function)
{
  int index;
  tree decl;
  tree type;
  const char *fn = IDENTIFIER_POINTER (DECL_ASSEMBLER_NAME (function));
  const char *section_name = concat (LTO_SECTION_NAME_PREFIX, fn, NULL);
  struct lto_function_header function_header;
  lto_out_ref out_ref = {0, NULL, NULL};

  /* The entire header is stream computed here.  */
  switch_to_section (get_section (section_name, SECTION_DEBUG, function));
  
  /* Write the header which says how to decode the pieces of the
     function.  */
  function_header.major_version = LTO_major_version;
  function_header.minor_version = LTO_minor_version;
  
  function_header.num_field_decls = VEC_length (tree, ob->field_decls);
  function_header.num_fn_decls = VEC_length (tree, ob->fn_decls);
  function_header.num_var_decls = VEC_length (tree, ob->var_decls);
  function_header.num_types = VEC_length (tree, ob->types);
  function_header.num_local_decls = VEC_length (tree, ob->local_decls);
  function_header.num_named_labels = ob->next_named_label_index;
  function_header.num_unnamed_labels = -ob->next_unnamed_label_index;

  function_header.compressed_size = 0;
  function_header.named_label_size = ob->named_label_stream->total_size;
  function_header.local_decls_size = ob->local_decl_stream->total_size;
  function_header.main_size = ob->main_stream->total_size;
  function_header.string_size = ob->string_stream->total_size;
#ifdef LTO_STREAM_DEBUGGING
  function_header.debug_decl_size = ob->debug_decl_stream->total_size;
  function_header.debug_label_size = ob->debug_label_stream->total_size;
  function_header.debug_main_size = ob->debug_main_stream->total_size;
#else
  function_header.debug_decl_size = -1;
  function_header.debug_label_size = -1;
  function_header.debug_main_size = -1;
#endif

  assemble_string ((const char *)&function_header, 
		   sizeof (struct lto_function_header));

  /* Write the global type references.  */
  for (index = 0; VEC_iterate(tree, ob->field_decls, index, decl); index++)
    {
#ifdef GIMPLE_SYMBOL_TABLE_WORKS
      lto_field_ref (decl, out_ref);
#else
      out_ref.section = 0;
      out_ref.base_label = "0";
      out_ref.label = "0";
#endif
      dw2_asm_output_data (8, out_ref.section, " ");
      dw2_asm_output_delta (8, out_ref.label,
			    out_ref.base_label, " ");
    }

  /* Write the global type references.  */
  for (index = 0; VEC_iterate(tree, ob->fn_decls, index, decl); index++)
    {
#ifdef GIMPLE_SYMBOL_TABLE_WORKS
      lto_fn_ref (delc, &out_ref);
#else
      out_ref.section = 0;
      out_ref.base_label = "0";
      out_ref.label = "0";
#endif
      dw2_asm_output_data (8, out_ref.section, " ");
      dw2_asm_output_delta (8, out_ref.label,
			    out_ref.base_label, " ");
    }

  /* Write the global type references.  */
  for (index = 0; VEC_iterate(tree, ob->var_decls, index, decl); index++)
    {
#ifdef GIMPLE_SYMBOL_TABLE_WORKS
      lto_var_ref (decl, &out_ref);
#else
      out_ref.section = 0;
      out_ref.base_label = "0";
      out_ref.label = "0";
#endif
      dw2_asm_output_data (8, out_ref.section, " ");
      dw2_asm_output_delta (8, out_ref.label,
			    out_ref.base_label, " ");
    }

  /* Write the global type references.  */
  for (index = 0; VEC_iterate(tree, ob->types, index, type); index++)
    {
      fprintf (stderr, "type %d = ", index);
      print_generic_expr (stderr, type, TDF_VOPS|TDF_UID);
      fprintf (stderr, "\n");
      print_node (stderr, "", type, 0);
      fprintf (stderr, "\n\n");

      lto_type_ref (type, &out_ref);
      dw2_asm_output_data (8, out_ref.section, " ");
      dw2_asm_output_delta (8, out_ref.label,
			    out_ref.base_label, " ");
    }

  /* Put all of the gimple and the string table out the asm file as a
     block of text.  */
  write_stream (ob->named_label_stream);
  write_stream (ob->local_decl_stream);
  write_stream (ob->main_stream);
  write_stream (ob->string_stream);
#ifdef LTO_STREAM_DEBUGGING
  write_stream (ob->debug_decl_stream);
  write_stream (ob->debug_label_stream);
  write_stream (ob->debug_main_stream);
#endif
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
  RESET_BIT (lto_flags_needed_for, VAR_DECL);
  RESET_BIT (lto_flags_needed_for, PARM_DECL);
  RESET_BIT (lto_flags_needed_for, FIELD_DECL);

  lto_types_needed_for = sbitmap_alloc (NUM_TREE_CODES);

#if REDUNDANT_TYPE_SYSTEM
  /* These forms never need types.  */
  sbitmap_ones (lto_types_needed_for);
  RESET_BIT (lto_types_needed_for, ASM_EXPR);
  RESET_BIT (lto_types_needed_for, CASE_LABEL_EXPR);
  RESET_BIT (lto_types_needed_for, LABEL_DECL);
  RESET_BIT (lto_types_needed_for, LABEL_EXPR);
  RESET_BIT (lto_types_needed_for, MODIFY_EXPR);
  RESET_BIT (lto_types_needed_for, PARM_DECL);
  RESET_BIT (lto_types_needed_for, RESULT_DECL);
  RESET_BIT (lto_types_needed_for, RESX_EXPR);
  RESET_BIT (lto_types_needed_for, RETURN_EXPR);
  RESET_BIT (lto_types_needed_for, STRING_CST);
  RESET_BIT (lto_types_needed_for, SWITCH_EXPR);
  RESET_BIT (lto_types_needed_for, VAR_DECL);
#else
  /* These forms will need types, even when the type system is fixed.  */
  SET_BIT (lto_types_needed_for, COMPLEX_CST);
  SET_BIT (lto_types_needed_for, CONSTRUCTOR);
  SET_BIT (lto_types_needed_for, CONVERT_EXPR);
  SET_BIT (lto_types_needed_for, INTEGER_CST);
  SET_BIT (lto_types_needed_for, NOP_EXPR);
  SET_BIT (lto_types_needed_for, REAL_CST);
  SET_BIT (lto_types_needed_for, VECTOR_CST );
  SET_BIT (lto_types_needed_for, VIEW_CONVERT_EXPR);
#endif

#ifdef LTO_STREAM_DEBUGGING
#define LTO_STREAM_DEBUGGING_INIT_NAMES
#define SET_NAME(index,value) LTO_tag_names[index] = value;
#include "lto-tree-tags.def"
#undef SET_NAME
#undef LTO_STREAM_DEBUGGING_INIT_NAMES
#endif
}


/* Static initialization for the lto writer.  */

static void
lto_static_init_local (void)
{
  if (initialized_local)
    return;

  initialized_local = true;

  /* Initialize the expression to tag mapping.  */
#define MAP_EXPR_TAG(expr,tag)   expr_to_tag [expr] = tag;
#define TREE_SINGLE_MECHANICAL_TRUE
#define TREE_SINGLE_MECHANICAL_FALSE
#include "lto-tree-tags.def"

#undef MAP_EXPR_TAG
#undef TREE_SINGLE_MECHANICAL_TRUE
#undef TREE_SINGLE_MECHANICAL_FALSE

  lto_static_init ();
}


#ifdef FILE_PER_FUNCTION
/* The once per compilation unit initialization flag.  */
static int function_num;
#endif

/* Output CFUN.  */

static void
output_function (tree function)
{
  struct function *this_cfun = DECL_STRUCT_FUNCTION (function);
  basic_block bb;
  struct output_stream *tmp_stream;

  struct output_block *ob = xcalloc (1, sizeof (struct output_block));
  ob->main_stream = xcalloc (1, sizeof (struct output_stream));
  ob->string_stream = xcalloc (1, sizeof (struct output_stream));
  ob->local_decl_stream = xcalloc (1, sizeof (struct output_stream));
  ob->named_label_stream = xcalloc (1, sizeof (struct output_stream));
#ifdef LTO_STREAM_DEBUGGING
  ob->debug_decl_stream = xcalloc (1, sizeof (struct output_stream));
  ob->debug_label_stream = xcalloc (1, sizeof (struct output_stream));
  ob->debug_main_stream = xcalloc (1, sizeof (struct output_stream));
  
  lto_debug_context.out = &debug_out_fun;
  lto_debug_context.decl_data = ob->debug_decl_stream;
  lto_debug_context.label_data = ob->debug_label_stream;
  lto_debug_context.main_data = ob->debug_main_stream;
  lto_debug_context.current_data = ob->debug_main_stream;
  lto_debug_context.indent = 0;
#endif

  ob->last_file = NULL;
  ob->last_line = -1;

  /* This file code is just a hack to get the stuff where it can be
     measured.  The real lto will put the info into the assembly
     stream.  */

  ob->label_hash_table
    = htab_create (37, hash_label_slot_node, eq_label_slot_node, free);
  ob->local_decl_hash_table
    = htab_create (37, hash_decl_slot_node, eq_decl_slot_node, free);
  ob->field_decl_hash_table
    = htab_create (37, hash_decl_slot_node, eq_decl_slot_node, free);
  ob->fn_decl_hash_table
    = htab_create (37, hash_decl_slot_node, eq_decl_slot_node, free);
  ob->var_decl_hash_table
    = htab_create (37, hash_decl_slot_node, eq_decl_slot_node, free);
  ob->string_hash_table
    = htab_create (37, hash_string_slot_node, eq_string_slot_node, free);
  ob->type_hash_table
    = htab_create (37, hash_type_slot_node, eq_type_slot_node, free);

  /* The unnamed labels must all be negative.  */
  ob->next_unnamed_label_index = -1;
  /* Make string 0 be a NULL string.  */
  output_1_stream (ob->string_stream, 0);

  last_eh_region_seen = 0;

  output_record_start (ob, NULL, NULL, LTO_function);

  /* Output any exception-handling regions.  */
  output_eh_regions (ob, this_cfun);

  /* Output the number of the highest basic block.  */
  output_uleb128 (ob, last_basic_block_for_function(this_cfun));

  /* Output the code for the function.  */
  FOR_ALL_BB_FN (bb, this_cfun)
    output_bb (ob, bb, this_cfun);

  /* The terminator for this function.  */
  output_zero (ob);
  LTO_DEBUG_UNDENT()

  /* We have found MOST of the local vars by scanning the function.
     There is always the possibility that there may be some lurking on
     the fields (such as the two size fields) of the local vars that
     we must put out.

     The easiest way to get all of this stuff generated is to play
     pointer games with the streams and reuse the code for putting out
     the function bodies for putting out the local decls.  It needs to
     go into a separate stream because the LTO reader will want to
     process the local variables first, rather than have to back patch
     them.
  */
  tmp_stream = ob->main_stream;
  ob->main_stream = ob->local_decl_stream;
  output_local_vars (ob);
  ob->main_stream = tmp_stream;

  /* Output the names in the named labels.  */
  output_named_labels (ob);

  /* Create a file to hold the pickled output of this function.  This
     is a temp standin until we start writing sections.  */
  produce_asm (ob, function);

  htab_delete (ob->label_hash_table);
  htab_delete (ob->local_decl_hash_table);
  htab_delete (ob->field_decl_hash_table);
  htab_delete (ob->fn_decl_hash_table);
  htab_delete (ob->var_decl_hash_table);
  htab_delete (ob->string_hash_table);
  htab_delete (ob->type_hash_table);
  VEC_free (tree, heap, ob->local_decls);
  VEC_free (tree, heap, ob->field_decls);
  VEC_free (tree, heap, ob->fn_decls);
  VEC_free (tree, heap, ob->named_labels);
  VEC_free (tree, heap, ob->var_decls);
  VEC_free (tree, heap, ob->types);
  free (ob);
}


/* Main entry point from the pass manager.  */

static unsigned int
lto_output (void)
{
  struct cgraph_node *node;
  section *saved_section = in_section;

  lto_static_init_local ();

  /* Process only the fuctions with bodies and only process the master
     ones of them.  */
  for (node = cgraph_nodes; node; node = node->next)
    if (node->analyzed && cgraph_is_master_clone (node, false))
      output_function (node->decl);

  /* Put back the assembly section that was there before we started
     writing lto info.  */
  if (saved_section)
    switch_to_section (saved_section);

  return 0;
}

static bool
gate_lto_out (void)
{
  return (flag_generate_lto
	  /* Don't bother doing anything if the program has errors.  */
	  && !(errorcount || sorrycount));
}

struct tree_opt_pass pass_ipa_lto_out =
{
  "lto-function-out",	                /* name */
  gate_lto_out,			        /* gate */
  lto_output,		        	/* execute */
  NULL,					/* sub */
  NULL,					/* next */
  0,					/* static_pass_number */
  TV_IPA_LTO_OUT,		        /* tv_id */
  0,	                                /* properties_required */
  0,					/* properties_provided */
  0,					/* properties_destroyed */
  0,					/* todo_flags_start */
  0,                                    /* todo_flags_finish */
  0					/* letter */
};


#ifdef LTO_STREAM_DEBUGGING

/* The low level output routine to print a single character to the
   debugging stream.  */

static void
debug_out_fun (struct lto_debug_context *context, char c)
{
  struct output_stream * stream 
    = (struct output_stream *)context->current_data;
  output_1_stream (stream, c);
}
#endif
