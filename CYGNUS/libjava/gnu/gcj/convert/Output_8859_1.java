/* Copyright (C) 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

package gnu.gcj.convert; 
 
public class Output_8859_1 extends UnicodeToBytes
{
  public String getName() { return "8859_1"; }

  /**
   * @return number of chars converted. */
  public int write (char[] inbuffer, int inpos, int inlength)
  {
    int count = this.count;
    byte[] buf = this.buf;
    int avail = buf.length - count;
    if (inlength > avail)
      inlength = avail;
    for (int i = inlength;  --i >= 0;  )
      {
	buf[count++] = (byte) inbuffer[inpos++];
      }
    this.count = count;
    return inlength;
  }
}
