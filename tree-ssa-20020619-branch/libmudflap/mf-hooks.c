/* Mudflap: narrow-pointer bounds-checking by tree rewriting.
   Copyright (C) 2002, 2003 Free Software Foundation, Inc.
   Contributed by Frank Ch. Eigler <fche@redhat.com>
   and Graydon Hoare <graydon@redhat.com>

This file is part of GCC.
XXX: libgcc license?
*/

#include "config.h"

#define _POSIX_SOURCE
#define _GNU_SOURCE

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

#include "mf-runtime.h"
#include "mf-impl.h"

#ifdef _MUDFLAP
#error "Do not compile this file with -fmudflap!"
#endif




/* ------------------------------------------------------------------------ */
/* These hook functions are intercepted via linker wrapping or shared
   library ordering.  */


#define MF_VALIDATE_EXTENT(value,size,acc,context)            \
 {                                                            \
  if (UNLIKELY (size > 0 && __MF_CACHE_MISS_P (value, size))) \
    __mf_check ((void *) (value), (size), acc, "(" context ")");       \
 }


#define BEGIN_PROTECT(ty, fname, ...)       \
  ty result;                                \
  enum __mf_state old_state;                \
  if (UNLIKELY (__mf_state == reentrant))   \
  {                                         \
    return CALL_REAL(fname, __VA_ARGS__);   \
  }                                         \
  else if (UNLIKELY (__mf_state == starting)) \
  {                                         \
    return CALL_BACKUP(fname, __VA_ARGS__); \
  }                                         \
  else                                      \
  {                                         \
     old_state = __mf_state;                \
     __mf_state = reentrant;                \
     TRACE ("mf: %s\n", __PRETTY_FUNCTION__); \
  }

#define END_PROTECT(ty, fname, ...)              \
  result = (ty) CALL_REAL(fname, __VA_ARGS__);   \
  __mf_state = old_state;                        \
  return result;


/* malloc/free etc. */

#ifdef WRAP_malloc

#if PIC
/* A special bootstrap variant. */
static void *
__mf_0fn_malloc (size_t c)
{
  return NULL;
}
#endif

#undef malloc
WRAPPER(void *, malloc, size_t c)
{
  size_t size_with_crumple_zones;
  DECLARE(void *, malloc, size_t c);
  BEGIN_PROTECT (void *, malloc, c);
  
  size_with_crumple_zones = 
    CLAMPADD(c,CLAMPADD(__mf_opts.crumple_zone,
			__mf_opts.crumple_zone));
  result = (char *) CALL_REAL(malloc, size_with_crumple_zones);
  
  __mf_state = old_state;
  
  if (LIKELY(result))
    {
      result += __mf_opts.crumple_zone;
      __mf_register (result, c, __MF_TYPE_HEAP, "malloc region");
      /* XXX: register __MF_TYPE_NOACCESS for crumple zones.  */
    }

  return result;
}
#endif


#ifdef WRAP_calloc

#ifdef PIC
/* A special bootstrap variant. */
static void *
__mf_0fn_calloc (size_t c, size_t n)
{
  return NULL;
}
#endif

#undef calloc
WRAPPER(void *, calloc, size_t c, size_t n)
{
  size_t size_with_crumple_zones;
  DECLARE(void *, calloc, size_t, size_t);
  DECLARE(void *, malloc, size_t);
  DECLARE(void *, memset, void *, int, size_t);
  BEGIN_PROTECT (char *, calloc, c, n);
  
  size_with_crumple_zones = 
    CLAMPADD((c * n), /* XXX: CLAMPMUL */
	     CLAMPADD(__mf_opts.crumple_zone,
		      __mf_opts.crumple_zone));  
  result = (char *) CALL_REAL(malloc, size_with_crumple_zones);
  
  if (LIKELY(result))
    memset (result, 0, size_with_crumple_zones);
  
  __mf_state = old_state;
  
  if (LIKELY(result))
    {
      result += __mf_opts.crumple_zone;
      __mf_register (result, c*n /* XXX: clamp */, __MF_TYPE_HEAP_I, "calloc region");
      /* XXX: register __MF_TYPE_NOACCESS for crumple zones.  */
    }
  
  return result;
}
#endif

#ifdef WRAP_realloc

#if PIC
/* A special bootstrap variant. */
static void *
__mf_0fn_realloc (void *buf, size_t c)
{
  return NULL;
}
#endif

