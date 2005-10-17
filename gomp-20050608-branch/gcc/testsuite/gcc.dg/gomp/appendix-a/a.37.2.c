/* { dg-do compile } */

#include <omp.h>
void work (int i);
void
correct ()
{
  int i;
#pragma omp parallel private(i)
  {
    i = omp_get_thread_num ();
    work (i);
  }
}
