/* APPLE LOCAL file 4492976 */
/* Test if ObjC constant strings get placed in the correct section.  */

/* { dg-options "-fnext-runtime" } */
/* { dg-do compile { target *-*-darwin* } } */
/* { dg-options "-m64" } */

#include <objc/Object.h>

@interface NSConstantString: Object {
  char *cString;
  unsigned int len;
}
@end

extern struct objc_class _NSConstantStringClassReference;

const NSConstantString *appKey = @"MyApp";

/* { dg-final { scan-assembler ".section __OBJC, __cstring_object" } } */
/* { dg-final { scan-assembler ".quad\t__NSConstantStringClassReference\n\t.quad\t.*\n\t.long\t5\n\t.space 4\n\t.data" } } */
