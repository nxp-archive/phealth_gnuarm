/* Mudflap: narrow-pointer bounds-checking by tree rewriting.
   Copyright (C) 2002 Free Software Foundation, Inc.
   Contributed by Frank Ch. Eigler <fche@redhat.com>
   and Graydon Hoare <graydon@redhat.com>

This file is part of GCC.
XXX: libgcc license?
*/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <execinfo.h>
#include <assert.h>

#include "mf-runtime.h"
#include "mf-impl.h"

#ifdef _MUDFLAP
#error "Do not compile this file with -fmudflap!"
#endif

#define MF_VALIDATE_EXTENT(value,size,context)                \
 {                                                            \
  if (UNLIKELY (__MF_CACHE_MISS_P (value, size)))             \
    {                                                         \
    enum __mf_state resume_state = old_state;                 \
    __mf_state = old_state;                                   \
    __mf_check ((unsigned)value, size, __FILE__ ":" STRINGIFY(__LINE__) " (" context ")");  \
    old_state = resume_state;                                 \
    }                                                         \
 }



#define BEGIN_PROTECT(ty, fname, ...)       \
  ty result;                                \
  enum __mf_state old_state;                \
  if (UNLIKELY (__mf_state != active))      \
  {                                         \
    return CALL_REAL(fname, __VA_ARGS__);   \
  }                                         \
  else                                      \
  {                                         \
     TRACE ("mf: %s\n", __PRETTY_FUNCTION__); \
     old_state = __mf_state;                \
     __mf_state = reentrant;                \
  }

#define END_PROTECT(ty, fname, ...)              \
  result = (ty) CALL_REAL(fname, __VA_ARGS__);   \
  __mf_state = old_state;                        \
  return result;



/* malloc/free etc. */

#ifdef WRAP_malloc
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
      __mf_register ((uintptr_t) result, c,
		     __MF_TYPE_HEAP, "malloc region");
    }

  return result;
}
#endif


#ifdef WRAP_calloc
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
    CALL_REAL(memset, result, 0, size_with_crumple_zones);
  
  __mf_state = old_state;
  
  if (LIKELY(result))
    {
      result += __mf_opts.crumple_zone;
      __mf_register ((uintptr_t) result, c*n, /* XXX: CLAMPMUL */
		     __MF_TYPE_HEAP, "calloc region");
    }
  
  return result;
}
#endif

#ifdef WRAP_realloc
WRAPPER(void *, realloc, void *buf, size_t c)
{
  DECLARE(void * , realloc, void *, size_t);
  size_t size_with_crumple_zones;
  char *base = buf;

  BEGIN_PROTECT (char *, realloc, buf, c);

  if (LIKELY(buf))
    base -= __mf_opts.crumple_zone;

  size_with_crumple_zones = 
    CLAMPADD(c, CLAMPADD(__mf_opts.crumple_zone,
			 __mf_opts.crumple_zone));
  result = (char *) CALL_REAL(realloc, base, size_with_crumple_zones);

  __mf_state = old_state;      

  if (LIKELY(buf))
    __mf_unregister ((uintptr_t) buf, 0);
  
  if (LIKELY(result))
    {
      result += __mf_opts.crumple_zone;
      __mf_register ((uintptr_t) result, c, 
		     __MF_TYPE_HEAP, "realloc region");
    }

  return result;
}
#endif


