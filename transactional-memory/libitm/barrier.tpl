/* Copyright (C) 2008, 2009 Free Software Foundation, Inc.
   Contributed by Richard Henderson <rth@redhat.com>.

   This file is part of the GNU Transactional Memory Library (libitm).

   Libitm is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   Libitm is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
   more details.

   Under Section 7 of GPL version 3, you are granted additional
   permissions described in the GCC Runtime Library Exception, version
   3.1, as published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License and
   a copy of the GCC Runtime Library Exception along with this program;
   see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
   <http://www.gnu.org/licenses/>.  */

#include "unaligned.h"

namespace {

using namespace GTM;

template<typename T>
T do_read (const T *ptr, gtm_dispatch::lock_type lock)
{
  gtm_dispatch *disp = gtm_disp();
  uintptr_t iptr = reinterpret_cast<uintptr_t>(ptr);
  uintptr_t iline = iptr & -CACHELINE_SIZE;
  uintptr_t iofs = iptr & (CACHELINE_SIZE - 1);
  const gtm_cacheline *pline = reinterpret_cast<const gtm_cacheline *>(iline);
  const gtm_cacheline *line = disp->read_lock(pline, lock);

  ptr = reinterpret_cast<const T *>(&line->b[iofs]);

  if (__builtin_expect (strict_alignment<T>::value
			? (iofs & (sizeof (T) - 1)) == 0
			: iofs + sizeof(T) <= CACHELINE_SIZE, 1))
    {
    do_normal_load:
      return *ptr;
    }
  else if (__builtin_expect (strict_alignment<T>::value
			     && iofs + sizeof(T) <= CACHELINE_SIZE, 1))
    {
    do_unaligned_load:
      return unaligned_load<T>(ptr);
    }
  else
    {
      const gtm_cacheline *line2 = disp->read_lock(pline + 1, lock);

      if (line2 == line + 1)
	{
	  if (!strict_alignment<T>::value)
	    goto do_normal_load;
	  else
	    goto do_unaligned_load;
	}
      else
	return unaligned_load2<T>(line, line2, iofs);
    }
}

template<typename T>
void do_write (T *ptr, T val, gtm_dispatch::lock_type lock)
{
  gtm_dispatch *disp = gtm_disp();
  uintptr_t iptr = reinterpret_cast<uintptr_t>(ptr);
  uintptr_t iline = iptr & -CACHELINE_SIZE;
  uintptr_t iofs = iptr & (CACHELINE_SIZE - 1);
  gtm_cacheline *pline = reinterpret_cast<gtm_cacheline *>(iline);
  gtm_cacheline_mask m = ((gtm_cacheline_mask)2 << (sizeof(T) - 1)) - 1;
  gtm_dispatch::mask_pair pair = disp->write_lock(pline, lock);

  ptr = reinterpret_cast<T *>(&pair.line->b[iofs]);

  if (__builtin_expect (strict_alignment<T>::value
			? (iofs & (sizeof (val) - 1)) == 0
			: iofs + sizeof(val) <= CACHELINE_SIZE, 1))
    {
      *pair.mask |= m << iofs;
    do_normal_store:
      *ptr = val;
    }
  else if (__builtin_expect (strict_alignment<T>::value
			     && iofs + sizeof(val) <= CACHELINE_SIZE, 1))
    {
      *pair.mask |= m << iofs;
    do_unaligned_store:
      unaligned_store<T>(ptr, val);
    }
  else
    {
      *pair.mask |= m << iofs;
      gtm_dispatch::mask_pair pair2 = disp->write_lock(pline + 1, lock);

      uintptr_t ileft = CACHELINE_SIZE - iofs;
      *pair2.mask |= m >> ileft;

      if (pair2.line == pair.line + 1)
	{
	  if (!strict_alignment<T>::value)
	    goto do_normal_store;
	  else
	    goto do_unaligned_store;
	}
      else
	unaligned_store2<T>(pair.line, pair2.line, iofs, val);
    }
}

} /* anonymous namespace */

#define ITM_READ(T, LOCK)						\
  _ITM_TYPE_##T ITM_REGPARM _ITM_##LOCK##T (const _ITM_TYPE_##T *ptr)	\
  { return do_read (ptr, gtm_dispatch::LOCK); }

#define ITM_WRITE(T, LOCK)						\
  void ITM_REGPARM _ITM_##LOCK##T (_ITM_TYPE_##T *ptr, _ITM_TYPE_##T val) \
  { do_write (ptr, val, gtm_dispatch::LOCK); }

#define ITM_BARRIERS(T)		\
  ITM_READ(T, R)		\
  ITM_READ(T, RaR)		\
  ITM_READ(T, RaW)		\
  ITM_READ(T, RfW)		\
  ITM_WRITE(T, W)		\
  ITM_WRITE(T, WaR)		\
  ITM_WRITE(T, WaW)
