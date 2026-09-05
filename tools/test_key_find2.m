#import <Cocoa/Cocoa.h>

int main() {
    CGEventRef ev = CGEventCreateKeyboardEvent(NULL, 46, true); // 46 = 'm'
    uint8_t *rec = *(uint8_t **)((uint8_t *)ev + 0x18);
    printf("Offset 0x90 for 'm' (46): %d\n", *(uint16_t *)(rec + 0x90));
    CGEventRef ev2 = CGEventCreateKeyboardEvent(NULL, 12, true); // 12 = 'q'
    uint8_t *rec2 = *(uint8_t **)((uint8_t *)ev2 + 0x18);
    printf("Offset 0x90 for 'q' (12): %d\n", *(uint16_t *)(rec2 + 0x90));
    return 0;
}
