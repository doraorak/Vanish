// Exercises the shipping code paths in a throwaway process, so a mistake costs
// a rebuild instead of a reboot. Includes Vanish.m directly: same static
// functions the tweak uses, not a copy. The constructor bails on its own here,
// because this is not WindowServer.
#include "../Vanish.m"

static int gOrigCalls, gHookCalls;

__attribute__((noinline)) static int victim(int x) { gOrigCalls++; return x * 2; }
static int (*orig_victim)(int);
static int hooked_victim(int x) { gHookCalls++; return orig_victim(x) + 1; }

int main(void) {
    int bad = 0;

    // 1. resolution -- the server-side names dlsym cannot see
    struct { const char *name; const char *sym; } want[] = {
        { "CGXOrderWindowListSpaceSwitchOptions", kVNSymOrderWindowList },
        { "CGXWindow::release_window",            kVNSymReleaseWindow },
        { "window_by_id",                         kVNSymWindowByID },
        { "CGXWindow::set_mesh_warp",             kVNSymSetMeshWarp },
        { "CreateCloneOfWindow",                  kVNSymCreateCloneOfWindow },
        { "WSSystemWindowRelease",                kVNSymSystemWindowRelease },
        { "PKGWindowGetDisplay",                  kVNSymWindowGetDisplay },
        { "CGXWindow::screen_rect_from_rect",     kVNSymScreenRectFromRect },
        { "WSWindowGetID",                        kVNSymWindowGetID },
        { "CGXWindow::clipped_frame_bounds",      kVNSymClippedFrameBounds },
        { "WSScheduleCallbackOnCurrentSession",   kVNSymScheduleCallback },
        { "WSWindowGetOwningPID",                 kVNSymWSWindowGetOwningPID },
        { "CGXGetConnectionAppName",              kVNSymGetConnectionAppName },
        { "CGXPostEventByConnection",             kVNSymPostEventByConnection },
    };
    puts("resolution:");
    for (size_t i = 0; i < sizeof(want)/sizeof(want[0]); i++) {
        void *p = vn_skylight_symbol(want[i].sym);
        printf("  %-38s %p%s\n", want[i].name, p, p ? "" : "  UNRESOLVED");
        if (!p) bad++;
    }
    printf("  %-38s %p  (dlsym, for contrast)\n", "same, via dlsym",
           dlsym(RTLD_DEFAULT, "CGXOrderWindowListSpaceSwitchOptions"));

    // 2. PAC -- pointer authentication test
    puts("\npointer authentication:");
    typedef int (*ConnFn)(void);
    ConnFn viaSymtab = (ConnFn)vn_skylight_symbol("_SLSMainConnectionID");
    ConnFn viaDlsym  = (ConnFn)dlsym(RTLD_DEFAULT, "SLSMainConnectionID");
    if (!viaSymtab) { puts("  SLSMainConnectionID UNRESOLVED"); bad++; }
    else {
        printf("  symtab %p vs dlsym %p  %s\n", (void*)viaSymtab, (void*)viaDlsym,
               ptrauth_strip((void*)viaSymtab, ptrauth_key_function_pointer) ==
               ptrauth_strip((void*)viaDlsym,  ptrauth_key_function_pointer)
                   ? "same address" : "ADDRESS MISMATCH");
        int cid = viaSymtab();
        printf("  called through the signed pointer -> cid %d\n", cid);
    }

    // 3. ellekit -- trampoline test
    puts("\nellekit round trip:");
    MSHookFunction((void *)victim, (void *)hooked_victim, (void **)&orig_victim);
    if (!orig_victim) { puts("  no trampoline"); bad++; }
    else {
        int r = victim(21);
        printf("  victim(21) = %d  (hook %d, orig %d)\n", r, gHookCalls, gOrigCalls);
        if (r != 43 || gHookCalls != 1 || gOrigCalls != 1) { puts("  WRONG"); bad++; }
    }

    // 4. Hooking order and release functions with ellekit
    puts("\nhooking order and release targets:");
    void *target_order = vn_skylight_symbol(kVNSymOrderWindowList);
    void *target_release = vn_skylight_symbol(kVNSymReleaseWindow);
    void *orig_order_ptr = NULL, *orig_release_ptr = NULL;
    if (target_order) {
        void *raw = ptrauth_strip(target_order, ptrauth_key_function_pointer);
        MSHookFunction(raw, (void *)hooked_victim, (void **)&orig_order_ptr);
        printf("  hooked CGXOrderWindowListSpaceSwitchOptions -> orig %p\n", orig_order_ptr);
        if (!orig_order_ptr) { puts("  no trampoline for order target"); bad++; }
    } else {
        puts("  order target UNRESOLVED");
        bad++;
    }
    if (target_release) {
        void *raw = ptrauth_strip(target_release, ptrauth_key_function_pointer);
        MSHookFunction(raw, (void *)hooked_victim, (void **)&orig_release_ptr);
        printf("  hooked CGXWindow::release_window -> orig %p\n", orig_release_ptr);
        if (!orig_release_ptr) { puts("  no trampoline for release target"); bad++; }
    } else {
        puts("  release target UNRESOLVED");
        bad++;
    }
    void *target_postevent = vn_skylight_symbol(kVNSymPostEventByConnection);
    void *orig_postevent_ptr = NULL;
    if (target_postevent) {
        void *raw = ptrauth_strip(target_postevent, ptrauth_key_function_pointer);
        MSHookFunction(raw, (void *)hooked_victim, (void **)&orig_postevent_ptr);
        printf("  hooked CGXPostEventByConnection -> orig %p\n", orig_postevent_ptr);
        if (!orig_postevent_ptr) { puts("  no trampoline for postevent target"); bad++; }
    } else {
        puts("  postevent target UNRESOLVED");
        bad++;
    }

    // 5. Test target window filter logic
    puts("\ntesting target window filter logic:");
    vn_window_get_owning_pid = (VNWindowGetOwningPIDFn)vn_skylight_symbol(kVNSymWSWindowGetOwningPID);

    // Mock window with mock connection pointing to current pid
    char fake_conn[0x300] = {0};
    pid_t my_pid = getpid();
    *(pid_t *)(fake_conn + 0x268) = my_pid;

    char fake_win[0x300] = {0};
    *(void **)(fake_win + kVNWindowConnectionOffset) = fake_conn;

    char my_name[256] = {0};
    proc_name(my_pid, my_name, sizeof(my_name));
    printf("  current process: pid=%d name='%s'\n", my_pid, my_name);

    // 1. With targetApp="all" (default in plist), normal app should match
    bool match_all = vn_is_target_window((CGXWindow *)fake_win);
    printf("  matches default targetApp 'all': %d (expected 1)\n", match_all);
    if (!match_all) {
        puts("  target window filter failed to match normal app with targetApp='all'");
        bad++;
    }

    // 2. Now configure /tmp/vanish_target to 'VanishTest' -> our process ('restest') shouldn't match
    FILE *f = fopen("/tmp/vanish_target", "w");
    if (f) {
        fprintf(f, "VanishTest\n");
        fclose(f);
    }
    bool match_specific_negative = vn_is_target_window((CGXWindow *)fake_win);
    printf("  matches specific target 'VanishTest' for '%s': %d (expected 0)\n", my_name, match_specific_negative);
    if (match_specific_negative != false) {
        puts("  target window filter should not match non-target process when targetApp is specific");
        bad++;
    }

    // 3. Now configure /tmp/vanish_target to match current process name
    f = fopen("/tmp/vanish_target", "w");
    if (f) {
        fprintf(f, "%s\n", my_name);
        fclose(f);
    }
    bool match_configured = vn_is_target_window((CGXWindow *)fake_win);
    printf("  matches configured '%s': %d (expected 1)\n", my_name, match_configured);
    if (match_configured != true) {
        puts("  target window filter failed to match configured process");
        bad++;
    }
    unlink("/tmp/vanish_target");

    // 4. Test duration reading
    printf("  default duration: %.2fs\n", (double)vn_duration());
    FILE *f_dur = fopen("/tmp/vanish_duration", "w");
    if (f_dur) {
        fprintf(f_dur, "0.5\n");
        fclose(f_dur);
    }
    float dur = vn_duration();
    printf("  overridden duration (/tmp/vanish_duration 0.5): %.2fs (expected 0.50s)\n", (double)dur);
    if (fabsf(dur - 0.5f) > 0.01f) {
        puts("  duration override failed");
        bad++;
    }
    unlink("/tmp/vanish_duration");

    printf("\n%s\n", bad ? "FAIL" : "all checks passed");
    return bad != 0;
}

