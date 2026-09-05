#import <Cocoa/Cocoa.h>
#import <dlfcn.h>
#import <objc/runtime.h>

int main() {
    Method m = class_getInstanceMethod([NSWindow class], @selector(miniaturize:));
    if (!m) return 1;
    IMP imp = method_getImplementation(m);
    printf("-[NSWindow miniaturize:] = %p\n", imp);
    return 0;
}
