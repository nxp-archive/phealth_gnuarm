/* APPLE LOCAL file radar 4505126 */
/* Test lookup of properties in categories. */
/* Program should compile with no error or warning. */
/* { dg-do compile { target *-*-darwin* } } */
/* { dg-options "-fobjc-abi-version=2" } */
#import <Cocoa/Cocoa.h>

@interface NSWindow (Properties)
@property(readonly) NSSize size;
@property(copies) NSString* title;
@end

@implementation NSWindow (Properties)

- (NSSize)size {
    return _frame.size;
}

@end

int main(int argc, char **argv) {
    NSAutoreleasePool *pool = [NSAutoreleasePool new];

    NSWindow *window = [[NSWindow new] autorelease];
    window.title = @"test1";
    NSLog(@"window.title = %@", window.title);
    NSSize size = window.size;

    [pool drain];
    return 0;
}

