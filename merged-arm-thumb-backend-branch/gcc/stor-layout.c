/* C-compiler utilities for types and variables storage layout
   Copyright (C) 1987, 1988, 1992, 1993, 1994, 1995, 1996, 1996, 1998,
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


#include "config.h"
#include "system.h"
#include "tree.h"
#include "rtl.h"
#include "tm_p.h"
#include "flags.h"
#include "function.h"
#include "expr.h"
#include "toplev.h"
#include "ggc.h"

/* Set to one when set_sizetype has been called.  */
static int sizetype_set;

/* List of types created before set_sizetype has been called.  We do not
   make this a GGC root since we want these nodes to be reclaimed.  */
static tree early_type_list;

/* Data type for the expressions representing sizes of data types.
   It is the first integer type laid out.  */
tree sizetype_tab[(int) TYPE_KIND_LAST];

/* If nonzero, this is an upper limit on alignment of structure fields.
   The value is measured in bits.  */
unsigned int maximum_field_alignment;

/* If non-zero, the alignment of a bitstring or (power-)set value, in bits.
   May be overridden by front-ends.  */
unsigned int set_alignment = 0;

static void finalize_record_size PARAMS ((record_layout_info));
static void compute_record_mode PARAMS ((tree));
static void finalize_type_size PARAMS ((tree));
static void layout_union_field PARAMS ((record_layout_info, tree));
static void finish_union_layout PARAMS ((record_layout_info));

/* SAVE_EXPRs for sizes of types and decls, waiting to be expanded.  */

static tree pending_sizes;

/* Nonzero means cannot safely call expand_expr now,
   so put variable sizes onto `pending_sizes' instead.  */

int immediate_size_expand;

tree
get_pending_sizes ()
{
  tree chain = pending_sizes;
  tree t;

  /* Put each SAVE_EXPR into the current function.  */
  for (t = chain; t; t = TREE_CHAIN (t))
    SAVE_EXPR_CONTEXT (TREE_VALUE (t)) = current_function_decl;

  pending_sizes = 0;
  return chain;
}

void
put_pending_sizes (chain)
     tree chain;
{
  if (pending_sizes)
    abort ();

  pending_sizes = chain;
}

/* Given a size SIZE that may not be a constant, return a SAVE_EXPR
   to serve as the actual size-expression for a type or decl.  */

tree
variable_size (size)
     tree size;
{
  /* If the language-processor is to take responsibility for variable-sized
     items (e.g., languages which have elaboration procedures like Ada),
     just return SIZE unchanged.  Likewise for self-referential sizes.  */
  if (TREE_CONSTANT (size)
      || global_bindings_p () < 0 || contains_placeholder_p (size))
    return size;

  size = save_expr (size);

  /* If an array with a variable number of elements is declared, and
     the elements require destruction, we will emit a cleanup for the
     array.  That cleanup is run both on normal exit from the block
     and in the exception-handler for the block.  Normally, when code
     is used in both ordinary code and in an exception handler it is
     `unsaved', i.e., all SAVE_EXPRs are recalculated.  However, we do
     not wish to do that here; the array-size is the same in both
     places.  */
  if (TREE_CODE (size) == SAVE_EXPR)
    SAVE_EXPR_PERSISTENT_P (size) = 1;

  if (global_bindings_p ())
    {
      if (TREE_CONSTANT (size))
	error ("type size can't be explicitly evaluated");
      else
	error ("variable-size type declared outside of any function");

      return size_one_node;
    }

  if (immediate_size_expand)
    /* NULL_RTX is not defined; neither is the rtx type. 
       Also, we would like to pass const0_rtx here, but don't have it.  */
    expand_expr (size, expand_expr (integer_zero_node, NULL_PTR, VOIDmode, 0),
		 VOIDmode, 0);
  else if (cfun != 0
	   && cfun->x_dont_save_pending_sizes_p)
    /* The front-end doesn't want us to keep a list of the expressions
       that determine sizes for variable size objects.  */
    ;
  else
    pending_sizes = tree_cons (NULL_TREE, size, pending_sizes);

  return size;
}

#ifndef MAX_FIXED_MODE_SIZE
#define MAX_FIXED_MODE_SIZE GET_MODE_BITSIZE (DImode)
#endif

/* Return the machine mode to use for a nonscalar of SIZE bits.
   The mode must be in class CLASS, and have exactly that many bits.
   If LIMIT is nonzero, modes of wider than MAX_FIXED_MODE_SIZE will not
   be used.  */

enum machine_mode
mode_for_size (size, class, limit)
     int size;
     enum mode_class class;
     int limit;
{
  register enum machine_mode mode;

  if (limit && size > MAX_FIXED_MODE_SIZE)
    return BLKmode;

  /* Get the first mode which has this size, in the specified class.  */
  for (mode = GET_CLASS_NARROWEST_MODE (class); mode != VOIDmode;
       mode = GET_MODE_WIDER_MODE (mode))
    if (GET_MODE_BITSIZE (mode) == size)
      return mode;

  return BLKmode;
}

/* Similar, except passed a tree node.  */

enum machine_mode
mode_for_size_tree (size, class, limit)
     tree size;
     enum mode_class class;
     int limit;
{
  if (TREE_CODE (size) != INTEGER_CST
      /* What we really want to say here is that the size can fit in a
	 host integer, but we know there's no way we'd find a mode for
	 this many bits, so there's no point in doing the precise test.  */
      || compare_tree_int (size, 1000) > 0)
    return BLKmode;
  else
    return mode_for_size (TREE_INT_CST_LOW (size), class, limit);
}

/* Similar, but never return BLKmode; return the narrowest mode that
   contains at least the requested number of bits.  */

enum machine_mode
smallest_mode_for_size (size, class)
     int size;
     enum mode_class class;
{
  register enum machine_mode mode;

  /* Get the first mode which has at least this size, in the
     specified class.  */
  for (mode = GET_CLASS_NARROWEST_MODE (class); mode != VOIDmode;
       mode = GET_MODE_WIDER_MODE (mode))
    if (GET_MODE_BITSIZE (mode) >= size)
      return mode;

  abort ();
}

/* Find an integer mode of the exact same size, or BLKmode on failure.  */

enum machine_mode
int_mode_for_mode (mode)
     enum machine_mode mode;
{
  switch (GET_MODE_CLASS (mode))
    {
    case MODE_INT:
    case MODE_PARTIAL_INT:
      break;

    case MODE_COMPLEX_INT:
    case MODE_COMPLEX_FLOAT:
    case MODE_FLOAT:
      mode = mode_for_size (GET_MODE_BITSIZE (mode), MODE_INT, 0);
      break;

    case MODE_RANDOM:
      if (mode == BLKmode)
        break;

      /* ... fall through ... */

    case MODE_CC:
    default:
      abort ();
    }

  return mode;
}

/* Return the value of VALUE, rounded up to a multiple of DIVISOR.
   This can only be applied to objects of a sizetype.  */

tree
round_up (value, divisor)
     tree value;
     int divisor;
{
  tree arg = size_int_type (divisor, TREE_TYPE (value));

  return size_binop (MULT_EXPR, size_binop (CEIL_DIV_EXPR, value, arg), arg);
}

/* Likewise, but round down.  */

tree
round_down (value, divisor)
     tree value;
     int divisor;
{
  tree arg = size_int_type (divisor, TREE_TYPE (value));

  return size_binop (MULT_EXPR, size_binop (FLOOR_DIV_EXPR, value, arg), arg);
}

/* Set the size, mode and alignment of a ..._DECL node.
   TYPE_DECL does need this for C++.
   Note that LABEL_DECL and CONST_DECL nodes do not need this,
   and FUNCTION_DECL nodes have them set up in a special (and simple) way.
   Don't call layout_decl for them.

   KNOWN_ALIGN is the amount of alignment we can assume this
   decl has with no special effort.  It is relevant only for FIELD_DECLs
   and depends on the previous fields.
   All that matters about KNOWN_ALIGN is which powers of 2 divide it.
   If KNOWN_ALIGN is 0, it means, "as much alignment as you like":
   the record will be aligned to suit.  */

