
#include "../Vanish.m"
int main(void) {
    void *fn = vn_skylight_symbol(kVNSymSetAlpha);
    void *raw = ptrauth_strip(fn, ptrauth_key_function_pointer);
    uintptr_t addr = (uintptr_t)raw;
    uint32_t *insns = (uint32_t *)raw;
    uint32_t bl_insn = insns[0x8c / 4];
    int32_t imm26 = (bl_insn & 0x03ffffff);
    if (imm26 & 0x02000000) imm26 |= 0xfc000000;
    uintptr_t target = addr + 0x8c + (imm26 * 4);
    Dl_info di;
    dladdr((void *)target, &di);
    printf("Target of +0x8c: %p (%s)\n", (void *)target, di.dli_sname ? di.dli_sname : "(null)");
    return 0;
}
