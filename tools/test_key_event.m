#import <Cocoa/Cocoa.h>

int main() {
    CGEventRef ev = CGEventCreateKeyboardEvent(NULL, 13, true); // 'W' key down
    CGEventSetFlags(ev, kCGEventFlagMaskCommand);
    // Print the raw bytes of ev
    uint8_t *bytes = (uint8_t *)ev;
    printf("CGEventRef size/ptr: %p\n", ev);
    // Usually CGEventRef points to an object with internal event record
    // In CoreGraphics/SkyLight:
    for (int i = 0; i < 16; i++) {
        printf("0x%02x: ", i * 8);
        for (int j = 0; j < 8; j++) printf("%02x ", bytes[i*8 + j]);
        printf("\n");
    }
    return 0;
}
