/* { dg-do compile } */ 
/* { dg-options "-O1 -fscalar-evolutions -fdump-tree-scev -fall-data-deps -fdump-tree-alldd" } */

void bar (int);

int foo (void)
{
  int a;
  int x;
  int c[100][100];
  
  for (a = 11; a < 50; a++)
    {
      /* Array access functions have to be analyzed.  */
      x = a + 5;
      c[x][a+1] = c[x+2][a+3] + c[x-1][a+2];
    }
  bar (c[1][2]);
}

/* The analyzer has to detect the scalar functions:
   a   ->  {11, +, 1}_1
   x   ->  {16, +, 1}_1
   x+2 ->  {18, +, 1}_1
   x-1 ->  {15, +, 1}_1
*/

/* { dg-final { diff-tree-dumps "scev" } } */
/* { dg-final { diff-tree-dumps "alldd" } } */
