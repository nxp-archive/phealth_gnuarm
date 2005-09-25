/* { dg-do compile } */
/* { dg-options "-std=gnu99" } */

extern void abort (void);

#define OPERATE(OPRD1,OPRT,OPRD2) \
do \
{ \
  OPRD1 OPRT OPRD2; \
}while(0)



#define DECIMAL_BITWISE_OPERATOR(OPRT,OPRD) \
do \
{ \
OPERATE(OPRD,OPRT,1);     \
OPERATE(OPRD,OPRT,0);     \
OPERATE(OPRD,OPRT,0x15);  \
OPERATE(0,OPRT,OPRD);     \
OPERATE(1,OPRT,OPRD);     \
OPERATE(0x15,OPRT,OPRD);  \
}while(0)

void operator_notfor_decimal()
{
  _Decimal32 d32;
  _Decimal64 d64;
  _Decimal128 d128;

  /* C99 Section 6.5.{10,11,12} Bitwise operator.  Constraints: Each of
   the operands shall have integer type.  DFP type is reject by Compiler
   when bitwise operation is performed.  */

  DECIMAL_BITWISE_OPERATOR(&,d32); /* { dg-error "invalid operands to binary" } */
  DECIMAL_BITWISE_OPERATOR(&,d64); /* { dg-error "invalid operands to binary" } */
  DECIMAL_BITWISE_OPERATOR(&,d128); /* { dg-error "invalid operands to binary" } */

  DECIMAL_BITWISE_OPERATOR(|,d32); /* { dg-error "invalid operands to binary" } */
  DECIMAL_BITWISE_OPERATOR(|,d64); /* { dg-error "invalid operands to binary" } */
  DECIMAL_BITWISE_OPERATOR(|,d128); /* { dg-error "invalid operands to binary" } */

  DECIMAL_BITWISE_OPERATOR(^,d32); /* { dg-error "invalid operands to binary" } */
  DECIMAL_BITWISE_OPERATOR(^,d64); /* { dg-error "invalid operands to binary" } */
  DECIMAL_BITWISE_OPERATOR(^,d128); /* { dg-error "invalid operands to binary" } */
}
