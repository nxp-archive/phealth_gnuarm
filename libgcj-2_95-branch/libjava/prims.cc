// prims.cc - Code for core of runtime environment.

/* Copyright (C) 1998, 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

#include <config.h>

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#pragma implementation "java-array.h"

#include <cni.h>
#include <jvm.h>
#include <java-signal.h>

#include <java/lang/Class.h>
#include <java/lang/Runtime.h>
#include <java/lang/String.h>
#include <java/lang/Thread.h>
#include <java/lang/ThreadGroup.h>
#include <java/lang/FirstThread.h>
#include <java/lang/ArrayIndexOutOfBoundsException.h>
#include <java/lang/ArithmeticException.h>
#include <java/lang/ClassFormatError.h>
#include <java/lang/ClassCastException.h>
#include <java/lang/NegativeArraySizeException.h>
#include <java/lang/NullPointerException.h>
#include <java/lang/OutOfMemoryError.h>
#include <java/lang/ArrayStoreException.h>
#include <java/lang/System.h>
#include <java/lang/reflect/Modifier.h>
#include <java/io/PrintStream.h>

#ifdef USE_LTDL
#include <ltdl.h>
#endif

#define ObjectClass _CL_Q34java4lang6Object
extern java::lang::Class ObjectClass;

// We allocate a single OutOfMemoryError exception which we keep
// around for use if we run out of memory.
static java::lang::OutOfMemoryError *no_memory;

// Largest representable size_t.
#define SIZE_T_MAX ((size_t) (~ (size_t) 0))



#ifdef HANDLE_SEGV
static java::lang::NullPointerException *nullp;
SIGNAL_HANDLER (catch_segv)
{
  MAKE_THROW_FRAME;
  _Jv_Throw (nullp);
}
#endif

#ifdef HANDLE_FPE
static java::lang::ArithmeticException *arithexception;
SIGNAL_HANDLER (catch_fpe)
{
#ifdef HANDLE_DIVIDE_OVERFLOW
  HANDLE_DIVIDE_OVERFLOW;
#else
  MAKE_THROW_FRAME;
#endif
  _Jv_Throw (arithexception);
}
#endif



jboolean
_Jv_equalUtf8Consts (Utf8Const* a, Utf8Const *b)
{
  register int len;
  register _Jv_ushort *aptr, *bptr;
  if (a == b)
    return true;
  if (a->hash != b->hash)
    return false;
  len = a->length;
  if (b->length != len)
    return false;
  aptr = (_Jv_ushort *)a->data;
  bptr = (_Jv_ushort *)b->data;
  len = (len + 1) >> 1;
  while (--len >= 0)
    if (*aptr++ != *bptr++)
      return false;
  return true;
}

/* True iff A is equal to STR.
   HASH is STR->hashCode().  
*/

jboolean
_Jv_equal (Utf8Const* a, jstring str, jint hash)
{
  if (a->hash != (_Jv_ushort) hash)
    return false;
  jint len = str->length();
  jint i = 0;
  jchar *sptr = _Jv_GetStringChars (str);
  register unsigned char* ptr = (unsigned char*) a->data;
  register unsigned char* limit = ptr + a->length;
  for (;; i++, sptr++)
    {
      int ch = UTF8_GET (ptr, limit);
      if (i == len)
	return ch < 0;
      if (ch != *sptr)
	return false;
    }
  return true;
}

/* Count the number of Unicode chars encoded in a given Ut8 string. */
int
_Jv_strLengthUtf8(char* str, int len)
{
  register unsigned char* ptr;
  register unsigned char* limit;
  int str_length;

  ptr = (unsigned char*) str;
  limit = ptr + len;
  str_length = 0;
  for (; ptr < limit; str_length++) {
    if (UTF8_GET (ptr, limit) < 0) {
      return (-1);
    }
  }
  return (str_length);
}

/* Calculate a hash value for a string encoded in Utf8 format.
 * This returns the same hash value as specified or java.lang.String.hashCode.
 */
static jint
hashUtf8String (char* str, int len)
{
  register unsigned char* ptr = (unsigned char*) str;
  register unsigned char* limit = ptr + len;
  jint hash = 0;

  for (; ptr < limit;)
    {
      int ch = UTF8_GET (ptr, limit);
      /* Updated specification from
	 http://www.javasoft.com/docs/books/jls/clarify.html. */
      hash = (31 * hash) + ch;
    }
  return hash;
}

