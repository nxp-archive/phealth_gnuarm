/* Basile's static analysis (should have a better name) run-basilys.h
   all include files for generated code
   
   Copyright 2008 Free Software Foundation, Inc.
   Contributed by Basile Starynkevitch <basile@starynkevitch.net>
   Indented with GNU indent 

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


/* usual GCC middle-end includes, copied from basilys.c */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "obstack.h"
#include "tm.h"
#include "tree.h"
#include "filenames.h"
#include "tree-pass.h"
#include "tree-dump.h"
#include "basic-block.h"
#include "timevar.h"
#include "errors.h"
#include "ggc.h"
#include "cgraph.h"
#include "diagnostic.h"
#include "flags.h"
#include "toplev.h"
#include "options.h"
#include "params.h"
#include "real.h"
#include "prefix.h"
#include "md5.h"
#include "cppdefault.h"


/* basilys or MELT specific includes */

#include "compiler-probe.h"


#if HAVE_PARMAPOLY
#include <ppl_c.h>
#else
#error required parma polyedral library PPL
#endif /*HAVE_PARMAPOLY */

#if HAVE_LIBTOOLDYNL
#include <ltdl.h>
#else
#error required libtool dynamic loader library LTDL
#endif /*HAVE_LIBTOOLDYNL */

#include "basilys.h"
