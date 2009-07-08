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

#include "libitm.h"


__thread struct gtm_thread _gtm_thr;
gtm_rwlock gtm_serial_lock;

/* ??? Move elsewhere when we figure out library initialization.  */
unsigned long long gtm_spin_count_var = 1000;

static _ITM_transactionId_t global_tid;

/* Allocate a transaction structure.  Reuse an old one if possible.  */

static struct gtm_transaction *
alloc_tx (void)
{
  struct gtm_transaction *tx;
  struct gtm_thread *thr = gtm_thr ();

  if (thr->free_tx_count == 0)
    tx = malloc (sizeof (*tx));
  else
    {
      thr->free_tx_count--;
      tx = thr->free_tx[thr->free_tx_idx];
      thr->free_tx_idx = (thr->free_tx_idx + 1) % MAX_FREE_TX;
    }

  return tx;
}

/* Queue a transaction structure for freeing.  We never free the given
   transaction immediately -- this is a requirement of abortTransaction
   as the jmpbuf is used immediately after calling this function.  Thus
   the requirement that this queue be per-thread.  */

static void
free_tx (struct gtm_transaction *tx)
{
  struct gtm_thread *thr = gtm_thr ();
  unsigned idx = (thr->free_tx_idx + thr->free_tx_count) % MAX_FREE_TX;

  if (thr->free_tx_count == MAX_FREE_TX)
    {
      thr->free_tx_idx = (thr->free_tx_idx + 1) % MAX_FREE_TX;
      free (thr->free_tx[idx]);
    }
  else
    thr->free_tx_count++;

  thr->free_tx[idx] = tx;
}


uint32_t REGPARM
GTM_begin_transaction (uint32_t prop, const struct gtm_jmpbuf *jb)
{
  struct gtm_transaction *tx;
  const struct gtm_dispatch *disp;

  setup_gtm_thr ();

  tx = alloc_tx ();
  memset (tx, 0, sizeof (*tx));

  tx->prop = prop;
  tx->prev = gtm_tx();
  if (tx->prev)
    tx->nesting = tx->prev->nesting + 1;
  tx->id = __sync_add_and_fetch (&global_tid, 1);
  tx->jb = *jb;

  set_gtm_tx (tx);

  if ((prop & pr_doesGoIrrevocable) || !(prop & pr_instrumentedCode))
    {
      GTM_serialmode (true, true);
      return (prop & pr_uninstrumentedCode
	      ? a_runUninstrumentedCode : a_runInstrumentedCode);
    }

  /* ??? Probably want some environment variable to choose the default
     STM implementation once we have more than one implemented.  */
  disp = &wbetl_dispatch;
  set_gtm_disp (disp);
  disp->init (true);

  gtm_rwlock_read_lock (&gtm_serial_lock);

  return a_runInstrumentedCode | a_saveLiveVariables;
}

static void
GTM_rollback_transaction (void)
{
  struct gtm_transaction *tx;

  gtm_disp()->rollback ();
  GTM_rollback_local ();

  tx = gtm_tx();
  GTM_free_actions (&tx->commit_actions);
  GTM_run_actions (&tx->undo_actions);
}

void REGPARM
_ITM_rollbackTransaction (void)
{
  assert ((gtm_tx()->prop & pr_hasNoAbort) == 0);
  assert ((gtm_tx()->state & STATE_ABORTING) == 0);

  GTM_rollback_transaction ();
  gtm_tx()->state |= STATE_ABORTING;
}

void REGPARM
_ITM_abortTransaction (_ITM_abortReason reason)
{
  struct gtm_transaction *tx = gtm_tx();

  assert (reason == userAbort);
  assert ((tx->prop & pr_hasNoAbort) == 0);
  assert ((tx->state & STATE_ABORTING) == 0);

  if (tx->state & STATE_IRREVOKABLE)
    abort ();

  GTM_rollback_transaction ();
  gtm_disp()->fini ();

  if (tx->state & STATE_SERIAL)
    gtm_rwlock_write_unlock (&gtm_serial_lock);
  else
    gtm_rwlock_read_unlock (&gtm_serial_lock);

  set_gtm_tx (tx->prev);
  free_tx (tx);

  GTM_longjmp (&tx->jb, a_abortTransaction | a_restoreLiveVariables, tx->prop);
}

static bool
GTM_trycommit_transaction (void)
{
  if (gtm_disp()->trycommit ())
    {
      GTM_commit_local ();
      GTM_free_actions (&gtm_tx()->undo_actions);
      GTM_run_actions (&gtm_tx()->commit_actions);
      return true;
    }
  return false;
}

bool REGPARM
_ITM_tryCommitTransaction (void)
{
  assert ((gtm_tx()->state & STATE_ABORTING) == 0);
  return GTM_trycommit_transaction ();
}

void REGPARM NORETURN
GTM_restart_transaction (enum restart_reason r)
{
  struct gtm_transaction *tx = gtm_tx();
  uint32_t actions;

  GTM_rollback_transaction ();
  GTM_decide_retry_strategy (r);

  actions = a_runInstrumentedCode | a_restoreLiveVariables;
  if ((tx->prop & pr_uninstrumentedCode) && (tx->state & STATE_IRREVOKABLE))
    actions = a_runUninstrumentedCode | a_restoreLiveVariables;

  GTM_longjmp (&tx->jb, actions, tx->prop);
}

void REGPARM
_ITM_commitTransaction(void)
{
  struct gtm_transaction *tx = gtm_tx();

  if ((tx->state & STATE_ABORTING) || GTM_trycommit_transaction ())
    {
      gtm_disp()->fini ();
      set_gtm_tx (tx->prev);
      free_tx (tx);
    }
  else
    GTM_restart_transaction (RESTART_VALIDATE_COMMIT);
}
