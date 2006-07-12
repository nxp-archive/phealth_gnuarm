/* HtmlDemo.java -- HTML viewer demo
   Copyright (C) 2006 Free Software Foundation, Inc.

This file is part of GNU Classpath.

GNU Classpath is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU Classpath is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Classpath; see the file COPYING.  If not, write to the
Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301 USA.

Linking this library statically or dynamically with other modules is
making a combined work based on this library.  Thus, the terms and
conditions of the GNU General Public License cover the whole
combination.

As a special exception, the copyright holders of this library give you
permission to link this library with independent modules to produce an
executable, regardless of the license terms of these independent
modules, and to copy and distribute the resulting executable under
terms of your choice, provided that you also meet, for each linked
independent module, the terms and conditions of the license of that
module.  An independent module is a module which is not derived from
or based on this library.  If you modify this library, you may extend
this exception to your version of the library, but you are not
obligated to do so.  If you do not wish to do so, delete this
exception statement from your version. */


package gnu.classpath.examples.swing;

import java.awt.BorderLayout;
import java.awt.Dimension;
import java.awt.GridLayout;
import java.awt.event.ActionEvent;
import java.awt.event.ActionListener;

import javax.swing.JButton;
import javax.swing.JComponent;
import javax.swing.JFrame;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JTextArea;
import javax.swing.JTextPane;
import javax.swing.SwingUtilities;

/**
 * Parses and displays HTML content.
 * 
 * @author Audrius Meskauskas (audriusa@bioinformatics.org)
 */
public class HtmlDemo extends JPanel
{ 
  
  JTextPane html = new JTextPane();

  JTextArea text = new JTextArea("<html><body><p>" +
    "123456789HR!<hr>987654321"+
    "123456789BR!<br>987654321"+
    "<font color=red>ma</font>"+
    "<sup>sup</sup>normal<sub>sub</sub>normal</p><p>Table:"+
    "<table><tr>a<td>b<td>c<tr>x<td>y<td>z</table></body></html>");  
  
  JPanel buttons;

  public HtmlDemo()
  {
    super();
    html.setContentType("text/html"); // not now.
    createContent();
  }
  
  /**
   * Returns a panel with the demo content. The panel uses a BorderLayout(), and
   * the BorderLayout.SOUTH area is empty, to allow callers to add controls to
   * the bottom of the panel if they want to (a close button is added if this
   * demo is being run as a standalone demo).
   */
  private void createContent()
  {
    setLayout(new BorderLayout());
    
    JPanel center = new JPanel();
    GridLayout layout = new GridLayout();
    layout.setRows(2);
    center.setLayout(layout);
    center.add(new JScrollPane(text));
    center.add(new JScrollPane(html));
    
    buttons = new JPanel();
    
    JButton parse = new JButton("parse");
    parse.addActionListener(new ActionListener()
      {
        public void actionPerformed(ActionEvent event)
          {
            String t = text.getText();
            System.out.println("HtmlDemo.java.createContent:Parsing started");
            html.setText(t);
            System.out.println("HtmlDemo.java.createContent:Parsing completed");            
          }
      });
    
    buttons.add(parse);

    add(center, BorderLayout.CENTER);
    add(buttons, BorderLayout.SOUTH);
  }
 
  /**
   * The executable method to display the editable table.
   * 
   * @param args
   *          unused.
   */
  public static void main(String[] args)
  {
    SwingUtilities.invokeLater
    (new Runnable()
     {
       public void run()
       {
         HtmlDemo demo = new HtmlDemo();
         
         JButton exit = new JButton("exit");
         exit.addActionListener(new ActionListener()
           {
             public void actionPerformed(ActionEvent event)
               {
                 System.exit(0);
               }
           });
         
         demo.buttons.add(exit);
         
         JFrame frame = new JFrame();
         frame.getContentPane().add(demo);
         frame.setSize(new Dimension(640, 480));
         frame.setVisible(true);
       }
     });
  }

  /**
   * Returns a DemoFactory that creates a HtmlDemo.
   *
   * @return a DemoFactory that creates a HtmlDemo
   */
  public static DemoFactory createDemoFactory()
  {
    return new DemoFactory()
    {
      public JComponent createDemo()
      {
        return new HtmlDemo();
      }
    };
  }
}

