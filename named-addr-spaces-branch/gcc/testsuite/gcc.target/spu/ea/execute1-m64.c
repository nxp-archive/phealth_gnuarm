/* { dg-do compile } */
/* { dg-options "-O2 -std=gnu99 -g -pedantic-errors -mea64" } */

#include <stdlib.h>

__ea char str[] = "abc";

int
main (void)
{
  __ea char *p = str;

  if (*p++ != 'a')
    abort ();

  if (*p++ != 'b')
    abort ();

  if (*p++ != 'c')
    abort ();

  if (*p++ != '\0')
    abort ();

  return 0;
}
