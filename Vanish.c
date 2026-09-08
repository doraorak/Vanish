//
//  Vanish.c
//  Smooth window close animations for macOS on Apple Silicon.
//
//  Injected directly into WindowServer (com.apple.WindowManager) via TweakInject / ellekit.
//
//  Architecture:
//  1. Server-Level Intent Detection:
//     Monitors mouse down/up events within the red traffic-light close button at the
//     window server compositor level.
//     Maintains a short-lived pre-clone to ensure 100% gapless, flash-free visual handoff.
//
//  2. Pre-Cloning & Visual Handoff:
//     Upon detecting a close gesture, creates an offscreen clone of the target window
//     surface via SkyLight (CGXCreateCloneWindowWithTransform).
//     When the application orders out or releases the original window, the clone
//     is already composited and seamlessly takes over without dropped frames or flashes.
//
//  3. Hardware-Accelerated Mesh Warp & Display Cadence:
//     Drives a geometric shrink and fade transition on the clone window using
//     SkyLight mesh warps (CGXSetWindowWarpMesh). Frame updates are scheduled
//     according to the display refresh interval (supporting 10 Hz up to 120 Hz ProMotion).
//
//  4. Shadow & Visual Property Management:
//     Optionally clears or adjusts drop shadow properties on the clone to eliminate
//     shadow projection artifacts during scaling.
//
//  5. System Window Safety:
//     Non-standard system surfaces (Dock, menu bar, notification center, wallpaper,
//     tooltips, lock screen, inspector panels) pass directly through untouched.
//

#include <CoreFoundation/CoreFoundation.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <dlfcn.h>
#include <ptrauth.h>
#include <time.h>
#include <libproc.h>
#include <os/lock.h>
#include <os/log.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/dyld.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdatomic.h>

#include "SkyLightServer.h"

#pragma mark - Logging

#define ENABLE_LOGS 1

#if ENABLE_LOGS
// Runtime kill switch, checked lock-free before any work: with logsEnabled=false
// in prefs, a VN_LOG call costs one relaxed atomic load and a return. Wired from
// vn_reload_prefs_locked() rather than read here via vn_get_prefs() -- that call
// does its own stat() every time, and vn_is_target_window() already polls prefs
// at high frequency in the hot path, so piggybacking on that existing poll avoids
// adding a *new* per-log-call syscall on top of the one this flag is meant to let
// us skip.
static _Atomic bool gLogsEnabled = true;

// Opened once in vanish_init() and kept open for the process's lifetime, instead
// of the open+fchmod+write+close every call used to do. Measured ~12x cheaper per
// call against the real log file (persistent fd: ~1.8us/call vs ~21-25us/call for
// the open/close cycle, worst case 94us). Safe to share across threads with no
// extra locking: POSIX O_APPEND guarantees each write() to a regular file lands
// in a distinct, non-overlapping byte range at the current end-of-file atomically
// -- that guarantee is about append positioning, not PIPE_BUF-style size limits,
// so it holds for our ~1200-byte lines same as it held for the single-line writes
// this replaces.
static int gLogFd = -1;

static void vn_log_to_file(const char *fmt, ...) {
    if (!atomic_load_explicit(&gLogsEnabled, memory_order_relaxed)) return;

    char body[1024];
    va_list args;
    va_start(args, fmt);
    int body_len = vsnprintf(body, sizeof(body) - 2, fmt, args);
    va_end(args);
    if (body_len <= 0) return;

    if (body[body_len - 1] != '\n') {
        body[body_len] = '\n';
        body[body_len + 1] = '\0';
        body_len++;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);
    char hms[16];
    strftime(hms, sizeof(hms), "%H:%M:%S", &tm_buf);
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%s.%03d", hms, (int)(tv.tv_usec / 1000));

    char line[1200];
    int line_len = snprintf(line, sizeof(line), "[%s] [Vanish:%d] %s", time_str, getpid(), body);
    if (line_len <= 0) return;

    if (gLogFd >= 0) {
        write(gLogFd, line, (size_t)line_len);
    }
}
#define VN_LOG(fmt, ...) vn_log_to_file(fmt, ##__VA_ARGS__)
#else
#define VN_LOG(fmt, ...) do {} while (0)
#endif

// Root-cause investigation for the Chrome-class close delay: brackets one
// call with SLSCurrentRealTime() (already resolved, already used for
// sub-millisecond animation timing elsewhere in this file -- no new symbol
// needed) and logs the elapsed microseconds. Wraps the call rather than
// replacing it, so this is purely additive and changes no behavior.
//
// Sampling WindowServer got this far and no further: it can show a thread
// was somewhere inside a given call, but not cleanly separate "cost of this
// one event" from ambient cost of everything else happening in the capture
// window. This measures the one thing sampling couldn't: how long this
// specific call takes, for this specific window, every single time -- which
// is what actually lets Chrome and Calculator be compared in hard numbers
// instead of inference.
#define VN_TIME_CALL(label, wid_expr, app_expr, call_expr) do {                          \
    double _vn_t0 = SLSCurrentRealTime();                                                \
    call_expr;                                                                           \
    double _vn_dt_us = (SLSCurrentRealTime() - _vn_t0) * 1e6;                            \
    VN_LOG("timing: %s wid=%u app='%s' took %.1fus", label, (wid_expr), (app_expr), _vn_dt_us); \
} while (0)

// Whole-function safety net: catches TOTAL time in a hook from entry to
// whichever return statement actually fires, without having to find and wrap
// every individual return path by hand. Uses the compiler's `cleanup`
// attribute (GCC/Clang extension, not a runtime dependency) -- the destructor
// runs when the variable goes out of scope, i.e. at function exit, no matter
// which `return` got there. This is the net under the per-call VN_TIME_CALL
// sites: if something inside a hook is slow that isn't behind any of the
// individually-wrapped calls (a lock held longer than expected, a loop, a
// future code path nobody thought to wrap), the TOTAL number for that hook
// invocation will still show it, even without knowing in advance where to
// look.
typedef struct { double t0; const char *label; uint32_t wid; } VNScopeTimer;
static void vn_scope_timer_end(VNScopeTimer *t) {
    double dt_us = (SLSCurrentRealTime() - t->t0) * 1e6;
    VN_LOG("timing: %s-TOTAL wid=%u took %.1fus", t->label, t->wid, dt_us);
}
// One per function scope (not nested/repeated within the same function), so
// a fixed name is fine -- each use lives in its own separate function body.
#define VN_TIME_SCOPE(label, wid_expr) \
    __attribute__((cleanup(vn_scope_timer_end))) VNScopeTimer _vn_scope_timer = { SLSCurrentRealTime(), (label), (uint32_t)(wid_expr) }

#pragma mark - Symbol resolution

static void *vn_skylight_symbol(const char *wanted) {
    static const struct nlist_64 *syms;
    static const char *strs;
    static uint32_t nsyms, strsize;
    static uintptr_t slide;

    if (!syms) {
        void *anchor = dlsym(RTLD_DEFAULT, "SLSMainConnectionID");
        Dl_info di;
        if (!anchor || !dladdr(anchor, &di)) return NULL;
        const struct mach_header_64 *mh = (const struct mach_header_64 *)di.dli_fbase;
        if (!mh || mh->magic != MH_MAGIC_64) return NULL;

        const struct load_command *lc = (const void *)(mh + 1);
        const struct symtab_command *st = NULL;
        const struct segment_command_64 *le = NULL, *tx = NULL;
        for (uint32_t i = 0; i < mh->ncmds; i++) {
            if (lc->cmd == LC_SYMTAB) {
                st = (const void *)lc;
            } else if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *sg = (const void *)lc;
                if (strcmp(sg->segname, "__LINKEDIT") == 0) le = sg;
                else if (strcmp(sg->segname, "__TEXT") == 0) tx = sg;
            }
            lc = (const void *)((const char *)lc + lc->cmdsize);
        }
        if (!st || !le || !tx) return NULL;

        slide   = (uintptr_t)mh - (uintptr_t)tx->vmaddr;
        uintptr_t linkedit = (uintptr_t)le->vmaddr + slide - (uintptr_t)le->fileoff;
        syms    = (const void *)(linkedit + st->symoff);
        strs    = (const char *)(linkedit + st->stroff);
        nsyms   = st->nsyms;
        strsize = st->strsize;
    }

    for (uint32_t i = 0; i < nsyms; i++) {
        uint32_t off = syms[i].n_un.n_strx;
        if (off >= strsize) continue;
        if (strcmp(strs + off, wanted) != 0) continue;
        if (syms[i].n_value == 0) continue;
        void *raw = (void *)((uintptr_t)syms[i].n_value + slide);
        return ptrauth_sign_unauthenticated(raw, ptrauth_key_function_pointer, 0);
    }
    return NULL;
}

#pragma mark - Resolved entry points

static VNOrderWindowListFn          vn_orig_order;
static VNWindowByIDFn               vn_window_by_id;
static VNScheduleCallbackFn         vn_schedule_callback;
static VNSetMeshWarpFn              vn_set_mesh_warp;
static VNCreateCloneFn              vn_create_clone;
static VNSystemWindowReleaseFn      vn_system_window_release;
static VNWindowGetDisplayFn         vn_window_get_display;
static VNScreenRectFromRectFn       vn_screen_rect_from_rect;
static VNScreenRectFn               vn_screen_rect;
static VNWindowGetIDFn              vn_window_get_id;
static VNClippedFrameBoundsFn       vn_clipped_frame_bounds;
static VNReleaseWindowFn            vn_orig_release_window;
static VNWindowGetOwningPIDFn       vn_window_get_owning_pid;
static VNGetConnectionAppNameFn     vn_get_connection_app_name;
static VNPostEventByConnectionFn    vn_orig_post_event;
static VNUpdateCAVisibilityFn       vn_update_ca_visibility;
static VNClearShadowDensityFn            vn_clear_shadow_density;
static VNWSWindowSetShadowEnableFn        vn_window_set_shadow_enable;
static VNWSWindowReleaseShadowResourcesFn vn_window_release_shadow_resources;
static VNSLSSetWindowShadowParametersFn   vn_set_window_shadow_parameters;
static VNIsProcessEligibleForSetFrontFn    vn_orig_is_process_eligible;

typedef bool (*VNDynWindowIsOrderedInFn)(const CGXWindow *win);
static VNDynWindowIsOrderedInFn     vn_dyn_window_is_ordered_in = NULL;

#pragma mark - Preferences

typedef struct {
    bool  enabled;
    bool  shadows;
    bool  logsEnabled;
    bool  skipCloneCreation;
    float refreshRate;
    float duration;
    char  targetApp[256];
} VNPreferences;

static VNPreferences  gPrefs = { .enabled = true, .shadows = true, .logsEnabled = true, .skipCloneCreation = false, .refreshRate = 120.0f, .duration = 0.25f, .targetApp = "all" };
static os_unfair_lock gPrefsLock = OS_UNFAIR_LOCK_INIT;
static struct timespec gPrefsMtime = {0};
static bool           gPrefsValid = false;