#ifdef WRAP_free
WRAPPER(void, free, void *buf)
{
  /* Use a circular queue to delay some number (__mf_opts.free_queue_length) of free()s.  */
  static void *free_queue [__MF_FREEQ_MAX];
  static unsigned free_ptr = 0;
  static int freeq_initialized = 0;
  enum __mf_state old_state;
  DECLARE(void * , free, void *);  
  DECLARE(void *, memset, void *s, int c, size_t n);

  if (UNLIKELY (__mf_state != active))
    {
      CALL_REAL(free, buf);
      return;
    }

  if (UNLIKELY(!freeq_initialized))
    {
      CALL_REAL(memset, free_queue, 0, 
		     __MF_FREEQ_MAX * sizeof (void *));
      freeq_initialized = 1;
    }

  if (UNLIKELY(buf == NULL))
    return;

  TRACE ("mf: %s\n", __PRETTY_FUNCTION__);

  __mf_unregister ((uintptr_t) buf, 0);

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


#ifdef WRAP_dlopen
WRAPPER(void *, dlopen, const char *filename, int flag)
{
  DECLARE(void * , dlopen, const char *filename, int flag);
  BEGIN_PROTECT(void *, dlopen, filename, flag);
  result = CALL_REAL(dlopen, filename, flag);
  __mf_state = old_state;
  return result;
}
#endif


#ifdef WRAP_mmap
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

  __mf_state = old_state;

  if ((uintptr_t)result != -1)
    {
      __mf_register ((uintptr_t) result, 
		     (uintptr_t) CLAMPADD(((uintptr_t)result),
					  ((uintptr_t)
					   CLAMPSUB(((uintptr_t)length),
						    ((uintptr_t)1)))),
		     __MF_TYPE_GUESS, 
		     "(heuristic) mmap region");
    }

  return result;
}
#endif


#ifdef WRAP_munmap
WRAPPER(int , munmap, void *start, size_t length)
{
  DECLARE(int, munmap, void *, size_t);
  BEGIN_PROTECT(int, munmap, start, length);
  
  result = CALL_REAL(munmap, start, length);

  __mf_state = old_state;
  __mf_unregister ((uintptr_t)start, length);
  return result;
}
#endif


/* This wrapper is a little different, as it's implemented in terms
   of the wrapped malloc/free functions. */
#ifdef WRAP_alloca
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



/* str*,mem*,b* */

#ifdef WRAP_memcpy
WRAPPER(void *, memcpy, void *dest, const void *src, size_t n)
{
  DECLARE(void *, memcpy, void *dest, const void *src, size_t n);
  BEGIN_PROTECT(void *, memcpy, dest, src, n);
  MF_VALIDATE_EXTENT(src, n, "memcpy source");
  MF_VALIDATE_EXTENT(dest, n, "memcpy dest");
  END_PROTECT (void *, memcpy, dest, src, n);
}
#endif


#ifdef WRAP_memmove
WRAPPER(void *, memmove, void *dest, const void *src, size_t n)
{
  DECLARE(void *, memmove, void *dest, const void *src, size_t n);
  BEGIN_PROTECT(void *, memmove, dest, src, n);
  MF_VALIDATE_EXTENT(src, n, "memmove src");
  MF_VALIDATE_EXTENT(dest, n, "memmove dest");
  END_PROTECT(void *, memmove, dest, src, n);
}
#endif

#ifdef WRAP_memset
WRAPPER(void *, memset, void *s, int c, size_t n)
{
  DECLARE(void *, memset, void *s, int c, size_t n);
  BEGIN_PROTECT(void *, memset, s, c, n);
  MF_VALIDATE_EXTENT(s, n, "memset dest");
  END_PROTECT(void *, memset, s, c, n);
}
#endif

#ifdef WRAP_memcmp
WRAPPER(int, memcmp, const void *s1, const void *s2, size_t n)
{
  DECLARE(int , memcmp, const void *s1, const void *s2, size_t n);
  BEGIN_PROTECT(int, memcmp, s1, s2, n);
  MF_VALIDATE_EXTENT(s1, n, "memcmp 1st arg");
  MF_VALIDATE_EXTENT(s2, n, "memcmp 2nd arg");
  END_PROTECT(int, memcmp, s1, s2, n);
}
#endif

