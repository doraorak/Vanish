// Finds callers of a function in SkyLight by scanning __text for BL/B whose
// target is the given unslid address, and naming the enclosing symbol.
// SkyLight has no xrefs to grep and lldb cannot do this, so: do it by hand.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

static const struct nlist_64 *gSyms; static const char *gStrs;
static uint32_t gNsyms, gStrsize;

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
    if (argc < 2) { fprintf(stderr, "usage: xref <unslid-target-hex>\n"); return 2; }
    unsigned long long target = strtoull(argv[1], NULL, 0);
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
    unsigned long slide = (unsigned long)mh - (unsigned long)tx->vmaddr;
    unsigned long lb = (unsigned long)le->vmaddr + slide - (unsigned long)le->fileoff;
    gSyms = (const void *)(lb + st->symoff); gStrs = (const char *)(lb + st->stroff);
    gNsyms = st->nsyms; gStrsize = st->strsize;

    const uint32_t *p = (const uint32_t *)(text->addr + slide);
    size_t n = text->size / 4;
    for (size_t i = 0; i < n; i++) {
        uint32_t ins = p[i];
        int isBL = (ins & 0xFC000000u) == 0x94000000u;
        int isB  = (ins & 0xFC000000u) == 0x14000000u;
        if (!isBL && !isB) continue;
        int32_t imm = (int32_t)((ins & 0x03FFFFFFu) << 6) >> 6;   // sign-extend imm26
        unsigned long long here = (unsigned long long)(text->addr + i * 4);
        if (here + (long long)imm * 4 != target) continue;
        printf("  %-3s at 0x%llx   in  %s\n", isBL ? "BL" : "B", here, sym_for(here));
    }
    return 0;
}
