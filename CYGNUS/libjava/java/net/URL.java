// URL.java - A Uniform Resource Locator.

/* Copyright (C) 1999  Cygnus Solutions

   This file is part of libgcj.

This software is copyrighted work licensed under the terms of the
Libgcj License.  Please consult the file "LIBGCJ_LICENSE" for
details.  */

package java.net;

import java.io.*;
import java.util.Hashtable;
import java.util.StringTokenizer;

/**
 * @author Warren Levy <warrenl@cygnus.com>
 * @date March 4, 1999.
 */

/**
 * Written using on-line Java Platform 1.2 API Specification, as well
 * as "The Java Class Libraries", 2nd edition (Addison-Wesley, 1998).
 * Status:  Believed complete and correct.
 */

public final class URL implements Serializable
{
  private String protocol;
  private String host;
  private int port;
  private String file;
  private String ref;
  private URLStreamHandler handler;
  private static Hashtable handlers = new Hashtable();
  private static URLStreamHandlerFactory factory;

  public URL(String protocol, String host, int port, String file)
    throws MalformedURLException
  {
    this(protocol, host, port, file, null);
  }

  public URL(String protocol, String host, String file)
    throws MalformedURLException
  {
    this(protocol, host, -1, file, null);
  }

  // JDK1.2
  public URL(String protocol, String host, int port, String file,
    URLStreamHandler handler) throws MalformedURLException
  {
    if (protocol == null)
      throw new MalformedURLException("null protocol");
    this.protocol = protocol;

    if (handler != null)
      {
	// TODO12: Need SecurityManager.checkPermission and
	// TODO12: java.net.NetPermission from JDK 1.2 to be implemented.
	// Throw an exception if an extant security mgr precludes
	// specifying a StreamHandler.
	//
	// SecurityManager s = System.getSecurityManager();
	// if (s != null)
	//   s.checkPermission(NetPermission("specifyStreamHandler"));

        this.handler = handler;
      }
    else
      this.handler = setURLStreamHandler(protocol);

    if (this.handler == null)
      throw new MalformedURLException("Handler for protocol not found");

    this.host = host;

    this.port = port;

    int hashAt = file.indexOf('#');
    if (hashAt < 0)
      {
	this.file = file;
	this.ref = null;
      }
    else
      {
	this.file = file.substring(0, hashAt);
	this.ref = file.substring(hashAt + 1);
      }
  }

  public URL(String spec) throws MalformedURLException
  {
    this((URL) null, spec, (URLStreamHandler) null);
  }

  public URL(URL context, String spec) throws MalformedURLException
  {
    this(context, spec, (URLStreamHandler) null);
  }

  // JDK1.2
  public URL(URL context, String spec, URLStreamHandler handler)
    throws MalformedURLException
  {
    /* A protocol is defined by the doc as the substring before a ':'
     * as long as the ':' occurs before any '/'.
     *
     * If context is null, then spec must be an absolute URL.
     *
     * The relative URL need not specify all the components of a URL.
     * If the protocol, host name, or port number is missing, the value
     * is inherited from the context.  A bare file component is appended
     * to the context's file.  The optional anchor is not inherited. 
     */

    int colon;
    int slash;
    if ((colon = spec.indexOf(':')) > 0 &&
	(colon < (slash = spec.indexOf('/')) || slash < 0))
      {
	// Protocol specified in spec string.
	protocol = spec.substring(0, colon);
	if (context != null && context.protocol == protocol)
	  {
	    // The 1.2 doc specifically says these are copied to the new URL.
	    host = context.host;
	    port = context.port;
	    file = context.file;
	  }
      }
    else if (context != null)
      {
	// Protocol NOT specified in spec string.
	// Use context fields (except ref) as a foundation for relative URLs.
	colon = -1;
	protocol = context.protocol;
	host = context.host;
	port = context.port;
	file = context.file;
      }
    else	// Protocol NOT specified in spec. and no context available.
      throw new
	  MalformedURLException("Absolute URL required with null context");

    if (handler != null)
      {
	// TODO12: Need SecurityManager.checkPermission and
	// TODO12: java.net.NetPermission from JDK 1.2 to be implemented.
	// Throw an exception if an extant security mgr precludes
	// specifying a StreamHandler.
	//
	// SecurityManager s = System.getSecurityManager();
	// if (s != null)
	//   s.checkPermission(NetPermission("specifyStreamHandler"));

        this.handler = handler;
      }
    else
      this.handler = setURLStreamHandler(protocol);

    if (this.handler == null)
      throw new MalformedURLException("Handler for protocol not found");

    // JDK 1.2 doc for parseURL specifically states that any '#' ref
    // is to be excluded by passing the 'limit' as the indexOf the '#'
    // if one exists, otherwise pass the end of the string.
    int hashAt = spec.indexOf('#', colon + 1);
    this.handler.parseURL(this, spec, colon + 1,
			  hashAt < 0 ? spec.length() : hashAt);
    if (hashAt >= 0)
      ref = spec.substring(hashAt + 1);
  }

