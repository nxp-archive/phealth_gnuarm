/*

   Copyright (C) 2008-2009 Free Software Foundation, Inc.
   Contributed by STMicroelectronics

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
02110-1301, USA.  */

#ifndef _LIBGEN_H
#define _LIBGEN_H

#include <__cdefs.h>

__BEGIN_EXTERN_C

LIBSTD_LPROTO(char *, dirname, char *path);
LIBSTD_LPROTO(char *, basename, char *path);

__END_EXTERN_C

#endif