void
layout_decl (decl, known_align)
     tree decl;
     unsigned int known_align;
{
  register tree type = TREE_TYPE (decl);
  register enum tree_code code = TREE_CODE (decl);

  if (code == CONST_DECL)
    return;
  else if (code != VAR_DECL && code != PARM_DECL && code != RESULT_DECL
	   && code != TYPE_DECL && code != FIELD_DECL)
    abort ();

  if (type == error_mark_node)
    type = void_type_node;

  /* Usually the size and mode come from the data type without change.  */
  DECL_MODE (decl) = TYPE_MODE (type);
  TREE_UNSIGNED (decl) = TREE_UNSIGNED (type);
  if (DECL_SIZE (decl) == 0)
    {
      DECL_SIZE (decl) = TYPE_SIZE (type);
      DECL_SIZE_UNIT (decl) = TYPE_SIZE_UNIT (type);
    }
  else if (code == FIELD_DECL)
    {
      HOST_WIDE_INT spec_size;

      /* Size is specified in number of bits.  */
      spec_size = TREE_INT_CST_LOW (DECL_SIZE (decl));
      if (spec_size % BITS_PER_UNIT == 0)
	DECL_SIZE_UNIT (decl) = size_int (spec_size / BITS_PER_UNIT);
      else
	DECL_SIZE_UNIT (decl) = 0;
    }

  /* Force alignment required for the data type.
     But if the decl itself wants greater alignment, don't override that.
     Likewise, if the decl is packed, don't override it.  */
  if (!(code == FIELD_DECL && DECL_BIT_FIELD (decl))
      && (DECL_ALIGN (decl) == 0
	  || (! DECL_PACKED (decl) &&  TYPE_ALIGN (type) > DECL_ALIGN (decl))))
    DECL_ALIGN (decl) = TYPE_ALIGN (type);

  /* See if we can use an ordinary integer mode for a bit-field. 
     Conditions are: a fixed size that is correct for another mode
     and occupying a complete byte or bytes on proper boundary.  */
  if (code == FIELD_DECL)
    {
      DECL_BIT_FIELD_TYPE (decl) = DECL_BIT_FIELD (decl) ? type : 0;
      if (maximum_field_alignment != 0)
	DECL_ALIGN (decl) = MIN (DECL_ALIGN (decl), maximum_field_alignment);
      else if (DECL_PACKED (decl))
	DECL_ALIGN (decl) = MIN (DECL_ALIGN (decl), BITS_PER_UNIT);
    }

  if (DECL_BIT_FIELD (decl)
      && TYPE_SIZE (type) != 0
      && TREE_CODE (TYPE_SIZE (type)) == INTEGER_CST
      && GET_MODE_CLASS (TYPE_MODE (type)) == MODE_INT)
    {
      register enum machine_mode xmode
	= mode_for_size_tree (DECL_SIZE (decl), MODE_INT, 1);

      if (xmode != BLKmode
	  && known_align % GET_MODE_ALIGNMENT (xmode) == 0)
	{
	  DECL_ALIGN (decl) = MAX (GET_MODE_ALIGNMENT (xmode),
				   DECL_ALIGN (decl));
	  DECL_MODE (decl) = xmode;
	  DECL_SIZE (decl) = bitsize_int (GET_MODE_BITSIZE (xmode));
	  DECL_SIZE_UNIT (decl) = size_int (GET_MODE_SIZE (xmode));
	  /* This no longer needs to be accessed as a bit field.  */
	  DECL_BIT_FIELD (decl) = 0;
	}
    }

  /* Turn off DECL_BIT_FIELD if we won't need it set.  */
  if (DECL_BIT_FIELD (decl) && TYPE_MODE (type) == BLKmode
      && known_align % TYPE_ALIGN (type) == 0
      && DECL_SIZE_UNIT (decl) != 0
      && DECL_ALIGN (decl) >= TYPE_ALIGN (type))
    DECL_BIT_FIELD (decl) = 0;

  /* Evaluate nonconstant size only once, either now or as soon as safe.  */
  if (DECL_SIZE (decl) != 0 && TREE_CODE (DECL_SIZE (decl)) != INTEGER_CST)
    DECL_SIZE (decl) = variable_size (DECL_SIZE (decl));
  if (DECL_SIZE_UNIT (decl) != 0
      && TREE_CODE (DECL_SIZE_UNIT (decl)) != INTEGER_CST)
    DECL_SIZE_UNIT (decl) = variable_size (DECL_SIZE_UNIT (decl));

  /* If requested, warn about definitions of large data objects.  */
  if (warn_larger_than
      && (TREE_CODE (decl) == VAR_DECL || TREE_CODE (decl) == PARM_DECL)
      && ! DECL_EXTERNAL (decl))
    {
      tree size = DECL_SIZE_UNIT (decl);

      if (size != 0 && TREE_CODE (size) == INTEGER_CST
	  && compare_tree_int (size, larger_than_size) > 0)
	{
	  unsigned int size_as_int = TREE_INT_CST_LOW (size);

	  if (compare_tree_int (size, size_as_int) == 0)
	    warning_with_decl (decl, "size of `%s' is %d bytes", size_as_int);
	  else
	    warning_with_decl (decl, "size of `%s' is larger than %d bytes",
			       larger_than_size);
	}
    }
}

/* Create a new record_layout_info for T, which may be a RECORD_TYPE,
   UNION_TYPE, or QUAL_UNION_TYPE.  It is the responsibility of the
   caller to call `free' for the storage the returned.  */

record_layout_info
new_record_layout_info (t)
     tree t;
{
  record_layout_info rli 
    = (record_layout_info) xcalloc (1, sizeof (struct record_layout_info_s));

  rli->t = t;
  /* If the type has a minimum specified alignment (via an attribute
     declaration, for example) use it -- otherwise, start with a
     one-byte alignment.  */
  rli->record_align = MAX (BITS_PER_UNIT, TYPE_ALIGN (t));
  rli->unpacked_align = rli->record_align;

#ifdef STRUCTURE_SIZE_BOUNDARY
  /* Packed structures don't need to have minimum size.  */
  if (! TYPE_PACKED (t))
    rli->record_align = MAX (rli->record_align, STRUCTURE_SIZE_BOUNDARY);
#endif

  return rli;
}

/* Like layout_field, but for unions.  */

static void
layout_union_field (rli, field)
     record_layout_info rli;
     tree field;
{
  tree dsize;
      
  /* This function should only be used for unions; use layout_field
     for RECORD_TYPEs.  */
  if (TREE_CODE (rli->t) != UNION_TYPE
      && TREE_CODE (rli->t) != QUAL_UNION_TYPE)
    abort ();

  /* By now, we should only be seeing FIELD_DECLs.  */
  if (TREE_CODE (field) != FIELD_DECL)
    abort ();

  layout_decl (field, 0);
  DECL_FIELD_BITPOS (field) = bitsize_int (0);

  /* Union must be at least as aligned as any field requires.  */
  rli->record_align = MAX (rli->record_align, DECL_ALIGN (field));

#ifdef PCC_BITFIELD_TYPE_MATTERS
  /* On the m88000, a bit field of declare type `int' forces the
     entire union to have `int' alignment.  */
  if (PCC_BITFIELD_TYPE_MATTERS && DECL_BIT_FIELD_TYPE (field))
    rli->record_align = MAX (rli->record_align, 
			     TYPE_ALIGN (TREE_TYPE (field)));
#endif

  dsize = DECL_SIZE (field);
  if (TREE_CODE (rli->t) == UNION_TYPE)
    {
      /* Set union_size to max (decl_size, union_size).  There are
	 more and less general ways to do this.  Use only CONST_SIZE
	 unless forced to use VAR_SIZE.  */

      if (TREE_CODE (dsize) == INTEGER_CST
	  && ! TREE_CONSTANT_OVERFLOW (dsize)
	  && TREE_INT_CST_HIGH (dsize) == 0)
	rli->const_size
	  = MAX (rli->const_size, TREE_INT_CST_LOW (dsize));
      else if (rli->var_size == 0)
	rli->var_size = dsize;
      else
	rli->var_size = size_binop (MAX_EXPR, rli->var_size, dsize);
    }
  else if (TREE_CODE (rli->t) == QUAL_UNION_TYPE)
    rli->var_size = fold (build (COND_EXPR, bitsizetype, 
				 DECL_QUALIFIER (field),
				 DECL_SIZE (field),
				 (rli->var_size
				  ? rli->var_size 
				  : bitsize_int (0))));
}

