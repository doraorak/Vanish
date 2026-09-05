#import <Cocoa/Cocoa.h>

int main() {
    CGEventRef ev = CGEventCreateKeyboardEvent(NULL, 13, true);
    uint8_t *rec = *(uint8_t **)((uint8_t *)ev + 0x18);
    printf("wid at 0x3c: %u\n", *(uint32_t *)(rec + 0x3c));
    return 0;
}
