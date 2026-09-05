
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
    // +140 in cb
    uint32_t bl = cb[0x140 / 4];
    int32_t imm26 = (bl & 0x03ffffff);
    if (imm26 & 0x02000000) imm26 |= 0xfc000000;
    uintptr_t target = (uintptr_t)&cb[0x140 / 4] + (imm26 * 4);
    Dl_info di;
    dladdr((void *)target, &di);
    printf("Target of +140: %p (%s)\n", (void *)target, di.dli_sname ? di.dli_sname : "(null)");
    return 0;
}