#ifdef WRAP_memchr
WRAPPER(void *, memchr, const void *s, int c, size_t n)
{
  DECLARE(void *, memchr, const void *s, int c, size_t n);
  BEGIN_PROTECT(void *, memchr, s, c, n);
  MF_VALIDATE_EXTENT(s, n, "memchr region");
  END_PROTECT(void *, memchr, s, c, n);
}
#endif

#ifdef WRAP_memrchr
WRAPPER(void *, memrchr, const void *s, int c, size_t n)
{
  DECLARE(void *, memrchr, const void *s, int c, size_t n);
  BEGIN_PROTECT(void *, memrchr, s, c, n);
  MF_VALIDATE_EXTENT(s, n, "memrchr region");
  END_PROTECT(void *, memrchr, s, c, n);
}
#endif

#ifdef WRAP_strcpy
WRAPPER(char *, strcpy, char *dest, const char *src)
{
  DECLARE(size_t, strlen, const char *s);
  DECLARE(char *, strcpy, char *dest, const char *src);
  size_t n;

  /* nb: just because strlen(src) == n doesn't mean (src + n) or (src + n +
     1) are valid pointers. the allocated object might have size < n.
     check anyways. */

  BEGIN_PROTECT(char *, strcpy, dest, src); 
  TRACE("mf: strcpy %08lx -> %08lx\n", (uintptr_t) src, (uintptr_t) dest);
  n = CALL_REAL(strlen, src);
  MF_VALIDATE_EXTENT(src, CLAMPADD(n, 1), "strcpy src"); 
  MF_VALIDATE_EXTENT(dest, CLAMPADD(n, 1), "strcpy dest");
  END_PROTECT(char *, strcpy, dest, src);
}
#endif

#ifdef WRAP_strncpy
WRAPPER(char *, strncpy, char *dest, const char *src, size_t n)
{
  DECLARE(size_t, strnlen, const char *s, size_t n);
  DECLARE(char *, strncpy, char *dest, const char *src, size_t n);
  size_t len;

  BEGIN_PROTECT(char *, strncpy, dest, src, n);
  TRACE("mf: strncpy %08lx -> %08lx\n", (uintptr_t) src, (uintptr_t) dest);
  len = CALL_REAL(strnlen, src, n);
  MF_VALIDATE_EXTENT(src, len, "strncpy src");
  MF_VALIDATE_EXTENT(dest, len, "strncpy dest"); /* nb: strNcpy */
  END_PROTECT(char *, strncpy, dest, src, n);
}
#endif

#ifdef WRAP_strcat
WRAPPER(char *, strcat, char *dest, const char *src)
{
  DECLARE(size_t, strlen, const char *s);
  DECLARE(char *, strcat, char *dest, const char *src);
  size_t dest_sz;
  size_t src_sz;

  BEGIN_PROTECT(char *, strcat, dest, src);
  dest_sz = CALL_REAL(strlen, dest);
  src_sz = CALL_REAL(strlen, src);  
  MF_VALIDATE_EXTENT(src, CLAMPADD(src_sz, 1), "strcat src");
  MF_VALIDATE_EXTENT(dest, CLAMPADD(dest_sz, CLAMPADD(src_sz, 1)), "strcat dest");
  END_PROTECT(char *, strcat, dest, src);
}
#endif

#ifdef WRAP_strncat
WRAPPER(char *, strncat, char *dest, const char *src, size_t n)
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

  DECLARE(size_t, strnlen, const char *s, size_t n);
  DECLARE(char *, strncat, char *dest, const char *src, size_t n);
  size_t src_sz;
  size_t dest_sz;

  BEGIN_PROTECT(char *, strncat, dest, src, n);
  src_sz = CALL_REAL(strnlen, src, n);
  dest_sz = CALL_REAL(strnlen, dest, n);
  MF_VALIDATE_EXTENT(src, src_sz, "strncat src");
  MF_VALIDATE_EXTENT(dest, (CLAMPADD(dest_sz, CLAMPADD(src_sz, 1))), 
		     "strncat dest");
  END_PROTECT(char *, strncat, dest, src, n);
}
#endif

