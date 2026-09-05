
#include "../Vanish.m"
int main(void) {
    void *fn = vn_skylight_symbol("__ZN9CGXWindow10fade_beginEP13CGXConnectionffU13block_pointerFvPS_bE");
    void *raw = ptrauth_strip(fn, ptrauth_key_function_pointer);
    uint32_t *insns = (uint32_t *)raw;
    uint32_t bl = insns[0x190 / 4];
    int32_t imm26 = (bl & 0x03ffffff);
    if (imm26 & 0x02000000) imm26 |= 0xfc000000;
    uintptr_t target = (uintptr_t)raw + 0x190 + (imm26 * 4);
    uint32_t *t_insns = (uint32_t *)target;
    for (int i = 0; i < 40; i++) {
        printf("  +%03d: 0x%08x\n", i * 4, t_insns[i]);
    }
    return 0;
}
