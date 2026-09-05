
#include "../Vanish.m"
int main(void) {
    void *anchor = dlsym(RTLD_DEFAULT, "SLSMainConnectionID");
    Dl_info di;
    dladdr(anchor, &di);
    const struct mach_header_64 *mh = (const struct mach_header_64 *)di.dli_fbase;
    intptr_t slide = 0;
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)lc;
            if (strcmp(sg->segname, "__TEXT") == 0) {
                slide = (uintptr_t)mh - (uintptr_t)sg->vmaddr;
                break;
            }
        }
        lc = (const void *)((const char *)lc + lc->cmdsize);
    }
    void *fn = vn_skylight_symbol("__ZN9CGXWindow10fade_beginEP13CGXConnectionffU13block_pointerFvPS_bE");
    void *raw = ptrauth_strip(fn, ptrauth_key_function_pointer);
    uint32_t *insns = (uint32_t *)raw;
    // +1b0: adrp x16, page
    // +1b4: add x16, x16, #0xdf8
    // Let's decode x16
    uint32_t adrp = insns[0x1b0 / 4];
    uint32_t add = insns[0x1b4 / 4];
    int64_t immhi = (adrp >> 5) & 0x7ffff;
    int64_t immlo = (adrp >> 29) & 0x3;
    int64_t imm = (immhi << 2) | immlo;
    if (imm & 0x100000) imm |= ~0x1fffffLL;
    uintptr_t pc_page = ((uintptr_t)&insns[0x1b0 / 4]) & ~0xfffULL;
    uintptr_t target_page = pc_page + (imm << 12);
    uint32_t add_imm = (add >> 10) & 0xfff;
    uintptr_t cb = target_page + add_imm;
    dladdr((void *)cb, &di);
    printf("Fade timer callback at %p (%s)\n", (void *)cb, di.dli_sname ? di.dli_sname : "(null)");
    return 0;
}