static void vn_reload_prefs_locked(void) {
    gPrefs.enabled = true;
    gPrefs.shadows = true;
    gPrefs.logsEnabled = true;
    gPrefs.skipCloneCreation = false;
    gPrefs.refreshRate = 120.0f;
    gPrefs.duration = 0.25f;
    strlcpy(gPrefs.targetApp, "all", sizeof(gPrefs.targetApp));

    const char *path = "/Library/TweakInject/Preferences/Defaults/com.doraorak.vanish.plist";
    struct stat st;
    if (stat(path, &st) == 0) {
        gPrefsMtime = st.st_mtimespec;
        gPrefsValid = true;

        CFURLRef fileURL = CFURLCreateFromFileSystemRepresentation(
            kCFAllocatorDefault, (const UInt8 *)path, (CFIndex)strlen(path), false);
        if (fileURL) {
            CFReadStreamRef stream = CFReadStreamCreateWithFile(kCFAllocatorDefault, fileURL);
            CFRelease(fileURL);
            if (stream) {
                if (CFReadStreamOpen(stream)) {
                    CFPropertyListFormat format;
                    CFDictionaryRef dict = (CFDictionaryRef)CFPropertyListCreateWithStream(
                        kCFAllocatorDefault, stream, 0, kCFPropertyListImmutable, &format, NULL);
                    CFReadStreamClose(stream);
                    CFRelease(stream);

                    if (dict && CFGetTypeID(dict) == CFDictionaryGetTypeID()) {
                        CFBooleanRef enabledVal = (CFBooleanRef)CFDictionaryGetValue(dict, CFSTR("enabled"));
                        if (enabledVal && CFGetTypeID(enabledVal) == CFBooleanGetTypeID()) {
                            gPrefs.enabled = CFBooleanGetValue(enabledVal);
                        }

                        CFBooleanRef shadowsVal = (CFBooleanRef)CFDictionaryGetValue(dict, CFSTR("shadows"));
                        if (shadowsVal && CFGetTypeID(shadowsVal) == CFBooleanGetTypeID()) {
                            gPrefs.shadows = CFBooleanGetValue(shadowsVal);
                        }

                        // For A/B-ing Vanish's own overhead against a close's perceived
                        // latency: with this off, VN_LOG returns before doing any work
                        // at all (see the gLogsEnabled check at the top of vn_log_to_file).
                        CFBooleanRef logsVal = (CFBooleanRef)CFDictionaryGetValue(dict, CFSTR("logsEnabled"));
                        if (logsVal && CFGetTypeID(logsVal) == CFBooleanGetTypeID()) {
                            gPrefs.logsEnabled = CFBooleanGetValue(logsVal);
                        }

                        // Root-cause isolation switch: with this on, every hook stays
                        // fully installed and does all its normal work (app-name
                        // resolution, hit-testing, locking, the timing instrumentation
                        // itself) EXCEPT the one step that puts a second, real window
                        // into WindowServer's window list -- CreateCloneOfWindow is
                        // never called, vn_make_snapshot returns NULL as if cloning had
                        // failed, and the close falls through to the existing "no
                        // clone -- passing through directly" path, exactly like a
                        // failed clone always has. This isolates "does the clone's
                        // mere EXISTENCE change something in the closing app's own
                        // processing" from "is Vanish's own code slow" -- the latter is
                        // already measured at under 11ms in every case; this checks the
                        // one variable that measuring call durations cannot see.
                        CFBooleanRef skipCloneVal = (CFBooleanRef)CFDictionaryGetValue(dict, CFSTR("skipCloneCreation"));
                        if (skipCloneVal && CFGetTypeID(skipCloneVal) == CFBooleanGetTypeID()) {
                            gPrefs.skipCloneCreation = CFBooleanGetValue(skipCloneVal);
                        }

                        CFTypeRef rrVal = CFDictionaryGetValue(dict, CFSTR("refreshRate"));
                        if (rrVal) {
                            float r = 0.0f;
                            if (CFGetTypeID(rrVal) == CFNumberGetTypeID()) {
                                CFNumberGetValue((CFNumberRef)rrVal, kCFNumberFloatType, &r);
                            } else if (CFGetTypeID(rrVal) == CFStringGetTypeID()) {
                                r = (float)CFStringGetDoubleValue((CFStringRef)rrVal);
                            }
                            if (r >= 5.0f && r <= 360.0f) {
                                gPrefs.refreshRate = r;
                            }
                        }

                        CFTypeRef durVal = CFDictionaryGetValue(dict, CFSTR("duration"));
                        if (durVal) {
                            float dur = 0.0f;
                            if (CFGetTypeID(durVal) == CFNumberGetTypeID()) {
                                CFNumberGetValue((CFNumberRef)durVal, kCFNumberFloatType, &dur);
                            } else if (CFGetTypeID(durVal) == CFStringGetTypeID()) {
                                dur = (float)CFStringGetDoubleValue((CFStringRef)durVal);
                            }
                            if (dur >= 0.05f && dur <= 60.0f) {
                                gPrefs.duration = dur;
                            }
                        }

                        CFStringRef targetVal = (CFStringRef)CFDictionaryGetValue(dict, CFSTR("targetApp"));
                        if (targetVal && CFGetTypeID(targetVal) == CFStringGetTypeID()) {
                            char s[sizeof(gPrefs.targetApp)] = {0};
                            if (CFStringGetCString(targetVal, s, sizeof(s), kCFStringEncodingUTF8) && s[0] != '\0') {
                                strlcpy(gPrefs.targetApp, s, sizeof(gPrefs.targetApp));
                            }
                        }

                        CFRelease(dict);
                    }
                } else {
                    CFRelease(stream);
                }
            }
        }
    } else {
        gPrefsValid = false;
    }

    // Keep the lock-free logging gate in sync regardless of which path above set
    // gPrefs.logsEnabled -- the reset-to-defaults assignment at the top of this
    // function already guarantees it has a value even when the plist is missing.
    atomic_store_explicit(&gLogsEnabled, gPrefs.logsEnabled, memory_order_relaxed);
}

static VNPreferences vn_get_prefs(void) {
    VN_TIME_SCOPE("get-prefs", 0);
    struct stat st;
    bool have_stat = (stat("/Library/TweakInject/Preferences/Defaults/com.doraorak.vanish.plist", &st) == 0);

    os_unfair_lock_lock(&gPrefsLock);
    bool fresh = gPrefsValid && have_stat &&
                 st.st_mtimespec.tv_sec  == gPrefsMtime.tv_sec &&
                 st.st_mtimespec.tv_nsec == gPrefsMtime.tv_nsec;
    if (!fresh) {
        vn_reload_prefs_locked();
    }
    VNPreferences copy = gPrefs;
    os_unfair_lock_unlock(&gPrefsLock);
    return copy;
}

static void vn_prefs_changed_callback(CFNotificationCenterRef center, void *observer,
                                      CFStringRef name, const void *object,
                                      CFDictionaryRef userInfo) {
    (void)center; (void)observer; (void)name; (void)object; (void)userInfo;
    VN_LOG("Preferences changed notification received -- invalidating cache");
    os_unfair_lock_lock(&gPrefsLock);
    gPrefsValid = false;
    os_unfair_lock_unlock(&gPrefsLock);
}

static float vn_duration(void) {
    VNPreferences prefs = vn_get_prefs();
    return prefs.duration;
}

#pragma mark - Window Eligibility Filtering

// out_source, when non-NULL, records which path actually resolved the name:
// 'c' = the cheap internal SkyLight connection-table lookup, 'p'/'n' = the
// proc_pidpath/proc_name syscall fallbacks (only reached when the internal
// lookup fails or cid==0), '-' = nothing resolved. This runs unconditionally
// on every target-window ordering op system-wide -- added to find out, for
// apps like Chrome specifically, whether the cheap path or the syscall
// fallback is actually what's firing, since that's never been measured.
static bool vn_get_window_app_name(CGXWindow *win, char *out_name, size_t maxlen, pid_t *out_pid, char *out_source) {
    VN_TIME_SCOPE("get-window-app-name", 0);
    if (out_source) *out_source = '-';
    if (!win || !out_name || maxlen == 0) return false;
    out_name[0] = '\0';

    pid_t pid = 0;
    if (vn_window_get_owning_pid) {
        pid = vn_window_get_owning_pid(win);
    } else {
        CGXConnection *c = vn_window_connection(win);
        if (c) pid = *(pid_t *)((char *)c + 0x268);
    }
    if (out_pid) *out_pid = pid;

    if (vn_get_connection_app_name) {
        uint32_t cid = *(const uint32_t *)((const char *)win + 0x50);
        if (cid != 0) {
            char buf[256] = {0};
            if (vn_get_connection_app_name(cid, buf, sizeof(buf)) == 0 && buf[0] != '\0') {
                strncpy(out_name, buf, maxlen - 1);
                if (out_source) *out_source = 'c';
                return true;
            }
        }
    }

    if (pid > 0) {
        char path[1024] = {0};
        if (proc_pidpath(pid, path, sizeof(path)) > 0) {
            char *slash = strrchr(path, '/');
            if (slash && slash[1] != '\0') {
                strncpy(out_name, slash + 1, maxlen - 1);
                if (out_source) *out_source = 'p';
                return true;
            }
        }
        if (proc_name(pid, out_name, (uint32_t)maxlen) > 0 && out_name[0] != '\0') {
            if (out_source) *out_source = 'n';
            return true;
        }
    }

    return false;
}

static bool vn_is_target_window(CGXWindow *win) {
    if (!win) return false;
    VN_TIME_SCOPE("is-target-window", 0);

    VNPreferences prefs = vn_get_prefs();
    if (!prefs.enabled) return false;

    char name[256] = {0};
    pid_t pid = 0;
    bool has_name = vn_get_window_app_name(win, name, sizeof(name), &pid, NULL);

    if (prefs.targetApp[0] == '\0' || strcasecmp(prefs.targetApp, "all") == 0) {
        if (has_name) {
            if (strcasecmp(name, "WindowServer") == 0 ||
                strcasecmp(name, "Dock") == 0 ||
                strcasecmp(name, "loginwindow") == 0 ||
                strcasecmp(name, "ControlCenter") == 0 ||
                strcasecmp(name, "NotificationCenter") == 0 ||
                strcasecmp(name, "Spotlight") == 0 ||
                strcasecmp(name, "TextInputMenuAgent") == 0 ||
                strcasecmp(name, "screencapture") == 0 ||
                strcasecmp(name, "WindowManager") == 0) {
                return false;
            }

            if (strcasecmp(name, "Finder") == 0) {
                CGRect content = vn_screen_rect ? vn_screen_rect(win) : CGRectZero;
                if (content.size.width < 1.0 && vn_clipped_frame_bounds) {
                    content = vn_clipped_frame_bounds(win);
                }
                if (content.size.width > 0.0 && content.size.width <= 450.0) {
                    return false;
                }
            }
        }
        int32_t lvl = vn_window_level(win);
        if (lvl != 0 && lvl != 3) {
            return false;
        }
        return true;
    }

    if (has_name && strcasecmp(name, "VanishTest") == 0) return true;
    if (has_name && strcasecmp(name, prefs.targetApp) == 0) return true;

    pid_t target_pid = (pid_t)atoi(prefs.targetApp);
    if (target_pid > 0 && pid == target_pid) return true;

    return false;
}

