/* { dg-do run } */
/* { dg-options "-std=gnu99" } */

/* Test passing varargs of the combination of decimal float types and
   other types.  Not discussed in N1107, but still necessary.  */

#include <assert.h>
#include <stdarg.h>

/* Supposing the list of varying number of arguments is:
   unsigned int, _Decimal128, doulbe, _Decimal32, _Decimal64.  */

static _Decimal32
vararg_d32 (unsigned arg, ...)
{
  va_list ap;
  _Decimal32 result;

  va_start (ap, arg);

  va_arg (ap, unsigned int);
  va_arg (ap, _Decimal128);
  va_arg (ap, double);
  result = va_arg (ap, _Decimal32);

  va_end (ap);
  return result;
}

static _Decimal32
vararg_d64 (unsigned arg, ...)
{
  va_list ap;
  _Decimal64 result;

  va_start (ap, arg);

  va_arg (ap, unsigned int);
  va_arg (ap, _Decimal128);
  va_arg (ap, double);
  va_arg (ap, _Decimal32);
  result = va_arg (ap, _Decimal64);

  va_end (ap);
  return result;
}

static _Decimal128
vararg_d128 (unsigned arg, ...)
{
  va_list ap;
  _Decimal128 result;

  va_start (ap, arg);

  va_arg (ap, unsigned int);
  result = va_arg (ap, _Decimal128);

  va_end (ap);
  return result;
}

static unsigned int
vararg_int (unsigned arg, ...)
{
  va_list ap;
  unsigned int result;

  va_start (ap, arg);

  result = va_arg (ap, unsigned int);

  va_end (ap);
  return result;
}

static double
vararg_double (unsigned arg, ...)
{
  va_list ap;
  float result;

  va_start (ap, arg);

  va_arg (ap, unsigned int);
  va_arg (ap, _Decimal128);
  result = va_arg (ap, double);

  va_end (ap);
  return result;
}


int main ()
{
  assert (vararg_d32 (3, 0, 1.0dl, 2.0, 3.0df, 4.0dd) == 3.0df);
  assert (vararg_d64 (4, 0, 1.0dl, 2.0, 3.0df, 4.0dd) == 4.0dd);
  assert (vararg_d128 (1, 0, 1.0dl, 2.0, 3.0df, 4.0dd) == 1.0dl);
  assert (vararg_int (0, 0, 1.0dl, 2.0, 3.0df, 4.0dd) == 0);
  assert (vararg_double (2, 0, 1.0dl, 2.0, 3.0df, 4.0dd) == 2.0);

  return 0;
}
