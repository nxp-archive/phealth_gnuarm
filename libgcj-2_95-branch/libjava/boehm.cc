// boehm.cc - interface between libjava and Boehm GC.

/* Copyright (C) 1998, 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

#include <config.h>

#include <stdio.h>

#include <cni.h>
#include <java/lang/Class.h>
#include <jvm.h>
#include <java-field.h>

// We need to include gc_priv.h.  However, it tries to include
// config.h if it hasn't already been included.  So we force the
// inclusion of the Boehm config.h.
extern "C"
{
#include <boehm-config.h>
#include <gc_priv.h>
#include <gc_mark.h>

  // These aren't declared in any Boehm GC header.
  void GC_finalize_all (void);
  ptr_t GC_debug_generic_malloc (size_t size, int k, GC_EXTRA_PARAMS);
};

// FIXME: this should probably be defined in some GC header.
#ifdef GC_DEBUG
#  define GC_GENERIC_MALLOC(Size, Type) \
    GC_debug_generic_malloc (Size, Type, GC_EXTRAS)
#else
#  define GC_GENERIC_MALLOC(Size, Type) GC_generic_malloc (Size, Type)
#endif

// We must check for plausibility ourselves.
#define MAYBE_MARK(Obj, Top, Limit, Source, Exit)  \
      if ((ptr_t) (Obj) >= GC_least_plausible_heap_addr \
	  && (ptr_t) (Obj) <= GC_greatest_plausible_heap_addr) \
        PUSH_CONTENTS (Obj, Top, Limit, Source, Exit)

#define ObjectClass _CL_Q34java4lang6Object
extern java::lang::Class ObjectClass;
#define ClassClass _CL_Q34java4lang5Class
extern java::lang::Class ClassClass;



// Nonzero if this module has been initialized.
static int initialized = 0;

// `kind' index used when allocating Java objects.
static int obj_kind_x;

// `kind' index used when allocating Java arrays.
static int array_kind_x;

// Freelist used for Java objects.
static ptr_t *obj_free_list;

// Freelist used for Java arrays.
static ptr_t *array_free_list;



// This is called by the GC during the mark phase.  It marks a Java
// object.  We use `void *' arguments and return, and not what the
// Boehm GC wants, to avoid pollution in our headers.
void *
_Jv_MarkObj (void *addr, void *msp, void *msl, void * /*env*/)
{
  mse *mark_stack_ptr = (mse *) msp;
  mse *mark_stack_limit = (mse *) msl;
  jobject obj = (jobject) addr;

  _Jv_VTable *dt = *(_Jv_VTable **) addr;
  // We check this in case a GC occurs before the vtbl is set.  FIXME:
  // should use allocation lock while initializing object.
  if (! dt)
    return mark_stack_ptr;
  jclass klass = dt->clas;

  // Every object has a sync_info pointer.
  word w = (word) obj->sync_info;
  MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, obj, o1label);
  // Mark the object's class.
  w = (word) klass;
  MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, obj, o2label);

  if (klass == &ClassClass)
    {
      jclass c = (jclass) addr;

      w = (word) c->next;
      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, c2label);
      w = (word) c->name;
      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, c3label);
      w = (word) c->superclass;
      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, c4label);
      for (int i = 0; i < c->constants.size; ++i)
	{
	  w = (word) c->constants.data[i];
	  MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, c5label);
	}

      // If the class is an array, then the methods field holds a
      // pointer to the element class.  If the class is primitive,
      // then the methods field holds a pointer to the array class.
      w = (word) c->methods;
      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, c6label);

      if (! c->isArray() && ! c->isPrimitive())
	{
	  // Scan each method in the cases where `methods' really
	  // points to a methods structure.
	  for (int i = 0; i < c->method_count; ++i)
	    {
	      w = (word) c->methods[i].name;
	      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c,
			     cm1label);
	      w = (word) c->methods[i].signature;
	      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c,
			     cm2label);
	      // FIXME: `ncode' entry?
	    }
	}

      // Mark all the fields.
      w = (word) c->fields;
      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, c8label);
      for (int i = 0; i < c->field_count; ++i)
	{
#ifndef COMPACT_FIELDS
	  w = (word) c->fields[i].name;
	  MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, c8alabel);
#endif
	  w = (word) c->fields[i].type;
	  MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, c8blabel);
	}

      w = (word) c->vtable;
      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, c9label);
      w = (word) c->interfaces;
      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, cAlabel);
      for (int i = 0; i < c->interface_count; ++i)
	{
	  w = (word) c->interfaces[i];
	  MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, cClabel);
	}
      w = (word) c->loader;
      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, c, cBlabel);
    }
  else
    {
      // NOTE: each class only holds information about the class
      // itself.  So we must do the marking for the entire inheritance
      // tree in order to mark all fields.  FIXME: what about
      // interfaces?  We skip Object here, because Object only has a
      // sync_info, and we handled that earlier.
      // Note: occasionally `klass' can be null.  For instance, this
      // can happen if a GC occurs between the point where an object
      // is allocated and where the vtbl slot is set.
      while (klass && klass != &ObjectClass)
	{
	  jfieldID field = JvGetFirstInstanceField (klass);
	  jint max = JvNumInstanceFields (klass);

	  for (int i = 0; i < max; ++i)
	    {
	      if (JvFieldIsRef (field))
		{
		  jobject val = JvGetObjectField (obj, field);
		  w = (word) val;
		  MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit,
			      obj, elabel);
		}
	      field = field->getNextInstanceField ();
	    }
	  klass = klass->getSuperclass();
	}
    }

  return mark_stack_ptr;
}

