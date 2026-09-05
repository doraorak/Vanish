
#include "../Vanish.m"
int main(void) {
    void *fn = vn_skylight_symbol("__ZN9CGXWindow10fade_beginEP13CGXConnectionffU13block_pointerFvPS_bE");
    void *raw = ptrauth_strip(fn, ptrauth_key_function_pointer);
    uint32_t *insns = (uint32_t *)raw;
    for (int i = 0x170 / 4; i < 0x1b0 / 4; i++) {
        printf("  +%03d: 0x%08x\n", i * 4, insns[i]);
    }
    return 0;
}
