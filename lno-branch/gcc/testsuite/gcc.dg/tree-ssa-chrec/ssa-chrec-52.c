/* { dg-do compile } */ 
/* { dg-options "-O1 -fscalar-evolutions -fdump-scalar-evolutions -fall-data-deps -fdump-all-data-deps" } */

int bar (int);

int foo (void)
{
  int a;
  int parm = 11;
  int x;
  int c[100];
  
  for (a = parm; a < 50; a++)
    {
      /* Array access functions have to be analyzed.  */
      x = a + 5;
      c[x] = c[x+2] + c[x-1];
    }
  bar (c[1]);
}

/* { dg-final { diff-tree-dumps "scev" } } */
/* { dg-final { diff-tree-dumps "alldd" } } */