_Jv_Utf8Const *
_Jv_makeUtf8Const (char* s, int len)
{
  if (len < 0)
    len = strlen (s);
  Utf8Const* m = (Utf8Const*) _Jv_AllocBytes (sizeof(Utf8Const) + len + 1);
  if (! m)
    JvThrow (no_memory);
  memcpy (m->data, s, len);
  m->data[len] = 0;
  m->length = len;
  m->hash = hashUtf8String (s, len) & 0xFFFF;
  return (m);
}



#ifdef DEBUG
void
_Jv_Abort (const char *function, const char *file, int line,
	   const char *message)
#else
void
_Jv_Abort (const char *, const char *, int, const char *message)
#endif
{
#ifdef DEBUG
  fprintf (stderr,
	   "libgcj failure: %s\n   in function %s, file %s, line %d\n",
	   message, function, file, line);
#else
  java::io::PrintStream *err = java::lang::System::err;
  err->print(JvNewStringLatin1 ("libgcj failure: "));
  err->println(JvNewStringLatin1 (message));
  err->flush();
#endif
  abort ();
}

static void
fail_on_finalization (jobject)
{
  JvFail ("object was finalized");
}

void
_Jv_GCWatch (jobject obj)
{
  _Jv_RegisterFinalizer (obj, fail_on_finalization);
}

void
_Jv_ThrowBadArrayIndex(jint bad_index)
{
  JvThrow (new java::lang::ArrayIndexOutOfBoundsException
	   (java::lang::String::valueOf(bad_index)));
}

void*
_Jv_CheckCast (jclass c, jobject obj)
{
  if (obj != NULL && ! c->isAssignableFrom(obj->getClass()))
    JvThrow (new java::lang::ClassCastException);
  return obj;
}

void
_Jv_CheckArrayStore (jobject arr, jobject obj)
{
  if (obj)
    {
      JvAssert (arr != NULL);
      jclass arr_class = arr->getClass();
      JvAssert (arr_class->isArray());
      jclass elt_class = arr_class->getComponentType();
      jclass obj_class = obj->getClass();
      if (! elt_class->isAssignableFrom(obj_class))
	JvThrow (new java::lang::ArrayStoreException);
    }
}



// Allocate some unscanned memory and throw an exception if no memory.
void *
_Jv_AllocBytesChecked (jsize size)
{
  void *r = _Jv_AllocBytes (size);
  if (! r)
    _Jv_Throw (no_memory);
  return r;
}

// Allocate a new object of class C.  SIZE is the size of the object
// to allocate.  You might think this is redundant, but it isn't; some
// classes, such as String, aren't of fixed size.
jobject
_Jv_AllocObject (jclass c, jint size)
{
  _Jv_InitClass (c);

  jobject obj = (jobject) _Jv_AllocObj (size);
  if (! obj)
    JvThrow (no_memory);
  *((_Jv_VTable **) obj) = c->vtable;

  // If this class has inherited finalize from Object, then don't
  // bother registering a finalizer.  We know that finalize() is the
  // very first method after the dummy entry.  If this turns out to be
  // unreliable, a more robust implementation can be written.  Such an
  // implementation would look for Object.finalize in Object's method
  // table at startup, and then use that information to find the
  // appropriate index in the method vector.
  if (c->vtable->method[1] != ObjectClass.vtable->method[1])
    _Jv_RegisterFinalizer (obj, _Jv_FinalizeObject);

  return obj;
}

// Allocate a new array of Java objects.  Each object is of type
// `elementClass'.  `init' is used to initialize each slot in the
// array.
jobjectArray
_Jv_NewObjectArray (jsize count, jclass elementClass, jobject init)
{
  if (count < 0)
    JvThrow (new java::lang::NegativeArraySizeException);

  // Check for overflow.
  if ((size_t) count > (SIZE_T_MAX - sizeof (__JArray)) / sizeof (jobject))
    JvThrow (no_memory);

  size_t size = count * sizeof (jobject) + sizeof (__JArray);
  jclass clas = _Jv_FindArrayClass (elementClass);
  jobjectArray obj = (jobjectArray) _Jv_AllocArray (size);
  if (! obj)
    JvThrow (no_memory);
  obj->length = count;
  jobject* ptr = elements(obj);
  // We know the allocator returns zeroed memory.  So don't bother
  // zeroing it again.
  if (init)
    {
      while (--count >= 0)
	*ptr++ = init;
    }
  // Set the vtbl last to avoid problems if the GC happens during the
  // window in this function between the allocation and this
  // assignment.
  *((_Jv_VTable **) obj) = clas->vtable;
  return obj;
}