/* RLI contains information about the layout of a RECORD_TYPE.  FIELD
   is a FIELD_DECL to be added after those fields already present in
   T.  (FIELD is not actually added to the TYPE_FIELDS list here;
   callers that desire that behavior must manually perform that step.)  */

void
layout_field (rli, field)
     record_layout_info rli;
     tree field;
{
  /* The alignment required for FIELD.  */
  unsigned int desired_align;
  /* The alignment FIELD would have if we just dropped it into the
     record as it presently stands.  */
  unsigned int known_align;
  /* The type of this field.  */
  tree type = TREE_TYPE (field);
  /* The size of this field, in bits.  */
  tree dsize;

  /* If FIELD is static, then treat it like a separate variable, not
     really like a structure field.  If it is a FUNCTION_DECL, it's a
     method.  In both cases, all we do is lay out the decl, and we do
     it *after* the record is laid out.  */
  if (TREE_CODE (field) == VAR_DECL)
    {
      rli->pending_statics = tree_cons (NULL_TREE, field,
					rli->pending_statics);
      return;
    }
  /* Enumerators and enum types which are local to this class need not
     be laid out.  Likewise for initialized constant fields.  */
  else if (TREE_CODE (field) != FIELD_DECL)
    return;
  /* This function should only be used for records; use
     layout_union_field for unions.  */
  else if (TREE_CODE (rli->t) != RECORD_TYPE)
    {
      layout_union_field (rli, field);
      return;
    }

  /* Work out the known alignment so far.  */
  known_align = rli->var_size ? rli->var_align : rli->const_size;

  /* Lay out the field so we know what alignment it needs.  For a
     packed field, use the alignment as specified, disregarding what
     the type would want.  */
  if (DECL_PACKED (field))
    desired_align = DECL_ALIGN (field);
  layout_decl (field, known_align);
  if (! DECL_PACKED (field))
    desired_align = DECL_ALIGN (field);
  /* Some targets (i.e. VMS) limit struct field alignment
     to a lower boundary than alignment of variables.  */
#ifdef BIGGEST_FIELD_ALIGNMENT
  desired_align = MIN (desired_align, BIGGEST_FIELD_ALIGNMENT);
#endif
#ifdef ADJUST_FIELD_ALIGN
  desired_align = ADJUST_FIELD_ALIGN (field, desired_align);
#endif

  /* Record must have at least as much alignment as any field.
     Otherwise, the alignment of the field within the record is
     meaningless.  */
#ifdef PCC_BITFIELD_TYPE_MATTERS
  if (PCC_BITFIELD_TYPE_MATTERS && type != error_mark_node
      && DECL_BIT_FIELD_TYPE (field)
      && ! integer_zerop (TYPE_SIZE (type)))
    {
      /* For these machines, a zero-length field does not
	 affect the alignment of the structure as a whole.
	 It does, however, affect the alignment of the next field
	 within the structure.  */
      if (! integer_zerop (DECL_SIZE (field)))
	rli->record_align = MAX (rli->record_align, desired_align);
      else if (! DECL_PACKED (field))
	desired_align = TYPE_ALIGN (type);
      /* A named bit field of declared type `int'
	 forces the entire structure to have `int' alignment.  */
      if (DECL_NAME (field) != 0)
	{
	  unsigned int type_align = TYPE_ALIGN (type);

	  if (maximum_field_alignment != 0)
	    type_align = MIN (type_align, maximum_field_alignment);
	  else if (DECL_PACKED (field))
	    type_align = MIN (type_align, BITS_PER_UNIT);

	  rli->record_align = MAX (rli->record_align, type_align);
	  if (warn_packed)
	    rli->unpacked_align = MAX (rli->unpacked_align, 
				       TYPE_ALIGN (type));
	}
    }
  else
#endif
    {
      rli->record_align = MAX (rli->record_align, desired_align);
      if (warn_packed)
	rli->unpacked_align = MAX (rli->unpacked_align, TYPE_ALIGN (type));
    }

  if (warn_packed && DECL_PACKED (field))
    {
      if (rli->const_size % TYPE_ALIGN (type) == 0
	  || (rli->var_align % TYPE_ALIGN (type) == 0 
	      && rli->var_size != NULL_TREE))
	{
	  if (TYPE_ALIGN (type) > desired_align)
	    {
	      if (STRICT_ALIGNMENT)
		warning_with_decl (field, "packed attribute causes inefficient alignment for `%s'");
	      else
		warning_with_decl (field, "packed attribute is unnecessary for `%s'");
	    }
	}
      else
	rli->packed_maybe_necessary = 1;
    }

  /* Does this field automatically have alignment it needs by virtue
     of the fields that precede it and the record's own alignment?  */
  if (rli->const_size % desired_align != 0
      || (rli->var_align % desired_align != 0 
	  && rli->var_size != NULL_TREE))
    {
      /* No, we need to skip space before this field.
	 Bump the cumulative size to multiple of field alignment.  */

      if (warn_padded)
	warning_with_decl (field, "padding struct to align `%s'");

      if (rli->var_size == NULL_TREE || rli->var_align % desired_align == 0)
	rli->const_size
	  = CEIL (rli->const_size, desired_align) * desired_align;
      else
	{
	  if (rli->const_size > 0)
	    rli->var_size = size_binop (PLUS_EXPR, rli->var_size,
					bitsize_int (rli->const_size));
	  rli->const_size = 0;
	  rli->var_size = round_up (rli->var_size, desired_align);
	  rli->var_align = MIN (rli->var_align, desired_align);
	}
    }

#ifdef PCC_BITFIELD_TYPE_MATTERS
  if (PCC_BITFIELD_TYPE_MATTERS
      && TREE_CODE (field) == FIELD_DECL
      && type != error_mark_node
      && DECL_BIT_FIELD_TYPE (field)
      && !DECL_PACKED (field)
      && maximum_field_alignment == 0
      && !integer_zerop (DECL_SIZE (field)))
    {
      unsigned int type_align = TYPE_ALIGN (type);
      register tree dsize = DECL_SIZE (field);
      unsigned int field_size = TREE_INT_CST_LOW (dsize);

      /* A bit field may not span more units of alignment of its type
	 than its type itself.  Advance to next boundary if necessary.  */
      if (((rli->const_size + field_size + type_align - 1) / type_align
	   - rli->const_size / type_align)
	  > TREE_INT_CST_LOW (TYPE_SIZE (TREE_TYPE (field))) / type_align)
	rli->const_size = CEIL (rli->const_size, type_align) * type_align;
    }
#endif

  /* No existing machine description uses this parameter.  So I have
     made it in this aspect identical to PCC_BITFIELD_TYPE_MATTERS.  */
#ifdef BITFIELD_NBYTES_LIMITED
  if (BITFIELD_NBYTES_LIMITED
      && TREE_CODE (field) == FIELD_DECL
      && type != error_mark_node
      && DECL_BIT_FIELD_TYPE (field)
      && !DECL_PACKED (field)
      && !integer_zerop (DECL_SIZE (field)))
    {
      unsigned int type_align = TYPE_ALIGN (type);
      register tree dsize = DECL_SIZE (field);
      int field_size = TREE_INT_CST_LOW (dsize);

      if (maximum_field_alignment != 0)
	type_align = MIN (type_align, maximum_field_alignment);
      /* ??? This test is opposite the test in the containing if
	 statement, so this code is unreachable currently.  */
      else if (DECL_PACKED (field))
	type_align = MIN (type_align, BITS_PER_UNIT);

      /* A bit field may not span the unit of alignment of its type.
	 Advance to next boundary if necessary.  */
      /* ??? This code should match the code above for the
	 PCC_BITFIELD_TYPE_MATTERS case.  */
      if (rli->const_size / type_align
	  != (rli->const_size + field_size - 1) / type_align)
	rli->const_size = CEIL (rli->const_size, type_align) * type_align;
    }
#endif

  /* Size so far becomes the position of this field.  */

  if (rli->var_size && rli->const_size)
    DECL_FIELD_BITPOS (field)
      = size_binop (PLUS_EXPR, rli->var_size, bitsize_int (rli->const_size));
  else if (rli->var_size)
    DECL_FIELD_BITPOS (field) = rli->var_size;
  else
    {
      DECL_FIELD_BITPOS (field) = bitsize_int (rli->const_size);

      /* If this field ended up more aligned than we thought it
	 would be (we approximate this by seeing if its position
	 changed), lay out the field again; perhaps we can use an
	 integral mode for it now.  */
      if (known_align != rli->const_size)
	layout_decl (field, rli->const_size);
    }

  /* Now add size of this field to the size of the record.  */
  dsize = DECL_SIZE (field);

  /* This can happen when we have an invalid nested struct definition,
     such as struct j { struct j { int i; } }.  The error message is
     printed in finish_struct.  */
  if (dsize == 0)
    /* Do nothing.  */;
  else if (TREE_CODE (dsize) == INTEGER_CST
	   && ! TREE_CONSTANT_OVERFLOW (dsize)
	   && TREE_INT_CST_HIGH (dsize) == 0
	   && TREE_INT_CST_LOW (dsize) + rli->const_size >= rli->const_size)
    /* Use const_size if there's no overflow.  */
    rli->const_size += TREE_INT_CST_LOW (dsize);
  else
    {
      if (rli->var_size == NULL_TREE)
	rli->var_size = dsize;
      else
	rli->var_size = size_binop (PLUS_EXPR, rli->var_size, dsize);
    }
}