#undef realloc
WRAPPER(void *, realloc, void *buf, size_t c)
{
  DECLARE(void * , realloc, void *, size_t);
  size_t size_with_crumple_zones;
  char *base = buf;
  unsigned saved_wipe_heap;

  BEGIN_PROTECT (char *, realloc, buf, c);

  if (LIKELY(buf))
    base -= __mf_opts.crumple_zone;

  size_with_crumple_zones = 
    CLAMPADD(c, CLAMPADD(__mf_opts.crumple_zone,
			 __mf_opts.crumple_zone));
  result = (char *) CALL_REAL(realloc, base, size_with_crumple_zones);

  __mf_state = old_state;      

  /* Ensure heap wiping doesn't occur during this peculiar
     unregister/reregister pair.  */
  LOCKTH ();
  saved_wipe_heap = __mf_opts.wipe_heap;
  __mf_opts.wipe_heap = 0;

  if (LIKELY(buf))
    __mfu_unregister (buf, 0);
  
  if (LIKELY(result))
    {
      result += __mf_opts.crumple_zone;
      __mfu_register (result, c, __MF_TYPE_HEAP_I, "realloc region");
      /* XXX: register __MF_TYPE_NOACCESS for crumple zones.  */
    }

  /* Restore previous setting.  */
  __mf_opts.wipe_heap = saved_wipe_heap;
  UNLOCKTH ();

  return result;
}
#endif


#ifdef WRAP_free

#if PIC
/* A special bootstrap variant. */
static void
__mf_0fn_free (void *buf)
{
  return;
}
#endif

#undef free
WRAPPER(void, free, void *buf)
{
  /* Use a circular queue to delay some number (__mf_opts.free_queue_length) of free()s.  */
  static void *free_queue [__MF_FREEQ_MAX];
  static unsigned free_ptr = 0;
  static int freeq_initialized = 0;
  enum __mf_state old_state;
  DECLARE(void * , free, void *);  

  if (UNLIKELY (__mf_state != active))
    {
      CALL_REAL(free, buf);
      return;
    }

  if (UNLIKELY(!freeq_initialized))
    {
      memset (free_queue, 0, 
		     __MF_FREEQ_MAX * sizeof (void *));
      freeq_initialized = 1;
    }

  if (UNLIKELY(buf == NULL))
    return;

  TRACE ("mf: %s\n", __PRETTY_FUNCTION__);

  __mf_unregister (buf, 0);

  old_state = __mf_state;
  __mf_state = reentrant;
      
  if (UNLIKELY(__mf_opts.free_queue_length > 0))
    {
      
      if (free_queue [free_ptr] != NULL)
	{
	  char *base = free_queue [free_ptr];
	  base -= __mf_opts.crumple_zone;
	  if (__mf_opts.trace_mf_calls)
	    {
	      VERBOSE_TRACE ("mf: freeing deferred pointer #%d %08lx = %08lx - %u\n", 
			     __mf_opts.free_queue_length, 
			     (uintptr_t) base,
			     (uintptr_t) free_queue [free_ptr],
			     __mf_opts.crumple_zone);
	    }
	  CALL_REAL(free, base);
	}
      free_queue [free_ptr] = buf;
      free_ptr = (free_ptr == (__mf_opts.free_queue_length-1) ? 0 : free_ptr + 1);
    } 
  else 
    {
      /* back pointer up a bit to the beginning of crumple zone */
      char *base = (char *)buf;
      base -= __mf_opts.crumple_zone;
      if (__mf_opts.trace_mf_calls)
	{
	  VERBOSE_TRACE ("mf: freeing pointer %08lx = %08lx - %u\n",
			 (uintptr_t) base, 
			 (uintptr_t) buf, 
			 __mf_opts.crumple_zone);
	}
      CALL_REAL(free, base);
    }
  
  __mf_state = old_state;
}
#endif


#ifdef WRAP_mmap

#if PIC
/* A special bootstrap variant. */
static void *
__mf_0fn_mmap (void *start, size_t l, int prot, int f, int fd, off_t off)
{
  return (void *) -1;
}
#endif


