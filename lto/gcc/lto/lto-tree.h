/* Language-dependent trees for LTO.
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

#ifndef GCC_LTO_TREE_H
#define GCC_LTO_TREE_H

struct lang_identifier GTY(())
{
  struct tree_identifier base;
  /* LTO_IDENTIFIER_DECL */
  tree decl;
};

struct lang_decl GTY(())
{
};

struct lang_type GTY(())
{
};

struct language_function GTY(())
{
};

enum lto_tree_node_structure_enum {
  TS_LTO_GENERIC
};

union lang_tree_node GTY(
 (desc ("lto_tree_node_structure (&%h)"),
  chain_next ("(union lang_tree_node *)TREE_CHAIN (&%h.generic)")))
{
  union tree_node GTY ((tag ("TS_LTO_GENERIC"),
			desc ("tree_node_structure (&%h)"))) generic;
};

/* Return NODE (an IDENTIFIER_NODE) as a pointer to a
   "lang_identifier".  */
#define LANG_IDENTIFIER_CAST(NODE) \
  ((struct lang_identifier*)IDENTIFIER_NODE_CHECK (NODE))

/* Return the VAR_DECL or FUNCTION_DECL with external linkage whose
   DECL_ASSEMBLER_NAME is NODE, or NULL_TREE if there is no such
   declaration.  */ 
#define LTO_IDENTIFIER_DECL(NODE)		\
  (LANG_IDENTIFIER_CAST (NODE)->decl)

#endif /* GCC_LTO_TREE_H */
