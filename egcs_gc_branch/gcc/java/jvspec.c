 /* Specific flags and argument handling of the front-end of the 
   GNU compiler for the Java(TM) language.
   Copyright (C) 1996, 1997, 1998, 1999 Free Software Foundation, Inc.

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

#include "config.h"

#include "system.h"

#include "gansidecl.h"

#if defined (WITH_THREAD_posix) || defined (WITH_THREAD_pthreads)
#define THREAD_NAME "-lpthread"
#elif defined (WITH_THREAD_qt)
#define THREAD_NAME "-lgcjcoop"
#endif

#if defined (WITH_GC_boehm)
#define GC_NAME "-lgcjgc"
#endif

/* This bit is set if we saw a `-xfoo' language specification.  */
#define LANGSPEC	(1<<1)
/* This bit is set if they did `-lm' or `-lmath'.  */
#define MATHLIB		(1<<2)
/* This bit is set if they did `-lc'.  */
#define WITHLIBC	(1<<3)
/* This bit is set if they did `-lgcjgc'.  */
#define GCLIB		(1<<4)
/* This bit is set if they did `-lpthread' (or added some other thread
   library).  */
#define THREADLIB	(1<<5)
/* True if this arg is a parameter to the previous option-taking arg. */
#define PARAM_ARG	(1<<6)
/* True if this arg is a .java input file name. */
#define JAVA_FILE_ARG	(1<<7)
/* True if this arg is a .class input file name. */
#define CLASS_FILE_ARG	(1<<8)

#ifndef MATH_LIBRARY
#define MATH_LIBRARY "-lm"
#endif

extern int do_spec		PROTO((char *));
extern char *input_filename;
extern size_t input_filename_length;

char *main_class_name = NULL;
int lang_specific_extra_outfiles = 0;

/* Once we have the proper support in jc1 (and gcc.c) working,
   set COMBINE_INPUTS to one.  This enables combining multiple *.java
   and *.class input files to be passed to a single jc1 invocation. */
#define COMBINE_INPUTS 0

char jvgenmain_spec[] =
  "jvgenmain %i %{!pipe:%u.i} |\n\
   cc1 %{!pipe:%U.i} %1 \
		   %{!Q:-quiet} -dumpbase %b.c %{d*} %{m*} %{a*}\
		   %{g*} %{O*} \
		   %{v:-version} %{pg:-p} %{p} %{f*}\
		   %{aux-info*}\
		   %{pg:%{fomit-frame-pointer:%e-pg and -fomit-frame-pointer are incompatible}}\
		   %{S:%W{o*}%{!o*:-o %b.s}}%{!S:-o %{|!pipe:%U.s}} |\n\
              %{!S:as %a %Y -o %d%w%u%O %{!pipe:%U.s} %A\n }";