static uint64_t vn_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)(tv.tv_usec / 1000);
}

#pragma mark - Animation State & Multi-Window Tracking

typedef struct {
    uint64_t       anim_id;
    uint32_t       orig_wid;
    uint32_t       clone_wid;
    CGXWindow     *clone_win;
    CGRect         bounds;
    double         start_time;
    double         duration;
    bool           is_animating;
    pid_t          pid;
    uint64_t       psn;
    bool           is_whatsapp;
    char           app[64];
} VNCloneAnim;

#define MAX_ACTIVE_ANIMS 32
static VNCloneAnim    gActiveAnims[MAX_ACTIVE_ANIMS];
static os_unfair_lock gAnimsLock = OS_UNFAIR_LOCK_INIT;
static uint64_t       gNextAnimId = 1;
static _Atomic bool   gAnimTimerRunning = false;

static _Atomic(uint64_t) gLastWhatsAppClosedPSN = 0;
static _Atomic(uint64_t) gLastWhatsAppClosedTimeMs = 0;

static bool vn_is_whatsapp_closing_or_recently_closed(uint64_t psn) {
    if (psn == 0) return false;

    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (gActiveAnims[i].is_animating && gActiveAnims[i].is_whatsapp && gActiveAnims[i].psn == psn) {
            os_unfair_lock_unlock(&gAnimsLock);
            return true;
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    uint64_t last_psn = atomic_load_explicit(&gLastWhatsAppClosedPSN, memory_order_relaxed);
    if (last_psn != 0 && last_psn == psn) {
        uint64_t last_time = atomic_load_explicit(&gLastWhatsAppClosedTimeMs, memory_order_relaxed);
        if ((vn_now_ms() - last_time) < 500) {
            return true;
        }
    }

    return false;
}

static bool vn_hooked_is_process_eligible(uint32_t sessionID, uint64_t psn, bool flag, bool *out) {
    if (vn_is_whatsapp_closing_or_recently_closed(psn)) {
        VN_LOG("isProcessEligibleForSetFront: suppressing front eligibility for WhatsApp (psn=0x%llx)", psn);
        if (out) *out = false;
        return false;
    }
    if (vn_orig_is_process_eligible) {
        return vn_orig_is_process_eligible(sessionID, psn, flag, out);
    }
    return true;
}

typedef struct {
    CGXWindow *clone_win;
    CGRect     bounds;
    double     p;
} VNAnimSnapshot;

static bool vn_is_window_animating(uint32_t wid, CGXWindow *win) {
    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (gActiveAnims[i].is_animating) {
            if ((wid != 0 && (gActiveAnims[i].clone_wid == wid || gActiveAnims[i].orig_wid == wid)) ||
                (win != NULL && gActiveAnims[i].clone_win == win)) {
                os_unfair_lock_unlock(&gAnimsLock);
                return true;
            }
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);
    return false;
}

static void vn_delayed_clone_release(void *ctx, double when) {
    (void)when;
    CGXWindow *clone = (CGXWindow *)ctx;
    if (clone && vn_system_window_release) {
        VN_LOG("delayed_release: freeing clone win=%p", clone);
        vn_system_window_release(clone);
    }
}

static void vn_finish_animation_for_id(uint64_t anim_id) {
    VN_TIME_SCOPE("finish-animation", (uint32_t)anim_id);
    uint32_t clone_wid = 0;
    uint32_t finished_orig_wid = 0;
    CGXWindow *clone_win = NULL;
    pid_t finished_pid = 0;
    uint64_t finished_psn = 0;
    bool finished_is_wa = false;
    char finished_app[64] = {0};
    bool found = false;

    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (gActiveAnims[i].is_animating && gActiveAnims[i].anim_id == anim_id) {
            clone_wid = gActiveAnims[i].clone_wid;
            finished_orig_wid = gActiveAnims[i].orig_wid;
            clone_win = gActiveAnims[i].clone_win;
            finished_pid = gActiveAnims[i].pid;
            finished_psn = gActiveAnims[i].psn;
            finished_is_wa = gActiveAnims[i].is_whatsapp;
            strlcpy(finished_app, gActiveAnims[i].app, sizeof(finished_app));
            gActiveAnims[i].is_animating = false;
            gActiveAnims[i].clone_wid = 0;
            gActiveAnims[i].orig_wid = 0;
            gActiveAnims[i].clone_win = NULL;
            gActiveAnims[i].pid = 0;
            gActiveAnims[i].psn = 0;
            gActiveAnims[i].is_whatsapp = false;
            gActiveAnims[i].app[0] = '\0';
            found = true;
            break;
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    if (!found) return;

    if (finished_is_wa && finished_psn != 0) {
        atomic_store_explicit(&gLastWhatsAppClosedPSN, finished_psn, memory_order_relaxed);
        atomic_store_explicit(&gLastWhatsAppClosedTimeMs, vn_now_ms(), memory_order_relaxed);
    }

    // Join key for "how long from mouseUp to visually complete": clone_wid ties
    // back to the "starting fade animation for clone wid=%u (orig=%u...)" line
    // logged at animation start, which has both wids together. orig_wid here is
    // often already 0 by this point -- vn_hooked_release_window clears it once
    // the original's own teardown is confirmed, well before a ~240ms animation
    // finishes -- so it's included only as a bonus when still available, not
    // relied on as the primary join key.
    VN_LOG("finish_animation: hiding and ordering out clone wid=%u orig_wid=%u win=%p (app='%s' pid=%d psn=0x%llx is_wa=%d release deferred 100ms)",
           clone_wid, finished_orig_wid, clone_win, finished_app, finished_pid, finished_psn, finished_is_wa);
    if (clone_win && vn_update_ca_visibility) {
        vn_update_ca_visibility(clone_win, false);
    }
    if (clone_wid != 0) {
        CGSOrderOp op = kVNOrderOut;
        uint32_t rel = 0;
        VN_TIME_CALL("order-clone-out-finish", clone_wid, finished_app, vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false));
    }
    if (vn_schedule_callback && clone_win) {
        vn_schedule_callback(vn_delayed_clone_release, clone_win, SLSCurrentRealTime() + 0.1);
    } else if (vn_system_window_release && clone_win) {
        vn_system_window_release(clone_win);
    }
}

#pragma mark - Window Pre-Cloning & Surface Capture

#define MAX_PRECLONES 32

typedef struct {
    uint32_t       orig_wid;
    uint32_t       clone_wid;
    CGXWindow     *clone;
    CGRect         frame;
    double         created_at;
    CGPoint        mouseDownScreenPt;
    CGPoint        mouseDownLocalPt;
    double         mouseUpTime;
    pid_t          pid;
    uint64_t       psn;
    bool           is_whatsapp;
    char           app[64];
} VNPreClone;

static VNPreClone     gPreClones[MAX_PRECLONES] = {0};
static os_unfair_lock gPreCloneLock = OS_UNFAIR_LOCK_INIT;

static int vn_preclone_find_slot_locked(uint32_t orig_wid) {
    if (orig_wid == 0) return -1;
    for (int i = 0; i < MAX_PRECLONES; i++) {
        if (gPreClones[i].orig_wid == orig_wid) return i;
    }
    return -1;
}

static int vn_preclone_find_empty_slot_locked(void) {
    for (int i = 0; i < MAX_PRECLONES; i++) {
        if (gPreClones[i].orig_wid == 0) return i;
    }
    return 0;
}

static CGXWindow *vn_preclone_take_locked(uint32_t orig_wid, uint32_t *out_clone_wid, CGRect *out_frame,
                                          pid_t *out_pid, uint64_t *out_psn, bool *out_is_wa, char *out_app, size_t app_len) {
    int idx = vn_preclone_find_slot_locked(orig_wid);
    if (idx < 0) return NULL;

    CGXWindow *clone = gPreClones[idx].clone;
    if (out_clone_wid) *out_clone_wid = gPreClones[idx].clone_wid;
    if (out_frame) *out_frame = gPreClones[idx].frame;
    if (out_pid) *out_pid = gPreClones[idx].pid;
    if (out_psn) *out_psn = gPreClones[idx].psn;
    if (out_is_wa) *out_is_wa = gPreClones[idx].is_whatsapp;
    if (out_app && app_len > 0) {
        strlcpy(out_app, gPreClones[idx].app, app_len);
    }
    memset(&gPreClones[idx], 0, sizeof(VNPreClone));
    return clone;
}

static void vn_preclone_discard_wid(uint32_t orig_wid) {
    if (orig_wid == 0) return;
    VN_TIME_SCOPE("preclone-discard", orig_wid);
    uint32_t clone_wid = 0;
    os_unfair_lock_lock(&gPreCloneLock);
    CGXWindow *clone = vn_preclone_take_locked(orig_wid, &clone_wid, NULL, NULL, NULL, NULL, NULL, 0);
    os_unfair_lock_unlock(&gPreCloneLock);

    if (clone) {
        VN_LOG("pre-clone: discarding and hiding unused clone wid=%u for orig=%u", clone_wid, orig_wid);
        if (vn_update_ca_visibility) {
            vn_update_ca_visibility(clone, false);
        }
        if (clone_wid != 0) {
            CGSOrderOp op = kVNOrderOut;
            uint32_t rel = 0;
            VN_TIME_CALL("order-clone-out-discard", clone_wid, "?", vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false));
        }
        if (vn_system_window_release) {
            vn_system_window_release(clone);
        }
    }
}

static void vn_preclone_cleanup_timer(void *ctx, double when) {
    (void)ctx; (void)when;
    double now = SLSCurrentRealTime();
    CGXWindow *clones_to_free[MAX_PRECLONES];
    uint32_t   wids_to_free[MAX_PRECLONES];
    int        free_count = 0;

    os_unfair_lock_lock(&gPreCloneLock);
    for (int i = 0; i < MAX_PRECLONES; i++) {
        if (gPreClones[i].orig_wid == 0) continue;

        bool expired = false;
        if (gPreClones[i].mouseUpTime > 0.0) {
            if ((now - gPreClones[i].mouseUpTime) >= 1.0) {
                expired = true;
            }
        } else {
            if ((now - gPreClones[i].created_at) >= 30.0) {
                expired = true;
            }
        }
        if (expired) {
            VN_LOG("pre-clone: slot %d (wid=%u, age=%.2fs) timed out without close -- cleaning up",
                   i, gPreClones[i].orig_wid, now - gPreClones[i].created_at);
            clones_to_free[free_count] = gPreClones[i].clone;
            wids_to_free[free_count]   = gPreClones[i].clone_wid;
            free_count++;
            memset(&gPreClones[i], 0, sizeof(VNPreClone));
        }
    }
    os_unfair_lock_unlock(&gPreCloneLock);

    for (int i = 0; i < free_count; i++) {
        if (clones_to_free[i]) {
            if (vn_update_ca_visibility) {
                vn_update_ca_visibility(clones_to_free[i], false);
            }
            if (wids_to_free[i] != 0) {
                CGSOrderOp op = kVNOrderOut;
                uint32_t rel = 0;
                VN_TIME_CALL("order-clone-out-sweep", wids_to_free[i], "?", vn_orig_order(NULL, &wids_to_free[i], &op, &rel, 1, false));
            }
            if (vn_system_window_release) {
                vn_system_window_release(clones_to_free[i]);
            }
        }
    }
}