/* Assuming that all the fields have been laid out, this function uses
   RLI to compute the final TYPE_SIZE, TYPE_ALIGN, etc. for the type
   inidicated by RLI.  */

static void
finalize_record_size (rli)
     record_layout_info rli;
{
  /* Work out the total size and alignment of the record as one
     expression and store in the record type.  Round it up to a
     multiple of the record's alignment.  */
  if (rli->var_size == NULL_TREE)
    TYPE_SIZE (rli->t) = bitsize_int (rli->const_size);
  else
    {
      if (rli->const_size)
	rli->var_size = size_binop (PLUS_EXPR, rli->var_size,
				    bitsize_int (rli->const_size));
      TYPE_SIZE (rli->t) = rli->var_size;
    }

  /* Determine the desired alignment.  */
#ifdef ROUND_TYPE_ALIGN
  TYPE_ALIGN (rli->t) = ROUND_TYPE_ALIGN (rli->t, TYPE_ALIGN (rli->t),
					  rli->record_align);
#else
  TYPE_ALIGN (rli->t) = MAX (TYPE_ALIGN (rli->t), rli->record_align);
#endif

  /* Record the un-rounded size in the binfo node.  But first we check
     the size of TYPE_BINFO to make sure that BINFO_SIZE is available.  */
  if (TYPE_BINFO (rli->t) && TREE_VEC_LENGTH (TYPE_BINFO (rli->t)) > 6)
    {
      TYPE_BINFO_SIZE (rli->t) = TYPE_SIZE (rli->t);
      TYPE_BINFO_SIZE_UNIT (rli->t)
	= convert (sizetype,
		   size_binop (FLOOR_DIV_EXPR, TYPE_SIZE (rli->t),
			       bitsize_int (BITS_PER_UNIT)));
    }
  
  {
    tree unpadded_size = TYPE_SIZE (rli->t);

#ifdef ROUND_TYPE_SIZE
    TYPE_SIZE (rli->t) = ROUND_TYPE_SIZE (rli->t, TYPE_SIZE (rli->t),
					  TYPE_ALIGN (rli->t));
#else
    /* Round the size up to be a multiple of the required alignment */
    TYPE_SIZE (rli->t) = round_up (TYPE_SIZE (rli->t), TYPE_ALIGN (rli->t));
#endif

    if (warn_padded && rli->var_size == NULL_TREE
	&& simple_cst_equal (unpadded_size, TYPE_SIZE (rli->t)) == 0)
      warning ("padding struct size to alignment boundary");
  }
  
  if (warn_packed && TYPE_PACKED (rli->t) && !rli->packed_maybe_necessary
      && rli->var_size == NULL_TREE)
    {
      tree unpacked_size;

      TYPE_PACKED (rli->t) = 0;
#ifdef ROUND_TYPE_ALIGN
      rli->unpacked_align
	= ROUND_TYPE_ALIGN (rli->t, TYPE_ALIGN (rli->t), rli->unpacked_align);
#else
      rli->unpacked_align = MAX (TYPE_ALIGN (rli->t), rli->unpacked_align);
#endif
#ifdef ROUND_TYPE_SIZE
      unpacked_size = ROUND_TYPE_SIZE (rli->t, TYPE_SIZE (rli->t),
				       rli->unpacked_align);
#else
      unpacked_size = round_up (TYPE_SIZE (rli->t), rli->unpacked_align);
#endif

      if (simple_cst_equal (unpacked_size, TYPE_SIZE (rli->t)))
	{
	  if (TYPE_NAME (rli->t))
	    {
	      char *name;

	      if (TREE_CODE (TYPE_NAME (rli->t)) == IDENTIFIER_NODE)
		name = IDENTIFIER_POINTER (TYPE_NAME (rli->t));
	      else
		name = IDENTIFIER_POINTER (DECL_NAME (TYPE_NAME (rli->t)));
	      if (STRICT_ALIGNMENT)
		warning ("packed attribute causes inefficient alignment for `%s'", name);
	      else
		warning ("packed attribute is unnecessary for `%s'", name);
	    }
	  else
	    {
	      if (STRICT_ALIGNMENT)
		warning ("packed attribute causes inefficient alignment");
	      else
		warning ("packed attribute is unnecessary");
	    }
	}
      TYPE_PACKED (rli->t) = 1;
    }
}

/* Compute the TYPE_MODE for the TYPE (which is a RECORD_TYPE).  */

static void
compute_record_mode (type)
     tree type;
{
  /* Most RECORD_TYPEs have BLKmode, so we start off assuming that.
     However, if possible, we use a mode that fits in a register
     instead, in order to allow for better optimization down the
     line.  */
  TYPE_MODE (type) = BLKmode;
  if (TREE_CODE (TYPE_SIZE (type)) == INTEGER_CST)
    {
      tree field;
      enum machine_mode mode = VOIDmode;

      /* A record which has any BLKmode members must itself be
	 BLKmode; it can't go in a register.  Unless the member is
	 BLKmode only because it isn't aligned.  */
      for (field = TYPE_FIELDS (type); field; field = TREE_CHAIN (field))
	{
	  unsigned HOST_WIDE_INT bitpos;

	  if (TREE_CODE (field) != FIELD_DECL
	      || TREE_CODE (TREE_TYPE (field)) == ERROR_MARK)
	    continue;

	  if (TYPE_MODE (TREE_TYPE (field)) == BLKmode
	      && ! TYPE_NO_FORCE_BLK (TREE_TYPE (field)))
	    return;

	  if (TREE_CODE (DECL_FIELD_BITPOS (field)) != INTEGER_CST)
	    return;

	  bitpos = TREE_INT_CST_LOW (DECL_FIELD_BITPOS (field));

	  /* Must be BLKmode if any field crosses a word boundary,
	     since extract_bit_field can't handle that in registers.  */
	  if (bitpos / BITS_PER_WORD
	      != ((TREE_INT_CST_LOW (DECL_SIZE (field)) + bitpos - 1)
		  / BITS_PER_WORD)
	      /* But there is no problem if the field is entire words.  */
	      && TREE_INT_CST_LOW (DECL_SIZE (field)) % BITS_PER_WORD != 0)
	    return;

	  /* If this field is the whole struct, remember its mode so
	     that, say, we can put a double in a class into a DF
	     register instead of forcing it to live in the stack.  */
	  if (simple_cst_equal (TYPE_SIZE (type), DECL_SIZE (field)))
	    mode = DECL_MODE (field);

#ifdef STRUCT_FORCE_BLK
	  /* With some targets, eg. c4x, it is sub-optimal
	     to access an aligned BLKmode structure as a scalar.  */
	  if (mode == VOIDmode && STRUCT_FORCE_BLK (field))
	    return;
#endif /* STRUCT_FORCE_BLK  */
	}

      if (mode != VOIDmode)
	/* We only have one real field; use its mode.  */
	TYPE_MODE (type) = mode;
      else
	TYPE_MODE (type)
	  = mode_for_size_tree (TYPE_SIZE (type), MODE_INT, 1);

      /* If structure's known alignment is less than what the scalar
	 mode would need, and it matters, then stick with BLKmode.  */
      if (TYPE_MODE (type) != BLKmode
	  && STRICT_ALIGNMENT
	  && ! (TYPE_ALIGN (type) >= BIGGEST_ALIGNMENT
		|| (TYPE_ALIGN (type) >=
		    GET_MODE_ALIGNMENT (TYPE_MODE (type)))))
	{
	  /* If this is the only reason this type is BLKmode, then
	     don't force containing types to be BLKmode.  */
	  TYPE_NO_FORCE_BLK (type) = 1;
	  TYPE_MODE (type) = BLKmode;
	}
    }
}

