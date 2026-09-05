#import <Cocoa/Cocoa.h>

int main() {
    CGEventRef ev = CGEventCreateKeyboardEvent(NULL, 13, true); // 13 = 0x0d
    CGEventSetFlags(ev, kCGEventFlagMaskCommand);
    uint8_t *rec = *(uint8_t **)((uint8_t *)ev + 0x18);
    for (int i = 0; i < 200; i++) {
        if (rec[i] == 13) {
            printf("Found 13 (0x0d) at offset 0x%02x (%d)\n", i, i);
        }
    }
    return 0;
}
