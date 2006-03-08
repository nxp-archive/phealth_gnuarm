/* Definitions of target machine for GNU compiler.  "naked" 68020.
   Copyright (C) 1994, 1996, 2003 Free Software Foundation, Inc.

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

/* Default to m68k (m68020).  */
#ifndef TARGET_CPU_DEFAULT
#define TARGET_CPU_DEFAULT TARGET_CPU_m68020
#define ASM_CPU_DEFAULT_SPEC "-mcpu=68020"
#endif

#ifndef TARGET_DEFAULT
#define TARGET_DEFAULT 0
#endif


#ifndef ASM_CPU_DEFAULT_SPEC
#define ASM_CPU_DEFAULT_SPEC ""
#endif

/* Handle --with-cpu, --with-float default options from configure script.  */
#define OPTION_DEFAULT_SPECS						\
  { "cpu",   "%{!mcpu=*:%{!march=*:-mcpu=%(VALUE)}}" },			\
  { "float", "%{!msoft-float:%{!mhard-float:%{!m68881:-m%(VALUE)-float}}}" },

/* Pass flags to gas indicating which type of processor we have.  */

#undef ASM_SPEC
#define ASM_SPEC "\
%{m68851}%{mno-68851}%{m68881}%{mno-68881}%{msoft-float:-mno-float}\
%{m68000}%{m68302}%{mc68000}%{m68010}%{m68020}%{mc68020}%{m68030}\
%{m68040}%{m68020-40:-m68040} %{m68020-60:-m68040}\
%{m68060}%{mcpu32}%{m68332}%{m5200}%{m5206e}%{m528x}%{m5307}%{m5407}%{mcfv4e}\
%{mcpu=*:-mcpu=%*}\
%{march=*:-march=%*}\
%{!mc68000:%{!m68000:%{!m68302:%{!m68010:%{!mc68020:%{!m68020:\
 %{!m68030:%{!m68040:%{!m68020-40:%{!m68020-60:%{!m68060:%{!mcpu32:\
 %{!m68332:%{!m5200:%{!m5206e:%{!m528x:%{!m5307:%{!m5407:%{!mcfv4e:\
 %{!mcpu=*:%{!march=*:%(asm_cpu_default)}}}}}}}}}}}}}}}}}}}}} \
%{fPIC:--pcrel} %{fpic:--pcrel} %{msep-data:--pcrel}\
%{mid-shared-library:--pcrel}"

/* cc1/cc1plus always receives all the -m flags. If the specs strings above 
   are consistent with the flags in m68k.opt, there should be no need for
   any further cc1/cc1plus specs.  */

#undef CC1_SPEC
#define CC1_SPEC ""

/* This macro defines names of additional specifications to put in the specs
   that can be used in various specifications like CC1_SPEC.  Its definition
   is an initializer with a subgrouping for each command option.

   Each subgrouping contains a string constant, that defines the
   specification name, and a string constant that used by the GCC driver
   program.

   Do not define this macro if it does not need to do anything.  */

#define EXTRA_SPECS					\
  { "asm_cpu_default",	ASM_CPU_DEFAULT_SPEC },		\
  SUBTARGET_EXTRA_SPECS

#define CPP_SUBTARGET_SPEC ""
#define SUBTARGET_EXTRA_SPECS

/* Avoid building multilib libraries for the defaults.
   For targets not handled here, just build the full set of multilibs.
   The default is m68k 99.9% of the time anyway.  */

/* FIXME: Re-implement behaviour described above.

#define MULTILIB_DEFAULTS ...  */