// Allocate a new array of primitives.  ELTYPE is the type of the
// element, COUNT is the size of the array.
jobject
_Jv_NewPrimArray (jclass eltype, jint count)
{
  int elsize = eltype->size();
  if (count < 0)
    JvThrow (new java::lang::NegativeArraySizeException ());

  // Check for overflow.
  if ((size_t) count > (SIZE_T_MAX - sizeof (__JArray)) / elsize)
    JvThrow (no_memory);

  __JArray *arr = (__JArray*) _Jv_AllocObj (sizeof (__JArray)
					    + elsize * count);
  if (! arr)
    JvThrow (no_memory);
  arr->length = count;
  // Note that we assume we are given zeroed memory by the allocator.

  jclass klass = _Jv_FindArrayClass (eltype);
  // Set the vtbl last to avoid problems if the GC happens during the
  // window in this function between the allocation and this
  // assignment.
  *((_Jv_VTable **) arr) = klass->vtable;
  return arr;
}

jcharArray
JvNewCharArray (jint length)
{
  return (jcharArray) _Jv_NewPrimArray (JvPrimClass (char), length);
}

jbooleanArray
JvNewBooleanArray (jint length)
{
  return (jbooleanArray) _Jv_NewPrimArray (JvPrimClass (boolean), length);
}

jbyteArray
JvNewByteArray (jint length)
{
  return (jbyteArray) _Jv_NewPrimArray (JvPrimClass (byte), length);
}

jshortArray
JvNewShortArray (jint length)
{
  return (jshortArray) _Jv_NewPrimArray (JvPrimClass (short), length);
}

jintArray
JvNewIntArray (jint length)
{
  return (jintArray) _Jv_NewPrimArray (JvPrimClass (int), length);
}

jlongArray
JvNewLongArray (jint length)
{
  return (jlongArray) _Jv_NewPrimArray (JvPrimClass (long), length);
}

jfloatArray
JvNewFloatArray (jint length)
{
  return (jfloatArray) _Jv_NewPrimArray (JvPrimClass (float), length);
}

jdoubleArray
JvNewDoubleArray (jint length)
{
  return (jdoubleArray) _Jv_NewPrimArray (JvPrimClass (double), length);
}

jobject
_Jv_NewArray (jint type, jint size)
{
  switch (type)
    {
      case  4:  return JvNewBooleanArray (size);
      case  5:  return JvNewCharArray (size);
      case  6:  return JvNewFloatArray (size);
      case  7:  return JvNewDoubleArray (size);
      case  8:  return JvNewByteArray (size);
      case  9:  return JvNewShortArray (size);
      case 10:  return JvNewIntArray (size);
      case 11:  return JvNewLongArray (size);
    }
  JvFail ("newarray - bad type code");
  return NULL;			// Placate compiler.
}

jobject
_Jv_NewMultiArray (jclass type, jint dimensions, jint *sizes)
{
  JvAssert (type->isArray());
  jclass element_type = type->getComponentType();
  jobject result;
  if (element_type->isPrimitive())
    result = _Jv_NewPrimArray (element_type, sizes[0]);
  else
    result = _Jv_NewObjectArray (sizes[0], element_type, NULL);

  if (dimensions > 1)
    {
      JvAssert (! element_type->isPrimitive());
      JvAssert (element_type->isArray());
      jobject *contents = elements ((jobjectArray) result);
      for (int i = 0; i < sizes[0]; ++i)
	contents[i] = _Jv_NewMultiArray (element_type, dimensions - 1,
					 sizes + 1);
    }

  return result;
}

jobject
_Jv_NewMultiArray (jclass array_type, jint dimensions, ...)
{
  va_list args;
  jint sizes[dimensions];
  va_start (args, dimensions);
  for (int i = 0; i < dimensions; ++i)
    {
      jint size = va_arg (args, jint);
      sizes[i] = size;
    }
  va_end (args);

  return _Jv_NewMultiArray (array_type, dimensions, sizes);
}



