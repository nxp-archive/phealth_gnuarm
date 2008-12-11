/* Copyright (C) 2008 Free Software Foundation, Inc.
   Contributed by Antoniu Pop <antoniu.pop@gmail.com> 
   and Sebastian Pop <sebastian.pop@amd.com>.

   This file is part of the GNU OpenMP Library (libgomp).

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

/* This is a Linux specific implementation of a stream communication
   mechanism for libgomp.  This type is private to the library.  This
   implementation relies on the futex syscall.  */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "sem.h"
#include "libgomp.h"

void
debug_stream (void *is)
{
  gomp_stream s = (gomp_stream) is;

  printf ("Stream debug info:\n");
  printf ("  capacity           - %zu\n", s->capacity);
  printf ("  size_elt           - %zu\n", s->size_elt);
  printf ("  size_local_buffer  - %zu\n", s->size_local_buffer);
  printf ("  eos_p              - %d\n", s->eos_p);

  printf ("  write_index        - %zu\n", s->write_index);
  printf ("  read_index         - %zu\n", s->read_index);
  printf ("  write_buffer_index - %zu\n", s->write_buffer_index);
  printf ("  read_buffer_index  - %zu\n", s->read_buffer_index);

  printf ("  write_sem          - %d\n", (int)s->write_buffer_index_sem);
  printf ("  read_sem           - %d\n\n", (int)s->read_buffer_index_sem);  
}

static inline size_t
next_window (gomp_stream s, size_t index)
{
  size_t next = index + s->size_local_buffer;
  return ((next >= s->capacity) ? 0 : next);
}

/* Returns a new stream of COUNT * WINDOW_SIZE elements.  Each element
   is of size SIZE bytes.  Returns NULL when the allocation fails or
   when COUNT is less than 2.  */

void *
GOMP_stream_create (size_t size, size_t count, size_t window_size)
{
  gomp_stream s;

  /* There should be enough place for two sliding windows.  */
  if (count < 2)
    return NULL;

  s = (gomp_stream) gomp_malloc (sizeof (struct gomp_stream));

  s->capacity = count * window_size;
  s->size_elt = size;
  s->size_local_buffer = window_size;
  s->eos_p = false;
  s->read_ready_p = false;

  s->write_index = 0;
  s->read_index = 0;
  s->write_buffer_index = 0;
  s->read_buffer_index = 0;

  gomp_sem_init (&s->write_buffer_index_sem, 0);
  gomp_sem_init (&s->read_buffer_index_sem, count - 1);
  s->buffer = (char *) gomp_malloc (s->capacity);

  return s;
}

static inline void 
slide_read_window (gomp_stream s)
{
  size_t next = next_window (s, s->read_buffer_index);

  gomp_sem_wait (&s->write_buffer_index_sem);
  s->read_buffer_index = next;
  s->read_index = next;
  gomp_sem_post (&s->read_buffer_index_sem);
}

static inline void
slide_write_window (gomp_stream s)
{
  size_t next = next_window (s, s->write_buffer_index);

  gomp_sem_wait (&s->read_buffer_index_sem);
  s->write_buffer_index = next;
  s->write_index = next;
  gomp_sem_post (&s->write_buffer_index_sem);
}

/* Returns the number of read elements in the read sliding window of
   stream S.  */

static inline size_t
read_bytes_in_read_window (gomp_stream s)
{
  return s->read_index - s->read_buffer_index;
}

/* Returns the number of written elements in the write sliding window
   of stream S.  */

static inline size_t
written_bytes_in_write_window (gomp_stream s)
{
  return s->write_index - s->write_buffer_index;
}

/* Commit the current element to stream S.  */

static inline void
gomp_stream_commit (gomp_stream s)
{
  if (written_bytes_in_write_window (s) + 2 * s->size_elt > s->size_local_buffer)
    slide_write_window (s);
  else
    s->write_index += s->size_elt;
}

/* Push element ELT to stream S.  This version of push copies the
   element ELT, then commits.  */

static inline void
gomp_stream_push (gomp_stream s, char *elt)
{
  memcpy (s->buffer + s->write_index, elt, s->size_elt);
  gomp_stream_commit(s);
}

