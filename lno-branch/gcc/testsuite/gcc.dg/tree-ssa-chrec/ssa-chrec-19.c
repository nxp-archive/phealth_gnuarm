/* { dg-do compile } */ 
/* { dg-options "-O1 -fscalar-evolutions -fdump-scalar-evolutions" } */


int main ()
{
  int b = 2;
  
  while (b)
    {
      /* Exercises the MULT_EXPR.  */
      b = 2*b;
    }
}

/* b  ->  {2, *, 2}_1
*/

/* { dg-final { diff-tree-dumps "scev" } } */