#undef mmap
WRAPPER(void *, mmap, 
	void  *start,  size_t length, int prot, 
	int flags, int fd, off_t offset)
{

  DECLARE(void *, mmap, void *, size_t, int, 
			    int, int, off_t);
  BEGIN_PROTECT(void *, mmap, start, length, 
		prot, flags, fd, offset);

  result = CALL_REAL(mmap, start, length, prot, 
			flags, fd, offset);

  /*
  VERBOSE_TRACE ("mf: mmap (%08lx, %08lx, ...) => %08lx\n", 
		 (uintptr_t) start, (uintptr_t) length,
		 (uintptr_t) result);
  */

  __mf_state = old_state;

  if (result != (void *)-1)
    {
      /* Register each page as a heap object.  Why not register it all
	 as a single segment?  That's so that a later munmap() call
	 can unmap individual pages.  XXX: would __MF_TYPE_GUESS make
	 this more automatic?  */
      size_t ps = getpagesize ();
      uintptr_t base = (uintptr_t) result;
      uintptr_t offset;

      for (offset=0; offset<length; offset+=ps)
	{
	  /* XXX: We could map PROT_NONE to __MF_TYPE_NOACCESS. */
	  /* XXX: Unaccessed HEAP pages are reported as leaks.  Is this
	     appropriate for unaccessed mmap pages? */
	  __mf_register ((void *) CLAMPADD (base, offset), ps,
			 __MF_TYPE_HEAP_I, "mmap page");
	}
    }

  return result;
}
#endif


#ifdef WRAP_munmap

#if PIC
/* A special bootstrap variant. */
static int
__mf_0fn_munmap (void *start, size_t length)
{
  return -1;
}
#endif


#undef munmap
WRAPPER(int , munmap, void *start, size_t length)
{
  DECLARE(int, munmap, void *, size_t);
  BEGIN_PROTECT(int, munmap, start, length);
  
  result = CALL_REAL(munmap, start, length);

  /*
  VERBOSE_TRACE ("mf: munmap (%08lx, %08lx, ...) => %08lx\n", 
		 (uintptr_t) start, (uintptr_t) length,
		 (uintptr_t) result);
  */

  __mf_state = old_state;

  if (result == 0)
    {
      /* Unregister each page as a heap object.  */
      size_t ps = getpagesize ();
      uintptr_t base = (uintptr_t) start & (~ (ps - 1)); /* page align */
      uintptr_t offset;

      for (offset=0; offset<length; offset+=ps)
	__mf_unregister ((void *) CLAMPADD (base, offset), ps);
    }
  return result;
}
#endif


/* This wrapper is a little different, as it's implemented in terms
   of the wrapped malloc/free functions. */
#ifdef WRAP_alloca
#undef alloca
WRAPPER(void *, alloca, size_t c)
{
  DECLARE (void *, malloc, size_t);
  DECLARE (void, free, void *);

  /* This struct, a linked list, tracks alloca'd objects.  The newest
     object is at the head of the list.  If we detect that we've
     popped a few levels of stack, then the listed objects are freed
     as needed.  NB: The tracking struct is allocated with
     real_malloc; the user data with wrap_malloc.
  */
  struct alloca_tracking { void *ptr; void *stack; struct alloca_tracking* next; };
  static struct alloca_tracking *alloca_history = NULL;

  void *stack = __builtin_frame_address (0);
  char *result;
  struct alloca_tracking *track;

  TRACE ("mf: %s\n", __PRETTY_FUNCTION__);
  VERBOSE_TRACE ("mf: alloca stack level %08lx\n", (uintptr_t) stack);

  /* Free any previously alloca'd blocks that belong to deeper-nested functions,
     which must therefore have exited by now.  */
#define DEEPER_THAN < /* for x86 */
  while (alloca_history &&
	 ((uintptr_t) alloca_history->stack DEEPER_THAN (uintptr_t) stack))
    {
      struct alloca_tracking *next = alloca_history->next;
      CALL_WRAP (free, alloca_history->ptr);
      CALL_REAL (free, alloca_history);
      alloca_history = next;
    }

  /* Allocate new block.  */
  result = NULL;
  if (LIKELY (c > 0)) /* alloca(0) causes no allocation.  */
    {
      track = (struct alloca_tracking *) CALL_REAL (malloc, 
						    sizeof (struct alloca_tracking));
      if (LIKELY (track != NULL))
	{
	  result = (char *) CALL_WRAP (malloc, c);
	  if (UNLIKELY (result == NULL))
	    {
	      CALL_REAL (free, track);
	      /* Too bad.  XXX: What about errno?  */
	    }
	  else
	    {
	      track->ptr = result;
	      track->stack = stack;
	      track->next = alloca_history;
	      alloca_history = track;
	    }
	}
    }
  
  return result;
}
#endif



