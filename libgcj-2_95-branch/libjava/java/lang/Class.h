// Class.h - Header file for java.lang.Class.  -*- c++ -*-

/* Copyright (C) 1998, 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

// Written primary using compiler source and Class.java as guides.
#ifndef __JAVA_LANG_CLASS_H__
#define __JAVA_LANG_CLASS_H__

#pragma interface

#include <java/lang/Object.h>
#include <java/lang/String.h>

// We declare these here to avoid including cni.h.
extern "C" void _Jv_InitClass (jclass klass);
extern "C" void _Jv_RegisterClasses (jclass *classes);

struct _Jv_Field;
struct _Jv_VTable;

#define CONSTANT_Class 7
#define CONSTANT_Fieldref 9
#define CONSTANT_Methodref 10
#define CONSTANT_InterfaceMethodref 11
#define CONSTANT_String 8
#define CONSTANT_Integer 3
#define CONSTANT_Float 4
#define CONSTANT_Long 5
#define CONSTANT_Double 6
#define CONSTANT_NameAndType 12
#define CONSTANT_Utf8 1
#define CONSTANT_Unicode 2
#define CONSTANT_ResolvedFlag 16
#define CONSTANT_ResolvedString    (CONSTANT_String+CONSTANT_ResolvedFlag)
#define CONSTANT_ResolvedClass     (CONSTANT_Class+CONSTANT_ResolvedFlag)

struct _Jv_Constants
{
  jint size;
  jbyte *tags;
  void **data;
};

struct _Jv_Method
{
  _Jv_Utf8Const *name;
  _Jv_Utf8Const *signature;
  unsigned short accflags;
  void *ncode;
};

#define JV_PRIMITIVE_VTABLE ((_Jv_VTable *) -1)

class java::lang::Class : public java::lang::Object
{
public:
  static jclass forName (jstring className);
  JArray<jclass> *getClasses (void);

  java::lang::ClassLoader *getClassLoader (void)
    {
      return loader;
    }

  jclass getComponentType (void)
    {
      return isArray () ? (* (jclass *) &methods) : 0;
    }

  java::lang::reflect::Constructor *getConstructor (JArray<jclass> *);
  JArray<java::lang::reflect::Constructor *> *getConstructors (void);
  java::lang::reflect::Constructor *getDeclaredConstructor (JArray<jclass> *);
  JArray<java::lang::reflect::Constructor *> *getDeclaredConstructors (void);
  java::lang::reflect::Field *getDeclaredField (jstring);
  JArray<java::lang::reflect::Field *> *getDeclaredFields (void);
  java::lang::reflect::Method *getDeclaredMethod (jstring, JArray<jclass> *);
  JArray<java::lang::reflect::Method *> *getDeclaredMethods (void);

  JArray<jclass> *getDeclaredClasses (void);
  jclass getDeclaringClass (void);

  java::lang::reflect::Field *getField (jstring);
private:
  java::lang::reflect::Field *getField (jstring, jint);
public:
  JArray<java::lang::reflect::Field *> *getFields (void);

  JArray<jclass> *getInterfaces (void);

  java::lang::reflect::Method *getMethod (jstring, JArray<jclass> *);
  JArray<java::lang::reflect::Method *> *getMethods (void);

  jint getModifiers (void)
    {
      return accflags;
    }

  jstring getName (void);

  java::io::InputStream *getResourceAsStream (jstring resourceName);
  JArray<jobject> *getSigners (void);

  jclass getSuperclass (void)
    {
      return superclass;
    }

  jboolean isArray (void)
    {
      return name->data[0] == '[';
    }

  jboolean isAssignableFrom (jclass cls);
  jboolean isInstance (jobject obj);
  jboolean isInterface (void);

  jboolean isPrimitive (void)
    {
      return vtable == JV_PRIMITIVE_VTABLE;
    }

  jobject newInstance (void);
  jstring toString (void);

  // FIXME: this probably shouldn't be public.
  jint size (void)
    {
      return size_in_bytes;
    }

private:
  void checkMemberAccess (jint flags);
  void resolveConstants (void);

  // Various functions to handle class initialization.
  java::lang::Throwable *hackTrampoline (jint, java::lang::Throwable *);
  void hackRunInitializers (void);
  void initializeClass (void);

  // Friend functions implemented in natClass.cc.
  friend _Jv_Method *_Jv_GetMethodLocal (jclass klass, _Jv_Utf8Const *name,
					 _Jv_Utf8Const *signature);
  friend void _Jv_InitClass (jclass klass);
  friend void _Jv_RegisterClasses (jclass *classes);
  friend jclass _Jv_FindClassInCache (_Jv_Utf8Const *name,
				      java::lang::ClassLoader *loader);
  friend jclass _Jv_FindArrayClass (jclass element);
  friend jclass _Jv_NewClass (_Jv_Utf8Const *name, jclass superclass,
			      java::lang::ClassLoader *loader);

  friend jfieldID JvGetFirstInstanceField (jclass);
  friend jint JvNumInstanceFields (jclass);
  friend jobject _Jv_AllocObject (jclass, jint);
  friend jobjectArray _Jv_NewObjectArray (jsize, jclass, jobject);
  friend jobject _Jv_NewPrimArray (jclass, jint);
  friend jobject _Jv_JNI_ToReflectedField (_Jv_JNIEnv *, jclass, jfieldID);
  friend jfieldID _Jv_FromReflectedField (java::lang::reflect::Field *);
  friend jmethodID _Jv_FromReflectedMethod (java::lang::reflect::Method *);

  friend class _Jv_PrimClass;

#ifdef JV_MARKOBJ_DECL
  friend JV_MARKOBJ_DECL;
#endif

  // Chain for class pool.
  jclass next;
  // Name of class.
  _Jv_Utf8Const *name;
  // Access flags for class.
  unsigned short accflags;
  // The superclass, or null for Object.
  jclass superclass;
  // Class constants.
  _Jv_Constants constants;
  // Methods.  If this is an array class, then this field holds a
  // pointer to the element type.  If this is a primitive class, this
  // is used to cache a pointer to the appropriate array type.
  _Jv_Method *methods;
  // Number of methods.  If this class is primitive, this holds the
  // character used to represent this type in a signature.
  short method_count;
  // Number of methods in the vtable.
  short vtable_method_count;
  // The fields.
  _Jv_Field *fields;
  // Size of instance fields, in bytes.
  int size_in_bytes;
  // Total number of fields (instance and static).
  short field_count;
  // Number of static fields.
  short static_field_count;
  // The vtbl for all objects of this class.
  _Jv_VTable *vtable;
  // Interfaces implemented by this class.
  jclass *interfaces;
  // The class loader for this class.
  java::lang::ClassLoader *loader;
  // Number of interfaces.
  short interface_count;
  // State of this class.
  jbyte state;
  // The thread which has locked this class.  Used during class
  // initialization.
  java::lang::Thread *thread;
};

#endif /* __JAVA_LANG_CLASS_H__ */
