#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

int main() {
    Method m = class_getInstanceMethod([NSWindow class], @selector(isMiniaturized));
    if (!m) return 1;
    uint32_t *code = (uint32_t *)method_getImplementation(m);
    printf("-[NSWindow isMiniaturized]:\n");
    for (int i = 0; i < 15; i++) {
        printf("  0x%08x\n", code[i]);
    }
    return 0;
}
