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


typedef struct gtm_local_undo
{
  void *addr;
  size_t len;
  char saved[];
} gtm_local_undo;


void
GTM_commit_local (void)
{
  gtm_transaction *tx = gtm_tx();
  gtm_local_undo **local_undo = tx->local_undo;
  size_t i, n = tx->n_local_undo;

  if (n > 0)
    {
      for (i = 0; i < n; ++i)
	free (local_undo[i]);
      tx->n_local_undo = 0;
    }
  if (local_undo)
    {
      free (local_undo);
      tx->local_undo = NULL;
      tx->size_local_undo = 0;
    }
}

void
GTM_rollback_local (void)
{
  gtm_transaction *tx = gtm_tx();
  gtm_local_undo **local_undo = tx->local_undo;
  size_t i, n = tx->n_local_undo;

  if (n > 0)
    {
      for (i = n; i-- > 0; )
	{
	  gtm_local_undo *u = local_undo[i];
	  memcpy (u->addr, u->saved, u->len);
	  free (u);
	}
      tx->n_local_undo = 0;
    }
}

void ITM_REGPARM
GTM_LB (const void *ptr, size_t len)
{
  gtm_transaction *tx = gtm_tx();
  gtm_local_undo *undo;

  undo = malloc (sizeof (struct gtm_local_undo) + len);
  undo->addr = (void *) ptr;
  undo->len = len;

  if (tx->local_undo == NULL)
    {
      tx->size_local_undo = 32;
      tx->local_undo = malloc (sizeof (undo) * tx->size_local_undo);
    }
  else if (tx->n_local_undo == tx->size_local_undo)
    {
      tx->size_local_undo *= 2;
      tx->local_undo = realloc (tx->local_undo,
				sizeof (undo) * tx->size_local_undo);
    }
  tx->local_undo[tx->n_local_undo++] = undo;

  memcpy (undo->saved, ptr, len);
}

void _ITM_LB (const void *ptr, size_t len) ITM_REGPARM
	__attribute__((alias("GTM_LB")));

#define ITM_LOG_DEF(T) \
void ITM_REGPARM _ITM_L##T (const _ITM_TYPE_##T *ptr) \
{ GTM_LB (ptr, sizeof (*ptr)); }

ITM_LOG_DEF(U1)
ITM_LOG_DEF(U2)
ITM_LOG_DEF(U4)
ITM_LOG_DEF(U8)
ITM_LOG_DEF(F)
ITM_LOG_DEF(D)
ITM_LOG_DEF(E)
ITM_LOG_DEF(CF)
ITM_LOG_DEF(CD)
ITM_LOG_DEF(CE)
