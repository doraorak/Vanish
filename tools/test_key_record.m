#import <Cocoa/Cocoa.h>

int main() {
    CGEventRef ev = CGEventCreateKeyboardEvent(NULL, 13, true); // 13 = 'w'
    CGEventSetFlags(ev, kCGEventFlagMaskCommand);
    uint8_t *rec = *(uint8_t **)((uint8_t *)ev + 0x18);
    printf("event record ptr: %p\n", rec);
    for (int i = 0; i < 16; i++) {
        printf("0x%02x: ", i * 8);
        for (int j = 0; j < 8; j++) printf("%02x ", rec[i*8 + j]);
        printf("\n");
    }
    return 0;
}
