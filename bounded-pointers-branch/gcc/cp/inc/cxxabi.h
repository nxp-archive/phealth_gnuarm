/* new abi support -*- C++ -*-
   Copyright (C) 2000
   Free Software Foundation, Inc.
   Written by Nathan Sidwell, Codesourcery LLC, <nathan@codesourcery.com>

   This file declares the new abi entry points into the runtime. It is not
   normally necessary for user programs to include this header, or use the
   entry points directly. However, this header is available should that be
   needed.
   
   Some of the entry points are intended for both C and C++, thus this header
   is includable from both C and C++. Though the C++ specific parts are not
   available in C, naturally enough.  */

#ifndef __CXXABI_H
#define __CXXABI_H 1

#if defined(__cplusplus) && (!defined(__GXX_ABI_VERSION) || __GXX_ABI_VERSION < 100)
/* These structures only make sense when targeting the new abi, catch a
   bonehead error early rather than let the user get very confused.  */
#error "Not targetting the new abi, supply -fnew-abi"
#endif

#ifdef __cplusplus

#include <typeinfo>
// This should really be cstddef, but that currently is not available when
// building the runtime.
#include <stddef.h>

namespace __cxxabiv1
{

/* type information for int, float etc */
class __fundamental_type_info
  : public std::type_info
{
public:
  virtual ~__fundamental_type_info ();
public:
  explicit __fundamental_type_info (const char *__n)
    : std::type_info (__n)
    { }
};

/* type information for pointer to data or function, but not pointer to member */
class __pointer_type_info
  : public std::type_info
{
/* abi defined member variables */
public:
  int quals;                    /* qualification of the target object */
  const std::type_info *type;   /* type of pointed to object */

/* abi defined member functions */
public:
  virtual ~__pointer_type_info ();
public:
  explicit __pointer_type_info (const char *__n,
                                int __quals,
                                const std::type_info *__type)
    : std::type_info (__n), quals (__quals), type (__type)
    { }

/* implementation defined types */
public:
  enum quals_masks {
    const_mask = 0x1,
    volatile_mask = 0x2,
    restrict_mask = 0x4,
    incomplete_mask = 0x8,
    incomplete_class_mask = 0x10
  };

/* implementation defined member functions */
protected:
  virtual bool __is_pointer_p () const;
protected:
  virtual bool __do_catch (const std::type_info *__thr_type,
                           void **__thr_obj,
                           unsigned __outer) const;
protected:
  virtual bool __pointer_catch (const __pointer_type_info *__thr_type,
                                void **__thr_obj,
                                unsigned __outer) const;
};

/* type information for array objects */
class __array_type_info
  : public std::type_info
{
/* abi defined member functions */
protected:
  virtual ~__array_type_info ();
public:
  explicit __array_type_info (const char *__n)
    : std::type_info (__n)
    { }
};

/* type information for functions (both member and non-member) */
class __function_type_info
  : public std::type_info
{
/* abi defined member functions */
public:
  virtual ~__function_type_info ();
public:
  explicit __function_type_info (const char *__n)
    : std::type_info (__n)
    { }
  
/* implementation defined member functions */
protected:
  virtual bool __is_function_p () const;
};

/* type information for enumerations */
class __enum_type_info
  : public std::type_info
{
/* abi defined member functions */
public:
  virtual ~__enum_type_info ();
public:
  explicit __enum_type_info (const char *__n)
    : std::type_info (__n)
    { }
};

/* type information for a pointer to member variable (not function) */
class __pointer_to_member_type_info
  : public __pointer_type_info
{
/* abi defined member variables */
public:
  const __class_type_info *klass;   /* class of the member */

/* abi defined member functions */
public:
  virtual ~__pointer_to_member_type_info ();
public:
  explicit __pointer_to_member_type_info (const char *__n,
                                          int __quals,
                                          const std::type_info *__type,
                                          const __class_type_info *__klass)
    : __pointer_type_info (__n, __quals, __type), klass (__klass)
    { }

/* implementation defined member functions */
protected:
  virtual bool __is_pointer_p () const;
protected:
  virtual bool __pointer_catch (const __pointer_type_info *__thr_type,
                                void **__thr_obj,
                                unsigned __outer) const;
};

class __class_type_info;

/* helper class for __vmi_class_type */
class __base_class_info
{
/* abi defined member variables */
public:
  const __class_type_info *base;    /* base class type */
  long vmi_offset_flags;            /* offset and info */

/* implementation defined types */
public:
  enum vmi_masks {
    virtual_mask = 0x1,
    public_mask = 0x2,
    hwm_bit = 2,
    offset_shift = 8          /* bits to shift offset by */
  };
  
/* implementation defined member functions */
public:
  bool __is_virtual_p () const
    { return vmi_offset_flags & virtual_mask; }
  bool __is_public_p () const
    { return vmi_offset_flags & public_mask; }
  std::ptrdiff_t __offset () const
    { 
      // This shift, being of a signed type, is implementation defined. GCC
      // implements such shifts as arithmetic, which is what we want.
      return std::ptrdiff_t (vmi_offset_flags) >> offset_shift;
    }
};

/* type information for a class */
class __class_type_info
  : public std::type_info
{
/* abi defined member functions */
public:
  virtual ~__class_type_info ();
public:
  explicit __class_type_info (const char *__n)
    : type_info (__n)
    { }

/* implementation defined types */
public:
  /* sub_kind tells us about how a base object is contained within a derived
     object. We often do this lazily, hence the UNKNOWN value. At other times
     we may use NOT_CONTAINED to mean not publicly contained. */
  enum __sub_kind
  {
    __unknown = 0,              /* we have no idea */
    __not_contained,            /* not contained within us (in some */
                                /* circumstances this might mean not contained */
                                /* publicly) */
    __contained_ambig,          /* contained ambiguously */
    
    __contained_virtual_mask = __base_class_info::virtual_mask, /* via a virtual path */
    __contained_public_mask = __base_class_info::public_mask,   /* via a public path */
    __contained_mask = 1 << __base_class_info::hwm_bit,         /* contained within us */
    
    __contained_private = __contained_mask,
    __contained_public = __contained_mask | __contained_public_mask
  };

public:  
  struct __upcast_result;
  struct __dyncast_result;

/* implementation defined member functions */
protected:
  virtual bool __do_upcast (const __class_type_info *__dst_type, void **__obj_ptr) const;

protected:
  virtual bool __do_catch (const type_info *__thr_type, void **__thr_obj,
                           unsigned __outer) const;


public:
  /* Helper for upcast. See if DST is us, or one of our bases. ACCESS_PATH */
  /* gives the access from the start object. Return TRUE if we know the upcast */
  /* fails. */
  virtual bool __do_upcast (__sub_kind __access_path,
                            const __class_type_info *__dst,
                            const void *__obj,
                            __upcast_result &__restrict __result) const;

public:
  /* Indicate whether SRC_PTR of type SRC_TYPE is contained publicly within
     OBJ_PTR. OBJ_PTR points to a base object of our type, which is the
     destination type. SRC2DST indicates how SRC objects might be contained
     within this type.  If SRC_PTR is one of our SRC_TYPE bases, indicate the
     virtuality. Returns not_contained for non containment or private
     containment. */
  inline __sub_kind __find_public_src (std::ptrdiff_t __src2dst,
                                       const void *__obj_ptr,
                                       const __class_type_info *__src_type,
                                       const void *__src_ptr) const;

public:
  /* dynamic cast helper. ACCESS_PATH gives the access from the most derived
     object to this base. DST_TYPE indicates the desired type we want. OBJ_PTR
     points to a base of our type within the complete object. SRC_TYPE
     indicates the static type started from and SRC_PTR points to that base
     within the most derived object. Fill in RESULT with what we find. Return
     true if we have located an ambiguous match. */
  virtual bool __do_dyncast (std::ptrdiff_t __src2dst,
                             __sub_kind __access_path,
                             const __class_type_info *__dst_type,
                             const void *__obj_ptr,
                             const __class_type_info *__src_type,
                             const void *__src_ptr,
                             __dyncast_result &__result) const;
public:
  /* Helper for find_public_subobj. SRC2DST indicates how SRC_TYPE bases are
     inherited by the type started from -- which is not necessarily the
     current type. The current type will be a base of the destination type.
     OBJ_PTR points to the current base. */
  virtual __sub_kind __do_find_public_src (std::ptrdiff_t __src2dst,
                                           const void *__obj_ptr,
                                           const __class_type_info *__src_type,
                                           const void *__src_ptr) const;
};

/* type information for a class with a single non-virtual base */
class __si_class_type_info
  : public __class_type_info
{
/* abi defined member variables */
protected:
  const __class_type_info *base;    /* base type */

/* abi defined member functions */
public:
  virtual ~__si_class_type_info ();
public:
  explicit __si_class_type_info (const char *__n,
                                 const __class_type_info *__base)
    : __class_type_info (__n), base (__base)
    { }

/* implementation defined member functions */
protected:
  virtual bool __do_dyncast (std::ptrdiff_t __src2dst,
                             __sub_kind __access_path,
                             const __class_type_info *__dst_type,
                             const void *__obj_ptr,
                             const __class_type_info *__src_type,
                             const void *__src_ptr,
                             __dyncast_result &__result) const;
  virtual __sub_kind __do_find_public_src (std::ptrdiff_t __src2dst,
                                           const void *__obj_ptr,
                                           const __class_type_info *__src_type,
                                           const void *__sub_ptr) const;
  virtual bool __do_upcast (__sub_kind __access_path,
                            const __class_type_info *__dst,
                            const void *__obj,
                            __upcast_result &__restrict __result) const;
};

/* type information for a class with multiple and/or virtual bases */
class __vmi_class_type_info : public __class_type_info {
/* abi defined member variables */
public:
  int vmi_flags;                  /* details about the class heirarchy */
  int vmi_base_count;             /* number of direct bases */
  __base_class_info vmi_bases[1]; /* array of bases */
  /* The array of bases uses the trailing array struct hack
     so this class is not constructable with a normal constructor. It is
     internally generated by the compiler. */

/* abi defined member functions */
public:
  virtual ~__vmi_class_type_info ();
public:
  explicit __vmi_class_type_info (const char *__n,
                                  int __flags)
    : __class_type_info (__n), vmi_flags (__flags), vmi_base_count (0)
    { }

/* implementation defined types */
public:
  enum vmi_flags_masks {
    non_diamond_repeat_mask = 0x1,   /* distinct instance of repeated base */
    diamond_shaped_mask = 0x2,       /* diamond shaped multiple inheritance */
    non_public_base_mask = 0x4,      /* has non-public direct or indirect base */
    public_base_mask = 0x8,          /* has public base (direct) */
    
