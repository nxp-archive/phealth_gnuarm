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

#include "libitm_i.h"


using namespace GTM;

__thread gtm_thread GTM::_gtm_thr;
gtm_rwlock GTM::gtm_transaction::serial_lock;

gtm_stmlock GTM::gtm_stmlock_array[LOCK_ARRAY_SIZE];
gtm_version GTM::gtm_clock;

/* ??? Move elsewhere when we figure out library initialization.  */
uint64_t GTM::gtm_spin_count_var = 1000;

static _ITM_transactionId_t global_tid;

/* Allocate a transaction structure.  Reuse an old one if possible.  */

static gtm_transaction *
alloc_tx (void)
{
  gtm_transaction *tx;
  gtm_thread *thr = gtm_thr ();

  if (thr->free_tx_count == 0)
    tx = static_cast<gtm_transaction *>(xmalloc (sizeof (gtm_transaction)));
  else
    {
      thr->free_tx_count--;
      tx = thr->free_tx[thr->free_tx_idx];
      thr->free_tx_idx = (thr->free_tx_idx + 1) % gtm_thread::MAX_FREE_TX;
    }
  memset (tx, 0, sizeof (*tx));

  return tx;
}

/* Queue a transaction structure for freeing.  We never free the given
   transaction immediately -- this is a requirement of abortTransaction
   as the jmpbuf is used immediately after calling this function.  Thus
   the requirement that this queue be per-thread.  */

static void
free_tx (gtm_transaction *tx)
{
  gtm_thread *thr = gtm_thr ();
  unsigned idx
    = (thr->free_tx_idx + thr->free_tx_count) % gtm_thread::MAX_FREE_TX;

  if (thr->free_tx_count == gtm_thread::MAX_FREE_TX)
    {
      thr->free_tx_idx = (thr->free_tx_idx + 1) % gtm_thread::MAX_FREE_TX;
      free (thr->free_tx[idx]);
    }
  else
    thr->free_tx_count++;

  thr->free_tx[idx] = tx;
}


uint32_t
GTM::gtm_transaction::begin_transaction (uint32_t prop, const gtm_jmpbuf *jb)
{
  gtm_transaction *tx;
  gtm_dispatch *disp;
  uint32_t ret;

  setup_gtm_thr ();

  tx = alloc_tx ();

  tx->prop = prop;
  tx->prev = gtm_tx();
  if (tx->prev)
    tx->nesting = tx->prev->nesting + 1;
  tx->id = __sync_add_and_fetch (&global_tid, 1);
  tx->jb = *jb;

  set_gtm_tx (tx);

  if ((prop & pr_doesGoIrrevocable) || !(prop & pr_instrumentedCode))
    {
      serial_lock.write_lock ();

      tx->state = (STATE_SERIAL | STATE_IRREVOCABLE);

      disp = dispatch_serial ();

      ret = a_runUninstrumentedCode;
      if ((prop & pr_multiwayCode) == pr_instrumentedCode)
	ret = a_runInstrumentedCode;
    }
  else
    {
      serial_lock.read_lock ();

      // ??? Probably want some environment variable to choose the default
      // STM implementation once we have more than one implemented.
      if (prop & pr_readOnly)
	disp = dispatch_readonly ();
      else
	disp = dispatch_wbetl ();

      ret = a_runInstrumentedCode | a_saveLiveVariables;
    }

  set_gtm_disp (disp);

  return ret;
}

void
GTM::gtm_transaction::rollback ()
{
  gtm_disp()->rollback ();
  rollback_local ();

  free_actions (&this->commit_actions);
  run_actions (&this->undo_actions);
  commit_allocations (true);
  revert_cpp_exceptions ();

  if (this->eh_in_flight)
    {
      _Unwind_DeleteException ((_Unwind_Exception *) this->eh_in_flight);
      this->eh_in_flight = NULL;
    }
}

void ITM_REGPARM
_ITM_rollbackTransaction (void)
{
  gtm_transaction *tx = gtm_tx();
  
  assert ((tx->prop & pr_hasNoAbort) == 0);
  assert ((tx->state & gtm_transaction::STATE_ABORTING) == 0);

  tx->rollback ();
  tx->state |= gtm_transaction::STATE_ABORTING;
}

void ITM_REGPARM
_ITM_abortTransaction (_ITM_abortReason reason)
{
  gtm_transaction *tx = gtm_tx();

  assert (reason == userAbort);
  assert ((tx->prop & pr_hasNoAbort) == 0);
  assert ((tx->state & gtm_transaction::STATE_ABORTING) == 0);

  if (tx->state & gtm_transaction::STATE_IRREVOCABLE)
    abort ();

  tx->rollback ();
  gtm_disp()->fini ();

  if (tx->state & gtm_transaction::STATE_SERIAL)
    gtm_transaction::serial_lock.write_unlock ();
  else
    gtm_transaction::serial_lock.read_unlock ();

  set_gtm_tx (tx->prev);
  free_tx (tx);

  GTM_longjmp (&tx->jb, a_abortTransaction | a_restoreLiveVariables, tx->prop);
}

bool
GTM::gtm_transaction::trycommit ()
{
  if (gtm_disp()->trycommit ())
    {
      commit_local ();
      free_actions (&this->undo_actions);
      run_actions (&this->commit_actions);
      commit_allocations (false);
      return true;
    }
  return false;
}

bool
GTM::gtm_transaction::trycommit_and_finalize ()
{
  if ((this->state & gtm_transaction::STATE_ABORTING) || trycommit ())
    {
      gtm_disp()->fini ();
      set_gtm_tx (this->prev);
      free_tx (this);
      return true;
    }
  return false;
}

bool ITM_REGPARM
_ITM_tryCommitTransaction (void)
{
  gtm_transaction *tx = gtm_tx();
  assert ((tx->state & gtm_transaction::STATE_ABORTING) == 0);
  return tx->trycommit ();
}

void ITM_NORETURN
GTM::gtm_transaction::restart (gtm_restart_reason r)
{
  uint32_t actions;

  rollback ();
  decide_retry_strategy (r);

  actions = a_runInstrumentedCode | a_restoreLiveVariables;
  if ((this->prop & pr_uninstrumentedCode)
      && (this->state & gtm_transaction::STATE_IRREVOCABLE))
    actions = a_runUninstrumentedCode | a_restoreLiveVariables;

  GTM_longjmp (&this->jb, actions, this->prop);
}

void ITM_REGPARM
_ITM_commitTransaction(void)
{
  gtm_transaction *tx = gtm_tx();
  if (!tx->trycommit_and_finalize ())
    tx->restart (RESTART_VALIDATE_COMMIT);
}

void ITM_REGPARM
_ITM_commitTransactionEH(void *exc_ptr)
{
  gtm_transaction *tx = gtm_tx();
  if (!tx->trycommit_and_finalize ())
    {
      tx->eh_in_flight = exc_ptr;
      tx->restart (RESTART_VALIDATE_COMMIT);
    }
}
