/* Copyright (C) 1998, 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

package java.util;
 
/**
 * @author Warren Levy <warrenl@cygnus.com>
 * @date August 31, 1998.
 */
/* Written using "Java Class Libraries", 2nd edition, ISBN 0-201-31002-3
 * "The Java Language Specification", ISBN 0-201-63451-1
 * plus online API docs for JDK 1.2 beta from http://www.javasoft.com.
 * Status:  Believed complete and correct
 */
 
/* The JDK 1.2 beta doc indicates that Dictionary is obsolete and that the
 * new java.util.Map interface should be used instead.
 */
public abstract class Dictionary
{
  public abstract Enumeration elements();
  public abstract Object get(Object key) throws NullPointerException;
  public abstract boolean isEmpty();
  public abstract Enumeration keys();
  public abstract Object put(Object key, Object elem)
			   throws NullPointerException;
  public abstract Object remove(Object key) throws NullPointerException;
  public abstract int size();
}