/* ------------------------------------------------------------------------ */
/* These hook functions are intercepted via compile-time macros only.  */


#undef MF_VALIDATE_EXTENT
#define MF_VALIDATE_EXTENT(value,size,acc,context)            \
 do {                                                         \
  if (UNLIKELY (size > 0 && __MF_CACHE_MISS_P (value, size))) \
    {                                                         \
    __mf_check ((void *) (value), (size), acc, "(" context ")");  \
    }                                                         \
 } while (0)



/* str*,mem*,b* */

#ifdef WRAP_memcpy
WRAPPER2(void *, memcpy, void *dest, const void *src, size_t n)
{
  MF_VALIDATE_EXTENT(src, n, __MF_CHECK_READ, "memcpy source");
  MF_VALIDATE_EXTENT(dest, n, __MF_CHECK_WRITE, "memcpy dest");
  return memcpy (dest, src, n);
}
#endif


#ifdef WRAP_memmove
WRAPPER2(void *, memmove, void *dest, const void *src, size_t n)
{
  MF_VALIDATE_EXTENT(src, n, __MF_CHECK_READ, "memmove src");
  MF_VALIDATE_EXTENT(dest, n, __MF_CHECK_WRITE, "memmove dest");
  return memmove (dest, src, n);
}
#endif

#ifdef WRAP_memset
WRAPPER2(void *, memset, void *s, int c, size_t n)
{
  MF_VALIDATE_EXTENT(s, n, __MF_CHECK_WRITE, "memset dest");
  return memset (s, c, n);
}
#endif

#ifdef WRAP_memcmp
WRAPPER2(int, memcmp, const void *s1, const void *s2, size_t n)
{
  MF_VALIDATE_EXTENT(s1, n, __MF_CHECK_READ, "memcmp 1st arg");
  MF_VALIDATE_EXTENT(s2, n, __MF_CHECK_READ, "memcmp 2nd arg");
  return memcmp (s1, s2, n);
}
#endif

#ifdef WRAP_memchr
WRAPPER2(void *, memchr, const void *s, int c, size_t n)
{
  MF_VALIDATE_EXTENT(s, n, __MF_CHECK_READ, "memchr region");
  return memchr (s, c, n);
}
#endif

#ifdef WRAP_memrchr
WRAPPER2(void *, memrchr, const void *s, int c, size_t n)
{
  MF_VALIDATE_EXTENT(s, n, __MF_CHECK_READ, "memrchr region");
  return memrchr (s, c, n);
}
#endif

#ifdef WRAP_strcpy
WRAPPER2(char *, strcpy, char *dest, const char *src)
{
  /* nb: just because strlen(src) == n doesn't mean (src + n) or (src + n +
     1) are valid pointers. the allocated object might have size < n.
     check anyways. */

  size_t n = strlen (src);
  MF_VALIDATE_EXTENT(src, CLAMPADD(n, 1), __MF_CHECK_READ, "strcpy src"); 
  MF_VALIDATE_EXTENT(dest, CLAMPADD(n, 1), __MF_CHECK_WRITE, "strcpy dest");
  return strcpy (dest, src);
}
#endif

#ifdef WRAP_strncpy
WRAPPER2(char *, strncpy, char *dest, const char *src, size_t n)
{
  size_t len = strnlen (src, n);
  MF_VALIDATE_EXTENT(src, len, __MF_CHECK_READ, "strncpy src");
  MF_VALIDATE_EXTENT(dest, len, __MF_CHECK_WRITE, "strncpy dest"); /* nb: strNcpy */
  return strncpy (dest, src, n);
}
#endif

#ifdef WRAP_strcat
WRAPPER2(char *, strcat, char *dest, const char *src)
{
  size_t dest_sz;
  size_t src_sz;

  dest_sz = strlen (dest);
  src_sz = strlen (src);  
  MF_VALIDATE_EXTENT(src, CLAMPADD(src_sz, 1), __MF_CHECK_READ, "strcat src");
  MF_VALIDATE_EXTENT(dest, CLAMPADD(dest_sz, CLAMPADD(src_sz, 1)),
		     __MF_CHECK_WRITE, "strcat dest");
  return strcat (dest, src);
}
#endif

