
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
    const struct section_64 *sec = NULL;
    lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)lc;
            if (strcmp(sg->segname, "__TEXT") == 0) {
                const struct section_64 *s = (const struct section_64 *)(sg + 1);
                for (uint32_t j = 0; j < sg->nsects; j++) {
                    if (strcmp(s[j].sectname, "__text") == 0) { sec = &s[j]; break; }
                }
            }
        }
        lc = (const void *)((const char *)lc + lc->cmdsize);
    }
    uint32_t *text = (uint32_t *)(sec->addr + slide);
    size_t count = sec->size / 4;
    int hits = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = text[i];
        // ldr sX, [xY, #0x1f4] -> ldr (single float): size=10, V=1, opc=00
        // encoding: 10 111 1 01 01 <imm12: 0x1f4/4 = 0x7d> <Rn> <Rt>
        // 0xbd400000 | (0x7d << 10) = 0xbd41f400
        if ((insn & 0xffc00000) == 0xbd41f400) {
            Dl_info ci;
            dladdr(&text[i], &ci);
            printf("LDR float from +0x1f4 at %p (symbol: %s)\n", &text[i], ci.dli_sname ? ci.dli_sname : "(null)");
            if (++hits > 15) break;
        }
    }
    return 0;
}