/* Compute TYPE_SIZE and TYPE_ALIGN for TYPE, once it has been laid
   out.  */

static void
finalize_type_size (type)
     tree type;
{
  /* Normally, use the alignment corresponding to the mode chosen.
     However, where strict alignment is not required, avoid
     over-aligning structures, since most compilers do not do this
     alignment.  */

  if (TYPE_MODE (type) != BLKmode && TYPE_MODE (type) != VOIDmode
      && (STRICT_ALIGNMENT
	  || (TREE_CODE (type) != RECORD_TYPE && TREE_CODE (type) != UNION_TYPE
	      && TREE_CODE (type) != QUAL_UNION_TYPE
	      && TREE_CODE (type) != ARRAY_TYPE)))
    TYPE_ALIGN (type) = GET_MODE_ALIGNMENT (TYPE_MODE (type));

  /* Do machine-dependent extra alignment.  */
#ifdef ROUND_TYPE_ALIGN
  TYPE_ALIGN (type)
    = ROUND_TYPE_ALIGN (type, TYPE_ALIGN (type), BITS_PER_UNIT);
#endif

#ifdef ROUND_TYPE_SIZE
  if (TYPE_SIZE (type) != 0)
    TYPE_SIZE (type)
      = ROUND_TYPE_SIZE (type, TYPE_SIZE (type), TYPE_ALIGN (type));
#endif

  /* Evaluate nonconstant size only once, either now or as soon as safe.  */
  if (TYPE_SIZE (type) != 0 && TREE_CODE (TYPE_SIZE (type)) != INTEGER_CST)
    TYPE_SIZE (type) = variable_size (TYPE_SIZE (type));

  /* If we failed to find a simple way to calculate the unit size
     of the type above, find it by division.  */
  if (TYPE_SIZE_UNIT (type) == 0 && TYPE_SIZE (type) != 0)
    /* TYPE_SIZE (type) is computed in bitsizetype.  After the division, the
       result will fit in sizetype.  We will get more efficient code using
       sizetype, so we force a conversion.  */
    TYPE_SIZE_UNIT (type)
      = convert (sizetype,
		 size_binop (FLOOR_DIV_EXPR, TYPE_SIZE (type),
			     bitsize_int (BITS_PER_UNIT)));

  /* Once again evaluate only once, either now or as soon as safe.  */
  if (TYPE_SIZE_UNIT (type) != 0
      && TREE_CODE (TYPE_SIZE_UNIT (type)) != INTEGER_CST)
    TYPE_SIZE_UNIT (type) = variable_size (TYPE_SIZE_UNIT (type));

  /* Also layout any other variants of the type.  */
  if (TYPE_NEXT_VARIANT (type)
      || type != TYPE_MAIN_VARIANT (type))
    {
      tree variant;
      /* Record layout info of this variant.  */
      tree size = TYPE_SIZE (type);
      tree size_unit = TYPE_SIZE_UNIT (type);
      unsigned int align = TYPE_ALIGN (type);
      enum machine_mode mode = TYPE_MODE (type);

      /* Copy it into all variants.  */
      for (variant = TYPE_MAIN_VARIANT (type);
	   variant != 0;
	   variant = TYPE_NEXT_VARIANT (variant))
	{
	  TYPE_SIZE (variant) = size;
	  TYPE_SIZE_UNIT (variant) = size_unit;
	  TYPE_ALIGN (variant) = align;
	  TYPE_MODE (variant) = mode;
	}
    }
}

/* Do all of the work required to layout the type indicated by RLI,
   once the fields have been laid out.  This function will call `free'
   for RLI.  */

void
finish_record_layout (rli)
     record_layout_info rli;
{
  /* Use finish_union_layout for unions.  */
  if (TREE_CODE (rli->t) != RECORD_TYPE)
    finish_union_layout (rli);
  else
    {
      /* Compute the final size.  */
      finalize_record_size (rli);
      /* Compute the TYPE_MODE for the record.  */
      compute_record_mode (rli->t);
    }

  /* Lay out any static members.  This is done now because their type
     may use the record's type.  */
  while (rli->pending_statics)
    {
      layout_decl (TREE_VALUE (rli->pending_statics), 0);
      rli->pending_statics = TREE_CHAIN (rli->pending_statics);
    }

  /* Perform any last tweaks to the TYPE_SIZE, etc.  */
  finalize_type_size (rli->t);
  /* Clean up.  */
  free (rli);
}

/* Like finish_record_layout, but for unions.  */

static void
finish_union_layout (rli)
     record_layout_info rli;
{
  /* This function should only be used for unions; use
     finish_record_layout for RECORD_TYPEs.  */
  if (TREE_CODE (rli->t) != UNION_TYPE
      && TREE_CODE (rli->t) != QUAL_UNION_TYPE)
    abort ();

  /* Determine the ultimate size of the union (in bytes).  */
  if (NULL == rli->var_size)
    TYPE_SIZE (rli->t)
      = bitsize_int (CEIL (rli->const_size, BITS_PER_UNIT) * BITS_PER_UNIT);

  else if (rli->const_size == 0)
    TYPE_SIZE (rli->t) = rli->var_size;
  else
    TYPE_SIZE (rli->t) = size_binop (MAX_EXPR, rli->var_size,
				     round_up (bitsize_int (rli->const_size),
					       BITS_PER_UNIT));

  /* Determine the desired alignment.  */
#ifdef ROUND_TYPE_ALIGN
  TYPE_ALIGN (rli->t) = ROUND_TYPE_ALIGN (rli->t, TYPE_ALIGN (rli->t), 
					  rli->record_align);
#else
  TYPE_ALIGN (rli->t) = MAX (TYPE_ALIGN (rli->t), rli->record_align);
#endif

#ifdef ROUND_TYPE_SIZE
  TYPE_SIZE (rli->t) = ROUND_TYPE_SIZE (rli->t, TYPE_SIZE (rli->t), 
					TYPE_ALIGN (rli->t));
#else
  /* Round the size up to be a multiple of the required alignment */
  TYPE_SIZE (rli->t) = round_up (TYPE_SIZE (rli->t), 
				 TYPE_ALIGN (rli->t));
#endif

  TYPE_MODE (rli->t) = BLKmode;
  if (TREE_CODE (TYPE_SIZE (rli->t)) == INTEGER_CST
      /* If structure's known alignment is less than
	 what the scalar mode would need, and it matters,
	 then stick with BLKmode.  */
      && (! STRICT_ALIGNMENT
	  || TYPE_ALIGN (rli->t) >= BIGGEST_ALIGNMENT
	  || compare_tree_int (TYPE_SIZE (rli->t), 
			       TYPE_ALIGN (rli->t)) <= 0))
    {
      tree field;

      /* A union which has any BLKmode members must itself be BLKmode;
	 it can't go in a register.
	 Unless the member is BLKmode only because it isn't aligned.  */
      for (field = TYPE_FIELDS (rli->t); 
	   field; 
	   field = TREE_CHAIN (field))
	{
	  if (TREE_CODE (field) != FIELD_DECL)
	    continue;

	  if (TYPE_MODE (TREE_TYPE (field)) == BLKmode
	      && ! TYPE_NO_FORCE_BLK (TREE_TYPE (field)))
	    return;
	}

      TYPE_MODE (rli->t) 
	= mode_for_size_tree (TYPE_SIZE (rli->t), MODE_INT, 1);
    }
}