#pragma mark - Window Surface Cloning

static CGXWindow *vn_make_snapshot(CGXWindow *win, CGXConnection *conn,
                                   uint32_t orig_wid, uint32_t *out_wid,
                                   CGRect *out_frame,
                                   CGSOrderOp place) {
    VN_TIME_SCOPE("make-snapshot", orig_wid);
    if (!vn_create_clone || !vn_window_get_display || !vn_screen_rect_from_rect ||
        !vn_window_get_id) {
        VN_LOG("snapshot: symbols unresolved (clone=%p disp=%p rect=%p id=%p)",
               (void *)vn_create_clone, (void *)vn_window_get_display,
               (void *)vn_screen_rect_from_rect, (void *)vn_window_get_id);
        return NULL;
    }

    const void *display = vn_window_get_display(win);
    if (!display) { VN_LOG("snapshot: no display for wid=%u", orig_wid); return NULL; }

    CGRect bounds = vn_clipped_frame_bounds ? vn_clipped_frame_bounds(win) : CGRectZero;
    CGRect content = CGRectZero;
    if (vn_screen_rect) {
        content = vn_screen_rect(win);
    }
    if (content.size.width < 1.0 || content.size.height < 1.0) {
        if (vn_screen_rect_from_rect) {
            content = vn_screen_rect_from_rect(win, CGRectMake(0.0, 0.0, 1.0, 1.0));
        }
    }

    VNPreferences prefs = vn_get_prefs();
    CGRect frame = CGRectZero;
    if (prefs.shadows && bounds.size.width >= 1.0 && bounds.size.height >= 1.0) {
        frame = bounds;
    } else if (content.size.width >= 1.0 && content.size.height >= 1.0) {
        frame = content;
    } else if (bounds.size.width >= 1.0 && bounds.size.height >= 1.0) {
        frame = bounds;
    }

    if (frame.size.width < 1.0 || frame.size.height < 1.0) {
        VN_LOG("snapshot: no usable frame for wid=%u -- not cloning", orig_wid);
        return NULL;
    }

    if (prefs.skipCloneCreation) {
        // Root-cause isolation: everything above this point (display lookup,
        // bounds/content computation, the prefs fetch) ran identically to a
        // normal close. The only thing skipped is the one step that puts a
        // second, real window into WindowServer's window list. Falls through
        // to the exact same "no clone" path a genuine CreateCloneOfWindow
        // failure already takes -- the close proceeds with no animation, but
        // every hook stays fully active and does all its normal work for it.
        VN_LOG("snapshot: skipCloneCreation is on -- not calling CreateCloneOfWindow for wid=%u", orig_wid);
        return NULL;
    }

    // The single biggest untimed opaque call before this pass: per
    // SkyLightServer.h, CreateCloneOfWindow internally does new_window,
    // set_level_internal, WSWindowSetTitle, set_window_list_tags,
    // WSWindowSetDepth, WSWindowSetHasAlpha, WSWindowInternalSetSharedState,
    // WSWindowSetResolution, WSWindowGetShape, WSWindowSetCapturedContent --
    // by far the most complex single operation Vanish performs, and never
    // measured on its own (only order-clone-in, the call *after* this one,
    // was timed before).
    double _vn_clone_t0 = SLSCurrentRealTime();
    CGXWindow *clone = vn_create_clone(win, frame, display, true);
    double _vn_clone_dt_us = (SLSCurrentRealTime() - _vn_clone_t0) * 1e6;
    VN_LOG("timing: create-clone wid=%u app='?' took %.1fus", orig_wid, _vn_clone_dt_us);
    if (!clone) { VN_LOG("snapshot: CreateCloneOfWindow returned NULL"); return NULL; }

    uint32_t wid = vn_window_get_id(clone);
    if (wid != 0) {
        CGSOrderOp op  = place;
        uint32_t   rel = orig_wid;
        VN_TIME_CALL("order-clone-in", orig_wid, "?", vn_orig_order(conn, &wid, &op, &rel, 1, false));
        if (out_wid) *out_wid = wid;
        if (out_frame) *out_frame = frame;
    }

    if (!prefs.shadows) {
        if (vn_clear_shadow_density) {
            vn_clear_shadow_density(clone);
        } else if (vn_window_set_shadow_enable) {
            vn_window_set_shadow_enable(clone);
        }
        if (vn_window_release_shadow_resources) {
            vn_window_release_shadow_resources(clone);
        }
        if (wid != 0 && vn_set_window_shadow_parameters) {
            vn_set_window_shadow_parameters(0, wid, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        VN_LOG("snapshot: disabled shadow property on clone wid=%u win=%p", wid, clone);
    }

    VN_LOG("snapshot: clone=%p wid=%u %s %u frame=(%.1f,%.1f %.1fx%.1f) display=%p",
           clone, wid, place == kVNOrderBelow ? "below" : "above", orig_wid,
           frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
           display);
    return clone;
}

#pragma mark - Animations

#define kVNMeshW 5
#define kVNMeshH 5
#define kVNMeshCount (kVNMeshW * kVNMeshH)

static double vn_get_display_refresh_interval(CGXWindow *win) {
    static int (*s_PKGDisplayGetCurrentMode)(const void *, void *) = NULL;
    static uint32_t (*s_SLMainDisplayID)(void) = NULL;
    static void * (*s_SLDisplayCopyDisplayMode)(uint32_t) = NULL;
    static double (*s_SLDisplayModeGetRefreshRate)(void *) = NULL;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        s_PKGDisplayGetCurrentMode = (int (*)(const void *, void *))vn_skylight_symbol("_PKGDisplayGetCurrentMode");
        s_SLMainDisplayID = (uint32_t (*)(void))vn_skylight_symbol("SLMainDisplayID");
        s_SLDisplayCopyDisplayMode = (void * (*)(uint32_t))vn_skylight_symbol("SLDisplayCopyDisplayMode");
        s_SLDisplayModeGetRefreshRate = (double (*)(void *))vn_skylight_symbol("SLDisplayModeGetRefreshRate");
    });

    if (win && vn_window_get_display && s_PKGDisplayGetCurrentMode) {
        const void *display = vn_window_get_display(win);
        if (display) {
            uint8_t mode[64] = {0};
            if (s_PKGDisplayGetCurrentMode(display, mode) == 1) {
                float rate = *(float *)(mode + 0x10);
                if (rate >= 30.0f && rate <= 360.0f) {
                    return 1.0 / (double)rate;
                }
            }
        }
    }

    if (s_SLMainDisplayID && s_SLDisplayCopyDisplayMode && s_SLDisplayModeGetRefreshRate) {
        uint32_t disp = s_SLMainDisplayID();
        if (disp != 0) {
            void *mode = s_SLDisplayCopyDisplayMode(disp);
            if (mode) {
                double rate = s_SLDisplayModeGetRefreshRate(mode);
                CFRelease((CFTypeRef)mode);
                if (rate >= 30.0 && rate <= 360.0) {
                    return 1.0 / rate;
                }
            }
        }
    }

    return (1.0 / 120.0);
}

static double vn_get_refresh_interval(CGXWindow *win) {
    VNPreferences prefs = vn_get_prefs();
    if (prefs.refreshRate > 0) {
        return (1.0 / (double)prefs.refreshRate);
    }
    return vn_get_display_refresh_interval(win);
}

static void vn_anim_shrink(VNPointWarp *mesh, CGRect bounds, double t) {
    double s  = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;

    for (unsigned row = 0; row < kVNMeshH; row++) {
        for (unsigned col = 0; col < kVNMeshW; col++) {
            double lx = bounds.size.width  * ((double)col / (kVNMeshW - 1));
            double ly = bounds.size.height * ((double)row / (kVNMeshH - 1));
            double gx = bounds.origin.x + lx;
            double gy = bounds.origin.y + ly;

            VNPointWarp *pt = &mesh[row * kVNMeshW + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + (gx - cx) * s);
            pt->global.y = (float)(cy + (gy - cy) * s);
        }
    }
}

