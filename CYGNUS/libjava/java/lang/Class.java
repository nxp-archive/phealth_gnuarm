// Class.java - Representation of a Java class.

/* Copyright (C) 1998, 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

package java.lang;
import java.io.Serializable;
import java.io.InputStream;
import java.lang.reflect.*;

/**
 * @author Tom Tromey <tromey@cygnus.com>
 * @date October 1, 1998 
 */

/* Written using "Java Class Libraries", 2nd edition, ISBN 0-201-31002-3
 * "The Java Language Specification", ISBN 0-201-63451-1
 * plus online API docs for JDK 1.2 beta from http://www.javasoft.com.
 * plus gcj compiler sources (to determine object layout)
 * Status:  Sufficient for our purposes, but some methods missing
 * and some not implemented.
 */

public final class Class implements Serializable
{
  public static native Class forName (String className)
    throws ClassNotFoundException;
  public native Class[] getClasses ();
  public native ClassLoader getClassLoader ();
  public native Class getComponentType ();

  public native Constructor getConstructor (Class[] parameterTypes)
    throws NoSuchMethodException, SecurityException;
  public native Constructor[] getConstructors () throws SecurityException;

  public native Class[] getDeclaredClasses () throws SecurityException;

  public native Constructor getDeclaredConstructor (Class[] parameterTypes)
    throws NoSuchMethodException, SecurityException;
  public native Constructor[] getDeclaredConstructors ()
    throws SecurityException;
  public native Field getDeclaredField (String fieldName)
    throws NoSuchFieldException, SecurityException;
  public native Field[] getDeclaredFields () throws SecurityException;
  public native Method getDeclaredMethod (String methodName,
					  Class[] parameterTypes)
    throws NoSuchMethodException, SecurityException;
  public native Method[] getDeclaredMethods () throws SecurityException;

  // This is marked as unimplemented in the JCL book.
  public native Class getDeclaringClass ();

  private native Field getField (String fieldName, int hash)
    throws NoSuchFieldException, SecurityException;

  public Field getField (String fieldName)
    throws NoSuchFieldException, SecurityException
  {
    SecurityManager s = System.getSecurityManager();
    if (s != null)
      s.checkMemberAccess (this, java.lang.reflect.Member.DECLARED);
    Field fld = getField(fieldName, fieldName.hashCode());
    if (fld == null)
      throw new NoSuchFieldException(fieldName);
    return fld;
  }
  public native Field[] getFields () throws SecurityException;

  public native Class[] getInterfaces ();

  public native Method getMethod (String methodName, Class[] parameterTypes)
    throws NoSuchMethodException, SecurityException;
  public native Method[] getMethods () throws SecurityException;

  public native int getModifiers ();
  public native String getName ();

  // FIXME: can't implement this until we have java.net.
  // public URL getResource (String resourceName);

  // FIXME: implement.
  public InputStream getResourceAsStream (String resourceName)
  {
    return null;
  }

  // FIXME: implement.  Requires java.security.
  public Object[] getSigners ()
  {
    return null;
  }

  public native Class getSuperclass ();
  public native boolean isArray ();
  public native boolean isAssignableFrom (Class cls);
  public native boolean isInstance (Object obj);
  public native boolean isInterface ();
  public native boolean isPrimitive ();
  public native Object newInstance ()
    throws InstantiationException, IllegalAccessException;

  public String toString ()
  {
    if (isPrimitive ())
      return getName ();
    return (isInterface () ? "interface " : "class ") + getName ();
  }

  // Don't allow new classes to be made.
  private Class ()
  {
  }

  // Do a security check.
  private void checkMemberAccess (int flags)
  {
    SecurityManager sm = System.getSecurityManager();
    if (sm != null)
      sm.checkMemberAccess(this, flags);
  }

  // FIXME: this method exists only because we cannot catch Java
  // exceptions from C++ code.  This is a helper for initializeClass.
  private Throwable hackTrampoline (int what, Throwable old_exception)
    {
      Throwable new_val = null;
      try
	{
	  if (what == 0)
	    initializeClass ();
	  else if (what == 1)
	    hackRunInitializers ();
	  else if (what == 2)
	    new_val = new ExceptionInInitializerError (old_exception);
	}
      catch (Throwable t)
	{
	  new_val = t;
	}
      return new_val;
    }

  // FIXME: this is a hack to let us run the class initializers.  We
  // could do it inline in initializeClass() if we could catch Java
  // exceptions from C++.
  private native void hackRunInitializers ();

  // Initialize the class.
  private native void initializeClass ();
}