/* Release from stream S the next element.  */

static inline void
gomp_stream_pop (gomp_stream s)
{
  if (read_bytes_in_read_window (s) + 2 * s->size_elt > s->size_local_buffer)
    slide_read_window (s);
  else
    s->read_index += s->size_elt;
}

/* Returns the first element of the stream S.  Don't remove the
   element: for that, a call to gomp_stream_pop is needed.  */

void *
GOMP_stream_head (void *s)
{
  return ((gomp_stream) s)->buffer + ((gomp_stream) s)->read_index;
}

/* Returns a pointer to the next available location in stream S that
   can hold an element.  Don't commit the element: for that, a call to
   gomp_stream_push is needed.  */

void *
GOMP_stream_tail (void *s)
{
  return ((gomp_stream) s)->buffer + ((gomp_stream) s)->write_index;
}

/* Returns true when there are no more elements to be read from the
   stream S.  Returning false guarantees that at least one element
   will be available for reading.  Unless this function is called
   before accessing the head of the stream, a deadlock is
   possible.  */

bool
GOMP_stream_eos_p (void *s)
{
  /* First time: wait for the writer to leave this first window.  */
  if (! ((gomp_stream) s)->read_ready_p)
    {
      ((gomp_stream) s)->read_ready_p = true;
      gomp_sem_wait (&((gomp_stream) s)->write_buffer_index_sem);
    }

  return (((gomp_stream) s)->eos_p 
	  && (((gomp_stream) s)->read_index == ((gomp_stream) s)->write_index));
}

/* Producer can set End Of Stream to stream S.  The producer has to
   slide the write window if it wrote something.  */

void
GOMP_stream_set_eos (void *s)
{
  /* Allow the reader to access this same window to finish reading.
     Witerr can no longer touch this window.  */
  ((gomp_stream) s)->eos_p = true;
  gomp_sem_post (&((gomp_stream) s)->write_buffer_index_sem);
}

/* Free stream S.  */

void
GOMP_stream_destroy (void *s)
{
  /* No need to synchronize here: the consumer that detects when eos
     is set, and based on that it decides to destroy the stream.  */

  gomp_sem_destroy (&((gomp_stream) s)->write_buffer_index_sem);
  gomp_sem_destroy (&((gomp_stream) s)->read_buffer_index_sem);
  free (((gomp_stream) s)->buffer);
  free ((gomp_stream) s);
}

/* Align the producer and consumer accesses by pushing in the stream
   COUNT successive elements starting at address START.  */

void
GOMP_stream_align_push (void *s, void *start, size_t count)
{
  size_t i;

  for (i = 0; i < count; ++i)
    {
      gomp_stream_push ((gomp_stream) s, (char *) start);
      start += ((gomp_stream) s)->size_elt;
    }
}

/* Align the producer and consumer accesses by removing from the
   stream COUNT elements.  */

void
GOMP_stream_align_pop (void *s, size_t count)
{
  size_t i;

  for (i = 0; i < count; ++i)
    gomp_stream_pop ((gomp_stream) s);
}

void
GOMP_stream_commit (void *s)
{
  gomp_stream_commit ((gomp_stream) s);
}

void
GOMP_stream_push (void *s, void *elt)
{
  gomp_stream_push ((gomp_stream) s, (char *) elt);
}

void
GOMP_stream_pop (void *s)
{
  gomp_stream_pop ((gomp_stream) s);
}

/* Wrappers for semaphore interface.  */

void *
GOMP_sem_create (void)
{
  return gomp_malloc (sizeof (gomp_sem_t));
}

void
GOMP_sem_init (void *sem, size_t val)
{
  gomp_sem_init ((gomp_sem_t *) sem, val);
}

void
GOMP_sem_destroy (void *sem)
{
  gomp_sem_destroy ((gomp_sem_t *) sem);
  free (sem);
}

void
GOMP_sem_post (void *sem)
{
  gomp_sem_post ((gomp_sem_t *) sem);
}

void
GOMP_sem_wait (void *sem)
{
  gomp_sem_wait ((gomp_sem_t *) sem);
}
