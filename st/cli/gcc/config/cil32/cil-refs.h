/* Database of entities referenced in a compilation unit.

   Copyright (C) 2006-2008 Free Software Foundation, Inc.

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
Software Foundation, 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301, USA.

Authors:
   Andrea Ornstein
   Erven Rohou
   Gabriele Svelto

Contact information at STMicroelectronics:
Andrea C. Ornstein      <andrea.ornstein@st.com>
Erven Rohou             <erven.rohou@st.com>
*/

#ifndef CIL_REFS_H
#define CIL_REFS_H

#include "coretypes.h"
#include "ggc.h"
#include "debug.h"
#include "hashtab.h"
#include "tree.h"
#include "cil-types.h"

/******************************************************************************
 * Macros                                                                     *
 ******************************************************************************/

/* Nonzero for a type which is at file scope.  */
#define TYPE_FILE_SCOPE_P(EXP)                                  \
  (! TYPE_CONTEXT (EXP)                                         \
   || TREE_CODE (TYPE_CONTEXT (EXP)) == TRANSLATION_UNIT_DECL)

/* Nonzero for a zero-length array type */
#define ARRAY_TYPE_ZEROLENGTH(EXP)                              \
  (TYPE_SIZE (EXP) == NULL_TREE)

/* Nonzero for a variable-length array type */
#define ARRAY_TYPE_VARLENGTH(EXP)                               \
  (TYPE_SIZE (EXP) != NULL_TREE && TREE_CODE (TYPE_SIZE (EXP)) != INTEGER_CST)

/* Length of compacted identifiers (in characters) */
#define COMPACT_ID_LENGTH 16

/*****************************************************************************
 * Initialization and teardown                                               *
 *****************************************************************************/

extern void refs_init (void);
extern void refs_fini (void);

/*****************************************************************************
 * Types                                                                     *
 *****************************************************************************/

extern void mark_referenced_type (tree);
extern htab_t referenced_types_htab ( void );

/******************************************************************************
 * Strings                                                                    *
 ******************************************************************************/

extern tree mark_referenced_string (tree);
extern unsigned int get_string_cst_id (tree);
extern htab_t referenced_strings_htab ( void );

/******************************************************************************
 * Functions                                                                  *
 ******************************************************************************/

extern void cil_add_pinvoke (tree);
extern htab_t pinvokes_htab ( void );

/******************************************************************************
 * Labels                                                                     *
 ******************************************************************************/

extern void record_addr_taken_label (tree);
extern tree get_addr_taken_label_id (tree);
extern tree get_label_addrs ( void );

/******************************************************************************
 * Constructors                                                               *
 ******************************************************************************/

extern void record_ctor (tree);
extern void create_init_method (void);
extern void expand_init_to_stmt_list (tree, tree, tree *);

#endif /* !CIL_REFS_H */
