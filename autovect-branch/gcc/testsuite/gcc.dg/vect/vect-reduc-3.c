/* { dg-options "-O2 -ftree-vectorize -fdump-tree-vect-stats -maltivec" { target powerpc*-*-* } } */

#include <stdarg.h>
#include <stdio.h>
#include "tree-vect.h"

#define N 16
#define DIFF 240

int main1 (int n)
{
  int i;
  unsigned int ub[N] = {0,3,6,9,12,15,18,21,24,27,30,33,36,39,42,45};
  unsigned int uc[N] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
  unsigned int udiff;

  udiff = 0;
  for (i = 0; i < n; i++) {
    udiff += (ub[i] - uc[i]);
  }

  /* check results:  */
  if (udiff != DIFF)
    abort ();

  return 0;
}

int main (void)
{ 
  check_vect ();
  
  return main1 (N);
  return main1 (N-1);
}

/* Need to support swapped operands, and peeling.  */
/* { dg-final { scan-tree-dump-times "vectorized 1 loops" 1 "vect" { xfail *-*-* } } } */