#ifdef WRAP_strcmp
WRAPPER(int, strcmp, const char *s1, const char *s2)
{
  DECLARE(size_t, strlen, const char *s);
  DECLARE(int, strcmp, const char *s1, const char *s2);
  size_t s1_sz;
  size_t s2_sz;

  BEGIN_PROTECT(int, strcmp, s1, s2);
  s1_sz = CALL_REAL(strlen, s1);
  s2_sz = CALL_REAL(strlen, s2);  
  MF_VALIDATE_EXTENT(s1, CLAMPADD(s1_sz, 1), "strcmp 1st arg");
  MF_VALIDATE_EXTENT(s2, CLAMPADD(s2_sz, 1), "strcmp 2nd arg");
  END_PROTECT(int, strcmp, s1, s2);
}
#endif

#ifdef WRAP_strcasecmp
WRAPPER(int, strcasecmp, const char *s1, const char *s2)
{
  DECLARE(size_t, strlen, const char *s);
  DECLARE(int, strcasecmp, const char *s1, const char *s2);
  size_t s1_sz;
  size_t s2_sz;

  BEGIN_PROTECT(int, strcasecmp, s1, s2);
  s1_sz = CALL_REAL(strlen, s1);
  s2_sz = CALL_REAL(strlen, s2);  
  MF_VALIDATE_EXTENT(s1, CLAMPADD(s1_sz, 1), "strcasecmp 1st arg");
  MF_VALIDATE_EXTENT(s2, CLAMPADD(s2_sz, 1), "strcasecmp 2nd arg");
  END_PROTECT(int, strcasecmp, s1, s2);
}
#endif

#ifdef WRAP_strncmp
WRAPPER(int, strncmp, const char *s1, const char *s2, size_t n)
{
  DECLARE(size_t, strnlen, const char *s, size_t n);
  DECLARE(int, strncmp, const char *s1, const char *s2, size_t n);
  size_t s1_sz;
  size_t s2_sz;

  BEGIN_PROTECT(int, strncmp, s1, s2, n);
  s1_sz = CALL_REAL(strnlen, s1, n);
  s2_sz = CALL_REAL(strnlen, s2, n);
  MF_VALIDATE_EXTENT(s1, s1_sz, "strncmp 1st arg");
  MF_VALIDATE_EXTENT(s2, s2_sz, "strncmp 2nd arg");
  END_PROTECT(int, strncmp, s1, s2, n);
}
#endif

#ifdef WRAP_strncasecmp
WRAPPER(int, strncasecmp, const char *s1, const char *s2, size_t n)
{
  DECLARE(size_t, strnlen, const char *s, size_t n);
  DECLARE(int, strncasecmp, const char *s1, const char *s2, size_t n);
  size_t s1_sz;
  size_t s2_sz;

  BEGIN_PROTECT(int, strncasecmp, s1, s2, n);
  s1_sz = CALL_REAL(strnlen, s1, n);
  s2_sz = CALL_REAL(strnlen, s2, n);
  MF_VALIDATE_EXTENT(s1, s1_sz, "strncasecmp 1st arg");
  MF_VALIDATE_EXTENT(s2, s2_sz, "strncasecmp 2nd arg");
  END_PROTECT(int, strncasecmp, s1, s2, n);
}
#endif

#ifdef WRAP_strdup
WRAPPER(char *, strdup, const char *s)
{
  DECLARE(size_t, strlen, const char *s);
  DECLARE(char *, strdup, const char *s);
  DECLARE(void *, malloc, size_t sz);
  DECLARE(void *, memcpy, void *dest, const void *src, size_t n);
  size_t n;

  BEGIN_PROTECT(char *, strdup, s);
  n = CALL_REAL(strlen, s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(n,1), "strdup region");

  result = (char *)CALL_REAL(malloc, 
			     CLAMPADD(CLAMPADD(n,1),
				      CLAMPADD(__mf_opts.crumple_zone,
					       __mf_opts.crumple_zone)));

  if (UNLIKELY(!result))
    {
      __mf_state = old_state;
      return result;
    }

  result += __mf_opts.crumple_zone;
  CALL_REAL(memcpy, result, s, n);
  result[n] = '\0';

  __mf_register ((uintptr_t) result, CLAMPADD(n,1), 
		 __MF_TYPE_HEAP, "strdup region");
  __mf_state = old_state;
  return result;
}
#endif

