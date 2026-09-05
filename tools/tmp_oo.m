
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#import <ptrauth.h>

int main(int argc, char **argv) {
    @autoreleasepool {
        Method m = class_getInstanceMethod([NSWindow class], @selector(orderOut:));
        void *imp = (void *)method_getImplementation(m);
        void *raw = ptrauth_strip(imp, ptrauth_key_function_pointer);
        printf("-[NSWindow orderOut:] at %p\n", raw);
        uint32_t *insns = (uint32_t *)raw;
        for (int i = 0; i < 40; i++) {
            printf("  +%03d: 0x%08x\n", i * 4, insns[i]);
        }
    }
    return 0;
}
