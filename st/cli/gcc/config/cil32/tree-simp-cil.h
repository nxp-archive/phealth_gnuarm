/* CIL simplification definitions for GNU compiler.

   Copyright (C) 2006 Free Software Foundation, Inc.

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
   Andrea Bona
   Andrea Ornstein
   Erven Rohou
   Roberto Costa

Contact information at STMicroelectronics:
Andrea C. Ornstein      <andrea.ornstein@st.com>
Erven Rohou             <erven.rohou@st.com>
*/

#ifndef TREE_SIMP_CIL_H
#define TREE_SIMP_CIL_H

#include "tree.h"

extern void expand_init_to_stmt_list (tree, tree, tree *);
extern tree get_integer_type (int, bool);

#endif  /* TREE_SIMP_CIL_H */
