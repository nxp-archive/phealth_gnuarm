// Runtime.java - Runtime class.

/* Copyright (C) 1998, 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

package java.lang;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * @author Tom Tromey <tromey@cygnus.com>
 * @date August 27, 1998 
 */

/* Written using "Java Class Libraries", 2nd edition, ISBN 0-201-31002-3
 * "The Java Language Specification", ISBN 0-201-63451-1
 * plus online API docs for JDK 1.2 beta from http://www.javasoft.com.
 * Status:  All 1.1 methods exist.  exec(), load(), and loadLibrary()
 * are not fully implemented.
 */

public class Runtime
{
  public Process exec (String prog) throws IOException
  {
    String[] a = new String[1];
    a[0] = prog;
    return exec (a, null);
  }

  public Process exec (String prog, String[] envp) throws IOException
  {
    String[] a = new String[1];
    a[0] = prog;
    return exec (a, envp);
  }

  public Process exec (String[] progarray) throws IOException
  {
    return exec (progarray, null);
  }

  public Process exec (String[] progarray, String[] envp) throws IOException
  {
    SecurityManager s = System.getSecurityManager();
    if (s != null)
      s.checkExec(progarray[0]);
    // FIXME.
    return null;
  }

  private final static void checkExit (int status)
  {
    SecurityManager s = System.getSecurityManager();
    if (s != null)
      s.checkExit(status);
  }

  public native void exit (int status);

  public native long freeMemory ();
  public native void gc ();

  // Deprecated in 1.1.  We implement what the JCL book says.
  public InputStream getLocalizedInputStream (InputStream in)
  {
    return in;
  }

  // Deprecated in 1.1.  We implement what the JCL book says.
  public OutputStream getLocalizedOutputStream (OutputStream out)
  {
    return out;
  }

  public static Runtime getRuntime ()
  {
    return self;
  }

  private final void checkLink (String lib)
  {
    if (lib == null)
      throw new NullPointerException ();
    SecurityManager s = System.getSecurityManager();
    if (s != null)
      s.checkLink(lib);
  }

  public synchronized void load (String pathname)
  {
    checkLink (pathname);
    // FIXME.
    throw new UnsatisfiedLinkError ("Runtime.load not implemented");
  }
  public synchronized void loadLibrary (String libname)
  {
    checkLink (libname);
    // FIXME.
    throw new UnsatisfiedLinkError ("Runtime.loadLibrary not implemented");
  }

  public native void runFinalization ();

  // This method is static in JDK 1.1, but isn't listed as static in
  // the books.  It is marked as static in the 1.2 docs.
  public static void runFinalizersOnExit (boolean run)
  {
    // The status we pass to the security check is unspecified.
    checkExit (0);
    self.finalize_on_exit = run;
  }

  public native long totalMemory ();
  public native void traceInstructions (boolean on);
  public native void traceMethodCalls (boolean on);

  // The sole constructor.
  private Runtime ()
  {
    finalize_on_exit = false;
  }

  // Private data.
  private static Runtime self = new Runtime ();
  // FIXME: for now this can't be static.  If it is, our compiler will
  // mark it as local, and it will be inaccessible to natRuntime.cc.
  private boolean finalize_on_exit;
}
