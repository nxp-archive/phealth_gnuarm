/* Copyright (C) 2008 Free Software Foundation, Inc.
   Contributed by Richard Henderson <rth@redhat.com>.

   This file is part of the GNU Transactional Memory Library (libitm).

   Libgomp is free software; you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version.

   Libgomp is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
   more details.

   You should have received a copy of the GNU Lesser General Public License 
   along with libgomp; see the file COPYING.LIB.  If not, write to the
   Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA.  */

/* As a special exception, if you link this library with other files, some
   of which are compiled with GCC, to produce an executable, this library
   does not by itself cause the resulting executable to be covered by the
   GNU General Public License.  This exception does not however invalidate
   any other reasons why the executable file might be covered by the GNU
   General Public License.  */

#include <limits.h>
#include "libitm.h"
#include "futex.h"


/* Lock the summary bit on LOCK.  Return the contents of the summary word
   (without the summary lock bit included).  */

static int REGPARM
rwlock_lock_summary (gtm_rwlock *lock)
{
  int o;

 restart:
  o = __sync_fetch_and_or (&lock->summary, RWLOCK_S_LOCK);
  if (o & RWLOCK_S_LOCK)
    {
      do
	cpu_relax ();
      while (lock->summary & RWLOCK_S_LOCK);
      goto restart;
    }

  return o;
}


/* Acquire a RW lock for reading.  */

void REGPARM
gtm_rwlock_read_lock (gtm_rwlock *lock)
{
  int o, n;

  while (1)
    {
      o = rwlock_lock_summary (lock);

      /* If there is an active or waiting writer, then new readers
	 must wait.  Increment the waiting reader count, then wait
	 on the reader queue.  */
      if (o & (RWLOCK_A_WRITER | RWLOCK_W_WRITER | RWLOCK_RW_UPGRADE))
	{
	  n = ++lock->w_readers;
	  atomic_write_barrier ();
	  lock->summary = o | RWLOCK_W_READER;
	  futex_wait (&lock->w_readers, n);
	  continue;
	}

      /* Otherwise, we may become a reader.  */
      ++lock->a_readers;
      atomic_write_barrier ();
      lock->summary = o | RWLOCK_A_READER;
      return;
    }
}


/* Acquire a RW lock for writing.  */

void REGPARM
gtm_rwlock_write_lock (gtm_rwlock *lock)
{
  int o, n;

 restart:
  o = lock->summary;

  /* If anyone is manipulating the summary lock, the rest of the
     data structure is volatile.  */
  if (o & RWLOCK_S_LOCK)
    {
      cpu_relax ();
      goto restart;
    }

  /* If there is an active reader or active writer, then new writers
     must wait.  Increment the waiting writer count, then wait
     on the writer queue.  */
  if (o & (RWLOCK_A_WRITER | RWLOCK_A_READER | RWLOCK_RW_UPGRADE))
    {
      /* Grab the summary lock.  We'll need it for incrementing
	 the waiting reader.  */
      n = o | RWLOCK_S_LOCK;
      if (!__sync_bool_compare_and_swap (&lock->summary, o, n))
	goto restart;

      n = ++lock->w_writers;
      atomic_write_barrier ();
      lock->summary = o | RWLOCK_W_WRITER;
      futex_wait (&lock->w_writers, n);
      goto restart;
    }

  /* Otherwise, may become a writer.  */
  n = o | RWLOCK_A_WRITER;
  if (!__sync_bool_compare_and_swap (&lock->summary, o, n))
    goto restart;
}


/* Upgrade a RW lock that has been locked for reading to a writing lock.
   Do this without possibility of another writer incoming.  Return false
   if this attempt fails.  */

bool REGPARM
gtm_rwlock_write_upgrade (gtm_rwlock *lock)
{
  int o, n;

 restart:
  o = lock->summary;

  /* If anyone is manipulating the summary lock, the rest of the
     data structure is volatile.  */
  if (o & RWLOCK_S_LOCK)
    {
      cpu_relax ();
      goto restart;
    }

  /* If there's already someone trying to upgrade, then we fail.  */
  if (o & RWLOCK_RW_UPGRADE)
    return false;

  /* Grab the summary lock.  We'll need it for manipulating the
     active reader count or the waiting writer count.  */
  n = o | RWLOCK_S_LOCK;
  if (!__sync_bool_compare_and_swap (&lock->summary, o, n))
    goto restart;

  /* If there are more active readers, then we have to wait.  */
  if (--lock->a_readers > 0)
    {
      atomic_write_barrier ();
      o |= RWLOCK_RW_UPGRADE;
      lock->summary = o;
      do
	{
	  futex_wait (&lock->summary, o);
	  o = lock->summary;
	}
      while (o & RWLOCK_A_READER);
    }

  atomic_write_barrier ();
  o &= ~(RWLOCK_A_READER | RWLOCK_RW_UPGRADE);
  o |= RWLOCK_A_WRITER;
  lock->summary = o;
  return true;
}


/* Release a RW lock from reading.  */

void REGPARM
gtm_rwlock_read_unlock (gtm_rwlock *lock)
{
  int o;

  o = rwlock_lock_summary (lock);

  /* If there are still active readers, nothing else to do.  */
  if (--lock->a_readers > 0)
    {
      atomic_write_barrier ();
      lock->summary = o;
      return;
    }
  o &= ~RWLOCK_A_READER;

  /* If there is a waiting upgrade, wake it.  */
  if (o & RWLOCK_RW_UPGRADE)
    {
      atomic_write_barrier ();
      lock->summary = o;
      futex_wake (&lock->summary, 1);
      return;
    }

  /* If there is a waiting writer, wake it.  */
  if (o & RWLOCK_W_WRITER)
    {
      if (--lock->w_writers == 0)
	o &= ~RWLOCK_W_WRITER;
      atomic_write_barrier ();
      lock->summary = o;
      futex_wake (&lock->w_writers, 1);
      return;
    }

  atomic_write_barrier ();
  lock->summary = o;
}


/* Release a RW lock from writing.  */

void REGPARM
gtm_rwlock_write_unlock (gtm_rwlock *lock)
{
  int o;

  o = rwlock_lock_summary (lock);
  o &= ~RWLOCK_A_WRITER;

  /* If there is a waiting writer, wake it.  */
  if (o & RWLOCK_W_WRITER)
    {
      if (--lock->w_writers == 0)
	o &= ~RWLOCK_W_WRITER;
      atomic_write_barrier ();
      lock->summary = o;
      futex_wake (&lock->w_writers, 1);
      return;
    }

  /* If there are waiting readers, wake them.  */
  if (o & RWLOCK_W_READER)
    {
      lock->w_readers = 0;
      atomic_write_barrier ();
      lock->summary = o & ~RWLOCK_W_READER;
      futex_wake (&lock->w_readers, INT_MAX);
      return;
    }

  lock->summary = o;
}