  public boolean equals(Object obj)
  {
    if (obj == null || ! (obj instanceof URL))
      return false;

    URL uObj = (URL) obj;
    if (protocol != uObj.protocol || host != uObj.host || port != uObj.port ||
	file != uObj.file || ref != uObj.ref)
      return false;

    return true;
  }

  public final Object getContent() throws IOException
  {
    return openConnection().getContent();
  }

  public String getFile()
  {
    return file;
  }

  public String getHost()
  {
    return host;
  }

  public int getPort()
  {
    return port;
  }

  public String getProtocol()
  {
    return protocol;
  }

  public String getRef()
  {
    return ref;
  }

  public int hashCode()
  {
    // JCL book says this is computed using (only) the hashcodes of the 
    // protocol, host and file fields.  Empirical evidence indicates this
    // is probably XOR.
    return (protocol.hashCode() ^ host.hashCode() ^ file.hashCode());
  }

  public URLConnection openConnection() throws IOException
  {
    return handler.openConnection(this);
  }

  public final InputStream openStream() throws IOException
  {
    return openConnection().getInputStream();
  }

  public boolean sameFile(URL other)
  {
    if (other == null || protocol != other.protocol || host != other.host ||
	port != other.port || file != other.file)
      return false;

    return true;
  }

  protected void set(String protocol, String host, int port, String file,
		     String ref)
  {
    // TBD: Theoretically, a poorly written StreamHandler could pass an
    // invalid protocol.  It will cause the handler to be set to null
    // thus overriding a valid handler.  Callers of this method should
    // be aware of this.
    this.handler = setURLStreamHandler(protocol);
    this.protocol = protocol;
    this.port = port;
    this.host = host;
    this.file = file;
    this.ref = ref;
  }

  public static synchronized void
	setURLStreamHandlerFactory(URLStreamHandlerFactory fac)
  {
    if (factory != null)
      throw new Error("URLStreamHandlerFactory already set");

    // Throw an exception if an extant security mgr precludes
    // setting the factory.
    SecurityManager s = System.getSecurityManager();
    if (s != null)
      s.checkSetFactory();
    factory = fac;
  }

  public String toExternalForm()
  {
    // Identical to toString().
    return handler.toExternalForm(this);
  }

  public String toString()
  {
    // Identical to toExternalForm().
    return handler.toExternalForm(this);
  }

  private URLStreamHandler setURLStreamHandler(String protocol)
  {
    URLStreamHandler handler;

    // See if a handler has been cached for this protocol.
    if ((handler = (URLStreamHandler) handlers.get(protocol)) != null)
      return handler;

    // If a non-default factory has been set, use it to find the protocol.
    if (factory != null)
      handler = factory.createURLStreamHandler(protocol);

    // Non-default factory may have returned null or a factory wasn't set.
    // Use the default search algorithm to find a handler for this protocol.
    if (handler == null)
      {
	// Get the list of packages to check and append our default handler
	// to it, along with the JDK specified default as a last resort.
	// Except in very unusual environments the JDK specified one shouldn't
	// ever be needed (or available).
	String propVal = System.getProperty("java.protocol.handler.pkgs");
	propVal = (propVal == null) ? "" : (propVal + "|");
	propVal = propVal + "gnu.gcj.protocol|sun.net.www.protocol";

	StringTokenizer pkgPrefix = new StringTokenizer(propVal, "|");
	do
	  {
	    String facName = pkgPrefix.nextToken() + "." + protocol +
				".Handler";
	    try
	      {
		handler =
		  (URLStreamHandler) Class.forName(facName).newInstance();
	      }
	    catch (Exception e)
	      {
		// Can't instantiate; handler still null, go on to next element.
	      }
	  } while ((handler == null ||
		    ! (handler instanceof URLStreamHandler)) &&
		   pkgPrefix.hasMoreTokens());
      }

    // Update the hashtable with the new protocol handler.
    if (handler != null)
      if (handler instanceof URLStreamHandler)
	handlers.put(protocol, handler);
      else
	handler = null;

    return handler;
  }
}