/* Calculate the mode, size, and alignment for TYPE.
   For an array type, calculate the element separation as well.
   Record TYPE on the chain of permanent or temporary types
   so that dbxout will find out about it.

   TYPE_SIZE of a type is nonzero if the type has been laid out already.
   layout_type does nothing on such a type.

   If the type is incomplete, its TYPE_SIZE remains zero.  */

void
layout_type (type)
     tree type;
{
  int old;

  if (type == 0)
    abort ();

  /* Do nothing if type has been laid out before.  */
  if (TYPE_SIZE (type))
    return;

  /* Make sure all nodes we allocate are not momentary; they must last
     past the current statement.  */
  old = suspend_momentary ();

  /* Put all our nodes into the same obstack as the type.  Also,
     make expressions saveable (this is a no-op for permanent types).  */

  push_obstacks (TYPE_OBSTACK (type), TYPE_OBSTACK (type));
  saveable_allocation ();

  switch (TREE_CODE (type))
    {
    case LANG_TYPE:
      /* This kind of type is the responsibility
	 of the language-specific code.  */
      abort ();

    case BOOLEAN_TYPE:  /* Used for Java, Pascal, and Chill. */
      if (TYPE_PRECISION (type) == 0)
	TYPE_PRECISION (type) = 1; /* default to one byte/boolean. */

      /* ... fall through ... */

    case INTEGER_TYPE:
    case ENUMERAL_TYPE:
    case CHAR_TYPE:
      if (TREE_CODE (TYPE_MIN_VALUE (type)) == INTEGER_CST
	  && tree_int_cst_sgn (TYPE_MIN_VALUE (type)) >= 0)
	TREE_UNSIGNED (type) = 1;

      TYPE_MODE (type) = smallest_mode_for_size (TYPE_PRECISION (type),
						 MODE_INT);
      TYPE_SIZE (type) = bitsize_int (GET_MODE_BITSIZE (TYPE_MODE (type)));
      TYPE_SIZE_UNIT (type) = size_int (GET_MODE_SIZE (TYPE_MODE (type)));
      break;

    case REAL_TYPE:
      TYPE_MODE (type) = mode_for_size (TYPE_PRECISION (type), MODE_FLOAT, 0);
      TYPE_SIZE (type) = bitsize_int (GET_MODE_BITSIZE (TYPE_MODE (type)));
      TYPE_SIZE_UNIT (type) = size_int (GET_MODE_SIZE (TYPE_MODE (type)));
      break;

    case COMPLEX_TYPE:
      TREE_UNSIGNED (type) = TREE_UNSIGNED (TREE_TYPE (type));
      TYPE_MODE (type)
	= mode_for_size (2 * TYPE_PRECISION (TREE_TYPE (type)),
			 (TREE_CODE (TREE_TYPE (type)) == INTEGER_TYPE
			  ? MODE_COMPLEX_INT : MODE_COMPLEX_FLOAT),
			 0);
      TYPE_SIZE (type) = bitsize_int (GET_MODE_BITSIZE (TYPE_MODE (type)));
      TYPE_SIZE_UNIT (type) = size_int (GET_MODE_SIZE (TYPE_MODE (type)));
      break;

    case VOID_TYPE:
      /* VOID_TYPE is an incompletable type, it has no size */
      TYPE_SIZE_UNIT (type) = size_zero_node;
      TYPE_ALIGN (type) = 1;
      TYPE_MODE (type) = VOIDmode;
      break;

    case OFFSET_TYPE:
      TYPE_SIZE (type) = bitsize_int (POINTER_SIZE);
      TYPE_SIZE_UNIT (type) = size_int (POINTER_SIZE / BITS_PER_UNIT);
      TYPE_MODE (type) = ptr_mode;
      break;

    case FUNCTION_TYPE:
    case METHOD_TYPE:
      TYPE_MODE (type) = mode_for_size (2 * POINTER_SIZE, MODE_INT, 0);
      TYPE_SIZE (type) = bitsize_int (2 * POINTER_SIZE);
      TYPE_SIZE_UNIT (type) = size_int ((2 * POINTER_SIZE) / BITS_PER_UNIT);
      break;

    case POINTER_TYPE:
    case REFERENCE_TYPE:
      TYPE_MODE (type) = ptr_mode;
      TYPE_SIZE (type) = bitsize_int (POINTER_SIZE);
      TYPE_SIZE_UNIT (type) = size_int (POINTER_SIZE / BITS_PER_UNIT);
      TREE_UNSIGNED (type) = 1;
      TYPE_PRECISION (type) = POINTER_SIZE;
      break;

    case ARRAY_TYPE:
      {
	register tree index = TYPE_DOMAIN (type);
	register tree element = TREE_TYPE (type);

	build_pointer_type (element);

	/* We need to know both bounds in order to compute the size.  */
	if (index && TYPE_MAX_VALUE (index) && TYPE_MIN_VALUE (index)
	    && TYPE_SIZE (element))
	  {
	    tree ub = TYPE_MAX_VALUE (index);
	    tree lb = TYPE_MIN_VALUE (index);
	    tree length;
	    tree element_size;

	    /* If UB is max (lb - 1, x), remove the MAX_EXPR since the
	       test for negative below covers it.  */
	    if (TREE_CODE (ub) == MAX_EXPR
		&& TREE_CODE (TREE_OPERAND (ub, 0)) == MINUS_EXPR
		&& integer_onep (TREE_OPERAND (TREE_OPERAND (ub, 0), 1))
		&& operand_equal_p (TREE_OPERAND (TREE_OPERAND (ub, 0), 0),
				    lb, 0))
	      ub = TREE_OPERAND (ub, 1);
	    else if (TREE_CODE (ub) == MAX_EXPR
		     && TREE_CODE (TREE_OPERAND (ub, 1)) == MINUS_EXPR
		     && integer_onep (TREE_OPERAND (TREE_OPERAND (ub, 1), 1))
		     && operand_equal_p (TREE_OPERAND (TREE_OPERAND (ub, 1),
						       0),
					 lb, 0))
	      ub = TREE_OPERAND (ub, 0);

	    /* The initial subtraction should happen in the original type so
	       that (possible) negative values are handled appropriately.  */
	    length = size_binop (PLUS_EXPR, size_one_node,
				 convert (sizetype,
					  fold (build (MINUS_EXPR,
						       TREE_TYPE (lb),
						       ub, lb))));

	    /* If neither bound is a constant and sizetype is signed, make
	       sure the size is never negative.  We should really do this
	       if *either* bound is non-constant, but this is the best
	       compromise between C and Ada.  */
	    if (! TREE_UNSIGNED (sizetype)
		&& TREE_CODE (TYPE_MIN_VALUE (index)) != INTEGER_CST
		&& TREE_CODE (TYPE_MAX_VALUE (index)) != INTEGER_CST)
	      length = size_binop (MAX_EXPR, length, size_zero_node);

	    /* Special handling for arrays of bits (for Chill).  */
	    element_size = TYPE_SIZE (element);
	    if (TYPE_PACKED (type) && INTEGRAL_TYPE_P (element))
	      {
		HOST_WIDE_INT maxvalue
		  = TREE_INT_CST_LOW (TYPE_MAX_VALUE (element));
		HOST_WIDE_INT minvalue
		  = TREE_INT_CST_LOW (TYPE_MIN_VALUE (element));

		if (maxvalue - minvalue == 1
		    && (maxvalue == 1 || maxvalue == 0))
		  element_size = integer_one_node;
	      }

	    TYPE_SIZE (type) = size_binop (MULT_EXPR, element_size,
					   convert (bitsizetype, length));

	    /* If we know the size of the element, calculate the total
	       size directly, rather than do some division thing below.
	       This optimization helps Fortran assumed-size arrays
	       (where the size of the array is determined at runtime)
	       substantially.
	       Note that we can't do this in the case where the size of
	       the elements is one bit since TYPE_SIZE_UNIT cannot be
	       set correctly in that case.  */
	    if (TYPE_SIZE_UNIT (element) != 0 && ! integer_onep (element_size))
	      TYPE_SIZE_UNIT (type)
		= size_binop (MULT_EXPR, TYPE_SIZE_UNIT (element), length);
	  }

	/* Now round the alignment and size,
	   using machine-dependent criteria if any.  */

#ifdef ROUND_TYPE_ALIGN
	TYPE_ALIGN (type)
	  = ROUND_TYPE_ALIGN (type, TYPE_ALIGN (element), BITS_PER_UNIT);
#else
	TYPE_ALIGN (type) = MAX (TYPE_ALIGN (element), BITS_PER_UNIT);
#endif

#ifdef ROUND_TYPE_SIZE
	if (TYPE_SIZE (type) != 0)
	  {
	    tree tmp
	      = ROUND_TYPE_SIZE (type, TYPE_SIZE (type), TYPE_ALIGN (type));

	    /* If the rounding changed the size of the type, remove any
	       pre-calculated TYPE_SIZE_UNIT.  */
	    if (simple_cst_equal (TYPE_SIZE (type), tmp) != 1)
	      TYPE_SIZE_UNIT (type) = NULL;

	    TYPE_SIZE (type) = tmp;
	  }
#endif

	TYPE_MODE (type) = BLKmode;
	if (TYPE_SIZE (type) != 0
	    /* BLKmode elements force BLKmode aggregate;
	       else extract/store fields may lose.  */
	    && (TYPE_MODE (TREE_TYPE (type)) != BLKmode
		|| TYPE_NO_FORCE_BLK (TREE_TYPE (type))))
	  {
	    TYPE_MODE (type)
	      = mode_for_size_tree (TYPE_SIZE (type), MODE_INT, 1);

	    if (TYPE_MODE (type) != BLKmode
		&& STRICT_ALIGNMENT && TYPE_ALIGN (type) < BIGGEST_ALIGNMENT
		&& TYPE_ALIGN (type) < GET_MODE_ALIGNMENT (TYPE_MODE (type))
		&& TYPE_MODE (type) != BLKmode)
	      {
		TYPE_NO_FORCE_BLK (type) = 1;
		TYPE_MODE (type) = BLKmode;
	      }
	  }
	break;
      }

    case RECORD_TYPE:
    case UNION_TYPE:
    case QUAL_UNION_TYPE:
      {
	tree field;
	record_layout_info rli;

	/* Initialize the layout information.  */
	rli = new_record_layout_info (type);
	/* If this is a QUAL_UNION_TYPE, we want to process the fields
	   in the reverse order in building the COND_EXPR that denotes
	   its size.  We reverse them again later.  */
	if (TREE_CODE (type) == QUAL_UNION_TYPE)
	  TYPE_FIELDS (type) = nreverse (TYPE_FIELDS (type));
	/* Layout all the fields.  */
	for (field = TYPE_FIELDS (type); field; field = TREE_CHAIN (field))
	  layout_field (rli, field);
	if (TREE_CODE (type) == QUAL_UNION_TYPE)
	  TYPE_FIELDS (type) = nreverse (TYPE_FIELDS (type));
	/* Finish laying out the record.  */
	finish_record_layout (rli);
      }
      break;

    case SET_TYPE:  /* Used by Chill and Pascal. */
      if (TREE_CODE (TYPE_MAX_VALUE (TYPE_DOMAIN (type))) != INTEGER_CST
	  || TREE_CODE (TYPE_MIN_VALUE (TYPE_DOMAIN (type))) != INTEGER_CST)
	abort();
      else
	{
#ifndef SET_WORD_SIZE
#define SET_WORD_SIZE BITS_PER_WORD
#endif
	  unsigned int alignment
	    = set_alignment ? set_alignment : SET_WORD_SIZE;
	  int size_in_bits
	    = (TREE_INT_CST_LOW (TYPE_MAX_VALUE (TYPE_DOMAIN (type)))
	       - TREE_INT_CST_LOW (TYPE_MIN_VALUE (TYPE_DOMAIN (type))) + 1);
	  int rounded_size
	    = ((size_in_bits + alignment - 1) / alignment) * alignment;

	  if (rounded_size > (int) alignment)
	    TYPE_MODE (type) = BLKmode;
	  else
	    TYPE_MODE (type) = mode_for_size (alignment, MODE_INT, 1);

	  TYPE_SIZE (type) = bitsize_int (rounded_size);
	  TYPE_SIZE_UNIT (type) = size_int (rounded_size / BITS_PER_UNIT);
	  TYPE_ALIGN (type) = alignment;
	  TYPE_PRECISION (type) = size_in_bits;
	}
      break;

    case FILE_TYPE:
      /* The size may vary in different languages, so the language front end
	 should fill in the size.  */
      TYPE_ALIGN (type) = BIGGEST_ALIGNMENT;
      TYPE_MODE  (type) = BLKmode;
      break;

    default:
      abort ();
    }

  /* Compute the final TYPE_SIZE, TYPE_ALIGN, etc. for TYPE.  For
     records and unions, finish_record_layout already called this
     function.  */
  if (TREE_CODE (type) != RECORD_TYPE 
      && TREE_CODE (type) != UNION_TYPE
      && TREE_CODE (type) != QUAL_UNION_TYPE)
    finalize_type_size (type);

  pop_obstacks ();
  resume_momentary (old);

  /* If this type is created before sizetype has been permanently set,
     record it so set_sizetype can fix it up.  */
  if (! sizetype_set)
    {
      TREE_CHAIN (type) = early_type_list;
      early_type_list = type;
    }
}

