#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        Method m = class_getInstanceMethod([NSWindow class], @selector(orderOut:));
        IMP imp = method_getImplementation(m);
        printf("-[NSWindow orderOut:] IMP at %p\n", imp);
        uint32_t *insns = (uint32_t *)imp;
        for (int i = 0; i < 40; i++) {
            printf("  +%03d: 0x%08x\n", i * 4, insns[i]);
        }
    }
    return 0;
}