#ifdef WRAP_strncat
WRAPPER2(char *, strncat, char *dest, const char *src, size_t n)
{

  /* nb: validating the extents (s,n) might be a mistake for two reasons.
     
  (1) the string s might be shorter than n chars, and n is just a 
  poor choice by the programmer. this is not a "true" error in the
  sense that the call to strncat would still be ok.
  
  (2) we could try to compensate for case (1) by calling strlen(s) and
  using that as a bound for the extent to verify, but strlen might fall off
  the end of a non-terminated string, leading to a false positive.
  
  so we will call strnlen(s,n) and use that as a bound.

  if strnlen returns a length beyond the end of the registered extent
  associated with s, there is an error: the programmer's estimate for n is
  too large _AND_ the string s is unterminated, in which case they'd be
  about to touch memory they don't own while calling strncat.

  this same logic applies to further uses of strnlen later down in this
  file. */

  size_t src_sz;
  size_t dest_sz;

  src_sz = strnlen (src, n);
  dest_sz = strnlen (dest, n);
  MF_VALIDATE_EXTENT(src, src_sz, __MF_CHECK_READ, "strncat src");
  MF_VALIDATE_EXTENT(dest, (CLAMPADD(dest_sz, CLAMPADD(src_sz, 1))),
		     __MF_CHECK_WRITE, "strncat dest");
  return strncat (dest, src, n);
}
#endif

#ifdef WRAP_strcmp
WRAPPER2(int, strcmp, const char *s1, const char *s2)
{
  size_t s1_sz;
  size_t s2_sz;

  s1_sz = strlen (s1);
  s2_sz = strlen (s2);  
  MF_VALIDATE_EXTENT(s1, CLAMPADD(s1_sz, 1), __MF_CHECK_READ, "strcmp 1st arg");
  MF_VALIDATE_EXTENT(s2, CLAMPADD(s2_sz, 1), __MF_CHECK_WRITE, "strcmp 2nd arg");
  return strcmp (s1, s2);
}
#endif

#ifdef WRAP_strcasecmp
WRAPPER2(int, strcasecmp, const char *s1, const char *s2)
{
  size_t s1_sz;
  size_t s2_sz;

  s1_sz = strlen (s1);
  s2_sz = strlen (s2);  
  MF_VALIDATE_EXTENT(s1, CLAMPADD(s1_sz, 1), __MF_CHECK_READ, "strcasecmp 1st arg");
  MF_VALIDATE_EXTENT(s2, CLAMPADD(s2_sz, 1), __MF_CHECK_READ, "strcasecmp 2nd arg");
  return strcasecmp (s1, s2);
}
#endif

#ifdef WRAP_strncmp
WRAPPER2(int, strncmp, const char *s1, const char *s2, size_t n)
{
  size_t s1_sz;
  size_t s2_sz;

  s1_sz = strnlen (s1, n);
  s2_sz = strnlen (s2, n);
  MF_VALIDATE_EXTENT(s1, s1_sz, __MF_CHECK_READ, "strncmp 1st arg");
  MF_VALIDATE_EXTENT(s2, s2_sz, __MF_CHECK_READ, "strncmp 2nd arg");
  return strncmp (s1, s2, n);
}
#endif

#ifdef WRAP_strncasecmp
WRAPPER2(int, strncasecmp, const char *s1, const char *s2, size_t n)
{
  size_t s1_sz;
  size_t s2_sz;

  s1_sz = strnlen (s1, n);
  s2_sz = strnlen (s2, n);
  MF_VALIDATE_EXTENT(s1, s1_sz, __MF_CHECK_READ, "strncasecmp 1st arg");
  MF_VALIDATE_EXTENT(s2, s2_sz, __MF_CHECK_READ, "strncasecmp 2nd arg");
  return strncasecmp (s1, s2, n);
}
#endif

#ifdef WRAP_strdup
WRAPPER2(char *, strdup, const char *s)
{
  DECLARE(void *, malloc, size_t sz);
  char *result;
  size_t n = strlen (s);

  MF_VALIDATE_EXTENT(s, CLAMPADD(n,1), __MF_CHECK_READ, "strdup region");
  result = (char *)CALL_REAL(malloc, 
			     CLAMPADD(CLAMPADD(n,1),
				      CLAMPADD(__mf_opts.crumple_zone,
					       __mf_opts.crumple_zone)));

  if (UNLIKELY(! result)) return result;

  result += __mf_opts.crumple_zone;
  memcpy (result, s, n);
  result[n] = '\0';

  __mf_register (result, CLAMPADD(n,1), __MF_TYPE_HEAP_I, "strdup region");
  return result;
}
#endif

