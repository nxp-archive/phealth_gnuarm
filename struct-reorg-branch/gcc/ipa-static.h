/* Callgraph handling code.
   Copyright (C) 2004 Free Software Foundation, Inc.
   Contributed by Kenneth Zadeck <zadeck@naturalbridge.com>

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

#ifndef GCC_IPA_STATIC_H
#define GCC_IPA_STATIC_H
#include "bitmap.h"
#include "tree.h"

/* The static variables defined within the compilation unit that are
   loaded or stored directly by function that owns this structure.  */ 

struct ipa_local_static_vars_info_d 
{
  bitmap statics_read_by_decl_uid;
  bitmap statics_written_by_decl_uid;
  bitmap statics_read_by_ann_uid;
  bitmap statics_written_by_ann_uid;

  /* Var_anns_valid is reset at the start of compilation for each
     function because the indexing that the "_var_anns" is based
     on is invalidated between function compilations.  This allows for
     lazy creation of the "_var_ann" variables.  */
  bool var_anns_valid;
};

struct ipa_global_static_vars_info_d
{
  bitmap statics_read_by_decl_uid;
  bitmap statics_written_by_decl_uid;
  bitmap statics_read_by_ann_uid;
  bitmap statics_written_by_ann_uid;
  bitmap statics_not_read_by_decl_uid;
  bitmap statics_not_written_by_decl_uid;
  bitmap statics_not_read_by_ann_uid;
  bitmap statics_not_written_by_ann_uid;

  /* Var_anns_valid is reset at the start of compilation for each
     function because the indexing that the "_var_anns" is based
     on is invalidated between function compilations.  This allows for
     lazy creation of the "_var_ann" variables.  */
  bool var_anns_valid;
};

/* Statics that are read and written by some set of functions. The
   local ones are based on the loads and stores local to the function.
   The global ones are based on the local info as well as the
   transitive closure of the functions that are called.  The
   structures are separated to allow the global structures to be
   shared between several functions since every function within a
   strongly connected component will have the same information.  This
   sharing saves both time and space in the computation of the vectors
   as well as their translation from decl_uid form to ann_uid
   form.  */ 

typedef struct ipa_local_static_vars_info_d *ipa_local_static_vars_info_t;
typedef struct ipa_global_static_vars_info_d *ipa_global_static_vars_info_t;

struct ipa_static_vars_info_d 
{
  ipa_local_static_vars_info_t local;
  ipa_global_static_vars_info_t global;
};

typedef struct ipa_static_vars_info_d *ipa_static_vars_info_t;

/* The cgraph data structure.
   Each function decl has assigned cgraph_node listing callees and callers.  */

/* In ipa-static-vars-anal.c  */
extern tree memory_identifier_string;

void   ipa_static_reset_maps (void);
bitmap ipa_get_statics_read_local (tree fn);
bitmap ipa_get_statics_written_local (tree fn);
bitmap ipa_get_statics_read_global (tree fn);
bitmap ipa_get_statics_written_global (tree fn);
bitmap ipa_get_statics_not_read_global (tree fn);
bitmap ipa_get_statics_not_written_global (tree fn);

#endif  /* GCC_IPA_STATIC_H  */
