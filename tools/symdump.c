#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
int main(int argc, char **argv) {
    void *h = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight", RTLD_NOW);
    Dl_info di; dladdr(dlsym(h, "SLSMainConnectionID"), &di);
    const struct mach_header_64 *mh = di.dli_fbase;
    const struct load_command *lc = (const void *)(mh + 1);
    const struct symtab_command *st = 0; const struct segment_command_64 *le = 0, *tx = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SYMTAB) st = (const void *)lc;
        else if (lc->cmd == LC_SEGMENT_64) { const struct segment_command_64 *s = (const void *)lc;
            if (!strcmp(s->segname, "__LINKEDIT")) le = s; else if (!strcmp(s->segname, "__TEXT")) tx = s; }
        lc = (const void *)((const char *)lc + lc->cmdsize);
    }
    unsigned long slide = (unsigned long)mh - (unsigned long)tx->vmaddr;
    unsigned long lb = (unsigned long)le->vmaddr + slide - (unsigned long)le->fileoff;
    const struct nlist_64 *sy = (const void *)(lb + st->symoff);
    const char *str = (const char *)(lb + st->stroff);
    for (uint32_t i = 0; i < st->nsyms; i++) {
        uint32_t o = sy[i].n_un.n_strx; if (o >= st->strsize) continue;
        const char *nm = str + o;
        for (int a = 1; a < argc; a++) if (strstr(nm, argv[a])) {
            printf("0x%011llx  type=0x%02x  %s\n", (unsigned long long)sy[i].n_value, sy[i].n_type, nm); break; }
    }
    return 0;
}
