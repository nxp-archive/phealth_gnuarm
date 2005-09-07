/* { dg-do compile } */
/* { dg-options "-std=gnu99" } */

/* Tests for sections 3 and 4 of WG14 N1107.  */

/* Section 3: Test for the existence of the types.  */
_Decimal32 sd1;
_Decimal64 dd2;
_Decimal128 td3;

#define ARRAY_SIZE      7

static _Decimal32 d32[ARRAY_SIZE];
static _Decimal64 d64[ARRAY_SIZE];
static _Decimal128 d128[ARRAY_SIZE];

/* Section 4: Test sizes for these types.  */
int ssize[sizeof (_Decimal32) == 4 ? 1 : -1];
int dsize[sizeof (_Decimal64) == 8 ? 1 : -1];
int tsize[sizeof (_Decimal128) == 16 ? 1 : -1];

int salign = __alignof (_Decimal32);
int dalign = __alignof (_Decimal64);
int talign = __alignof (_Decimal128);

/* operator of sizeof applied on an array of DFP types is n times
   the size of a single variable of this type.   */

int d32_array_size [sizeof(d32) == ARRAY_SIZE * sizeof(sd1) ? 1 : -1];
int d64_array_size [sizeof(d64) == ARRAY_SIZE * sizeof(dd2) ? 1 : -1];
int d128_array_size [sizeof(d128) == ARRAY_SIZE * sizeof(td3)? 1 : -1];

void f()
{
  _Decimal32 d32[ARRAY_SIZE];
  _Decimal64 d64[ARRAY_SIZE];
  _Decimal128 d128[ARRAY_SIZE];

  int d32_array_size [sizeof(d32) == ARRAY_SIZE * sizeof(_Decimal32) ? 1 : -1];
  int d64_array_size [sizeof(d64) == ARRAY_SIZE * sizeof(_Decimal64) ? 1 : -1];
  int d128_array_size [sizeof(d128) == ARRAY_SIZE * sizeof(_Decimal128)? 1 : -1];
}
