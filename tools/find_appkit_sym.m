#import <Cocoa/Cocoa.h>
#import <dlfcn.h>
#import <mach-o/loader.h>
#import <mach-o/nlist.h>

int main(int argc, const char *argv[]) {
    if (argc < 2) return 1;
    const char *query = argv[1];
    void *anchor = dlsym(RTLD_DEFAULT, "NSApplicationMain");
    Dl_info di;
    if (!anchor || !dladdr(anchor, &di)) return 1;
    const struct mach_header_64 *mh = (const struct mach_header_64 *)di.dli_fbase;
    const struct load_command *lc = (const void *)(mh + 1);
    const struct symtab_command *st = NULL;
    const struct segment_command_64 *le = NULL, *tx = NULL;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SYMTAB) st = (const void *)lc;
        else if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)lc;
            if (strcmp(sg->segname, "__LINKEDIT") == 0) le = sg;
            else if (strcmp(sg->segname, "__TEXT") == 0) tx = sg;
        }
        lc = (const void *)((const char *)lc + lc->cmdsize);
    }
    if (!st || !le || !tx) return 1;
    uintptr_t slide = (uintptr_t)mh - (uintptr_t)tx->vmaddr;
    uintptr_t linkedit = (uintptr_t)le->vmaddr + slide - (uintptr_t)le->fileoff;
    const struct nlist_64 *syms = (const void *)(linkedit + st->symoff);
    const char *strs = (const char *)(linkedit + st->stroff);
    for (uint32_t i = 0; i < st->nsyms; i++) {
        uint32_t off = syms[i].n_un.n_strx;
        if (off >= st->strsize) continue;
        const char *name = strs + off;
        if (strcasestr(name, query)) {
            printf("%s\n", name);
        }
    }
    return 0;
}