/* Create and return a type for signed integers of PRECISION bits.  */

tree
make_signed_type (precision)
     int precision;
{
  register tree type = make_node (INTEGER_TYPE);

  TYPE_PRECISION (type) = precision;

  fixup_signed_type (type);
  return type;
}

/* Create and return a type for unsigned integers of PRECISION bits.  */

tree
make_unsigned_type (precision)
     int precision;
{
  register tree type = make_node (INTEGER_TYPE);

  TYPE_PRECISION (type) = precision;

  fixup_unsigned_type (type);
  return type;
}

/* Initialize sizetype and bitsizetype to a reasonable and temporary
   value to enable integer types to be created.  */

void
initialize_sizetypes ()
{
  tree t = make_node (INTEGER_TYPE);

  /* Set this so we do something reasonable for the build_int_2 calls
     below.  */
  integer_type_node = t;

  TYPE_MODE (t) = SImode;
  TYPE_ALIGN (t) = GET_MODE_ALIGNMENT (SImode);
  TYPE_SIZE (t) = build_int_2 (GET_MODE_BITSIZE (SImode), 0);
  TYPE_SIZE_UNIT (t) = build_int_2 (GET_MODE_SIZE (SImode), 0);
  TREE_UNSIGNED (t) = 1;
  TYPE_PRECISION (t) = GET_MODE_BITSIZE (SImode);
  TYPE_MIN_VALUE (t) = build_int_2 (0, 0);

  /* 1000 avoids problems with possible overflow and is certainly
     larger than any size value we'd want to be storing.  */
  TYPE_MAX_VALUE (t) = build_int_2 (1000, 0);

  /* These two must be different nodes because of the caching done in
     size_int_wide.  */
  sizetype = t;
  bitsizetype = copy_node (t);
  integer_type_node = 0;
}

/* Set sizetype to TYPE, and initialize *sizetype accordingly.
   Also update the type of any standard type's sizes made so far.  */