#ifdef WRAP_strndup
WRAPPER(char *, strndup, const char *s, size_t n)
{
  DECLARE(size_t, strnlen, const char *s, size_t n);
  DECLARE(char *, strndup, const char *s, size_t n);
  DECLARE(void *, malloc, size_t sz);
  DECLARE(void *, memcpy, void *dest, const void *src, size_t n);
  size_t sz;

  BEGIN_PROTECT(char *, strndup, s, n);
  sz = CALL_REAL(strnlen, s, n);
  MF_VALIDATE_EXTENT(s, sz, "strndup region"); /* nb: strNdup */

  /* note: strndup still adds a \0, even with the N limit! */
  result = (char *)CALL_REAL(malloc, 
			     CLAMPADD(CLAMPADD(n,1),
				      CLAMPADD(__mf_opts.crumple_zone,
					       __mf_opts.crumple_zone)));
  
  if (UNLIKELY(!result))
    {
      __mf_state = old_state;
      return result;
    }

  result += __mf_opts.crumple_zone;
  CALL_REAL(memcpy, result, s, n);
  result[n] = '\0';

  __mf_register ((uintptr_t) result, CLAMPADD(n,1), 
		 __MF_TYPE_HEAP, "strndup region");
  __mf_state = old_state;
  return result;
}
#endif

#ifdef WRAP_strchr
WRAPPER(char *, strchr, const char *s, int c)
{
  DECLARE(size_t, strlen, const char *s);
  DECLARE(char *, strchr, const char *s, int c);
  size_t n;

  BEGIN_PROTECT(char *, strchr, s, c);
  n = CALL_REAL(strlen, s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(n,1), "strchr region");
  END_PROTECT(char *, strchr, s, c);
}
#endif

#ifdef WRAP_strrchr
WRAPPER(char *, strrchr, const char *s, int c)
{
  DECLARE(size_t, strlen, const char *s);
  DECLARE(char *, strrchr, const char *s, int c);
  size_t n;

  BEGIN_PROTECT(char *, strrchr, s, c);
  n = CALL_REAL(strlen, s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(n,1), "strrchr region");
  END_PROTECT(char *, strrchr, s, c);
}
#endif

#ifdef WRAP_strstr
WRAPPER(char *, strstr, const char *haystack, const char *needle)
{
  DECLARE(size_t, strlen, const char *s);
  DECLARE(char *, strstr, const char *haystack, const char *needle);
  size_t haystack_sz;
  size_t needle_sz;

  BEGIN_PROTECT(char *, strstr, haystack, needle);
  haystack_sz = CALL_REAL(strlen, haystack);
  needle_sz = CALL_REAL(strlen, needle);
  MF_VALIDATE_EXTENT(haystack, CLAMPADD(haystack_sz, 1), "strstr haystack");
  MF_VALIDATE_EXTENT(needle, CLAMPADD(needle_sz, 1), "strstr needle");
  END_PROTECT(char *, strstr, haystack, needle);
}
#endif

#ifdef WRAP_memmem
WRAPPER(void *, memmem, 
	const void *haystack, size_t haystacklen,
	const void *needle, size_t needlelen)
{
  DECLARE(void *, memmem, const void *haystack, size_t haystacklen,
			      const void *needle, size_t needlelen);

  BEGIN_PROTECT(void *, memmem, haystack, haystacklen, needle, needlelen);
  MF_VALIDATE_EXTENT(haystack, haystacklen, "memmem haystack");
  MF_VALIDATE_EXTENT(needle, needlelen, "memmem needle");
  END_PROTECT(char *, memmem, haystack, haystacklen, needle, needlelen);
}
#endif

