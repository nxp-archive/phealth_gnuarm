// Functions for Exception Support for Java.

/* Copyright (C) 1998, 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

#include <config.h>

#include "exception"
#include <stddef.h>

#include <java/lang/Class.h>
#include <java/lang/NullPointerException.h>
#include <cni.h>
#include <jvm.h>

// eh-common.h needs gansidecl.h.
#include "gansidecl.h"
#include "eh-common.h"

typedef struct {
  __eh_info eh_info;
  void *value;
} java_eh_info;


/* Language-specific EH info pointer, throw routine, and language/version
   info routines. All defined in libgcc2. */

extern "C" java_eh_info **__get_eh_info (); 
extern "C" void __throw () __attribute__ ((__noreturn__));
extern "C" short __get_eh_table_version (void *table);
extern "C" short __get_eh_table_language (void *table);


extern "C" void * malloc (size_t);
extern "C" void free (void *);


extern "C" void *
_Jv_type_matcher (java_eh_info *info, void* match_info, 
		  void *exception_table)
{
  if (__get_eh_table_language (exception_table) != EH_LANG_Java)
    return NULL;

  /* we don't worry about version info yet, there is only one version! */
  
  if (match_info != NULL)
    {
      // The match_info is either a (java::lang::Class*) or
      // match_info is one more than a (Utf8Const*).
      if (sizeof(void*) != sizeof(size_t))
	abort();
      size_t mi = (size_t) match_info;
      if ((mi & 1) != 0)
	match_info = _Jv_FindClass ((Utf8Const*) (mi - 1), NULL);
      if (! _Jv_IsInstanceOf ((jobject) info->value, (jclass) match_info))
	return NULL;
    }

  return info->value;
}

/* Compiler hook to return a pointer to java exception object. The value
   is cleared, so if the exception needs to be rethrown, it should be set 
   again */

extern "C" void *
_Jv_exception_info (void)
{
  java_eh_info *info = *(__get_eh_info ());
  void *ptr;

  if (info == NULL)
    abort ();

  ptr = info->value;

  /* clear the value so another throw is an error */
  info->value = NULL;

  return ptr;
}



/* Allocate an exception info structure for java. Called the first time
   an exception is thrown. */

extern "C" void
_Jv_eh_alloc ()
{
  /* FIXME: we should use _Jv_AllocBytes here.  However, libgcc2
     apparently can sometimes free() this value itself.  */
  java_eh_info *p = (java_eh_info *) malloc (sizeof (java_eh_info));
  if (p == 0)
    terminate ();

  p->value = 0;
  java_eh_info ** info_ptr = __get_eh_info ();

  /* There should NOT be an exception info pointer already. */
  if (*info_ptr != NULL)
    abort ();

  *info_ptr = p;
}

/* Deallocate the current exception info structure. Called at shutdown time. */

extern "C" void
_Jv_eh_free ()
{
  java_eh_info ** info_ptr = __get_eh_info ();
  if (*info_ptr == NULL)
    abort ();
  
  /* FIXME: ideally we should just let the GC handle this.  */
  free (*info_ptr);
  *info_ptr = NULL;
}

/* Perform a throw, Java style. Throw will unwind through this call, so
   there better not be any handlers or exception thrown here. */


/* Initialize an __eh_info structure with this libraries matching info. */

extern "C" void
_Jv_setup_eh_info (__eh_info *)
{
}

extern "C" void
_Jv_Throw (void *value)
{
  if (value == NULL)
    value = (void *) new java::lang::NullPointerException ();
  java_eh_info *ehinfo = *(__get_eh_info ());
  if (ehinfo == NULL)
    {
      _Jv_eh_alloc ();
      ehinfo = *(__get_eh_info ());
    }
  ehinfo->eh_info.match_function = _Jv_type_matcher;
  ehinfo->eh_info.language = EH_LANG_Java;
  ehinfo->eh_info.version = 1;
  ehinfo->value = value;
  __throw();
}
