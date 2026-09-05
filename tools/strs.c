// Dumps C string literals from SkyLight's __TEXT,__cstring matching a substring.
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
int main(int argc, char **argv) {
    const char *needle = argc > 1 ? argv[1] : "ade";
    void *h = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight", RTLD_NOW);
    Dl_info di; dladdr(dlsym(h, "SLSMainConnectionID"), &di);
    const struct mach_header_64 *mh = di.dli_fbase;
    const struct load_command *lc = (const void *)(mh + 1);
    const struct segment_command_64 *tx = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *s = (const void *)lc;
            if (!strcmp(s->segname, "__TEXT")) tx = s;
        }
        lc = (const void *)((const char *)lc + lc->cmdsize);
    }
    unsigned long slide = (unsigned long)mh - (unsigned long)tx->vmaddr;
    const struct section_64 *sec = (const void *)(tx + 1);
    for (uint32_t k = 0; k < tx->nsects; k++, sec++) {
        if (strcmp(sec->sectname, "__cstring")) continue;
        const char *p = (const char *)(sec->addr + slide);
        const char *end = p + sec->size;
        while (p < end) {
            size_t l = strnlen(p, (size_t)(end - p));
            if (l && strstr(p, needle)) printf("  %s\n", p);
            p += l + 1;
        }
    }
    return 0;
}