#ifdef WRAP_strndup
WRAPPER2(char *, strndup, const char *s, size_t n)
{
  DECLARE(void *, malloc, size_t sz);
  char *result;
  size_t sz = strnlen (s, n);

  MF_VALIDATE_EXTENT(s, sz, __MF_CHECK_READ, "strndup region"); /* nb: strNdup */

  /* note: strndup still adds a \0, even with the N limit! */
  result = (char *)CALL_REAL(malloc, 
			     CLAMPADD(CLAMPADD(n,1),
				      CLAMPADD(__mf_opts.crumple_zone,
					       __mf_opts.crumple_zone)));
  
  if (UNLIKELY(! result)) return result;

  result += __mf_opts.crumple_zone;
  memcpy (result, s, n);
  result[n] = '\0';

  __mf_register (result, CLAMPADD(n,1), __MF_TYPE_HEAP_I, "strndup region");
  return result;
}
#endif

#ifdef WRAP_strchr
WRAPPER2(char *, strchr, const char *s, int c)
{
  size_t n;

  n = strlen (s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(n,1), __MF_CHECK_READ, "strchr region");
  return strchr (s, c);
}
#endif

#ifdef WRAP_strrchr
WRAPPER2(char *, strrchr, const char *s, int c)
{
  size_t n;

  n = strlen (s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(n,1), __MF_CHECK_READ, "strrchr region");
  return strrchr (s, c);
}
#endif

#ifdef WRAP_strstr
WRAPPER2(char *, strstr, const char *haystack, const char *needle)
{
  size_t haystack_sz;
  size_t needle_sz;

  haystack_sz = strlen (haystack);
  needle_sz = strlen (needle);
  MF_VALIDATE_EXTENT(haystack, CLAMPADD(haystack_sz, 1), __MF_CHECK_READ, "strstr haystack");
  MF_VALIDATE_EXTENT(needle, CLAMPADD(needle_sz, 1), __MF_CHECK_READ, "strstr needle");
  return strstr (haystack, needle);
}
#endif

#ifdef WRAP_memmem
WRAPPER2(void *, memmem, 
	const void *haystack, size_t haystacklen,
	const void *needle, size_t needlelen)
{
  MF_VALIDATE_EXTENT(haystack, haystacklen, __MF_CHECK_READ, "memmem haystack");
  MF_VALIDATE_EXTENT(needle, needlelen, __MF_CHECK_READ, "memmem needle");
  return memmem (haystack, haystacklen, needle, needlelen);
}
#endif

#ifdef WRAP_strlen
WRAPPER2(size_t, strlen, const char *s)
{
  size_t result = strlen (s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(result, 1), __MF_CHECK_READ, "strlen region");
  return result;
}
#endif

#ifdef WRAP_strnlen
WRAPPER2(size_t, strnlen, const char *s, size_t n)
{
  size_t result = strnlen (s, n);
  MF_VALIDATE_EXTENT(s, result, __MF_CHECK_READ, "strnlen region");
  return result;
}
#endif

#ifdef WRAP_bzero
WRAPPER2(void, bzero, void *s, size_t n)
{
  MF_VALIDATE_EXTENT(s, n, __MF_CHECK_WRITE, "bzero region");
  bzero (s, n);
}
#endif

#ifdef WRAP_bcopy
#undef bcopy
WRAPPER2(void, bcopy, const void *src, void *dest, size_t n)
{
  MF_VALIDATE_EXTENT(src, n, __MF_CHECK_READ, "bcopy src");
  MF_VALIDATE_EXTENT(dest, n, __MF_CHECK_WRITE, "bcopy dest");
  bcopy (src, dest, n);
}
#endif

#ifdef WRAP_bcmp
#undef bcmp
WRAPPER2(int, bcmp, const void *s1, const void *s2, size_t n)
{
  MF_VALIDATE_EXTENT(s1, n, __MF_CHECK_READ, "bcmp 1st arg");
  MF_VALIDATE_EXTENT(s2, n, __MF_CHECK_READ, "bcmp 2nd arg");
  return bcmp (s1, s2, n);
}
#endif

#ifdef WRAP_index
WRAPPER2(char *, index, const char *s, int c)
{
  size_t n = strlen (s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(n, 1), __MF_CHECK_READ, "index region");
  return index (s, c);
}
#endif

