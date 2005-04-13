/* Definitions of decimal floating-point access for GNU compiler.
   Copyright (C) 2005 Free Software Foundation, Inc.

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

#ifndef GCC_DFP_H
#define GCC_DFP_H

void encode_decimal32 (const struct real_format *fmt,
		       long *, const REAL_VALUE_TYPE *);
void decode_decimal32 (const struct real_format *,
		       REAL_VALUE_TYPE *, const long *);
void encode_decimal64 (const struct real_format *fmt,
		       long *, const REAL_VALUE_TYPE *);
void decode_decimal64 (const struct real_format *,
		       REAL_VALUE_TYPE *, const long *);
void encode_decimal128 (const struct real_format *fmt,
			long *, const REAL_VALUE_TYPE *);
void decode_decimal128 (const struct real_format *,
			REAL_VALUE_TYPE *, const long *);
void decimal_real_from_string (REAL_VALUE_TYPE *, const char *,
			       enum machine_mode );
int decimal_do_compare (const REAL_VALUE_TYPE *, const REAL_VALUE_TYPE *, int);
void decimal_round_for_format (const struct real_format *, REAL_VALUE_TYPE *);
void decimal_real_convert (REAL_VALUE_TYPE *r, enum machine_mode, 
			   const REAL_VALUE_TYPE *a);

#define ENCODE_DECIMAL_SINGLE encode_decimal32
#define DECODE_DECIMAL_SINGLE decode_decimal32
#define ENCODE_DECIMAL_DOUBLE encode_decimal64
#define DECODE_DECIMAL_DOUBLE decode_decimal64
#define ENCODE_DECIMAL_QUAD encode_decimal128
#define DECODE_DECIMAL_QUAD decode_decimal128
#define REAL_OR_DECIMAL_FROM_STRING decimal_real_from_string
#define DECIMAL_DO_COMPARE decimal_do_compare

#endif /* ! GCC_DFP_H */