// This is called by the GC during the mark phase.  It marks a Java
// array (of objects).  We use `void *' arguments and return, and not
// what the Boehm GC wants, to avoid pollution in our headers.
void *
_Jv_MarkArray (void *addr, void *msp, void *msl, void * /*env*/)
{
  mse *mark_stack_ptr = (mse *) msp;
  mse *mark_stack_limit = (mse *) msl;
  jobjectArray array = (jobjectArray) addr;

  _Jv_VTable *dt = *(_Jv_VTable **) addr;
  // We check this in case a GC occurs before the vtbl is set.  FIXME:
  // should use allocation lock while initializing object.
  if (! dt)
    return mark_stack_ptr;
  jclass klass = dt->clas;

  // Every object has a sync_info pointer.
  word w = (word) array->sync_info;
  MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, array, e1label);
  // Mark the object's class.
  w = (word) klass;
  MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, obj, o2label);

  for (int i = 0; i < JvGetArrayLength (array); ++i)
    {
      jobject obj = elements (array)[i];
      w = (word) obj;
      MAYBE_MARK (w, mark_stack_ptr, mark_stack_limit, array, e2label);
    }

  return mark_stack_ptr;
}

// Allocate space for a new Java object.  FIXME: this might be the
// wrong interface; we might prefer to pass in the object type as
// well.  It isn't important for this collector, but it might be for
// other collectors.
void *
_Jv_AllocObj (jsize size)
{
  return GC_GENERIC_MALLOC (size, obj_kind_x);
}

// Allocate space for a new Java array.  FIXME: again, this might be
// the wrong interface.
void *
_Jv_AllocArray (jsize size)
{
  return GC_GENERIC_MALLOC (size, array_kind_x);
}

// Allocate some space that is known to be pointer-free.
void *
_Jv_AllocBytes (jsize size)
{
  return GC_GENERIC_MALLOC (size, PTRFREE);
}

static void
call_finalizer (GC_PTR obj, GC_PTR client_data)
{
  _Jv_FinalizerFunc *fn = (_Jv_FinalizerFunc *) client_data;
  jobject jobj = (jobject) obj;

  (*fn) (jobj);
}

void
_Jv_RegisterFinalizer (void *object, _Jv_FinalizerFunc *meth)
{
  GC_REGISTER_FINALIZER_NO_ORDER (object, call_finalizer, meth,
				  NULL, NULL);
}

void
_Jv_RunFinalizers (void)
{
  GC_invoke_finalizers ();
}

void
_Jv_RunAllFinalizers (void)
{
  GC_finalize_all ();
}

void
_Jv_RunGC (void)
{
  GC_gcollect ();
}

long
_Jv_GCTotalMemory (void)
{
  return GC_get_heap_size ();
}

/* Sum size of each hblk.  */
static void
sum_blocks (struct hblk *h, word arg)
{
  long *sump = (long *) arg;
  /* This evil computation is from boehm-gc/checksums.c.  */
  hdr *hhdr = HDR (h);
  int bytes = WORDS_TO_BYTES (hhdr->hb_sz);
  bytes += HDR_BYTES + HBLKSIZE - 1;
  bytes &= ~ (HBLKSIZE - 1);
  *sump += bytes;
}

/* This turns out to be expensive to implement.  For now, we don't
   care.  We could make it less expensive, perhaps, but that would
   require some changes to the collector.  */
long
_Jv_GCFreeMemory (void)
{
  long sum = 0;
  GC_apply_to_all_blocks (sum_blocks, &sum);
  return sum;
}

void
_Jv_InitGC (void)
{
  int proc;
  DCL_LOCK_STATE;

  DISABLE_SIGNALS ();
  LOCK ();

  if (initialized)
    {
      UNLOCK ();
      ENABLE_SIGNALS ();
      return;
    }
  initialized = 1;

  // Set up state for marking and allocation of Java objects.
  obj_free_list = (ptr_t *) GC_generic_malloc_inner ((MAXOBJSZ + 1)
						     * sizeof (ptr_t),
						     PTRFREE);
  memset (obj_free_list, 0, (MAXOBJSZ + 1) * sizeof (ptr_t));

  proc = GC_n_mark_procs++;
  GC_mark_procs[proc] = (mark_proc) _Jv_MarkObj;

  obj_kind_x = GC_n_kinds++;
  GC_obj_kinds[obj_kind_x].ok_freelist = obj_free_list;
  GC_obj_kinds[obj_kind_x].ok_reclaim_list = 0;
  GC_obj_kinds[obj_kind_x].ok_descriptor = MAKE_PROC (proc, 0);
  GC_obj_kinds[obj_kind_x].ok_relocate_descr = FALSE;
  GC_obj_kinds[obj_kind_x].ok_init = TRUE;

  // Set up state for marking and allocation of arrays of Java
  // objects.
  array_free_list = (ptr_t *) GC_generic_malloc_inner ((MAXOBJSZ + 1)
						       * sizeof (ptr_t),
						       PTRFREE);
  memset (array_free_list, 0, (MAXOBJSZ + 1) * sizeof (ptr_t));

  proc = GC_n_mark_procs++;
  GC_mark_procs[proc] = (mark_proc) _Jv_MarkArray;

  array_kind_x = GC_n_kinds++;
  GC_obj_kinds[array_kind_x].ok_freelist = array_free_list;
  GC_obj_kinds[array_kind_x].ok_reclaim_list = 0;
  GC_obj_kinds[array_kind_x].ok_descriptor = MAKE_PROC (proc, 0);
  GC_obj_kinds[array_kind_x].ok_relocate_descr = FALSE;
  GC_obj_kinds[array_kind_x].ok_init = TRUE;

  UNLOCK ();
  ENABLE_SIGNALS ();
}