static void vn_anim_tick(void *ctx, double when) {
    (void)ctx; (void)when;

    double now = SLSCurrentRealTime();
    VNAnimSnapshot snapshots[MAX_ACTIVE_ANIMS];
    int snapshot_count = 0;
    uint64_t finished[MAX_ACTIVE_ANIMS];
    int finished_count = 0;
    bool more = false;
    CGXWindow *first_active_win = NULL;

    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (!gActiveAnims[i].is_animating) continue;

        double dur = gActiveAnims[i].duration > 0.0 ? gActiveAnims[i].duration : 0.25;
        double p = (now - gActiveAnims[i].start_time) / dur;

        if (p >= 1.0) {
            finished[finished_count++] = gActiveAnims[i].anim_id;
        } else {
            more = true;
            if (!first_active_win && gActiveAnims[i].clone_win) {
                first_active_win = gActiveAnims[i].clone_win;
            }
            if (snapshot_count < MAX_ACTIVE_ANIMS) {
                snapshots[snapshot_count++] = (VNAnimSnapshot){
                    .clone_win = gActiveAnims[i].clone_win,
                    .bounds = gActiveAnims[i].bounds,
                    .p = p,
                };
            }
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    for (int i = 0; i < finished_count; i++) {
        vn_finish_animation_for_id(finished[i]);
    }

    for (int i = 0; i < snapshot_count; i++) {
        if (vn_set_mesh_warp && snapshots[i].clone_win) {
            VNPointWarp mesh[kVNMeshCount];
            vn_anim_shrink(mesh, snapshots[i].bounds, snapshots[i].p);
            vn_set_mesh_warp(snapshots[i].clone_win, NULL, kVNMeshW, kVNMeshH, (const float *)mesh);
        }
    }

    if (more && vn_schedule_callback) {
        double interval = vn_get_refresh_interval(first_active_win);
        vn_schedule_callback(vn_anim_tick, NULL, SLSCurrentRealTime() + interval);
    } else {
        atomic_store_explicit(&gAnimTimerRunning, false, memory_order_release);
    }
}

static void vn_start_clone_animation(CGXWindow *clone_win, uint32_t orig_wid, CGRect frame,
                                     pid_t pid, uint64_t psn, bool is_wa, const char *app_name) {
    if (!clone_win) return;
    VN_TIME_SCOPE("start-clone-animation", orig_wid);
    uint32_t clone_wid = vn_window_get_id ? vn_window_get_id(clone_win) : 0;
    float dur = vn_duration();

    if ((pid == 0 || psn == 0) && orig_wid != 0 && vn_window_by_id) {
        CGXWindow *orig_win = vn_window_by_id(orig_wid);
        if (orig_win) {
            if (pid == 0 && vn_window_get_owning_pid) pid = vn_window_get_owning_pid(orig_win);
            CGXConnection *c = vn_window_connection(orig_win);
            if (c) {
                if (pid == 0) pid = *(pid_t *)((char *)c + 0x268);
                if (psn == 0) psn = vn_conn_get_psn(c);
            }
        }
    }

    if (!is_wa && app_name && (strcasecmp(app_name, "WhatsApp") == 0)) {
        is_wa = true;
    }

    if (frame.size.width < 1.0 || frame.size.height < 1.0) {
        VNPreferences prefs = vn_get_prefs();
        CGRect b = vn_clipped_frame_bounds ? vn_clipped_frame_bounds(clone_win) : CGRectZero;
        CGRect p = vn_screen_rect ? vn_screen_rect(clone_win) : CGRectZero;
        if (!prefs.shadows && p.size.width >= 1.0 && p.size.height >= 1.0) {
            frame = p;
        } else if (b.size.width >= 1.0 && b.size.height >= 1.0) {
            frame = b;
        } else if (p.size.width >= 1.0 && p.size.height >= 1.0) {
            frame = p;
        } else if (vn_screen_rect_from_rect) {
            frame = vn_screen_rect_from_rect(clone_win, CGRectMake(0.0, 0.0, 1.0, 1.0));
        }
    }

    uint32_t lowest_clone_wid = 0;
    uint64_t max_anim_id = 0;
    int slot = -1;
    uint64_t anim_id = 0;

    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (gActiveAnims[i].is_animating && gActiveAnims[i].clone_wid != 0 && gActiveAnims[i].clone_wid != clone_wid) {
            if (gActiveAnims[i].anim_id > max_anim_id) {
                max_anim_id = gActiveAnims[i].anim_id;
                lowest_clone_wid = gActiveAnims[i].clone_wid;
            }
        }
    }

    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (!gActiveAnims[i].is_animating) {
            slot = i;
            break;
        }
    }
    if (slot == -1) slot = 0;

    anim_id = gNextAnimId++;
    gActiveAnims[slot] = (VNCloneAnim){
        .clone_wid = clone_wid,
        .orig_wid = orig_wid,
        .clone_win = clone_win,
        .anim_id = anim_id,
        .is_animating = true,
        .bounds = frame,
        .start_time = SLSCurrentRealTime(),
        .duration = (double)dur,
        .pid = pid,
        .psn = psn,
        .is_whatsapp = is_wa,
    };
    if (app_name && app_name[0] != '\0') {
        strlcpy(gActiveAnims[slot].app, app_name, sizeof(gActiveAnims[slot].app));
    }
    os_unfair_lock_unlock(&gAnimsLock);

    if (clone_wid != 0) {
        CGSOrderOp op = lowest_clone_wid != 0 ? kVNOrderBelow : kVNOrderAbove;
        uint32_t rel = lowest_clone_wid;
        VN_TIME_CALL("order-clone-visible", clone_wid, app_name ? app_name : "?", vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false));
        VN_LOG("anim: clone wid=%u ordered %s rel=%u (anim_id=%llu)",
               clone_wid, op == kVNOrderBelow ? "below" : "above", rel, anim_id);
    }

    double interval = vn_get_refresh_interval(clone_win);
    double hz = interval > 0.0 ? (1.0 / interval) : 120.0;

    VN_LOG("starting fade animation for clone wid=%u (orig=%u, app='%s' pid=%d psn=0x%llx is_wa=%d) win=%p duration=%.2fs interval=%.2fms (%.0fHz) (anim_id=%llu)",
           clone_wid, orig_wid, app_name ? app_name : "", pid, psn, is_wa, clone_win, dur, interval * 1000.0, hz, anim_id);

    if (vn_schedule_callback) {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(&gAnimTimerRunning, &expected, true,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            vn_schedule_callback(vn_anim_tick, NULL, SLSCurrentRealTime() + interval);
        } else {
            VN_LOG("anim_tick timer loop already active -- clone wid=%u animating concurrently", clone_wid);
        }
    } else {
        vn_finish_animation_for_id(anim_id);
    }
}

#pragma mark - Window Visibility & Process Window Counting

#define MAX_TRACKED_WINDOWS 512

typedef struct {
    uint32_t wid;
    pid_t    pid;
    bool     is_ordered_in;
} VNTrackedWindow;

static VNTrackedWindow gTrackedWindows[MAX_TRACKED_WINDOWS] = {0};
static os_unfair_lock  gTrackedLock = OS_UNFAIR_LOCK_INIT;