#ifdef WRAP_rindex
WRAPPER2(char *, rindex, const char *s, int c)
{
  size_t n = strlen (s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(n, 1), __MF_CHECK_READ, "rindex region");
  return rindex (s, c);
}
#endif

/* XXX:  stpcpy, memccpy */


/* XXX: *printf,*scanf */


/* XXX: setjmp, longjmp */

#ifdef WRAP_asctime
WRAPPER2(char *, asctime, struct tm *tm)
{
  static char *reg_result = NULL;
  char *result;
  MF_VALIDATE_EXTENT(tm, sizeof (struct tm), __MF_CHECK_READ, "asctime tm");
  result = asctime (tm);
  if (reg_result == NULL)
    {
      __mf_register (result, strlen (result)+1, __MF_TYPE_STATIC, "asctime string");
      reg_result = result;
    }
  return result;
}
#endif

#ifdef WRAP_ctime
WRAPPER2(char *, ctime, const time_t *timep)
{
  static char *reg_result = NULL;
  char *result;
  MF_VALIDATE_EXTENT(timep, sizeof (time_t), __MF_CHECK_READ, "ctime time");
  result = ctime (timep);
  if (reg_result == NULL)
    {
      /* XXX: what if asctime and ctime return the same static ptr? */
      __mf_register (result, strlen (result)+1, __MF_TYPE_STATIC, "ctime string");
      reg_result = result;
    }
  return result;
}
#endif


#ifdef WRAP_localtime
WRAPPER2(struct tm*, localtime, const time_t *timep)
{
  static struct tm *reg_result = NULL;
  struct tm *result;
  MF_VALIDATE_EXTENT(timep, sizeof (time_t), __MF_CHECK_READ, "localtime time");
  result = localtime (timep);
  if (reg_result == NULL)
    {
      __mf_register (result, sizeof (struct tm), __MF_TYPE_STATIC, "localtime tm");
      reg_result = result;
    }
  return result;
}
#endif

#ifdef WRAP_gmtime
WRAPPER2(struct tm*, gmtime, const time_t *timep)
{
  static struct tm *reg_result = NULL;
  struct tm *result;
  MF_VALIDATE_EXTENT(timep, sizeof (time_t), __MF_CHECK_READ, "gmtime time");
  result = gmtime (timep);
  if (reg_result == NULL)
    {
      __mf_register (result, sizeof (struct tm), __MF_TYPE_STATIC, "gmtime tm");
      reg_result = result;
    }
  return result;
}
#endif


/* ------------------------------------------------------------------------ */

#ifdef WRAP_pthreadstuff
#ifndef LIBMUDFLAPTH
#error "pthreadstuff is to be included only in libmudflapth"
#endif


/* Describes a thread (dead or alive). */
struct pthread_info
{
  short used_p;  /* Is this slot in use?  */

  pthread_t self; /* The thread id.  */
  short dead_p;  /* Has thread died?  */

  /* The user's thread entry point and argument.  */
  void * (*user_fn)(void *);
  void *user_arg;

  /* If libmudflapth allocated the stack, store its base/size.  */
  void *stack;
  size_t stack_size;
};


/* To avoid dynamic memory allocation, use static array.
   This should be defined in <limits.h>.  */
#ifndef PTHREAD_THREADS_MAX
#define PTHREAD_THREADS_MAX 1000
#endif

static struct pthread_info __mf_pthread_info[PTHREAD_THREADS_MAX];
/* XXX: needs a lock */


static void 
__mf_pthread_cleanup (void *arg)
{
  struct pthread_info *pi = arg;
  pi->dead_p = 1;
  /* Some subsequent pthread_create will garbage_collect our stack.  */
}



static void *
__mf_pthread_spawner (void *arg)
{
  struct pthread_info *pi = arg;
  void *result = NULL;

  /* XXX: register thread errno */
  pthread_cleanup_push (& __mf_pthread_cleanup, arg);

  pi->self = pthread_self ();

  /* Call user thread */
  result = pi->user_fn (pi->user_arg);

  pthread_cleanup_pop (1 /* execute */);

  /* NB: there is a slight race here.  The pthread_info field will now
     say this thread is dead, but it may still be running .. right
     here.  We try to check for this possibility using the
     pthread_signal test below. */
  /* XXX: Consider using pthread_key_t objects instead of cleanup
     stacks. */

  return result;
}


#if PIC
/* A special bootstrap variant. */
static int
__mf_0fn_pthread_create (pthread_t *thr, pthread_attr_t *attr, 
			 void * (*start) (void *), void *arg)
{
  return -1;
}
#endif