#ifdef WRAP_strlen
WRAPPER(size_t, strlen, const char *s)
{
  DECLARE(size_t, strlen, const char *s);

  BEGIN_PROTECT(size_t, strlen, s);
  result = CALL_REAL(strlen, s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(result, 1), "strlen region");
  __mf_state = old_state;
  return result;
}
#endif

#ifdef WRAP_strnlen
WRAPPER(size_t, strnlen, const char *s, size_t n)
{
  DECLARE(size_t, strnlen, const char *s, size_t n);

  BEGIN_PROTECT(size_t, strnlen, s, n);
  result = CALL_REAL(strnlen, s, n);
  MF_VALIDATE_EXTENT(s, result, "strnlen region");
  __mf_state = old_state;
  return result;
}
#endif

#ifdef WRAP_bzero
WRAPPER(void, bzero, void *s, size_t n)
{
  DECLARE(void , bzero, void *s, size_t n);
  enum __mf_state old_state;

  if (UNLIKELY (__mf_state != active))
    {
      CALL_REAL(bzero, s, n);
      return;
    }
  TRACE ("mf: %s\n", __PRETTY_FUNCTION__);
  old_state = __mf_state;
  __mf_state = reentrant;
  MF_VALIDATE_EXTENT(s, n, "bzero region");
  CALL_REAL(bzero, s, n);
  __mf_state = old_state;
}
#endif

#ifdef WRAP_bcopy
WRAPPER(void, bcopy, const void *src, void *dest, size_t n)
{
  DECLARE(void , bcopy, const void *src, void *dest, size_t n);
  enum __mf_state old_state;

  if (UNLIKELY (__mf_state != active))
    {
      CALL_REAL(bcopy, src, dest, n);
      return;
    }
  TRACE ("mf: %s\n", __PRETTY_FUNCTION__);
  old_state = __mf_state;
  __mf_state = reentrant;
  MF_VALIDATE_EXTENT(src, n, "bcopy src");
  MF_VALIDATE_EXTENT(dest, n, "bcopy dest");
  CALL_REAL(bcopy, src, dest, n);
  __mf_state = old_state;
}
#endif

#ifdef WRAP_bcmp
WRAPPER(int, bcmp, const void *s1, const void *s2, size_t n)
{
  DECLARE(int , bcmp, const void *s1, const void *s2, size_t n);

  BEGIN_PROTECT(int, bcmp, s1, s2, n);
  MF_VALIDATE_EXTENT(s1, n, "bcmp 1st arg");
  MF_VALIDATE_EXTENT(s2, n, "bcmp 2nd arg");
  END_PROTECT(int, bcmp, s1, s2, n);
}
#endif

#ifdef WRAP_index
WRAPPER(char *, index, const char *s, int c)
{
  DECLARE(size_t, strlen, const char *s);
  DECLARE(char *, index, const char *s, int c);
  size_t n;

  BEGIN_PROTECT(char *, index, s, c);
  n = CALL_REAL(strlen, s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(n, 1), "index region");
  END_PROTECT(char *, index, s, c);
}
#endif

#ifdef WRAP_rindex
WRAPPER(char *, rindex, const char *s, int c)
{
  DECLARE(size_t, strlen, const char *s);
  DECLARE(char *, rindex, const char *s, int c);
  size_t n;

  BEGIN_PROTECT(char *, rindex, s, c);
  n = CALL_REAL(strlen, s);
  MF_VALIDATE_EXTENT(s, CLAMPADD(n, 1), "rindex region");
  END_PROTECT(char *, rindex, s, c);
}
#endif

/* *printf,*scanf */



/* ------------------------------------------------------------------------ */