static bool vn_is_clone_wid(uint32_t wid, CGXWindow *win) {
    if (wid == 0 && win == NULL) return false;

    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (gActiveAnims[i].is_animating) {
            if ((wid != 0 && gActiveAnims[i].clone_wid == wid) ||
                (win != NULL && gActiveAnims[i].clone_win == win)) {
                os_unfair_lock_unlock(&gAnimsLock);
                return true;
            }
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    os_unfair_lock_lock(&gPreCloneLock);
    for (int i = 0; i < MAX_PRECLONES; i++) {
        if (gPreClones[i].orig_wid != 0) {
            if ((wid != 0 && gPreClones[i].clone_wid == wid) ||
                (win != NULL && gPreClones[i].clone == win)) {
                os_unfair_lock_unlock(&gPreCloneLock);
                return true;
            }
        }
    }
    os_unfair_lock_unlock(&gPreCloneLock);

    return false;
}

static bool vn_check_window_is_ordered_in(uint32_t wid, CGXWindow *win) {
    if (vn_dyn_window_is_ordered_in && win) {
        return vn_dyn_window_is_ordered_in(win);
    }

    if (wid == 0) return false;
    os_unfair_lock_lock(&gTrackedLock);
    for (int i = 0; i < MAX_TRACKED_WINDOWS; i++) {
        if (gTrackedWindows[i].wid == wid) {
            bool state = gTrackedWindows[i].is_ordered_in;
            os_unfair_lock_unlock(&gTrackedLock);
            return state;
        }
    }
    os_unfair_lock_unlock(&gTrackedLock);
    return false;
}

static void vn_track_window_order(uint32_t wid, pid_t pid, CGSOrderOp op, CGXWindow *win) {
    if (wid == 0 || pid == 0 || vn_is_clone_wid(wid, win)) return;

    os_unfair_lock_lock(&gTrackedLock);
    int empty_slot = -1;
    int found_slot = -1;

    for (int i = 0; i < MAX_TRACKED_WINDOWS; i++) {
        if (gTrackedWindows[i].wid == wid) {
            found_slot = i;
            break;
        }
        if (empty_slot == -1 && gTrackedWindows[i].wid == 0) {
            empty_slot = i;
        }
    }

    int target_slot = found_slot >= 0 ? found_slot : empty_slot;
    if (target_slot >= 0) {
        gTrackedWindows[target_slot].wid = wid;
        gTrackedWindows[target_slot].pid = pid;
        gTrackedWindows[target_slot].is_ordered_in = (op != kVNOrderOut);
    }
    os_unfair_lock_unlock(&gTrackedLock);
}

static void vn_track_window_release(uint32_t wid) {
    if (wid == 0) return;
    os_unfair_lock_lock(&gTrackedLock);
    for (int i = 0; i < MAX_TRACKED_WINDOWS; i++) {
        if (gTrackedWindows[i].wid == wid) {
            memset(&gTrackedWindows[i], 0, sizeof(VNTrackedWindow));
            break;
        }
    }
    os_unfair_lock_unlock(&gTrackedLock);
}

static int vn_count_visible_windows_for_pid(pid_t pid, uint32_t exclude_wid) {
    if (pid <= 0) return 0;
    uint32_t candidate_wids[MAX_TRACKED_WINDOWS];
    int candidate_count = 0;

    os_unfair_lock_lock(&gTrackedLock);
    for (int i = 0; i < MAX_TRACKED_WINDOWS; i++) {
        if (gTrackedWindows[i].pid == pid &&
            gTrackedWindows[i].is_ordered_in &&
            gTrackedWindows[i].wid != 0 &&
            gTrackedWindows[i].wid != exclude_wid) {
            candidate_wids[candidate_count++] = gTrackedWindows[i].wid;
        }
    }
    os_unfair_lock_unlock(&gTrackedLock);

    int count = 0;
    for (int i = 0; i < candidate_count; i++) {
        if (!vn_is_clone_wid(candidate_wids[i], NULL)) {
            count++;
        }
    }
    return count;
}

static void vn_cancel_window_animation_if_ordering_in(uint32_t wid, CGXWindow *win, pid_t pid, bool was_already_ordered_in) {
    VN_TIME_SCOPE("cancel-animation-if-ordering-in", wid);
    if (pid == 0 && win && vn_window_get_owning_pid) {
        pid = vn_window_get_owning_pid(win);
    }

    bool has_preclone = false;
    if (wid != 0) {
        os_unfair_lock_lock(&gPreCloneLock);
        has_preclone = (vn_preclone_find_slot_locked(wid) >= 0);
        os_unfair_lock_unlock(&gPreCloneLock);
    }

    int other_visible = vn_count_visible_windows_for_pid(pid, wid);

    CGXWindow *clones_to_release[MAX_ACTIVE_ANIMS] = {0};
    uint32_t clone_wids[MAX_ACTIVE_ANIMS] = {0};
    int release_count = 0;

    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (!gActiveAnims[i].is_animating) continue;

        bool match = false;
        if ((wid != 0 && (gActiveAnims[i].clone_wid == wid || gActiveAnims[i].orig_wid == wid)) ||
            (win != NULL && gActiveAnims[i].clone_win == win)) {
            match = true;
        }
        else if (pid != 0 && gActiveAnims[i].pid == pid && !has_preclone) {
            if (was_already_ordered_in) {
                VN_LOG("Window wid=%u was already ordered in (AppKit sibling restack/focus) -> NOT canceling clone wid=%u",
                       wid, gActiveAnims[i].clone_wid);
            } else {
                if (other_visible == 0) {
                    VN_LOG("Process pid=%d had 0 other visible windows and wid=%u is newly ordering in -> Dock/launch reopen, canceling clone wid=%u",
                           pid, wid, gActiveAnims[i].clone_wid);
                    match = true;
                } else {
                    VN_LOG("Process pid=%d still has %d other visible window(s); wid=%u is a sibling -> NOT canceling clone wid=%u",
                           pid, other_visible, wid, gActiveAnims[i].clone_wid);
                }
            }
        }

        if (match) {
            clones_to_release[release_count] = gActiveAnims[i].clone_win;
            clone_wids[release_count] = gActiveAnims[i].clone_wid;
            release_count++;

            gActiveAnims[i].is_animating = false;
            gActiveAnims[i].clone_wid = 0;
            gActiveAnims[i].orig_wid = 0;
            gActiveAnims[i].clone_win = NULL;
            gActiveAnims[i].pid = 0;
            gActiveAnims[i].psn = 0;
            gActiveAnims[i].is_whatsapp = false;
            gActiveAnims[i].app[0] = '\0';
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    for (int j = 0; j < release_count; j++) {
        CGXWindow *clone_to_release = clones_to_release[j];
        uint32_t clone_wid = clone_wids[j];

        VN_LOG("Target window wid=%u (pid=%d) ordered back in while clone wid=%u was animating; ordering out and destroying clone",
               wid, pid, clone_wid);
        if (vn_update_ca_visibility && clone_to_release) {
            vn_update_ca_visibility(clone_to_release, false);
        }
        if (clone_wid != 0) {
            CGSOrderOp op = kVNOrderOut;
            uint32_t rel = 0;
            VN_TIME_CALL("order-clone-out-cancel", clone_wid, "?", vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false));
        }
        if (clone_to_release) {
            if (vn_schedule_callback) {
                vn_schedule_callback(vn_delayed_clone_release, clone_to_release, SLSCurrentRealTime() + 0.1);
            } else if (vn_system_window_release) {
                vn_system_window_release(clone_to_release);
            }
        }
    }

    if (wid != 0) {
        vn_preclone_discard_wid(wid);
    }
}

#pragma mark - Hooks

static _Atomic(uint64_t) gNonCloseTimeMs = 0;
static _Atomic(uint32_t) gNonCloseWid = 0;

static void vn_hooked_release_window(CGXConnection *conn, CGXWindow *win) {
    uint32_t rel_wid = (win && vn_window_get_id) ? vn_window_get_id(win) : 0;
    VN_TIME_SCOPE("release-window-hook", rel_wid);

    if (rel_wid != 0) {
        vn_track_window_release(rel_wid);
    }

    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (!gActiveAnims[i].is_animating) continue;

        bool match_self = (gActiveAnims[i].clone_win == win) || (rel_wid != 0 && gActiveAnims[i].clone_wid == rel_wid);

        if (match_self) {
            VN_LOG("release_window arrived for animating clone wid=%u win=%p; removing from animation table",
                   gActiveAnims[i].clone_wid, win);
            gActiveAnims[i].is_animating = false;
            gActiveAnims[i].clone_wid = 0;
            gActiveAnims[i].orig_wid = 0;
            gActiveAnims[i].clone_win = NULL;
            break;
        } else if (rel_wid != 0 && gActiveAnims[i].orig_wid == rel_wid) {
            VN_LOG("release_window arrived for orig_wid=%u while clone wid=%u is animating (normal AppKit teardown); clone continues",
                   rel_wid, gActiveAnims[i].clone_wid);
            gActiveAnims[i].orig_wid = 0;
            break;
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    CGXWindow     *clone_to_animate = NULL;
    uint32_t clone_wid = 0, orig_wid = 0;
    CGRect clone_frame = CGRectZero;
    pid_t anim_pid = 0;
    uint64_t anim_psn = 0;
    bool anim_is_wa = false;
    char anim_app[64] = {0};

    os_unfair_lock_lock(&gPreCloneLock);
    for (int i = 0; i < MAX_PRECLONES; i++) {
        if (gPreClones[i].clone == win) {
            memset(&gPreClones[i], 0, sizeof(VNPreClone));
            break;
        }
        if (gPreClones[i].orig_wid != 0 &&
            ((rel_wid != 0 && gPreClones[i].orig_wid == rel_wid) ||
             (vn_window_by_id && vn_window_by_id(gPreClones[i].orig_wid) == win))) {
            orig_wid = gPreClones[i].orig_wid;
            clone_to_animate = vn_preclone_take_locked(orig_wid, &clone_wid, &clone_frame, &anim_pid, &anim_psn, &anim_is_wa, anim_app, sizeof(anim_app));
            break;
        }
    }
    os_unfair_lock_unlock(&gPreCloneLock);

    VN_TIME_CALL("release-window", rel_wid, "?", vn_orig_release_window(conn, win));

    if (clone_to_animate) {
        VN_LOG(">>> Close seen at release_window for wid=%u (app='%s' pid=%d psn=0x%llx is_wa=%d) -- animating pre-clone wid=%u",
               orig_wid, anim_app, anim_pid, anim_psn, anim_is_wa, clone_wid);
        vn_start_clone_animation(clone_to_animate, orig_wid, clone_frame, anim_pid, anim_psn, anim_is_wa, anim_app);
    }
}

// Traffic-light geometry, measured rather than estimated. These are
// -[NSWindow standardWindowButton:] frames read out of AppKit and converted
// into the window-local, top-left-origin space the event carries at +0x20:
//
//   plain titlebar       close (9,9)   14x14, centres x = 16, 39, 62
//   toolbar / unified    close (19,19) 14x14, the whole cluster shifted +10,+10
//
// The hit area is that 14x14 SQUARE, not the 12pt circle drawn inside it --
// a real logged click at (28.0, 32.8) sits 7.1pt from the centre, inside the
// square and outside the circle, so a circle would have started losing genuine
// closes.
//
// Matching the square exactly is what fixes the drag artifact. The button frame
// is precisely where the titlebar's drag region stops, so a pre-clone can no
// longer be built on a pixel that is able to start a drag: the old boxes ran
// from y=4.5 and out to x=26, and that margin was draggable titlebar. A slow
// drag started there moved the window a few points off the clone underneath it
// and showed both, because the 12pt screen-distance threshold had not tripped
// yet.
//
// Distance thresholds cannot fix that on their own. With any threshold T the
// clone is exposed for the first T points of movement, and T small enough to
// hide it is small enough for ordinary click jitter to abort real closes. This
// removes the situation instead of racing it.
#define VN_BTN_SIZE   14.0
#define VN_BTN_PITCH  23.0
#define VN_BTN_STD_X   9.0
#define VN_BTN_STD_Y   9.0
#define VN_BTN_UNI_X  19.0
#define VN_BTN_UNI_Y  19.0

static inline bool vn_pt_in_button(double lx, double ly, double x0, double y0) {
    return lx >= x0 && lx <= x0 + VN_BTN_SIZE &&
           ly >= y0 && ly <= y0 + VN_BTN_SIZE;
}

static inline bool vn_is_in_red_hitbox(double lx, double ly) {
    return vn_pt_in_button(lx, ly, VN_BTN_STD_X, VN_BTN_STD_Y) ||
           vn_pt_in_button(lx, ly, VN_BTN_UNI_X, VN_BTN_UNI_Y);
}

static inline bool vn_is_in_yellow_or_green_hitbox(double lx, double ly) {
    // Each button separately: the gaps between them are draggable titlebar, and
    // treating a gap as yellow/green would arm the 2s non-close suppression and
    // silently cost the next real close its animation.
    for (int i = 1; i <= 2; i++) {
        if (vn_pt_in_button(lx, ly, VN_BTN_STD_X + i * VN_BTN_PITCH, VN_BTN_STD_Y)) return true;
        if (vn_pt_in_button(lx, ly, VN_BTN_UNI_X + i * VN_BTN_PITCH, VN_BTN_UNI_Y)) return true;
    }
    return false;
}

static uint64_t gLastMouseDownTimeMs = 0;
static CGPoint  gLastMouseDownPt     = {0};
static uint32_t gLastMouseDownWid    = 0;
static uint64_t gDoubleClickSuppressUntilMs = 0;

static void vn_hooked_post_event(CGXConnection *conn, void *event) {
    if (!event) {
        if (vn_orig_post_event) vn_orig_post_event(conn, event);
        return;
    }

    uint32_t type = *(const uint32_t *)((const char *)event + 0x8);
    pid_t pid = 0;
    if (conn) {
        pid = *(const pid_t *)((const char *)conn + 0x268);
    }
    // Hoisted so it's available at dispatch: below for the timing log,
    // regardless of which branch (if any) ran. Every branch already reads
    // this same event+0x3c field independently; this doesn't change that,
    // it's a second, harmless read of the same value for a different use.
    uint32_t dispatch_wid = *(const uint32_t *)((const char *)event + 0x3c);
    VN_TIME_SCOPE("post-event-hook", dispatch_wid);

    if (type == 1) {
        uint32_t wid = *(const uint32_t *)((const char *)event + 0x3c);
        if (wid != 0 && vn_window_by_id) {
            CGXWindow *win = vn_window_by_id(wid);
            if (win && vn_is_target_window(win) && !vn_is_window_animating(wid, win)) {
                const CGPoint *screen_pt = (const CGPoint *)((const char *)event + 0x10);
                const CGPoint *local_pt  = (const CGPoint *)((const char *)event + 0x20);
                double lx = local_pt->x;
                double ly = local_pt->y;

                uint64_t now_ms = vn_now_ms();

                bool is_double_click = false;
                if (wid == gLastMouseDownWid && (now_ms - gLastMouseDownTimeMs) < 450) {
                    double dist = hypot(screen_pt->x - gLastMouseDownPt.x, screen_pt->y - gLastMouseDownPt.y);
                    if (dist < 8.0) {
                        is_double_click = true;
                    }
                }

                bool suppressed_by_double_click = (now_ms < gDoubleClickSuppressUntilMs);

                gLastMouseDownTimeMs = now_ms;
                gLastMouseDownPt = *screen_pt;
                gLastMouseDownWid = wid;

                if (is_double_click) {
                    VN_LOG("hit-test: double-click detected on wid=%u (pt=%.1f,%.1f) -- discarding pre-clone and suppressing for 600ms",
                           wid, lx, ly);
                    vn_preclone_discard_wid(wid);
                    gDoubleClickSuppressUntilMs = now_ms + 600;
                    goto dispatch;
                }

                if (suppressed_by_double_click) {
                    VN_LOG("hit-test: click on wid=%u suppressed due to active double-click window", wid);
                    vn_preclone_discard_wid(wid);
                    goto dispatch;
                }

                bool is_red = vn_is_in_red_hitbox(lx, ly);
                bool is_yellow_or_green = vn_is_in_yellow_or_green_hitbox(lx, ly);

                if (lx <= 150.0 && ly <= 60.0) {
                    char hit_app[256] = {0};
                    pid_t hit_pid = 0;
                    vn_get_window_app_name(win, hit_app, sizeof(hit_app), &hit_pid, NULL);
                    VN_LOG("hit-test: app='%s' pid=%d wid=%u pt=(%.1f, %.1f) lvl=%d -> %s",
                           hit_app, hit_pid, wid, lx, ly, vn_window_level(win),
                           is_red ? "RED (close)"
                                  : is_yellow_or_green ? "yellow/green (no animation)"
                                                       : "no traffic light -- NO PRE-CLONE");
                }

                if (is_red) {
                    char app[256] = {0};
                    vn_get_window_app_name(win, app, sizeof(app), &pid, NULL);

                    CGXConnection *c = conn ? conn : vn_window_connection(win);
                    uint64_t psn = vn_conn_get_psn(c);

                    VN_LOG(">>> Mouse down in RED CLOSE box of '%s' (pid=%d, psn=0x%llx, wid=%u, pt=(%.1f, %.1f)) -- pre-cloning now!",
                           app, pid, psn, wid, lx, ly);

                    vn_preclone_discard_wid(wid);
                    atomic_store_explicit(&gNonCloseWid, 0, memory_order_relaxed);

                    uint32_t clone_wid = 0;
                    CGRect clone_frame = CGRectZero;
                    CGXWindow *clone = vn_make_snapshot(win, conn, wid, &clone_wid, &clone_frame, kVNOrderBelow);
                    if (clone && clone_wid != 0) {
                        os_unfair_lock_lock(&gPreCloneLock);
                        int slot = vn_preclone_find_slot_locked(wid);
                        if (slot < 0) slot = vn_preclone_find_empty_slot_locked();
                        bool is_wa = (strcasecmp(app, "WhatsApp") == 0);
                        gPreClones[slot] = (VNPreClone){
                            .orig_wid = wid,
                            .clone_wid = clone_wid,
                            .clone = clone,
                            .frame = clone_frame,
                            .created_at = SLSCurrentRealTime(),
                            .mouseDownScreenPt = *screen_pt,
                            .mouseDownLocalPt  = *local_pt,
                            .mouseUpTime = 0.0,
                            .pid = pid,
                            .psn = psn,
                            .is_whatsapp = is_wa,
                        };
                        strlcpy(gPreClones[slot].app, app, sizeof(gPreClones[slot].app));
                        os_unfair_lock_unlock(&gPreCloneLock);

                        VN_LOG("pre-clone: ready! wid=%u clone_wid=%u in slot %d (ordered below original)", wid, clone_wid, slot);

                        if (vn_schedule_callback) {
                            vn_schedule_callback(vn_preclone_cleanup_timer, NULL, SLSCurrentRealTime() + 30.0);
                        }
                    } else {
                        VN_LOG("pre-clone: failed to create clone for wid=%u", wid);
                    }
                } else if (is_yellow_or_green) {
                    VN_LOG(">>> Mouse down in YELLOW/GREEN button of wid=%u (pt=(%.1f, %.1f)) -- ignoring close animation",
                           wid, lx, ly);
                    vn_preclone_discard_wid(wid);
                    atomic_store_explicit(&gNonCloseWid, wid, memory_order_relaxed);
                    atomic_store_explicit(&gNonCloseTimeMs, vn_now_ms(), memory_order_relaxed);
                } else {
                    vn_preclone_discard_wid(wid);
                }
            }
        }
    }
    else if (type == 2) {
        uint32_t wid = *(const uint32_t *)((const char *)event + 0x3c);
        int slot = -1;
        os_unfair_lock_lock(&gPreCloneLock);
        if (wid != 0) {
            slot = vn_preclone_find_slot_locked(wid);
        }
        os_unfair_lock_unlock(&gPreCloneLock);

        if (slot >= 0) {
            const CGPoint *local_pt = (const CGPoint *)((const char *)event + 0x20);
            double lx = local_pt->x;
            double ly = local_pt->y;

            bool near_red = (lx >= 6.0 && lx <= 42.0 && ly >= 3.0 && ly <= 40.0);
            if (!near_red) {
                VN_LOG("pre-clone: mouse up clearly outside button area (wid=%u pt=(%.1f, %.1f)) -- canceling pre-clone",
                       wid, lx, ly);
                vn_preclone_discard_wid(wid);
            } else {
                os_unfair_lock_lock(&gPreCloneLock);
                if (slot < MAX_PRECLONES && gPreClones[slot].orig_wid == wid) {
                    gPreClones[slot].mouseUpTime = SLSCurrentRealTime();
                }
                os_unfair_lock_unlock(&gPreCloneLock);

                // The success path never logged mouseUp at all before -- only the
                // abort path (near_red false) did. Without this there was no way
                // to measure mouseUp-to-close-visually-complete, which is what a
                // person actually experiences, as opposed to mousedown-to-close-
                // signal, which includes the physical click duration and stops
                // before any animation plays. This only fires when a genuine
                // red-button pre-clone exists for this exact wid -- same rarity
                // as the existing "Mouse down in RED CLOSE box" line, not a
                // high-volume path, so resolving the app name here is fine.
                char up_app[256] = {0};
                pid_t up_pid = 0;
                CGXWindow *up_win = vn_window_by_id ? vn_window_by_id(wid) : NULL;
                if (up_win) vn_get_window_app_name(up_win, up_app, sizeof(up_app), &up_pid, NULL);
                VN_LOG(">>> Mouse up on RED CLOSE box (app='%s' pid=%d wid=%u)", up_app, up_pid, wid);

                if (vn_schedule_callback) {
                    vn_schedule_callback(vn_preclone_cleanup_timer, NULL, SLSCurrentRealTime() + 1.0);
                }
            }
        }
    }
    else if (type == 6) {
        uint32_t wid = *(const uint32_t *)((const char *)event + 0x3c);
        int slot = -1;
        CGPoint down_scr = CGPointZero;
        os_unfair_lock_lock(&gPreCloneLock);
        if (wid != 0) {
            slot = vn_preclone_find_slot_locked(wid);
            if (slot >= 0) {
                down_scr = gPreClones[slot].mouseDownScreenPt;
            }
        }
        os_unfair_lock_unlock(&gPreCloneLock);

        if (slot >= 0) {
            const CGPoint *local_pt  = (const CGPoint *)((const char *)event + 0x20);
            const CGPoint *screen_pt = (const CGPoint *)((const char *)event + 0x10);
            double lx = local_pt->x;
            double ly = local_pt->y;

            bool on_red = vn_is_in_red_hitbox(lx, ly);
            if (!on_red) {
                VN_LOG("pre-clone: dragged off red button pt=(%.1f, %.1f) -- aborting pre-clone for wid=%u",
                       lx, ly, wid);
                vn_preclone_discard_wid(wid);
            } else {
                double d_scr = hypot(screen_pt->x - down_scr.x, screen_pt->y - down_scr.y);
                if (d_scr >= 12.0) {
                    VN_LOG("pre-clone: window drag detected (d_scr=%.1f) -- aborting pre-clone for wid=%u",
                           d_scr, wid);
                    vn_preclone_discard_wid(wid);
                }
            }
        }
    }

dispatch:
    if (vn_orig_post_event) {
        // Fires for every mouse/keyboard event system-wide, not just target
        // windows -- deliberately no app-name resolution here (that's a
        // syscall on the fallback path, per vn_get_window_app_name, and
        // would add cost to the mouseUp/mouseDrag paths that today do zero
        // syscalls of their own). Correlate by wid against the app name
        // already logged in the surrounding "Intercepted close"/pre-clone
        // lines for the same wid instead.
        VN_TIME_CALL("post-event", dispatch_wid, "?", vn_orig_post_event(conn, event));
    }
}

static void vn_order_window_list(CGXConnection *conn, const uint32_t *wids,
                                 const CGSOrderOp *ops, const uint32_t *relativeTo,
                                 unsigned count, bool spaceSwitch) {
    VN_TIME_SCOPE("order-window-list-hook", (wids && count > 0) ? wids[0] : 0);
    if (ops && count >= 1) {
        if (count == 1) {
            uint32_t wid = wids ? wids[0] : 0;
            CGXWindow *win = vn_window_by_id(wid);
            if (win && vn_is_target_window(win)) {
                char app[256] = {0};
                pid_t pid = 0;
                char name_src = '-';
                vn_get_window_app_name(win, app, sizeof(app), &pid, &name_src);

                if (ops[0] == kVNOrderOut) {
                    vn_track_window_order(wid, pid, kVNOrderOut, win);

                    bool animating = vn_is_window_animating(wid, win);
                    if (animating) {
                        VN_LOG("Window wid=%u (%p, app: '%s') already animating; dropping duplicate order-out",
                               wid, win, app);
                        return;
                    }

                    uint64_t now_ms = vn_now_ms();
                    uint64_t non_close_time = atomic_load_explicit(&gNonCloseTimeMs, memory_order_relaxed);
                    uint32_t non_close_wid = atomic_load_explicit(&gNonCloseWid, memory_order_relaxed);
                    if (non_close_wid == wid && (now_ms - non_close_time) < 2000) {
                        VN_LOG("Window wid=%u orderOut is from Miniaturize/Fullscreen (non-close recorded %llu ms ago) -- passing through without animation",
                               wid, (now_ms - non_close_time));
                        VN_TIME_CALL("order-passthrough-nonclose", wid, app, vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch));
                        return;
                    }

                    uint32_t clone_wid = 0;
                    CGXWindow *clone = NULL;
                    CGRect clone_frame = CGRectZero;
                    pid_t anim_pid = 0;
                    uint64_t anim_psn = 0;
                    bool anim_is_wa = false;
                    char anim_app[64] = {0};

                    os_unfair_lock_lock(&gPreCloneLock);
                    clone = vn_preclone_take_locked(wid, &clone_wid, &clone_frame, &anim_pid, &anim_psn, &anim_is_wa, anim_app, sizeof(anim_app));
                    os_unfair_lock_unlock(&gPreCloneLock);

                    if (!clone || clone_wid == 0) {
                        VN_LOG("Window wid=%u orderOut has no pre-clone (not closed via red button) -- passing through directly", wid);
                        VN_TIME_CALL("order-passthrough-noclone", wid, app, vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch));
                        return;
                    }

                    const char *final_app = anim_app[0] ? anim_app : app;
                    if (!anim_is_wa && (strcasecmp(final_app, "WhatsApp") == 0)) {
                        anim_is_wa = true;
                    }
                    VN_LOG(">>> Intercepted close for target window %u (%p, app: '%s' pid=%d psn=0x%llx is_wa=%d name_src=%c)",
                           wid, win, final_app, anim_pid ? anim_pid : pid, anim_psn, anim_is_wa, name_src);

                    VN_TIME_CALL("order-out-close", wid, final_app, vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch));
                    vn_start_clone_animation(clone, wid, clone_frame, anim_pid ? anim_pid : pid, anim_psn, anim_is_wa, final_app);
                    return;
                } else {
                    VN_LOG("Target order op: app='%s' pid=%d count=1 wid=%u op=%d lvl=%d name_src=%c",
                           app, pid, wid, ops[0], vn_window_level(win), name_src);

                    bool was_already_ordered_in = vn_check_window_is_ordered_in(wid, win);
                    vn_track_window_order(wid, pid, ops[0], win);
                    vn_cancel_window_animation_if_ordering_in(wid, win, pid, was_already_ordered_in);
                }
            }

            // The universal single-window fallback -- every non-close order op
            // on every window system-wide, target or not, lands here. Highest
            // volume of any wrapped call site; deliberately no app-name
            // resolution added (win/app aren't in scope here for non-target
            // windows without paying for another vn_get_window_app_name call,
            // which would defeat the point of measuring this cheaply).
            VN_TIME_CALL("order-passthrough-generic", wid, "?", vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch));
            return;
        }

        uint32_t pass_wids[count];
        CGSOrderOp pass_ops[count];
        uint32_t pass_rel[count];
        unsigned pass_count = 0;

        for (unsigned i = 0; i < count; i++) {
            uint32_t wid = wids ? wids[i] : 0;
            CGXWindow *win = vn_window_by_id(wid);

            if (win && vn_is_target_window(win)) {
                char app[256] = {0};
                pid_t pid = 0;
                char name_src = '-';
                vn_get_window_app_name(win, app, sizeof(app), &pid, &name_src);

                VN_LOG("Target order op: app='%s' pid=%d count=%u i=%u wid=%u op=%d lvl=%d name_src=%c",
                       app, pid, count, i, wid, ops[i], vn_window_level(win), name_src);

                if (ops[i] == kVNOrderOut) {
                    vn_track_window_order(wid, pid, kVNOrderOut, win);

                    if (vn_is_window_animating(wid, win)) {
                        VN_LOG("Window wid=%u (%p, app: '%s') already animating; dropping duplicate order-out",
                               wid, win, app);
                        continue;
                    }

                    uint64_t now_ms = vn_now_ms();
                    uint64_t non_close_time = atomic_load_explicit(&gNonCloseTimeMs, memory_order_relaxed);
                    uint32_t non_close_wid = atomic_load_explicit(&gNonCloseWid, memory_order_relaxed);
                    if (non_close_wid == wid && (now_ms - non_close_time) < 2000) {
                        VN_LOG("Window wid=%u multi-orderOut is from Miniaturize/Fullscreen -- passing through without animation", wid);
                        pass_wids[pass_count] = wid;
                        pass_ops[pass_count] = ops[i];
                        pass_rel[pass_count] = relativeTo ? relativeTo[i] : 0;
                        pass_count++;
                        continue;
                    }

                    uint32_t clone_wid = 0;
                    CGXWindow *clone = NULL;
                    CGRect clone_frame = CGRectZero;
                    pid_t anim_pid = 0;
                    uint64_t anim_psn = 0;
                    bool anim_is_wa = false;
                    char anim_app[64] = {0};

                    os_unfair_lock_lock(&gPreCloneLock);
                    clone = vn_preclone_take_locked(wid, &clone_wid, &clone_frame, &anim_pid, &anim_psn, &anim_is_wa, anim_app, sizeof(anim_app));
                    os_unfair_lock_unlock(&gPreCloneLock);

                    if (!clone || clone_wid == 0) {
                        VN_LOG("Window wid=%u multi-orderOut has no pre-clone (not closed via red button) -- passing through", wid);
                        pass_wids[pass_count] = wid;
                        pass_ops[pass_count] = ops[i];
                        pass_rel[pass_count] = relativeTo ? relativeTo[i] : 0;
                        pass_count++;
                        continue;
                    }

                    if (clone && clone_wid != 0) {
                        const char *final_app = anim_app[0] ? anim_app : app;
                        if (!anim_is_wa && (strcasecmp(final_app, "WhatsApp") == 0)) {
                            anim_is_wa = true;
                        }
                        VN_LOG(">>> Intercepted close for target window %u (%p, app: '%s' pid=%d psn=0x%llx is_wa=%d)",
                               wid, win, final_app, anim_pid ? anim_pid : pid, anim_psn, anim_is_wa);
                        vn_start_clone_animation(clone, wid, clone_frame, anim_pid ? anim_pid : pid, anim_psn, anim_is_wa, final_app);
                    }
                    pass_wids[pass_count] = wid;
                    pass_ops[pass_count] = ops[i];
                    pass_rel[pass_count] = relativeTo ? relativeTo[i] : 0;
                    pass_count++;
                    continue;
                } else {
                    bool was_already_ordered_in = vn_check_window_is_ordered_in(wid, win);
                    vn_track_window_order(wid, pid, ops[i], win);
                    vn_cancel_window_animation_if_ordering_in(wid, win, pid, was_already_ordered_in);
                }
            }

            pass_wids[pass_count] = wid;
            pass_ops[pass_count] = ops[i];
            pass_rel[pass_count] = relativeTo ? relativeTo[i] : 0;
            pass_count++;
        }

        if (pass_count > 0) {
            // Batch call, no single wid -- pass_wids[0] as a representative tag.
            VN_TIME_CALL("order-passthrough-batch", pass_wids[0], "?", vn_orig_order(conn, pass_wids, pass_ops, pass_rel, pass_count, spaceSwitch));
        }
        return;
    }

    VN_TIME_CALL("order-passthrough-outer", (wids && count > 0) ? wids[0] : 0, "?", vn_orig_order(conn, wids, ops, relativeTo, count, spaceSwitch));
}