#undef pthread_create
WRAPPER(int, pthread_create, pthread_t *thr, const pthread_attr_t *attr, 
	 void * (*start) (void *), void *arg)
{
  DECLARE(void, free, void *p);
  DECLARE(void *, malloc, size_t c);
  DECLARE(int, pthread_create, pthread_t *thr, const pthread_attr_t *attr, 
	  void * (*start) (void *), void *arg);
  int result;
  struct pthread_info *pi;
  pthread_attr_t override_attr;
  void *override_stack;
  size_t override_stacksize;
  unsigned i;

  TRACE ("mf: pthread_create\n");

  LOCKTH();

  /* Garbage collect dead thread stacks.  */
  for (i = 0; i < PTHREAD_THREADS_MAX; i++)
    {
      pi = & __mf_pthread_info [i];
      if (pi->used_p && pi->dead_p 
	  && !pthread_kill (pi->self, 0)) /* Really dead?  XXX: safe?  */ 
	{
	  if (pi->stack != NULL)
	    CALL_REAL (free, pi->stack);

	  pi->stack = NULL;
	  pi->stack_size = 0;
	  pi->used_p = 0;
	}
    }

  /* Find a slot in __mf_pthread_info to track this thread.  */
  for (i = 0; i < PTHREAD_THREADS_MAX; i++)
    {
      pi = & __mf_pthread_info [i];
      if (! pi->used_p)
	{
	  pi->used_p = 1;
	  break;
	}
    }
  UNLOCKTH();

  if (i == PTHREAD_THREADS_MAX) /* no slots free - simulated out-of-memory.  */
    {
      errno = EAGAIN;
      pi->used_p = 0;
      return -1;
    }

  /* Let's allocate a stack for this thread, if one is not already
     supplied by the caller.  We don't want to let e.g. the
     linuxthreads manager thread do this allocation.  */
  if (attr != NULL)
    override_attr = *attr;
  else
    pthread_attr_init (& override_attr);

  /* Get supplied attributes.  Give up on error.  */
  if (pthread_attr_getstackaddr (& override_attr, & override_stack) != 0 ||
      pthread_attr_getstacksize (& override_attr, & override_stacksize) != 0)
    {
      errno = EAGAIN;
      pi->used_p = 0;
      return -1;
    }

  /* Do we need to allocate the new thread's stack?  */
  if (override_stack == NULL)
    {
      uintptr_t alignment = 256; /* Must be a power of 2.  */

      /* Use glibc x86 defaults */
      if (override_stacksize < alignment)
/* Should have been defined in <limits.h> */
#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 65536
#endif
	override_stacksize = max (PTHREAD_STACK_MIN, 2 * 1024 * 1024);

      override_stack = CALL_REAL (malloc, override_stacksize);
      if (override_stack == NULL)
	{
	  errno = EAGAIN;
	  pi->used_p = 0;
	  return -1;
	}

      pi->stack = override_stack;
      pi->stack_size = override_stacksize;

      /* The stackaddr pthreads attribute is a candidate stack pointer.
	 It must point near the top or the bottom of this buffer, depending
	 on whether stack grows downward or upward, and suitably aligned.
	 On the x86, it grows down, so we set stackaddr near the top.  */
      override_stack = (void *)
	(((uintptr_t) override_stack + override_stacksize - alignment)
	 & (~(uintptr_t)(alignment-1)));
      
      if (pthread_attr_setstackaddr (& override_attr, override_stack) != 0 ||
	  pthread_attr_setstacksize (& override_attr, override_stacksize) != 0)
	{
	  /* Er, what now?  */
	  CALL_REAL (free, pi->stack);
	  pi->stack = NULL;
	  errno = EAGAIN;
	  pi->used_p = 0;
	  return -1;
	}

  }

  /* Fill in remaining fields.  */
  pi->user_fn = start;
  pi->user_arg = arg;
  pi->dead_p = 0;

  /* Actually create the thread.  */
  result = CALL_REAL (pthread_create, thr, & override_attr,
		      & __mf_pthread_spawner, (void *) pi);
  
  /* May need to clean up if we created a pthread_attr_t of our own.  */
  if (attr == NULL)
    pthread_attr_destroy (& override_attr); /* NB: this shouldn't deallocate stack */

  return result;
}


#endif /* pthreadstuff */
