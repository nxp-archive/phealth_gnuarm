/* Functions related to the Boehm garbage collector.
   Copyright (C) 2000 Free Software Foundation, Inc.

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
Boston, MA 02111-1307, USA.

Java and all Java-based marks are trademarks or registered trademarks
of Sun Microsystems, Inc. in the United States and other countries.
The Free Software Foundation is independent of Sun Microsystems, Inc.  */

/* Written by Tom Tromey <tromey@cygnus.com>.  */

#include <config.h>

#include "system.h"
#include "tree.h"
#include "java-tree.h"
#include "parse.h"

/* Compute a procedure-based object descriptor.  We know that our
   `kind' is 0, and `env' is likewise 0, so we have a simple
   computation.  From the GC sources:
	    (((((env) << LOG_MAX_MARK_PROCS) | (proc_index)) << DS_TAG_BITS) \
	    | DS_PROC)
   Here DS_PROC == 2.  */
#define PROCEDURE_OBJECT_DESCRIPTOR integer_two_node

/* Treat two HOST_WIDE_INT's as a contiguous bitmap, with bit 0 being
   the least significant.  This function sets bit N in the bitmap.  */
static void
set_bit (unsigned HOST_WIDE_INT *low, unsigned HOST_WIDE_INT *high,
	 unsigned int n)
{
  HOST_WIDE_INT *which;

  if (n >= HOST_BITS_PER_WIDE_INT)
    {
      n -= HOST_BITS_PER_WIDE_INT;
      which = high;
    }
  else
    which = low;

  *which |= (HOST_WIDE_INT) 1 << n;
}

/* Recursively mark reference fields.  */
static unsigned int
mark_reference_fields (field, low, high, ubit,
		       pointer_after_end, all_bits_set, last_set_index)
     tree field;
     unsigned HOST_WIDE_INT *low, *high;
     unsigned int ubit;
     int *pointer_after_end, *all_bits_set, *last_set_index;
{
  unsigned int count = 0;

  /* See if we have fields from our superclass.  */
  if (DECL_NAME (field) == NULL_TREE)
    {
      count += mark_reference_fields (TYPE_FIELDS (TREE_TYPE (field)),
				      low, high, ubit,
				      pointer_after_end, all_bits_set,
				      last_set_index);
      field = TREE_CHAIN (field);
    }

  for (; field != NULL_TREE; field = TREE_CHAIN (field))
    {
      if (FIELD_STATIC (field))
	continue;

      if (JREFERENCE_TYPE_P (TREE_TYPE (field)))
	{
	  *last_set_index = count;
	  /* First word in object corresponds to most significant byte
	     of bitmap.  */
	  set_bit (low, high, ubit - count - 1);
	  if (count > ubit - 2)
	    *pointer_after_end = 1;
	}
      else
	*all_bits_set = 0;

      ++count;
    }

  return count;
}

/* Return the marking bitmap for the class TYPE.  For now this is a
   single word describing the type.  */
tree
get_boehm_type_descriptor (tree type)
{
  unsigned int count, log2_size, ubit;
  int bit;
  int all_bits_set = 1;
  int last_set_index = 0;
  int pointer_after_end = 0;
  unsigned HOST_WIDE_INT low = 0, high = 0;
  tree field, value;

  /* If the GC wasn't requested, just use a null pointer.  */
  if (! flag_use_boehm_gc)
    return null_pointer_node;

  /* If we have a type of unknown size, use a proc.  */
  if (int_size_in_bytes (type) == -1)
    return PROCEDURE_OBJECT_DESCRIPTOR;

  bit = POINTER_SIZE / BITS_PER_UNIT;
  /* The size of this node has to be known.  And, we only support 32
     and 64 bit targets, so we need to know that the log2 is one of
     our values.  */
  log2_size = exact_log2 (bit);
  if (bit == -1 || (log2_size != 2 && log2_size != 3))
    {
      /* This means the GC isn't supported.  We should probably
	 abort or give an error.  Instead, for now, we just silently
	 revert.  FIXME.  */
      return null_pointer_node;
    }
  bit *= BITS_PER_UNIT;

  /* Warning avoidance.  */
  ubit = (unsigned int) bit;

  field = TYPE_FIELDS (type);
  count = mark_reference_fields (field, &low, &high, ubit,
				 &pointer_after_end, &all_bits_set,
				 &last_set_index);

  /* If the object is all pointers, or if the part with pointers fits
     in our bitmap, then we are ok.  Otherwise we have to allocate it
     a different way.  */
  if (all_bits_set)
    {
      /* In the GC the computation looks something like this:
	 value = DS_LENGTH | WORDS_TO_BYTES (last_set_index + 1);
	 DS_LENGTH is 0.
	 WORDS_TO_BYTES shifts by log2(bytes-per-pointer).  */
      count = 0;
      low = 0;
      high = 0;
      ++last_set_index;
      while (last_set_index)
	{
	  if ((last_set_index & 1))
	    set_bit (&low, &high, log2_size + count);
	  last_set_index >>= 1;
	  ++count;
	}
      value = build_int_2 (low, high);
    }
  else if (! pointer_after_end)
    {
      /* Bottom two bits for bitmap mark type are 01.  */
      set_bit (&low, &high, 0);
      value = build_int_2 (low, high);
    }
  else
    value = PROCEDURE_OBJECT_DESCRIPTOR;

  return value;
}
