/* APPLE LOCAL file radar 4498373 */
/* Test for a Dynamic Property */
/* { dg-do compile { target *-*-darwin* } } */
/* { dg-options "-fobjc-abi-version=2" } */
/* { dg-skip-if "" { powerpc*-*-darwin* } { "-m64" } { "" } } */

#include <objc/Object.h>

@interface ManagedObject: Object
@end

@interface ManagedObject (Asset)
@property const char *partNumber;
@property const char *serialNumber;
@property float *cost;
@end

@implementation  ManagedObject (Asset)
// partNumber, serialNumber, and cost are dynamic properties.
@end
/* { dg-final { scan-assembler ".long\t8\n\t.long\t3\n\t.long\t.*\n\t.long\t.*\n\t.long\t.*\n\t.long\t.*\n\t.long\t.*\n\t.long\t.*" } } */