void
lang_specific_driver (fn, in_argc, in_argv, in_added_libraries)
     void (*fn)();
     int *in_argc;
     char ***in_argv;
     int *in_added_libraries;
{
  int i, j;

  /* If non-zero, the user gave us the `-v' flag.  */ 
  int saw_verbose_flag = 0;

  /* This will be 0 if we encounter a situation where we should not
     link in libgcj.  */
  int library = 1;

#if COMBINE_INPUTS
  /* This will be 1 if multiple input files (.class and/or .java)
     should be passed to a single jc1 invocation. */
  int combine_inputs = 0;

  /* Index of last .java or .class argument. */
  int last_input_index;

  /* A buffer containing the concatenation of the inputs files
     (e.g. "foo.java&bar.class&baz.class"). if combine_inputs. */
  char* combined_inputs_buffer;

  /* Next available location in combined_inputs_buffer. */
  int combined_inputs_pos;

  /* Number of .java and .class source file arguments seen. */
  int java_files_count = 0;
  int class_files_count = 0;

  /* Cumulative length of the  .java and .class source file names. */
  int java_files_length = 0;
  int class_files_length = 0;
#endif

  /* The number of arguments being added to what's in argv, other than
     libraries.  We use this to track the number of times we've inserted
     -xc++/-xnone.  */
  int added = 2;

  /* Used to track options that take arguments, so we don't go wrapping
     those with -xc++/-xnone.  */
  char *quote = NULL;

  /* The new argument list will be contained in this.  */
  char **arglist;

  /* Non-zero if we saw a `-xfoo' language specification on the
     command line.  Used to avoid adding our own -xc++ if the user
     already gave a language for the file.  */
  int saw_speclang = 0;

  /* "-lm" or "-lmath" if it appears on the command line.  */
  char *saw_math = 0;

  /* "-lc" if it appears on the command line.  */
  char *saw_libc = 0;

  /* "-lgcjgc" if it appears on the command line.  */
  char *saw_gc = 0;

  /* Saw `-l' option for the thread library.  */
  char *saw_threadlib = 0;

  /* Saw `-lgcj' on command line.  */
  int saw_libgcj = 0;

  /* Saw -C or -o option, respectively. */
  int saw_C = 0;
  int saw_o = 0;

  /* Saw some -O* or -g* option, respectively. */
  int saw_O = 0;
  int saw_g = 0;

  /* An array used to flag each argument that needs a bit set for
     LANGSPEC, MATHLIB, WITHLIBC, or GCLIB.  */
  int *args;

  /* By default, we throw on the math library.  */
  int need_math = 1;

  /* By default, we throw in the thread library (if one is required).
   */
  int need_thread = 1;

  /* By default, we throw in the gc library (if one is required).  */
  int need_gc = 1;

  /* The total number of arguments with the new stuff.  */
  int argc;

  /* The argument list.  */
  char **argv;

  /* The number of libraries added in.  */
  int added_libraries;

  /* The total number of arguments with the new stuff.  */
  int num_args = 1;

  /* Non-zero if linking is supposed to happen.  */
  int will_link = 1;

  argc = *in_argc;
  argv = *in_argv;
  added_libraries = *in_added_libraries;

  args = (int *) xmalloc (argc * sizeof (int));
  bzero ((char *) args, argc * sizeof (int));

  for (i = 1; i < argc; i++)
    {
      /* If the previous option took an argument, we swallow it here.  */
      if (quote)
	{
	  quote = NULL;
	  args[i] |= PARAM_ARG;
	  continue;
	}

      /* We don't do this anymore, since we don't get them with minus
	 signs on them.  */
      if (argv[i][0] == '\0' || argv[i][1] == '\0')
	continue;

      if (argv[i][0] == '-')
	{
	  if (library != 0 && (strcmp (argv[i], "-nostdlib") == 0
			       || strcmp (argv[i], "-nodefaultlibs") == 0))
	    {
	      library = 0;
	    }
	  else if (strcmp (argv[i], "-lm") == 0
		   || strcmp (argv[i], "-lmath") == 0
#ifdef ALT_LIBM
		   || strcmp (argv[i], ALT_LIBM) == 0
#endif
		  )
	    {
	      args[i] |= MATHLIB;
	      need_math = 0;
	    }
	  else if (strncmp (argv[i], "-fmain=", 7) == 0)
	    {
	      main_class_name = argv[i] + 7;
	      added--;
	    }
	  else if (strcmp (argv[i], "-lgcj") == 0)
	    saw_libgcj = 1;
	  else if (strcmp (argv[i], "-lc") == 0)
	    args[i] |= WITHLIBC;
#ifdef GC_NAME
	  else if (strcmp (argv[i], GC_NAME) == 0)
	    {
	      args[i] |= GCLIB;
	      need_gc = 0;
	    }
#endif
#ifdef THREAD_NAME
	  else if (strcmp (argv[i], THREAD_NAME) == 0)
	    {
	      args[i] |= THREADLIB;
	      need_thread = 0;
	    }
#endif
	  else if (strcmp (argv[i], "-v") == 0)
	    {
	      saw_verbose_flag = 1;
	      if (argc == 2)
		{
		  /* If they only gave us `-v', don't try to link
		     in libgcj.  */ 
		  library = 0;
		}
	    }
	  else if (strncmp (argv[i], "-x", 2) == 0)
	    saw_speclang = 1;
	  else if (strcmp (argv[i], "-C") == 0)
	    {
	      saw_C = 1;
#if COMBINE_INPUTS
	      combine_inputs = 1;
#endif
	      if (library != 0)
		added -= 2;
	      library = 0;
	      will_link = 0;
	    }
	  else if (argv[i][1] == 'g')
	    saw_g = 1;
	  else if (argv[i][1] == 'O')
	    saw_O = 1;
	  else if (((argv[i][2] == '\0'
		     && (char *)strchr ("bBVDUoeTuIYmLiA", argv[i][1]) != NULL)
		    || strcmp (argv[i], "-Tdata") == 0))
	    {
	      if (strcmp (argv[i], "-o") == 0)
		saw_o = 1;
	      quote = argv[i];
	    }
	  else if (strcmp(argv[i], "-classpath") == 0
		   || strcmp(argv[i], "-CLASSPATH") == 0)
	    {
	      quote = argv[i];
	      added -= 1;
	    }
	  else if (library != 0 
		   && ((argv[i][2] == '\0'
			&& (char *) strchr ("cSEM", argv[i][1]) != NULL)
		       || strcmp (argv[i], "-MM") == 0))
	    {
	      /* Don't specify libraries if we won't link, since that would
		 cause a warning.  */
	      library = 0;
	      added -= 2;

	      /* Remember this so we can confirm -fmain option.  */
	      will_link = 0;
	    }
	  else if (strcmp (argv[i], "-d") == 0)
	    {
	      /* `-d' option is for javac compatibility.  */
	      quote = argv[i];
	      added -= 1;
	    }
	  else if (strcmp (argv[i], "-fsyntax-only") == 0
		   || strcmp (argv[i], "--syntax-only") == 0)
	    {
	      library = 0;
	      will_link = 0;
	      continue;
	    }
	  else
	    /* Pass other options through.  */
	    continue;
	}
      else
	{
#if COMBINE_INPUTS
	  int len; 
#endif

	  if (saw_speclang)
	    {
	      saw_speclang = 0;
	      continue;
	    }

#if COMBINE_INPUTS
	  len = strlen (argv[i]);
	  if (len > 5 && strcmp (argv[i] + len - 5, ".java") == 0)
	    {
	      args[i] |= JAVA_FILE_ARG;
	      java_files_count++;
	      java_files_length += len;
	      last_input_index = i;
	    }
	  if (len > 6 && strcmp (argv[i] + len - 6, ".class") == 0)
	    {
	      args[i] |= CLASS_FILE_ARG;
	      class_files_count++;
	      class_files_length += len;
	      last_input_index = i;
	    }
#endif
	}
    }

  if (quote)
    (*fn) ("argument to `%s' missing\n", quote);

  num_args = argc + added;
  if (will_link)
    num_args += need_math + need_thread + need_gc;
  if (saw_C)
    {
      num_args += 3;
#if COMBINE_INPUTS
      class_files_length = 0;
      num_args -= class_files_count;
      num_args += 2;  /* For -o NONE. */
#endif
      if (saw_o)
	(*fn) ("cannot specify both -C and -o");
    }
#if COMBINE_INPUTS
  if (saw_o && java_files_count + (saw_C ? 0 : class_files_count) > 1)
    combine_inputs = 1;

  if (combine_inputs)
    {
      int len = java_files_length + java_files_count - 1;
      num_args -= java_files_count;
      num_args++;  /* Add one for the combined arg. */
      if (class_files_length > 0)
	{
	  len += class_files_length + class_files_count - 1;
	  num_args -= class_files_count;
	}
      combined_inputs_buffer = (char*) xmalloc (len);
      combined_inputs_pos = 0;
    }
  /* If we know we don't have to do anything, bail now.  */
#endif
#if 0
  if (! added && ! library && main_class_name == NULL && ! saw_C)
    {
      free (args);
      return;
    }
#endif

  if (main_class_name)
    {
      lang_specific_extra_outfiles++;
    }
  if (saw_g + saw_O == 0)
    num_args++;
  arglist = (char **) xmalloc ((num_args + 1) * sizeof (char *));

  for (i = 0, j = 0; i < argc; i++, j++)
    {
      arglist[j] = argv[i];

      if ((args[i] & PARAM_ARG) || i == 0)
	continue;

      if (strcmp (argv[i], "-classpath") == 0
	  || strcmp (argv[i], "-CLASSPATH") == 0)
	{
	  char* patharg
	    = (char*) xmalloc (strlen (argv[i]) + strlen (argv[i+1]) + 3);
	  sprintf (patharg, "-f%s=%s", argv[i]+1, argv[i+1]);
	  arglist[j] = patharg;
	  i++;
	  continue;
	}

      if (strcmp (argv[i], "-d") == 0)
	{
	  char *patharg = (char *) xmalloc (sizeof ("-foutput-class-dir=")
					    + strlen (argv[i + 1]) + 1);
	  sprintf (patharg, "-foutput-class-dir=%s", argv[i + 1]);
	  arglist[j] = patharg;
	  ++i;
	  continue;
	}

      if (strncmp (argv[i], "-fmain=", 7) == 0)
	{
	  if (! will_link)
	    (*fn) ("cannot specify `main' class when not linking");
	  --j;
	  continue;
	}

      /* Make sure -lgcj is before the math library, since libgcj
	 itself uses those math routines.  */
      if (!saw_math && (args[i] & MATHLIB) && library)
	{
	  --j;
	  saw_math = argv[i];
	}

      /* Likewise -lgcj must come before -lc.  */
      if (!saw_libc && (args[i] & WITHLIBC) && library)
	{
	  --j;
	  saw_libc = argv[i];
	}

      /* And -lgcj must come before -lgcjgc.  */
      if (!saw_gc && (args[i] & GCLIB) && library)
	{
	  --j;
	  saw_gc = argv[i];
	}

      /* And -lgcj must come before thread library.  */
      if (!saw_threadlib && (args[i] & THREADLIB) && library)
	{
	  --j;
	  saw_threadlib = argv[i];
	}

      if ((args[i] & CLASS_FILE_ARG) && saw_C)
	{
	  --j;
	  continue;
	}

#if COMBINE_INPUTS
      if (combine_inputs && (args[i] & (CLASS_FILE_ARG|JAVA_FILE_ARG)) != 0)
	{
	  if (combined_inputs_pos > 0)
	    combined_inputs_buffer[combined_inputs_pos++] = '&';
	  strcpy (&combined_inputs_buffer[combined_inputs_pos], argv[i]);
	  combined_inputs_pos += strlen (argv[i]);
	  --j;
	  continue;
	}
#endif
  }

#if COMBINE_INPUTS
  if (combine_inputs)
    {
      combined_inputs_buffer[combined_inputs_pos] = '\0';
#if 0
      if (! saw_C)
#endif
      arglist[j++] = combined_inputs_buffer;
    }
#endif

  /* If we saw no -O or -g option, default to -g1, for javac compatibility. */
  if (saw_g + saw_O == 0)
    arglist[j++] = "-g1";

  /* Add `-lgcj' if we haven't already done so.  */
  if (library && ! saw_libgcj)
    {
      arglist[j++] = "-lgcj";
      added_libraries++;
    }

  if (saw_math)
    arglist[j++] = saw_math;
  else if (library)
    {
      arglist[j++] = MATH_LIBRARY;
      added_libraries++;
    }

  if (saw_gc)
    arglist[j++] = saw_gc;
#ifdef GC_NAME
  else if (library)
    {
      arglist[j++] = GC_NAME;
      added_libraries++;
    }
#endif

  /* Thread library must come after GC library as well as after
     -lgcj.  */
  if (saw_threadlib)
    arglist[j++] = saw_threadlib;
#ifdef THREAD_NAME
  else if (library)
    {
      arglist[j++] = THREAD_NAME;
      added_libraries++;
    }
#endif

  if (saw_libc)
    arglist[j++] = saw_libc;

  if (saw_C)
    {
      arglist[j++] = "-fsyntax-only";
      arglist[j++] = "-femit-class-files";
      arglist[j++] = "-S";
#if COMBINE_INPUTS
      arglist[j++] = "-o";
      arglist[j++] = "NONE";
#endif
    }

  arglist[j] = NULL;

  *in_argc = j;
  *in_argv = arglist;
  *in_added_libraries = added_libraries;
}

int
lang_specific_pre_link ()
{
  if (main_class_name == NULL)
    return 0;
  input_filename = main_class_name;
  input_filename_length = strlen (main_class_name);
  return do_spec (jvgenmain_spec);
}