class _Jv_PrimClass : public java::lang::Class
{
public:
  // FIXME: calling convention is weird.  If we use the natural types
  // then the compiler will complain because they aren't Java types.
  _Jv_PrimClass (jobject cname, jbyte sig, jint len)
    {
      using namespace java::lang::reflect;

      // We must initialize every field of the class.  We do this in
      // the same order they are declared in Class.h.
      next = NULL;
      name = _Jv_makeUtf8Const ((char *) cname, -1);
      accflags = Modifier::PUBLIC | Modifier::FINAL;
      superclass = NULL;
      constants.size = 0;
      constants.tags = NULL;
      constants.data = NULL;
      methods = NULL;
      method_count = sig;
      vtable_method_count = 0;
      fields = NULL;
      size_in_bytes = len;
      field_count = 0;
      static_field_count = 0;
      vtable = JV_PRIMITIVE_VTABLE;
      interfaces = NULL;
      loader = NULL;
      interface_count = 0;
      state = 0;		// FIXME.
      thread = NULL;
    }
};

#define DECLARE_PRIM_TYPE(NAME, SIG, LEN) \
  _Jv_PrimClass _Jv_##NAME##Class((jobject) #NAME, (jbyte) SIG, (jint) LEN)

DECLARE_PRIM_TYPE(byte, 'B', 1);
DECLARE_PRIM_TYPE(short, 'S', 2);
DECLARE_PRIM_TYPE(int, 'I', 4);
DECLARE_PRIM_TYPE(long, 'J', 8);
DECLARE_PRIM_TYPE(boolean, 'Z', 1);
DECLARE_PRIM_TYPE(char, 'C', 2);
DECLARE_PRIM_TYPE(float, 'F', 4);
DECLARE_PRIM_TYPE(double, 'D', 8);
DECLARE_PRIM_TYPE(void, 'V', 0);

jclass
_Jv_FindClassFromSignature (char *sig, java::lang::ClassLoader *loader)
{
  switch (*sig)
    {
    case 'B':
      return JvPrimClass (byte);
    case 'S':
      return JvPrimClass (short);
    case 'I':
      return JvPrimClass (int);
    case 'J':
      return JvPrimClass (long);
    case 'Z':
      return JvPrimClass (boolean);
    case 'C':
      return JvPrimClass (char);
    case 'F':
      return JvPrimClass (float);
    case 'D':
      return JvPrimClass (double);
    case 'V':
      return JvPrimClass (void);
    case 'L':
      {
	int i;
	for (i = 1; sig[i] && sig[i] != ';'; ++i)
	  ;
	_Jv_Utf8Const *name = _Jv_makeUtf8Const (&sig[1], i - 1);
	return _Jv_FindClass (name, loader);
      }
    case '[':
      return _Jv_FindArrayClass (_Jv_FindClassFromSignature (&sig[1], loader));
    }
  JvFail ("couldn't understand class signature");
  return NULL;			// Placate compiler.
}



JArray<jstring> *
JvConvertArgv (int argc, const char **argv)
{
  if (argc < 0)
    argc = 0;
  jobjectArray ar = JvNewObjectArray(argc, &StringClass, NULL);
  jobject* ptr = elements(ar);
  for (int i = 0;  i < argc;  i++)
    {
      const char *arg = argv[i];
      // FIXME - should probably use JvNewStringUTF.
      *ptr++ = JvNewStringLatin1(arg, strlen(arg));
    }
  return (JArray<jstring>*) ar;
}

// FIXME: These variables are static so that they will be
// automatically scanned by the Boehm collector.  This is needed
// because with qthreads the collector won't scan the initial stack --
// it will only scan the qthreads stacks.

// Command line arguments.
static jobject arg_vec;

// The primary threadgroup.
static java::lang::ThreadGroup *main_group;

// The primary thread.
static java::lang::Thread *main_thread;

void
JvRunMain (jclass klass, int argc, const char **argv)
{
  INIT_SEGV;
  INIT_FPE;

  no_memory = new java::lang::OutOfMemoryError;

#ifdef USE_LTDL
  LTDL_SET_PRELOADED_SYMBOLS ();
#endif

  arg_vec = JvConvertArgv (argc - 1, argv + 1);
  main_group = new java::lang::ThreadGroup (23);
  main_thread = new java::lang::FirstThread (main_group, klass, arg_vec);

  main_thread->start();
  _Jv_ThreadWait ();

  java::lang::Runtime::getRuntime ()->exit (0);
}



void *
_Jv_Malloc (jsize size)
{
  if (size == 0)
    size = 1;
  void *ptr = malloc ((size_t) size);
  if (ptr == NULL)
    JvThrow (no_memory);
  return ptr;
}

void
_Jv_Free (void* ptr)
{
  return free (ptr);
}
