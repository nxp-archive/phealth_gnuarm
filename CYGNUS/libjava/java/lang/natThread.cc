// natThread.cc - Native part of Thread class.

/* Copyright (C) 1998, 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

#include <config.h>

#include <stdlib.h>

#include <cni.h>
#include <jvm.h>
#include <java/lang/Thread.h>
#include <java/lang/ThreadGroup.h>
#include <java/lang/IllegalArgumentException.h>
#include <java/lang/IllegalThreadStateException.h>
#include <java/lang/InterruptedException.h>
#include <java/lang/NullPointerException.h>



// This structure is used to represent all the data the native side
// needs.  An object of this type is assigned to the `data' member of
// the Thread class.
struct natThread
{
  // These are used to interrupt sleep and join calls.  We can share a
  // condition variable here since this thread can either be sleeping
  // or waiting for a thread exit, but not both.
  _Jv_Mutex_t interrupt_mutex;
  _Jv_ConditionVariable_t interrupt_cond;

  // This is private data for the thread system layer.
  _Jv_Thread_t *thread;

  // All threads waiting to join this thread are linked together and
  // waiting on their respective `interrupt' condition variables.
  // When this thread exits, it notifies each such thread by
  // signalling the condition.  In this case the `interrupt_flag' is
  // not set; this is how the waiting thread knows whether the join
  // has failed or whether it should throw an exception.
  struct natThread *joiner;

  // Chain for waiters.
  struct natThread *next;
};

// This is called from the constructor to initialize the native side
// of the Thread.
void
java::lang::Thread::initialize_native (void)
{
  // FIXME: this must interact with the GC in some logical way.  At
  // the very least we must register a finalizer to clean up.  This
  // isn't easy to do.  If the Thread object resurrects itself in its
  // own finalizer then we will need to reinitialize this structure at
  // any "interesting" point.
  natThread *nt = (natThread *) _Jv_AllocBytes (sizeof (natThread));
  data = (jobject) nt;
  _Jv_MutexInit (&nt->interrupt_mutex);
  _Jv_CondInit (&nt->interrupt_cond);
  _Jv_ThreadInitData (&nt->thread, this);
  nt->joiner = 0;
  nt->next = 0;
}

jint
java::lang::Thread::countStackFrames (void)
{
  // NOTE: This is deprecated in JDK 1.2.
  JvFail ("java::lang::Thread::countStackFrames unimplemented");
  return 0;
}

java::lang::Thread *
java::lang::Thread::currentThread (void)
{
  return _Jv_ThreadCurrent ();
}

// FIXME: this is apparently the only way a thread can be removed from
// a ThreadGroup.  That seems wrong.
void
java::lang::Thread::destroy (void)
{
  // NOTE: This is marked as unimplemented in the JDK 1.2
  // documentation.
  JvFail ("java::lang::Thread::destroy unimplemented");
}

void
java::lang::Thread::dumpStack (void)
{
  // We don't implement this because it is very hard.  Once we have a
  // VM, this could potentially ask the VM to do the dump in cases
  // where it makes sense.
  JvFail ("java::lang::Thread::dumpStack unimplemented");
}

void
java::lang::Thread::interrupt (void)
{
  interrupt_flag = true;

  // Wake up this thread, whether it is sleeping or waiting for
  // another thread to exit.
  natThread *nt = (natThread *) data;
  _Jv_MutexLock (&nt->interrupt_mutex);
  _Jv_CondNotify (&nt->interrupt_cond, &nt->interrupt_mutex);
  _Jv_MutexUnlock (&nt->interrupt_mutex);

  _Jv_ThreadInterrupt (nt->thread);
}

void
java::lang::Thread::join (jlong millis, jint nanos)
{
  // FIXME: what if we are trying to join ourselves with no timeout?

  if (millis < 0 || nanos < 0 || nanos > 999999)
    _Jv_Throw (new IllegalArgumentException);

  Thread *current = currentThread ();
  if (current->isInterrupted ())
    _Jv_Throw (new InterruptedException);

  // Update the list of all threads waiting for this thread to exit.
  // We grab a mutex when doing this in order to ensure that the
  // required state changes are atomic.
  _Jv_MonitorEnter (this);
  if (! isAlive ())
    {
      _Jv_MonitorExit (this);
      return;
    }

  // Here `CURR_NT' is the native structure for the currently
  // executing thread, while `NT' is the native structure for the
  // thread we are trying to join.
  natThread *curr_nt = (natThread *) current->data;
  natThread *nt = (natThread *) data;

  JvAssert (curr_nt->next == NULL);
  // Put thread CURR_NT onto NT's list.  When NT exits, it will
  // traverse its list and notify all joiners.
  curr_nt->next = nt->joiner;
  nt->joiner = curr_nt;
  _Jv_MonitorExit (this);


  // Now wait for: (1) an interrupt, (2) the thread to exit, or (3)
  // the timeout to occur.
  _Jv_MutexLock (&curr_nt->interrupt_mutex);
  _Jv_CondWait (&curr_nt->interrupt_cond,
		  &curr_nt->interrupt_mutex,
		  millis, nanos);
  _Jv_MutexUnlock (&curr_nt->interrupt_mutex);

  // Now the join has completed, one way or another.  Update the
  // joiners list to account for this.
  _Jv_MonitorEnter (this);
  JvAssert (nt->joiner != NULL);
  natThread *prev = 0;
  natThread *t;
  for (t = nt->joiner; t != NULL; t = t->next)
    {
      if (t == curr_nt)
	{
	  if (prev)
	    prev->next = t->next;
	  else
	    nt->joiner = t->next;
	  t->next = 0;
	  break;
	}
    }
  JvAssert (t != NULL);
  _Jv_MonitorExit (this);

  if (current->isInterrupted ())
    _Jv_Throw (new InterruptedException);
}

void
java::lang::Thread::resume (void)
{
  checkAccess ();
  JvFail ("java::lang::Thread::resume unimplemented");
}

void
java::lang::Thread::setPriority (jint newPriority)
{
  checkAccess ();
  if (newPriority < MIN_PRIORITY || newPriority > MAX_PRIORITY)
    _Jv_Throw (new IllegalArgumentException);

  jint gmax = group->getMaxPriority();
  if (newPriority > gmax)
    newPriority = gmax;

  priority = newPriority;
  natThread *nt = (natThread *) data;
  _Jv_ThreadSetPriority (nt->thread, priority);
}

void
java::lang::Thread::sleep (jlong millis, jint nanos)
{
  if (millis < 0 || nanos < 0 || nanos > 999999)
    _Jv_Throw (new IllegalArgumentException);

  Thread *current = currentThread ();
  if (current->isInterrupted ())
    _Jv_Throw (new InterruptedException);

  // We use a condition variable to implement sleeping so that an
  // interrupt can wake us up.
  natThread *nt = (natThread *) current->data;
  _Jv_MutexLock (&nt->interrupt_mutex);
  _Jv_CondWait (&nt->interrupt_cond, &nt->interrupt_mutex,
		  millis, nanos);
  _Jv_MutexUnlock (&nt->interrupt_mutex);

  if (current->isInterrupted ())
    _Jv_Throw (new InterruptedException);
}

void
java::lang::Thread::finish_ (void)
{
  // Notify all threads waiting to join this thread.
  _Jv_MonitorEnter (this);
  alive_flag = false;

  // Note that we don't bother cleaning up the joiner list here.  That
  // is taken care of when each thread wakes up again.
  natThread *nt = (natThread *) data;
  for (natThread *t = nt->joiner; t != NULL; t = t->next)
    {
      _Jv_MutexLock (&t->interrupt_mutex);
      _Jv_CondNotify (&t->interrupt_cond, &t->interrupt_mutex);
      _Jv_MutexUnlock (&t->interrupt_mutex);
    }

  _Jv_MonitorExit (this);
}

void
java::lang::Thread::run__ (jobject obj)
{
  java::lang::Thread *thread = (java::lang::Thread *) obj;
  thread->run_ ();
}

void
java::lang::Thread::start (void)
{
  JvSynchronize sync (this);

  if (alive_flag)
    _Jv_Throw (new IllegalThreadStateException);

  alive_flag = true;
  natThread *nt = (natThread *) data;
  _Jv_ThreadStart (this, nt->thread, (_Jv_ThreadStartFunc *) &run__);
}

void
java::lang::Thread::stop (java::lang::Throwable *e)
{
  JvSynchronize sync (this);
  checkAccess ();
  if (! e)
    _Jv_Throw (new NullPointerException);
  natThread *nt = (natThread *) data;
  _Jv_ThreadCancel (nt->thread, e);
}

void
java::lang::Thread::suspend (void)
{
  checkAccess ();
  JvFail ("java::lang::Thread::suspend unimplemented");
}

void
java::lang::Thread::yield (void)
{
  _Jv_ThreadYield ();
}