    __flags_unknown_mask = 0x10
  };

/* implementation defined member functions */
protected:
  virtual bool __do_dyncast (std::ptrdiff_t __src2dst,
                             __sub_kind __access_path,
                             const __class_type_info *__dst_type,
                             const void *__obj_ptr,
                             const __class_type_info *__src_type,
                             const void *__src_ptr,
                             __dyncast_result &__result) const;
  virtual __sub_kind __do_find_public_src (std::ptrdiff_t __src2dst,
                                           const void *__obj_ptr,
                                           const __class_type_info *__src_type,
                                           const void *__src_ptr) const;
  virtual bool __do_upcast (__sub_kind __access_path,
                            const __class_type_info *__dst,
                            const void *__obj,
                            __upcast_result &__restrict __result) const;
};

/* dynamic cast runtime */
void *__dynamic_cast (const void *__src_ptr,    /* object started from */
                      const __class_type_info *__src_type, /* static type of object */
                      const __class_type_info *__dst_type, /* desired target type */
                      std::ptrdiff_t __src2dst); /* how src and dst are related */

    /* src2dst has the following possible values
       >= 0: src_type is a unique public non-virtual base of dst_type
             dst_ptr + src2dst == src_ptr
       -1: unspecified relationship
       -2: src_type is not a public base of dst_type
       -3: src_type is a multiple public non-virtual base of dst_type */

/* array ctor/dtor routines */

/* allocate and construct array */
void *__cxa_vec_new (size_t __element_count,
                     size_t __element_size,
                     size_t __padding_size,
                     void (*__constructor) (void *),
                     void (*__destructor) (void *));

/* construct array */
void __cxa_vec_ctor (void *__array_address,
                     size_t __element_count,
                     size_t __element_size,
                     void (*__constructor) (void *),
                     void (*__destructor) (void *));

/* destruct array */
void __cxa_vec_dtor (void *__array_address,
                     size_t __element_count,
                     size_t __element_size,
                     void (*__destructor) (void *));

/* destruct and release array */
void __cxa_vec_delete (void *__array_address,
                       size_t __element_size,
                       size_t __padding_size,
                       void (*__destructor) (void *));

} /* namespace __cxxabiv1 */

/* User programs should use the alias `abi'. */
namespace abi = __cxxabiv1;

#else
#endif /* __cplusplus */


#endif /* __CXXABI_H */
