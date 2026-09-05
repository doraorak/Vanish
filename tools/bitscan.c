// Finds every instruction that touches a given byte offset of a struct, and
// names the function it sits in. Used to answer "who sets this flag bit?" when
// there are no xrefs to grep.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

static const struct nlist_64 *gSyms; static const char *gStrs;
static uint32_t gNsyms, gStrsize; static unsigned long gSlide;

static const char *sym_for(unsigned long long unslid) {
    const char *best = "?"; unsigned long long bestv = 0;
    for (uint32_t i = 0; i < gNsyms; i++) {
        unsigned long long v = gSyms[i].n_value;
        if (v == 0 || v > unslid) continue;
        if (v > bestv) { uint32_t o = gSyms[i].n_un.n_strx;
            if (o < gStrsize) { bestv = v; best = gStrs + o; } }
    }
    return best;
}

int main(int argc, char **argv) {
    unsigned imm = (unsigned)strtoul(argc > 1 ? argv[1] : "0x967", NULL, 0);
    void *h = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight", RTLD_NOW);
    Dl_info di; dladdr(dlsym(h, "SLSMainConnectionID"), &di);
    const struct mach_header_64 *mh = di.dli_fbase;
    const struct load_command *lc = (const void *)(mh + 1);
    const struct symtab_command *st = 0; const struct segment_command_64 *le = 0, *tx = 0;
    const struct section_64 *text = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SYMTAB) st = (const void *)lc;
        else if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            if (!strcmp(s->segname, "__LINKEDIT")) le = s;
            else if (!strcmp(s->segname, "__TEXT")) { tx = s;
                const struct section_64 *sec = (const void *)(s + 1);
                for (uint32_t k = 0; k < s->nsects; k++, sec++)
                    if (!strcmp(sec->sectname, "__text")) text = sec; }
        }
        lc = (const void *)((const char *)lc + lc->cmdsize);
    }
    gSlide = (unsigned long)mh - (unsigned long)tx->vmaddr;
    unsigned long lb = (unsigned long)le->vmaddr + gSlide - (unsigned long)le->fileoff;
    gSyms = (const void *)(lb + st->symoff); gStrs = (const char *)(lb + st->stroff);
    gNsyms = st->nsyms; gStrsize = st->strsize;

    const uint32_t *p = (const uint32_t *)(text->addr + gSlide);
    size_t n = text->size / 4;
    unsigned size = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : 1;
    uint32_t base_st = (size == 8) ? 0xF9000000u : 0x39000000u;
    uint32_t base_ld = (size == 8) ? 0xF9400000u : 0x39400000u;
    uint32_t imm12 = imm / size;
    uint32_t strb = base_st | (imm12 << 10);
    uint32_t ldrb = base_ld | (imm12 << 10);
    printf("scanning %zu instructions for offset 0x%x (size %u)\n", n, imm, size);
    for (size_t i = 0; i < n; i++) {
        uint32_t ins = p[i];
        uint32_t masked = ins & 0xFFFFFC00u;
        if (masked != strb && masked != ldrb) continue;
        unsigned long long unslid = (unsigned long long)(text->addr + i * 4);
        printf("  %-5s  0x%llx  %s\n", masked == strb ? "STORE" : "LOAD", unslid, sym_for(unslid));
    }
    return 0;
}