#pragma mark - Entry

extern void MSHookFunction(void *symbol, void *replace, void **result);

__attribute__((constructor))
static void vanish_init(void) {
    // First thing, before anything else -- including the earliest VN_LOG calls
    // below (e.g. the "not WindowServer" line), which need gLogFd already open
    // to actually land anywhere. gLogsEnabled's own static initializer already
    // defaults it true, so nothing else is needed to make early logging work
    // before prefs have ever been loaded.
    gLogFd = open("/tmp/vanish_ws.log", O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (gLogFd >= 0) {
        fchmod(gLogFd, 0666);
    }

    char self[1024] = {0};
    uint32_t len = (uint32_t)sizeof(self);
    if (_NSGetExecutablePath(self, &len) != 0) return;
    if (strstr(self, "WindowServer") == NULL) {
        VN_LOG("not WindowServer (%{public}s) -- doing nothing", self);
        return;
    }

    void *targetOrder               = vn_skylight_symbol(kVNSymOrderWindowList);
    void *targetRelease             = vn_skylight_symbol(kVNSymReleaseWindow);
    void *targetPostEvent           = vn_skylight_symbol(kVNSymPostEventByConnection);
    vn_window_by_id                 = (VNWindowByIDFn)vn_skylight_symbol(kVNSymWindowByID);
    vn_window_get_owning_pid        = (VNWindowGetOwningPIDFn)vn_skylight_symbol(kVNSymWSWindowGetOwningPID);
    vn_get_connection_app_name      = (VNGetConnectionAppNameFn)vn_skylight_symbol(kVNSymGetConnectionAppName);
    vn_schedule_callback            = (VNScheduleCallbackFn)vn_skylight_symbol(kVNSymScheduleCallback);
    vn_set_mesh_warp                = (VNSetMeshWarpFn)vn_skylight_symbol(kVNSymSetMeshWarp);
    vn_create_clone                 = (VNCreateCloneFn)vn_skylight_symbol(kVNSymCreateCloneOfWindow);
    vn_system_window_release        = (VNSystemWindowReleaseFn)vn_skylight_symbol(kVNSymSystemWindowRelease);
    vn_window_get_display           = (VNWindowGetDisplayFn)vn_skylight_symbol(kVNSymWindowGetDisplay);
    vn_screen_rect_from_rect        = (VNScreenRectFromRectFn)vn_skylight_symbol(kVNSymScreenRectFromRect);
    vn_screen_rect                  = (VNScreenRectFn)vn_skylight_symbol(kVNSymScreenRect);
    vn_window_get_id                = (VNWindowGetIDFn)vn_skylight_symbol(kVNSymWindowGetID);
    vn_clipped_frame_bounds         = (VNClippedFrameBoundsFn)vn_skylight_symbol(kVNSymClippedFrameBounds);
    vn_update_ca_visibility         = (VNUpdateCAVisibilityFn)vn_skylight_symbol(kVNSymUpdateCAVisibility);
    vn_clear_shadow_density         = (VNClearShadowDensityFn)vn_skylight_symbol(kVNSymClearShadowDensity);
    vn_window_set_shadow_enable     = (VNWSWindowSetShadowEnableFn)vn_skylight_symbol(kVNSymWSWindowSetShadowEnable);
    vn_window_release_shadow_resources = (VNWSWindowReleaseShadowResourcesFn)vn_skylight_symbol(kVNSymWSWindowReleaseShadowResources);
    vn_set_window_shadow_parameters = (VNSLSSetWindowShadowParametersFn)vn_skylight_symbol(kVNSymSLSSetWindowShadowParameters);

    void *targetEligible = vn_skylight_symbol(kVNSymIsProcessEligibleForSetFront);

    vn_dyn_window_is_ordered_in = (VNDynWindowIsOrderedInFn)vn_skylight_symbol("CGXWindowIsOrderedIn");
    if (!vn_dyn_window_is_ordered_in) {
        vn_dyn_window_is_ordered_in = (VNDynWindowIsOrderedInFn)vn_skylight_symbol("_CGXWindowIsOrderedIn");
    }

    if (!targetOrder || !targetRelease || !vn_window_by_id ||
        !vn_schedule_callback || !vn_set_mesh_warp || !vn_clipped_frame_bounds) {
        VN_LOG("symbol resolution failed -- inert (order=%p release=%p win_by_id=%p mesh_warp=%p)",
               targetOrder, targetRelease, vn_window_by_id, (void *)vn_set_mesh_warp);
        return;
    }

    void *rawRelease = ptrauth_strip(targetRelease, ptrauth_key_function_pointer);
    MSHookFunction(rawRelease, (void *)vn_hooked_release_window, (void **)&vn_orig_release_window);

    void *rawOrder = ptrauth_strip(targetOrder, ptrauth_key_function_pointer);
    MSHookFunction(rawOrder, (void *)vn_order_window_list, (void **)&vn_orig_order);

    if (targetPostEvent) {
        void *rawPost = ptrauth_strip(targetPostEvent, ptrauth_key_function_pointer);
        MSHookFunction(rawPost, (void *)vn_hooked_post_event, (void **)&vn_orig_post_event);
        VN_LOG("hooked CGXPostEventByConnection -> orig %p", vn_orig_post_event);
    } else {
        VN_LOG("WARNING: CGXPostEventByConnection unresolved");
    }

    if (targetEligible) {
        void *rawEligible = ptrauth_strip(targetEligible, ptrauth_key_function_pointer);
        MSHookFunction(rawEligible, (void *)vn_hooked_is_process_eligible, (void **)&vn_orig_is_process_eligible);
        VN_LOG("hooked isProcessEligibleForSetFront -> orig %p", (void *)vn_orig_is_process_eligible);
    } else {
        VN_LOG("WARNING: isProcessEligibleForSetFront unresolved");
    }

    CFNotificationCenterAddObserver(
        CFNotificationCenterGetDarwinNotifyCenter(),
        NULL,
        vn_prefs_changed_callback,
        CFSTR("com.doraorak.vanish/prefsChanged"),
        NULL,
        CFNotificationSuspensionBehaviorDeliverImmediately);
    VN_LOG("Registered Darwin notification observer for com.doraorak.vanish/prefsChanged");

    VN_LOG("Vanish loaded successfully! Target window close hook active (duration: %.2fs)", (double)vn_duration());
}
