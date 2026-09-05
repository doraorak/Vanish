
#include "../Vanish.m"
int main(void) {
    void *fn = vn_skylight_symbol("__ZN9CGXWindow10fade_beginEP13CGXConnectionffU13block_pointerFvPS_bE");
    void *raw = ptrauth_strip(fn, ptrauth_key_function_pointer);
    uint32_t *insns = (uint32_t *)raw;
    uint32_t adrp = insns[0x1b0 / 4];
    uint32_t add = insns[0x1b4 / 4];
    int64_t immhi = (adrp >> 5) & 0x7ffff;
    int64_t immlo = (adrp >> 29) & 0x3;
    int64_t imm = (immhi << 2) | immlo;
    if (imm & 0x100000) imm |= ~0x1fffffLL;
    uintptr_t pc_page = ((uintptr_t)&insns[0x1b0 / 4]) & ~0xfffULL;
    uintptr_t target_page = pc_page + (imm << 12);
    uint32_t add_imm = (add >> 10) & 0xfff;
    uint32_t *cb = (uint32_t *)(target_page + add_imm);
    for (int i = 0; i < 150; i++) {
        printf("  +%03d: 0x%08x\n", i * 4, cb[i]);
    }
    return 0;
}
