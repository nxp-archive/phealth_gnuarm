// natRuntime.cc - Implementation of native side of Runtime class.

/* Copyright (C) 1998, 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

#include <config.h>

#include <stdlib.h>

#include <cni.h>
#include <jvm.h>
#include <java/lang/Runtime.h>
#include <java/lang/UnknownError.h>
#include <java/lang/UnsatisfiedLinkError.h>

#ifdef USE_LTDL
#include <ltdl.h>
#endif

void
java::lang::Runtime::exit (jint status)
{
  checkExit (status);

  // Make status right for Unix.  This is perhaps strange.
  if (status < 0 || status > 255)
    status = 255;

  if (finalize_on_exit)
    _Jv_RunAllFinalizers ();

  ::exit (status);
}

jlong
java::lang::Runtime::freeMemory (void)
{
  return _Jv_GCFreeMemory ();
}

void
java::lang::Runtime::gc (void)
{
  _Jv_RunGC ();
}

void
java::lang::Runtime::load (jstring path)
{
  JvSynchronize sync (this);
  checkLink (path);
  using namespace java::lang;
#ifdef USE_LTDL
  // FIXME: make sure path is absolute.
  lt_dlhandle h = lt_dlopen (FIXME);
  if (h == NULL)
    {
      const char *msg = lt_dlerror ();
      _Jv_Throw (new UnsatisfiedLinkError (JvNewStringLatin1 (msg)));
    }
#else
  _Jv_Throw (new UnknownError
	     (JvNewStringLatin1 ("Runtime.load not implemented")));
#endif /* USE_LTDL */
}

void
java::lang::Runtime::loadLibrary (jstring lib)
{
  JvSynchronize sync (this);
  checkLink (lib);
  using namespace java::lang;
#ifdef USE_LTDL
  // FIXME: make sure path is absolute.
  lt_dlhandle h = lt_dlopenext (FIXME);
  if (h == NULL)
    {
      const char *msg = lt_dlerror ();
      _Jv_Throw (new UnsatisfiedLinkError (JvNewStringLatin1 (msg)));
    }
#else
  _Jv_Throw (new UnknownError
	     (JvNewStringLatin1 ("Runtime.loadLibrary not implemented")));
#endif /* USE_LTDL */
}

void
java::lang::Runtime::init (void)
{
  finalize_on_exit = false;
#ifdef USE_LTDL
  lt_dlinit ();
#endif
}

void
java::lang::Runtime::runFinalization (void)
{
  _Jv_RunFinalizers ();
}

jlong
java::lang::Runtime::totalMemory (void)
{
  return _Jv_GCTotalMemory ();
}

void
java::lang::Runtime::traceInstructions (jboolean)
{
  // Do nothing.
}

void
java::lang::Runtime::traceMethodCalls (jboolean)
{
  // Do nothing.
}
