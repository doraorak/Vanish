// Throwaway host: loads SkyLight so lldb can disassemble it from memory.
// Deliberately NOT WindowServer -- attaching lldb there stops the compositor
// and freezes the GUI, which is the exact failure this work avoids.
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    void *h = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight", RTLD_NOW);
    printf("SkyLight: %p\n", h);
    if (!h) { printf("dlerror: %s\n", dlerror()); return 1; }
    printf("SLSMainConnectionID: %p\n", dlsym(h, "SLSMainConnectionID"));
    printf("SLSOrderWindow: %p\n", dlsym(h, "SLSOrderWindow"));
    fflush(stdout);
    pause();
    return 0;
}