void
set_sizetype (type)
     tree type;
{
  int oprecision = TYPE_PRECISION (type);
  /* The *bitsizetype types use a precision that avoids overflows when
     calculating signed sizes / offsets in bits.  However, when
     cross-compiling from a 32 bit to a 64 bit host, we are limited to 64 bit
     precision.  */
  int precision = MIN (oprecision + BITS_PER_UNIT_LOG + 1,
		       2 * HOST_BITS_PER_WIDE_INT);
  unsigned int i;
  tree t, next;

  if (sizetype_set)
    abort ();

  /* Make copies of nodes since we'll be setting TYPE_IS_SIZETYPE.  */
  sizetype = copy_node (type);
  TYPE_DOMAIN (sizetype) = type;
  bitsizetype = make_node (INTEGER_TYPE);
  TYPE_NAME (bitsizetype) = TYPE_NAME (type);
  TYPE_PRECISION (bitsizetype) = precision;

  if (TREE_UNSIGNED (type))
    fixup_unsigned_type (bitsizetype);
  else
    fixup_signed_type (bitsizetype);

  layout_type (bitsizetype);

  if (TREE_UNSIGNED (type))
    {
      usizetype = sizetype;
      ubitsizetype = bitsizetype;
      ssizetype = copy_node (make_signed_type (oprecision));
      sbitsizetype = copy_node (make_signed_type (precision));
    }
  else
    {
      ssizetype = sizetype;
      sbitsizetype = bitsizetype;
      usizetype = copy_node (make_unsigned_type (oprecision));
      ubitsizetype = copy_node (make_unsigned_type (precision));
    }

  TYPE_NAME (bitsizetype) = get_identifier ("bit_size_type");

  /* Show is a sizetype, is a main type, and has no pointers to it.  */
  for (i = 0; i < sizeof sizetype_tab / sizeof sizetype_tab[0]; i++)
    {
      TYPE_IS_SIZETYPE (sizetype_tab[i]) = 1;
      TYPE_MAIN_VARIANT (sizetype_tab[i]) = sizetype_tab[i];
      TYPE_NEXT_VARIANT (sizetype_tab[i]) = 0;
      TYPE_POINTER_TO (sizetype_tab[i]) = 0;
      TYPE_REFERENCE_TO (sizetype_tab[i]) = 0;
    }

  ggc_add_tree_root ((tree *) &sizetype_tab,
		     sizeof sizetype_tab / sizeof (tree));

  /* Go down each of the types we already made and set the proper type
     for the sizes in them.  */
  for (t = early_type_list; t != 0; t = next)
    {
      next = TREE_CHAIN (t);
      TREE_CHAIN (t) = 0;

      if (TREE_CODE (t) != INTEGER_TYPE)
	abort ();

      TREE_TYPE (TYPE_SIZE (t)) = bitsizetype;
      TREE_TYPE (TYPE_SIZE_UNIT (t)) = sizetype;
    }

  early_type_list = 0;
  sizetype_set = 1;
}

/* Set the extreme values of TYPE based on its precision in bits,
   then lay it out.  Used when make_signed_type won't do
   because the tree code is not INTEGER_TYPE.
   E.g. for Pascal, when the -fsigned-char option is given.  */

void
fixup_signed_type (type)
     tree type;
{
  register int precision = TYPE_PRECISION (type);

  TYPE_MIN_VALUE (type)
    = build_int_2 ((precision - HOST_BITS_PER_WIDE_INT > 0
		    ? 0 : (HOST_WIDE_INT) (-1) << (precision - 1)),
		   (((HOST_WIDE_INT) (-1)
		     << (precision - HOST_BITS_PER_WIDE_INT - 1 > 0
			 ? precision - HOST_BITS_PER_WIDE_INT - 1
			 : 0))));
  TYPE_MAX_VALUE (type)
    = build_int_2 ((precision - HOST_BITS_PER_WIDE_INT > 0
		    ? -1 : ((HOST_WIDE_INT) 1 << (precision - 1)) - 1),
		   (precision - HOST_BITS_PER_WIDE_INT - 1 > 0
		    ? (((HOST_WIDE_INT) 1
			<< (precision - HOST_BITS_PER_WIDE_INT - 1))) - 1
		    : 0));

  TREE_TYPE (TYPE_MIN_VALUE (type)) = type;
  TREE_TYPE (TYPE_MAX_VALUE (type)) = type;

  /* Lay out the type: set its alignment, size, etc.  */
  layout_type (type);
}

/* Set the extreme values of TYPE based on its precision in bits,
   then lay it out.  This is used both in `make_unsigned_type'
   and for enumeral types.  */

void
fixup_unsigned_type (type)
     tree type;
{
  register int precision = TYPE_PRECISION (type);

  TYPE_MIN_VALUE (type) = build_int_2 (0, 0);
  TYPE_MAX_VALUE (type)
    = build_int_2 (precision - HOST_BITS_PER_WIDE_INT >= 0
		   ? -1 : ((HOST_WIDE_INT) 1 << precision) - 1,
		   precision - HOST_BITS_PER_WIDE_INT > 0
		   ? ((unsigned HOST_WIDE_INT) ~0
		      >> (HOST_BITS_PER_WIDE_INT
			  - (precision - HOST_BITS_PER_WIDE_INT)))
		   : 0);
  TREE_TYPE (TYPE_MIN_VALUE (type)) = type;
  TREE_TYPE (TYPE_MAX_VALUE (type)) = type;

  /* Lay out the type: set its alignment, size, etc.  */
  layout_type (type);
}

/* Find the best machine mode to use when referencing a bit field of length
   BITSIZE bits starting at BITPOS.

   The underlying object is known to be aligned to a boundary of ALIGN bits.
   If LARGEST_MODE is not VOIDmode, it means that we should not use a mode
   larger than LARGEST_MODE (usually SImode).

   If no mode meets all these conditions, we return VOIDmode.  Otherwise, if
   VOLATILEP is true or SLOW_BYTE_ACCESS is false, we return the smallest
   mode meeting these conditions.

   Otherwise (VOLATILEP is false and SLOW_BYTE_ACCESS is true), we return
   the largest mode (but a mode no wider than UNITS_PER_WORD) that meets
   all the conditions.  */

enum machine_mode
get_best_mode (bitsize, bitpos, align, largest_mode, volatilep)
     int bitsize, bitpos;
     unsigned int align;
     enum machine_mode largest_mode;
     int volatilep;
{
  enum machine_mode mode;
  int unit = 0;

  /* Find the narrowest integer mode that contains the bit field.  */
  for (mode = GET_CLASS_NARROWEST_MODE (MODE_INT); mode != VOIDmode;
       mode = GET_MODE_WIDER_MODE (mode))
    {
      unit = GET_MODE_BITSIZE (mode);
      if ((bitpos % unit) + bitsize <= unit)
	break;
    }

  if (mode == VOIDmode
      /* It is tempting to omit the following line
	 if STRICT_ALIGNMENT is true.
	 But that is incorrect, since if the bitfield uses part of 3 bytes
	 and we use a 4-byte mode, we could get a spurious segv
	 if the extra 4th byte is past the end of memory.
	 (Though at least one Unix compiler ignores this problem:
	 that on the Sequent 386 machine.  */
      || MIN (unit, BIGGEST_ALIGNMENT) > (int) align
      || (largest_mode != VOIDmode && unit > GET_MODE_BITSIZE (largest_mode)))
    return VOIDmode;

  if (SLOW_BYTE_ACCESS && ! volatilep)
    {
      enum machine_mode wide_mode = VOIDmode, tmode;

      for (tmode = GET_CLASS_NARROWEST_MODE (MODE_INT); tmode != VOIDmode;
	   tmode = GET_MODE_WIDER_MODE (tmode))
	{
	  unit = GET_MODE_BITSIZE (tmode);
	  if (bitpos / unit == (bitpos + bitsize - 1) / unit
	      && unit <= BITS_PER_WORD
	      && unit <= (int) MIN (align, BIGGEST_ALIGNMENT)
	      && (largest_mode == VOIDmode
		  || unit <= GET_MODE_BITSIZE (largest_mode)))
	    wide_mode = tmode;
	}

      if (wide_mode != VOIDmode)
	return wide_mode;
    }

  return mode;
}

/* Return the alignment of MODE. This will be bounded by 1 and
   BIGGEST_ALIGNMENT.  */

unsigned int
get_mode_alignment (mode)
     enum machine_mode mode;
{
  unsigned alignment = GET_MODE_UNIT_SIZE (mode);
  
  /* Extract the LSB of the size.  */
  alignment = alignment & -alignment;
  
  alignment *= BITS_PER_UNIT;

  alignment = MIN (BIGGEST_ALIGNMENT, MAX (1, alignment));
  return alignment;
}

/* This function is run once to initialize stor-layout.c.  */

void
init_stor_layout_once ()
{
  ggc_add_tree_root (&pending_sizes, 1);
}
