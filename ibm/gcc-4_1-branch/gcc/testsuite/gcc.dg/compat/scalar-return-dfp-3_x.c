#include "scalar-return-dfp_x.h"

T(d128, _Decimal128, 123.456dl)

#undef T

void
scalar_return_dfp_3_x ()
{
DEBUG_INIT

#define T(NAME) testit##NAME ();

T(d128)

DEBUG_FINI

if (fails != 0)
  abort ();

#undef T
}
