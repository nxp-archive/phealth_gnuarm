/* APPLE LOCAL file radar 4523837 */
/* Test for correct property setter lookup when a similarl method name
   exists in an unrelated class. Test should compile with no error.
*/
/* { dg-options "-std=c99 -lobjc" } */
/* { dg-do run { target *-*-darwin* } } */

#include <objc/objc.h>
#include <objc/Object.h>

typedef struct {int x, y, w, h;} st1;
typedef struct {int x, y, w, h;} st2;

@interface bar : Object
- (void)setFrame:(st1)frameRect;
@end

@interface bar1 : Object
- (void)setFrame:(int)frameRect;
@end

@interface foo : Object
@property st2 frame;
@end
@implementation foo
@property st2 frame;
@end

extern void abort();

static   st2 r = {1,2,3,4};
st2 test (void)
{
    foo *obj = [foo new];
    id objid = [foo new];;

    obj.frame = r;

    ((foo*)objid).frame = obj.frame;

    return ((foo*)objid).frame;
}

int main ()
{
  st2 res = test ();
  if (res.x != 1 || res.x != 4)
    abort;
  return 0;
}
