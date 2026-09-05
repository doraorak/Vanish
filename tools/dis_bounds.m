#import <Foundation/Foundation.h>
#import "../Vanish.m"

int main() {
    void *cfb = vn_skylight_symbol("__ZN9CGXWindow20clipped_frame_boundsEv");
    void *fb = vn_skylight_symbol("__ZNK9CGXWindow12frame_boundsEv");
    printf("clipped_frame_bounds: %p\n", cfb);
    printf("frame_bounds:         %p\n", fb);
    if (cfb) {
        uint32_t *code = (uint32_t *)ptrauth_strip(cfb, ptrauth_key_function_pointer);
        printf("clipped_frame_bounds instructions:\n");
        for (int i = 0; i < 12; i++) printf("  0x%08x\n", code[i]);
    }
    if (fb) {
        uint32_t *code = (uint32_t *)ptrauth_strip(fb, ptrauth_key_function_pointer);
        printf("frame_bounds instructions:\n");
        for (int i = 0; i < 12; i++) printf("  0x%08x\n", code[i]);
    }
    return 0;
}
