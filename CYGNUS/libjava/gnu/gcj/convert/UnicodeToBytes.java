/* Copyright (C) 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

package gnu.gcj.convert; 
 
public abstract class UnicodeToBytes
{
  /** Buffer to emit bytes to.
   * The locations buf[count] ... buf[buf.length-1] are available. */
  public byte[] buf;
  public int count;

  static Class defaultEncodingClass;

  static synchronized void getDefaultEncodingClass()
  {
    // Test (defaultEncodingClass == null) again in case of race condition.
    if (defaultEncodingClass == null)
      {
	String encoding = System.getProperty("file.encoding");
	String className = "gnu.gcj.convert.Output_"+encoding;
	try
	  {
	    defaultEncodingClass = Class.forName(className);
	  }
	catch (ClassNotFoundException ex)
	  {
	    throw new NoClassDefFoundError("missing default encoding "
					   + encoding + " (class "
					   + className + " not found)");
	    
	  }
      }
  }

  public abstract String getName();

  public static UnicodeToBytes getDefaultEncoder()
  {
    try
      {
	if (defaultEncodingClass == null)
	  getDefaultEncodingClass();
	return (UnicodeToBytes) defaultEncodingClass.newInstance();
      }
    catch (Throwable ex)
      {
	return new Output_8859_1();
      }
  }

  /** Get a char-stream->byte-stream converter given an encoding name. */
  public static UnicodeToBytes getEncoder (String encoding)
    throws java.io.UnsupportedEncodingException
  {
    String className = "gnu.gcj.convert.Output_"+encoding;
    Class encodingClass;
    try 
      { 
	encodingClass = Class.forName(className); 
	return (UnicodeToBytes) encodingClass.newInstance();
      } 
    catch (Throwable ex) 
      { 
	throw new java.io.UnsupportedEncodingException(encoding + " ("
						       + ex + ')');
      }
  }

  public final void setOutput(byte[] buffer, int count)
  {
    this.buf = buffer;
    this.count = count;
  }

  /** Convert chars to bytes.
    * Converted bytes are written to buf, starting at count.
    * @param inbuffer sources of characters to convert
    * @param inpos index of initial character ininbuffer to convert
    * @param inlength number of characters to convert
    * @return number of chars converted
    * Also, this.count is increment by the number of bytes converted.
    */
  public abstract int write (char[] inbuffer, int inpos, int inlength);
}
