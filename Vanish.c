//
//  Vanish.c
//  Smooth window close animations for macOS on Apple Silicon.
//
//  Injected into WindowServer via TweakInject / ellekit. Everything here runs
//  server-side: Vanish never talks to the closing application, and never
//  relies on anything client-side.
//
//  Hooks (vn_hook_*), with their originals kept as vn_orig_*:
//    CGXOrderWindowListSpaceSwitchOptions  every window-ordering path
//    CGXWindow::release_window             window teardown
//    CGXPostEventByConnection              the mouse event stream
//    isProcessEligibleForSetFront          WhatsApp front-eligibility quirk
//  Everything else the server provides is resolved by name at runtime
//  (vn_resolved_*), since none of it is exported -- see SkyLightServer.h.
//
//  How a close actually plays out:
//
//  1. Intent, from the event stream. A mouse-down inside the red button's
//     region is recorded but does nothing yet. A drag off the button, or
//     more than 4pt of movement while held, cancels it. Only the release
//     commits -- so a press that turns into a window drag never produces a
//     clone, and there is nothing to be exposed when the window moves.
//
//  2. Pre-clone on release. The window's surface is cloned and ordered below
//     the original, after the release has been forwarded so the build cost
//     (2ms light, 16ms heavy) is off the input path. The clone's warp path is
//     pre-warmed with an identity mesh so the first animated frame does not
//     also have to realize its surface.
//
//  3. Start on the app's fade, not its order-out. An AppKit or Chromium
//     window does not vanish when closed: the app fades its alpha to zero
//     over ~250ms and only then orders out. Triggering on the order-out
//     therefore meant sitting through the entire close before starting.
//     Vanish watches the original's alpha instead and takes over the moment
//     it drops -- roughly 250ms earlier -- hiding the original so it cannot
//     show through behind the shrinking clone.
//
//  4. Shrink. A uniform scale about the window's centre, driven by
//     CGXWindow::set_mesh_warp on a 2x2 mesh (an affine map needs only its
//     four corners), on an absolute frame deadline so timer lateness cannot
//     accumulate. Despite the log wording, nothing fades: alpha is never
//     animated, only geometry.
//
//  5. Stay out of the way. Some windows already have a system close
//     animation -- double-click a file in Finder and the app transposes the
//     window back into its icon on close, using brand-new proxy windows that
//     fly to the icon. Running ours on top looks broken, so a genuinely new
//     window appearing from the same process just before we would start is
//     taken as a system animation in flight, and we defer to it.
//
//  6. System surfaces (Dock, menu bar, notification centre, wallpaper,
//     tooltips, lock screen, Finder's Get Info panel) pass through
//     untouched.
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

// Compile-time log level. Override with -DVN_LOG_LEVEL=... at build time.
//
//   ERROR  something is actually wrong: a symbol went unresolved, a clone
//          failed to build, a safety backstop had to fire.
//   INFO   one line per close -- what was decided and when. The default,
//          and enough to tell whether a close took the fade path or the
//          order-out path.
//   DEBUG  per-gesture detail: hit tests, pre-clone lifecycle, ordering
//          decisions, hook installation.
//   TRACE  firehose. Every order op on every target window, every hit-test
//          probe. Useful when chasing a specific bug, unusable otherwise.
#define VN_LOG_LEVEL_OFF   0
#define VN_LOG_LEVEL_ERROR 1
#define VN_LOG_LEVEL_INFO  2
#define VN_LOG_LEVEL_DEBUG 3
#define VN_LOG_LEVEL_TRACE 4

#ifndef VN_LOG_LEVEL
#define VN_LOG_LEVEL VN_LOG_LEVEL_TRACE
#endif

#if ENABLE_LOGS
static void vn_log_to_file(const char *fmt, ...) {
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

    int fd = open("/tmp/vanish_ws.log", O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd >= 0) {
        fchmod(fd, 0666);
        write(fd, line, (size_t)line_len);
        close(fd);
    }
}

// Anything above the configured level compiles to nothing -- no call, no
// argument evaluation, no format string in the binary.
#define VN_LOG(fmt, ...) vn_log_to_file(fmt, ##__VA_ARGS__)
#else
#define VN_LOG(fmt, ...) do {} while (0)
#endif

#if ENABLE_LOGS && VN_LOG_LEVEL >= VN_LOG_LEVEL_ERROR
#define VN_ERROR(fmt, ...) VN_LOG(fmt, ##__VA_ARGS__)
#else
#define VN_ERROR(fmt, ...) do {} while (0)
#endif

#if ENABLE_LOGS && VN_LOG_LEVEL >= VN_LOG_LEVEL_INFO
#define VN_INFO(fmt, ...) VN_LOG(fmt, ##__VA_ARGS__)
#else
#define VN_INFO(fmt, ...) do {} while (0)
#endif

#if ENABLE_LOGS && VN_LOG_LEVEL >= VN_LOG_LEVEL_DEBUG
#define VN_DEBUG(fmt, ...) VN_LOG(fmt, ##__VA_ARGS__)
#else
#define VN_DEBUG(fmt, ...) do {} while (0)
#endif

#if ENABLE_LOGS && VN_LOG_LEVEL >= VN_LOG_LEVEL_TRACE
#define VN_TRACE(fmt, ...) VN_LOG(fmt, ##__VA_ARGS__)
#else
#define VN_TRACE(fmt, ...) do {} while (0)
#endif

/// Close animations are described by a registry further down; the structs
/// that carry a chosen one are defined before it.
typedef struct VNAnimation VNAnimation;

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
static VNWindowByIDFn               vn_resolved_window_by_id;
static VNScheduleCallbackFn         vn_resolved_schedule_callback;
static VNSetMeshWarpFn              vn_resolved_set_mesh_warp;
static VNWindowGetFilterFn          vn_resolved_window_get_filter;
static VNWindowSetFilterFn          vn_resolved_window_set_filter;
static VNUpdateWindowFn             vn_resolved_update_window;
static VNCreateCloneFn              vn_resolved_create_clone;
static VNSystemWindowReleaseFn      vn_resolved_system_window_release;
static VNWindowGetDisplayFn         vn_resolved_window_get_display;
static VNScreenRectFromRectFn       vn_resolved_screen_rect_from_rect;
static VNScreenRectFn               vn_resolved_screen_rect;
static VNWindowGetIDFn              vn_resolved_window_get_id;
static VNClippedFrameBoundsFn       vn_resolved_clipped_frame_bounds;
static VNReleaseWindowFn            vn_orig_release_window;
static VNWindowGetOwningPIDFn       vn_resolved_window_get_owning_pid;
static VNGetConnectionAppNameFn     vn_resolved_get_connection_app_name;
static VNPostEventByConnectionFn    vn_orig_post_event;
static VNUpdateCAVisibilityFn       vn_resolved_update_ca_visibility;
static VNClearShadowDensityFn            vn_resolved_clear_shadow_density;
static VNWSWindowSetShadowEnableFn        vn_resolved_window_set_shadow_enable;
static VNWSWindowReleaseShadowResourcesFn vn_resolved_window_release_shadow_resources;
static VNSLSSetWindowShadowParametersFn   vn_resolved_set_window_shadow_parameters;
static VNIsProcessEligibleForSetFrontFn    vn_orig_is_process_eligible;
static VNMetalCompositeRippleFn     vn_orig_metal_composite_ripple;

typedef bool (*VNDynWindowIsOrderedInFn)(const CGXWindow *win);
static VNDynWindowIsOrderedInFn     vn_resolved_window_is_ordered_in = NULL;

#pragma mark - Preferences

typedef struct {
    bool  enabled;
    bool  shadows;
    float refreshRate;
    float duration;
    char  targetApp[256];
    char  animation[64];
} VNPreferences;

static VNPreferences  gPrefs = { .enabled = true, .shadows = true, .refreshRate = 120.0f, .duration = 0.25f, .targetApp = "all", .animation = "shrink" };
static os_unfair_lock gPrefsLock = OS_UNFAIR_LOCK_INIT;
static struct timespec gPrefsMtime = {0};
static bool           gPrefsValid = false;

static void vn_reload_prefs_locked(void) {
    gPrefs.enabled = true;
    gPrefs.shadows = true;
    gPrefs.refreshRate = 120.0f;
    gPrefs.duration = 0.25f;
    strlcpy(gPrefs.targetApp, "all", sizeof(gPrefs.targetApp));
    strlcpy(gPrefs.animation, "shrink", sizeof(gPrefs.animation));

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

                        CFStringRef animVal = (CFStringRef)CFDictionaryGetValue(dict, CFSTR("animation"));
                        if (animVal && CFGetTypeID(animVal) == CFStringGetTypeID()) {
                            char s[sizeof(gPrefs.animation)] = {0};
                            if (CFStringGetCString(animVal, s, sizeof(s), kCFStringEncodingUTF8) && s[0] != '\0') {
                                strlcpy(gPrefs.animation, s, sizeof(gPrefs.animation));
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
}

static VNPreferences vn_get_prefs(void) {
    // The stat() below is only a fallback for edits that arrive without the
    // Darwin notification (editing the plist directly, say). Rate-limit it so
    // that a caller on a hot path cannot turn prefs reads into a syscall per
    // frame; the notification still applies real changes immediately.
    static double s_last_stat = 0.0;
    double now = SLSCurrentRealTime();
    bool skip_stat = gPrefsValid && (now - s_last_stat) < 0.25;
    if (skip_stat) {
        os_unfair_lock_lock(&gPrefsLock);
        VNPreferences cached = gPrefs;
        os_unfair_lock_unlock(&gPrefsLock);
        return cached;
    }
    s_last_stat = now;

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
    VN_DEBUG("Preferences changed notification received -- invalidating cache");
    os_unfair_lock_lock(&gPrefsLock);
    gPrefsValid = false;
    os_unfair_lock_unlock(&gPrefsLock);
}

static float vn_duration(void) {
    VNPreferences prefs = vn_get_prefs();
    return prefs.duration;
}

#pragma mark - Window Eligibility Filtering

// Resolved app names, keyed by the window's connection id.
//
// Also found while chasing the resize flash, and also kept on its own merits
// rather than as a fix for it: this is called from vn_is_target_window, which
// runs on every order operation for every window -- and the order path sees
// real bursts (15 ops in 3ms from Chrome).
// Uncached, each of those could reach a proc_pidpath syscall. A connection
// belongs to one process for its whole life, so the name never changes under
// a live cid. A cid outliving its process and being reissued is guarded
// against by also matching the owning pid, which comes from a cheap server
// field read rather than the syscall this cache exists to avoid.
#define kVNAppNameCacheSlots 64

typedef struct {
    uint32_t cid;
    pid_t    pid;
    char     name[64];
    bool     valid;
} VNAppNameEntry;

static VNAppNameEntry gAppNameCache[kVNAppNameCacheSlots];
static os_unfair_lock gAppNameCacheLock = OS_UNFAIR_LOCK_INIT;

static bool vn_app_name_cache_get(uint32_t cid, pid_t pid, char *out_name, size_t maxlen) {
    if (cid == 0) return false;
    bool hit = false;
    os_unfair_lock_lock(&gAppNameCacheLock);
    VNAppNameEntry *e = &gAppNameCache[cid % kVNAppNameCacheSlots];
    if (e->valid && e->cid == cid && e->pid == pid) {
        strlcpy(out_name, e->name, maxlen);
        hit = true;
    }
    os_unfair_lock_unlock(&gAppNameCacheLock);
    return hit;
}

static void vn_app_name_cache_put(uint32_t cid, const char *name, pid_t pid) {
    if (cid == 0 || !name || !name[0]) return;
    os_unfair_lock_lock(&gAppNameCacheLock);
    VNAppNameEntry *e = &gAppNameCache[cid % kVNAppNameCacheSlots];
    e->cid = cid;
    e->pid = pid;
    strlcpy(e->name, name, sizeof(e->name));
    e->valid = true;
    os_unfair_lock_unlock(&gAppNameCacheLock);
}

static bool vn_get_window_app_name(CGXWindow *win, char *out_name, size_t maxlen, pid_t *out_pid) {
    if (!win || !out_name || maxlen == 0) return false;
    out_name[0] = '\0';

    pid_t pid = 0;
    if (vn_resolved_window_get_owning_pid) {
        pid = vn_resolved_window_get_owning_pid(win);
    } else {
        CGXConnection *c = vn_window_connection(win);
        if (c) pid = vn_conn_get_pid(c);
    }
    if (out_pid) *out_pid = pid;

    if (vn_app_name_cache_get(win->connection_id, pid, out_name, maxlen)) {
        return true;
    }

    if (vn_resolved_get_connection_app_name) {
        uint32_t cid = win->connection_id;
        if (cid != 0) {
            char buf[256] = {0};
            if (vn_resolved_get_connection_app_name(cid, buf, sizeof(buf)) == 0 && buf[0] != '\0') {
                strncpy(out_name, buf, maxlen - 1);
                vn_app_name_cache_put(cid, out_name, pid);
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
                vn_app_name_cache_put(win->connection_id, out_name, pid);
                return true;
            }
        }
        if (proc_name(pid, out_name, (uint32_t)maxlen) > 0 && out_name[0] != '\0') {
            vn_app_name_cache_put(win->connection_id, out_name, pid);
            return true;
        }
    }

    return false;
}

static bool vn_is_target_window(CGXWindow *win) {
    if (!win) return false;

    VNPreferences prefs = vn_get_prefs();
    if (!prefs.enabled) return false;

    char name[256] = {0};
    pid_t pid = 0;
    bool has_name = vn_get_window_app_name(win, name, sizeof(name), &pid);

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
                CGRect content = vn_resolved_screen_rect ? vn_resolved_screen_rect(win) : CGRectZero;
                if (content.size.width < 1.0 && vn_resolved_clipped_frame_bounds) {
                    content = vn_resolved_clipped_frame_bounds(win);
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
    const VNAnimation *anim;
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

static bool vn_hook_is_process_eligible(uint32_t sessionID, uint64_t psn, bool flag, bool *out) {
    if (vn_is_whatsapp_closing_or_recently_closed(psn)) {
        VN_DEBUG("isProcessEligibleForSetFront: suppressing front eligibility for WhatsApp (psn=0x%llx)", psn);
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
    const VNAnimation *anim;
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

static void vn_filter_detach(CGXWindow *clone);

/// Every path that releases a clone goes through here, so a filter can never
/// outlive the window it hangs off. There are four of them -- the animation
/// finishing, a pre-clone discarded, the pre-clone cleanup timer, and the
/// release-window hook -- and a detach missing from any one leaks the object.
static void vn_release_clone(CGXWindow *clone) {
    if (!clone) return;
    vn_filter_detach(clone);
    if (vn_resolved_system_window_release) vn_resolved_system_window_release(clone);
}

static void vn_delayed_clone_release(void *ctx, double when) {
    (void)when;
    CGXWindow *clone = (CGXWindow *)ctx;
    if (clone && vn_resolved_system_window_release) {
        VN_TRACE("delayed_release: freeing clone win=%p", clone);
        vn_release_clone(clone);
    }
}

static void vn_finish_animation_for_id(uint64_t anim_id) {
    uint32_t clone_wid = 0;
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
            gActiveAnims[i].anim = NULL;
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

    VN_DEBUG("finish_animation: hiding and ordering out clone wid=%u win=%p (app='%s' pid=%d psn=0x%llx is_wa=%d release deferred 100ms)",
           clone_wid, clone_win, finished_app, finished_pid, finished_psn, finished_is_wa);
    if (clone_win && vn_resolved_update_ca_visibility) {
        vn_resolved_update_ca_visibility(clone_win, false);
    }
    if (clone_wid != 0) {
        CGSOrderOp op = kVNOrderOut;
        uint32_t rel = 0;
        vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false);
    }
    if (vn_resolved_schedule_callback && clone_win) {
        vn_resolved_schedule_callback(vn_delayed_clone_release, clone_win, SLSCurrentRealTime() + 0.1);
    } else if (clone_win) {
        vn_release_clone(clone_win);
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
    // Resolved once, when the clone is built, and carried to the running
    // animation: no preference read or string compare per frame, and the
    // choice cannot change halfway through a close.
    const VNAnimation *anim;
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
                                          const VNAnimation **out_anim,
                                          pid_t *out_pid, uint64_t *out_psn, bool *out_is_wa, char *out_app, size_t app_len) {
    int idx = vn_preclone_find_slot_locked(orig_wid);
    if (idx < 0) return NULL;

    CGXWindow *clone = gPreClones[idx].clone;
    if (out_clone_wid) *out_clone_wid = gPreClones[idx].clone_wid;
    if (out_frame) *out_frame = gPreClones[idx].frame;
    if (out_anim) *out_anim = gPreClones[idx].anim;
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
    uint32_t clone_wid = 0;
    os_unfair_lock_lock(&gPreCloneLock);
    CGXWindow *clone = vn_preclone_take_locked(orig_wid, &clone_wid, NULL, NULL, NULL, NULL, NULL, NULL, 0);
    os_unfair_lock_unlock(&gPreCloneLock);

    if (clone) {
        VN_DEBUG("pre-clone: discarding and hiding unused clone wid=%u for orig=%u", clone_wid, orig_wid);
        if (vn_resolved_update_ca_visibility) {
            vn_resolved_update_ca_visibility(clone, false);
        }
        if (clone_wid != 0) {
            CGSOrderOp op = kVNOrderOut;
            uint32_t rel = 0;
            vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false);
        }
        if (vn_resolved_system_window_release) {
            vn_release_clone(clone);
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
            VN_DEBUG("pre-clone: slot %d (wid=%u, age=%.2fs) timed out without close -- cleaning up",
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
            if (vn_resolved_update_ca_visibility) {
                vn_resolved_update_ca_visibility(clones_to_free[i], false);
            }
            if (wids_to_free[i] != 0) {
                CGSOrderOp op = kVNOrderOut;
                uint32_t rel = 0;
                vn_orig_order(NULL, &wids_to_free[i], &op, &rel, 1, false);
            }
            if (vn_resolved_system_window_release) {
                vn_release_clone(clones_to_free[i]);
            }
        }
    }
}

#pragma mark - Window Surface Cloning

/// `conn` is the connection making the request, which is not necessarily the
/// window's own (win->connection) -- see the fallback below -- so it stays an
/// explicit parameter. The window id does not: it is win->window_id.
/// Mesh geometry and the shrink solver both live further down with the
/// animation; declared here so a freshly built clone can be pre-warmed.
// Kept with the same caveat as gAnimNextDeadline: this reduces per-frame
// compositor work, but it was never measured in isolation and the "hang" it
// was chasing had another cause. The equivalence argument below is exact
// regardless, so the change is free even if it buys nothing.
//
// The shrink is a uniform scale about the window's centre -- a pure affine
// map. Bilinear interpolation of a quad's corners reproduces an affine map
// exactly, so the four corners carry the same image a 5x5 grid did, and the
// compositor resamples 4 points per frame instead of 25. Raise this if the
// animation ever becomes non-affine (a bend, a ripple, per-point easing);
// interpolation would no longer be exact and the extra points would matter.
#pragma mark - Animation registry

// Each animation deforms the clone by filling a warp mesh: for every point,
// where it sits in the window and where to draw it on screen. Progress `t`
// runs 0 -> 1 across the animation's duration.
//
// Two rules for anything added here:
//
//   At t = 0 the mesh MUST be the identity (every point drawn where it
//   lives). vn_make_snapshot primes the warp path with a t = 0 fill so the
//   first animated frame does not also have to realize the clone's surface,
//   and that priming is only invisible if t = 0 changes nothing.
//
//   Pick the smallest mesh that is exact. Points between mesh vertices are
//   interpolated, which reproduces an affine map (scale, rotation, shear)
//   exactly from its corners alone -- so a uniform shrink needs 2x2 and
//   gains nothing from more. Only genuinely non-affine motion, where
//   different parts of the window move on different curves, needs a denser
//   grid, and every extra vertex is per-frame work for the compositor.
#define kVNMeshMaxDim   16
#define kVNMeshMaxCount (kVNMeshMaxDim * kVNMeshMaxDim)

// Swirl grid. Dense both ways: the twist carries horizontal and vertical
// phases plus two ripple harmonics travelling across the window, so extra
// rows AND columns both add real shape. 168 points at 16 bytes each is ~2.7KB
// of the 4KB stack budget in vn_anim_tick.
#define kVNMeshSwirlW 12
#define kVNMeshSwirlH 14

// Three ways to animate a clone, and they are different enough to warrant
// different arms rather than one flattened struct:
//
//   MESH    deforms the clone through CGXWindow::set_mesh_warp. Moves existing
//           pixels; cannot make new ones.
//   FILTER  hands the clone to one of the compositor's own effects by setting
//           CGXWindow::filter. A closed set of two -- ripple and colour invert
//           are all _XNewCIFilter will accept -- but they are real Metal
//           effects and cost us nothing per frame beyond five floats.
//   SHADER  our own Metal shader, reached through the same filter slot: a
//           type-1 filter routes the layer to metal_composite_ripple, which is
//           a narrow function we can hook without touching the compositor's
//           per-layer hot path. Nothing implements this yet.
typedef enum {
    VN_ANIM_MESH   = 0,
    VN_ANIM_FILTER = 1,
    VN_ANIM_SHADER = 2,   // reserved; nothing implements this yet
} VNAnimKind;

struct VNAnimation {
    const char *key;      // stored in prefs -- stable, do not rename
    const char *title;    // shown in the preferences pane
    VNAnimKind  kind;
    union {
        struct {
            unsigned w, h;
            void (*fill)(VNPointWarp *mesh, CGRect bounds, double t);
        } mesh;
        struct {
            uint32_t type;   // kVNFilterType*
            void (*fill_params)(float params[5], CGRect bounds, double t);
        } filter;
    };
};

static void vn_anim_shrink(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_squish(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_fall(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_swirl(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_flip(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_tilt(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_slide(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_genie(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_flag(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_spin(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_roll(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_barrel(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_clock(VNPointWarp *mesh, CGRect bounds, double t);
static void vn_anim_ripple(float params[5], CGRect bounds, double t);

// The first entry is the fallback for a missing or unrecognised preference.
static const VNAnimation gAnimations[] = {
    { "shrink",   "Shrink",   VN_ANIM_MESH, .mesh = { 2, 2, vn_anim_shrink } },
    { "squish",   "Squish",   VN_ANIM_MESH, .mesh = { 2, 3, vn_anim_squish } },
    { "fall",     "Fall",     VN_ANIM_MESH, .mesh = { 2, 2, vn_anim_fall } },
    { "swirl",    "Swirl",    VN_ANIM_MESH, .mesh = { kVNMeshSwirlW, kVNMeshSwirlH, vn_anim_swirl } },
    { "flip",     "Flip",     VN_ANIM_MESH, .mesh = { 2, 2, vn_anim_flip } },
    { "tilt",     "Tilt",     VN_ANIM_MESH, .mesh = { 2, 2, vn_anim_tilt } },
    { "slide",    "Slide",    VN_ANIM_MESH, .mesh = { 2, 2, vn_anim_slide } },
    { "genie",    "Genie",    VN_ANIM_MESH, .mesh = { 6, 10, vn_anim_genie } },
    { "flag",     "Flag",     VN_ANIM_MESH, .mesh = { 8, 6, vn_anim_flag } },
    { "spin",     "Spin",     VN_ANIM_MESH, .mesh = { 2, 2, vn_anim_spin } },
    { "roll",     "Roll",     VN_ANIM_MESH, .mesh = { 6, 12, vn_anim_roll } },
    { "barrel",   "Barrel",   VN_ANIM_MESH, .mesh = { 8, 8, vn_anim_barrel } },
    { "clock",    "Clock",    VN_ANIM_MESH, .mesh = { 8, 8, vn_anim_clock } },
    { "ripple",   "Ripple",   VN_ANIM_FILTER, .filter = { kVNFilterTypeRipple, vn_anim_ripple } },
};
#define kVNAnimationCount (sizeof(gAnimations) / sizeof(gAnimations[0]))

static const VNAnimation *vn_animation_for_key(const char *key) {
    if (key && key[0]) {
        for (size_t i = 0; i < kVNAnimationCount; i++) {
            if (strcasecmp(gAnimations[i].key, key) == 0) return &gAnimations[i];
        }
    }
    return &gAnimations[0];
}

#pragma mark - Window filters

// A filter hung off CGXWindow::filter routes the clone into one of the
// compositor's own composite functions, and carries five floats there every
// frame at no cost to us (see VNWindowFilter in SkyLightServer.h).
//
// We allocate the object ourselves rather than going through _XNewCIFilter,
// which is a MIG server routine expecting a serialized plist from a client and
// registers what it makes in a process-wide list. Skipping that is safe in both
// directions: nothing else ever sees our object, and if the server does tear it
// down -- CGXWindow's destructor calls CGXReleaseWindowCIFilters -- the unlink
// walk simply fails to find it in that list and falls through to the free().
// We still detach explicitly, so the lifetime does not depend on the
// destructor running.

static bool vn_filter_attach(CGXWindow *clone, uint32_t type, const float params[5]) {
    if (!clone || !vn_resolved_window_set_filter) return false;

    VNWindowFilter *f = calloc(1, sizeof(VNWindowFilter));
    if (!f) return false;

    f->refcount = 1;      // so a server-side release frees it exactly once
    f->type     = type;
    memcpy(f->params, params, sizeof(f->params));

    vn_resolved_window_set_filter(clone, f);
    return true;
}

/// New parameters for the next frame. Both this and the compositor's read in
/// generate_layers_for_window happen on the server's own thread -- the same one
/// the animation tick runs on -- so the five floats are never torn.
///
/// The update_window call is not optional: writing the parameters leaves the
/// window clean, and a clean window is never re-composited, so the change would
/// not appear until something else happened to dirty it.
static void vn_filter_update(CGXWindow *clone, const float params[5]) {
    if (!clone || !vn_resolved_window_get_filter) return;

    VNWindowFilter *f = vn_resolved_window_get_filter(clone);
    if (!f) return;
    memcpy(f->params, params, sizeof(f->params));

    if (vn_resolved_update_window) {
        vn_resolved_update_window(vn_window_connection(clone), clone);
    }
}

/// Must run before the clone is released, on every path that releases one.
static void vn_filter_detach(CGXWindow *clone) {
    if (!clone || !vn_resolved_window_get_filter || !vn_resolved_window_set_filter) return;

    VNWindowFilter *f = vn_resolved_window_get_filter(clone);
    if (!f) return;
    vn_resolved_window_set_filter(clone, NULL);
    free(f);
}

// CIShapedWaterRipple's five floats, as metal_composite_ripple consumes them:
// params[0..1] and params[2..3] are two float pairs it multiplies by the
// layer's scale, and params[4] goes into the uniform buffer untouched. Their
// exact meaning is not documented anywhere and is not yet worked out from the
// shader, so this drives them from t and leaves tuning to what it looks like.
//
// At t = 0 every one of them is zero, which is the registry's identity rule:
// vn_make_snapshot attaches the filter with a t = 0 fill while the clone is
// still hidden, and that priming is only invisible if it changes nothing.
static void vn_anim_ripple(float params[5], CGRect bounds, double t) {
    const float w = (float)bounds.size.width;
    const float h = (float)bounds.size.height;

    params[0] = (float)t * w * 0.5f;
    params[1] = (float)t * h * 0.5f;
    params[2] = (float)t * w * 0.5f;
    params[3] = (float)t * h * 0.5f;
    params[4] = (float)t;
}


static CGXWindow *vn_make_snapshot(CGXWindow *win, CGXConnection *conn,
                                   uint32_t *out_wid, CGRect *out_frame,
                                   const VNAnimation **out_anim,
                                   CGSOrderOp place) {
    const uint32_t orig_wid = win->window_id;
    if (!vn_resolved_create_clone || !vn_resolved_window_get_display || !vn_resolved_screen_rect_from_rect ||
        !vn_resolved_window_get_id) {
        VN_ERROR("snapshot: symbols unresolved (clone=%p disp=%p rect=%p id=%p)",
               (void *)vn_resolved_create_clone, (void *)vn_resolved_window_get_display,
               (void *)vn_resolved_screen_rect_from_rect, (void *)vn_resolved_window_get_id);
        return NULL;
    }

    double t_snap0 = SLSCurrentRealTime();
    const void *display = vn_resolved_window_get_display(win);
    if (!display) { VN_ERROR("snapshot: no display for wid=%u", orig_wid); return NULL; }

    CGRect bounds = vn_resolved_clipped_frame_bounds ? vn_resolved_clipped_frame_bounds(win) : CGRectZero;
    CGRect content = CGRectZero;
    if (vn_resolved_screen_rect) {
        content = vn_resolved_screen_rect(win);
    }
    if (content.size.width < 1.0 || content.size.height < 1.0) {
        if (vn_resolved_screen_rect_from_rect) {
            content = vn_resolved_screen_rect_from_rect(win, CGRectMake(0.0, 0.0, 1.0, 1.0));
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
        VN_ERROR("snapshot: no usable frame for wid=%u -- not cloning", orig_wid);
        return NULL;
    }

    const VNAnimation *anim = vn_animation_for_key(prefs.animation);

    double t_clone0 = SLSCurrentRealTime();
    CGXWindow *clone = vn_resolved_create_clone(win, frame, display, true);
    double clone_ms = (SLSCurrentRealTime() - t_clone0) * 1000.0;
    if (!clone) { VN_ERROR("snapshot: CreateCloneOfWindow returned NULL"); return NULL; }

    uint32_t wid = vn_resolved_window_get_id(clone);
    if (wid != 0) {
        CGSOrderOp op  = place;
        uint32_t   rel = orig_wid;
        vn_orig_order(conn, &wid, &op, &rel, 1, false);
        if (out_wid) *out_wid = wid;
        if (out_frame) *out_frame = frame;
        if (out_anim) *out_anim = anim;
    }

    // Pre-warm the warp path.
    //
    // Kept with the same caveat as gAnimNextDeadline: measured real, but the
    // "hang" it was chasing turned out to be the transpose collision.
    //
    // Measured: the first frame of an animation
    // arrives up to 18.5ms late -- over twice its 8.33ms budget, and by far
    // the worst frame in a run -- because the first set_mesh_warp on a clone
    // has to realize that window's surface and allocate mesh state. Doing it
    // here, tens of milliseconds before the animation starts and while the
    // clone is still hidden behind the original, moves that cost off the
    // first frame. At t=0 the shrink solver is an identity warp (s = 1.0), so
    // this cannot change what is on screen.
    if (anim->kind == VN_ANIM_FILTER && anim->filter.fill_params) {
        float params[5] = {0};
        anim->filter.fill_params(params, frame, 0.0);
        vn_filter_attach(clone, anim->filter.type, params);
    } else if (vn_resolved_set_mesh_warp && anim->kind == VN_ANIM_MESH && anim->mesh.fill) {
        VNPointWarp warm[kVNMeshMaxCount];
        anim->mesh.fill(warm, frame, 0.0);
        vn_resolved_set_mesh_warp(clone, NULL, anim->mesh.w, anim->mesh.h, (const float *)warm);
    }

    if (!prefs.shadows) {
        if (vn_resolved_clear_shadow_density) {
            vn_resolved_clear_shadow_density(clone);
        } else if (vn_resolved_window_set_shadow_enable) {
            vn_resolved_window_set_shadow_enable(clone);
        }
        if (vn_resolved_window_release_shadow_resources) {
            vn_resolved_window_release_shadow_resources(clone);
        }
        if (wid != 0 && vn_resolved_set_window_shadow_parameters) {
            vn_resolved_set_window_shadow_parameters(0, wid, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        VN_DEBUG("snapshot: disabled shadow property on clone wid=%u win=%p", wid, clone);
    }

    // The clone is built synchronously in the event hook, before the app is
    // even handed the mouse-up, so anything slow here is felt as a hitch at
    // the click itself rather than as a dropped animation frame.
    VN_DEBUG("snapshot: clone=%p wid=%u %s %u frame=(%.1f,%.1f %.1fx%.1f) display=%p build=%.2fms total=%.2fms",
           clone, wid, place == kVNOrderBelow ? "below" : "above", orig_wid,
           frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
           display, clone_ms, (SLSCurrentRealTime() - t_snap0) * 1000.0);
    return clone;
}

#pragma mark - Animations


static double vn_get_display_refresh_interval(CGXWindow *win) {
    static int (*vn_resolved_display_get_current_mode)(const void *, void *) = NULL;
    static uint32_t (*vn_resolved_main_display_id)(void) = NULL;
    static void * (*vn_resolved_display_copy_mode)(uint32_t) = NULL;
    static double (*vn_resolved_display_mode_refresh_rate)(void *) = NULL;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        vn_resolved_display_get_current_mode = (int (*)(const void *, void *))vn_skylight_symbol("_PKGDisplayGetCurrentMode");
        vn_resolved_main_display_id = (uint32_t (*)(void))vn_skylight_symbol("SLMainDisplayID");
        vn_resolved_display_copy_mode = (void * (*)(uint32_t))vn_skylight_symbol("SLDisplayCopyDisplayMode");
        vn_resolved_display_mode_refresh_rate = (double (*)(void *))vn_skylight_symbol("SLDisplayModeGetRefreshRate");
    });

    if (win && vn_resolved_window_get_display && vn_resolved_display_get_current_mode) {
        const void *display = vn_resolved_window_get_display(win);
        if (display) {
            uint8_t mode[64] = {0};
            if (vn_resolved_display_get_current_mode(display, mode) == 1) {
                float rate = *(float *)(mode + 0x10);
                if (rate >= 30.0f && rate <= 360.0f) {
                    return 1.0 / (double)rate;
                }
            }
        }
    }

    if (vn_resolved_main_display_id && vn_resolved_display_copy_mode && vn_resolved_display_mode_refresh_rate) {
        uint32_t disp = vn_resolved_main_display_id();
        if (disp != 0) {
            void *mode = vn_resolved_display_copy_mode(disp);
            if (mode) {
                double rate = vn_resolved_display_mode_refresh_rate(mode);
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

// Uniform scale about the centre. Affine, so 2x2 is exact.
static void vn_anim_shrink(VNPointWarp *mesh, CGRect bounds, double t) {
    double s  = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;

    for (unsigned row = 0; row < 2; row++) {
        for (unsigned col = 0; col < 2; col++) {
            double lx = bounds.size.width  * (double)col;
            double ly = bounds.size.height * (double)row;
            double gx = bounds.origin.x + lx;
            double gy = bounds.origin.y + ly;

            VNPointWarp *pt = &mesh[row * 2 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + (gx - cx) * s);
            pt->global.y = (float)(cy + (gy - cy) * s);
        }
    }
}

// Collapse toward the horizontal centre line, like a blind snapping shut.
// Affine in each axis; the middle row exists so the fold is visible rather
// than a plain vertical scale.
static void vn_anim_squish(VNPointWarp *mesh, CGRect bounds, double t) {
    double vs = 1.0 - t;
    if (vs < 0.005) vs = 0.005;
    double hs = 1.0 - t * 0.15;   // a touch of horizontal draw-in
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;

    for (unsigned row = 0; row < 3; row++) {
        for (unsigned col = 0; col < 2; col++) {
            double v  = (double)row * 0.5;
            double lx = bounds.size.width  * (double)col;
            double ly = bounds.size.height * v;
            double gx = bounds.origin.x + lx;
            double gy = bounds.origin.y + ly;

            VNPointWarp *pt = &mesh[row * 2 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + (gx - cx) * hs);
            pt->global.y = (float)(cy + (gy - cy) * vs);
        }
    }
}

// Rotate about the bottom-left corner and drop away. A rotation plus a
// translation is affine, so the four corners carry it exactly.
static void vn_anim_fall(VNPointWarp *mesh, CGRect bounds, double t) {
    double ease  = t * t;                       // accelerate, like gravity
    double ang   = ease * 1.15;                 // radians of tip-over
    double drop  = ease * bounds.size.height * 1.6;
    double shrink = 1.0 - t * 0.25;
    double ca = cos(ang), sa = sin(ang);

    // Pivot at the bottom-left of the window, in screen coordinates.
    double px = bounds.origin.x;
    double py = bounds.origin.y + bounds.size.height;

    for (unsigned row = 0; row < 2; row++) {
        for (unsigned col = 0; col < 2; col++) {
            double lx = bounds.size.width  * (double)col;
            double ly = bounds.size.height * (double)row;
            double dx = (bounds.origin.x + lx - px) * shrink;
            double dy = (bounds.origin.y + ly - py) * shrink;

            VNPointWarp *pt = &mesh[row * 2 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(px + dx * ca - dy * sa);
            pt->global.y = (float)(py + dx * sa + dy * ca + drop);
        }
    }
}

// Rotate about the centre while shrinking, with the twist increasing from
// the centre outward so the corners trail. Horizontal and vertical phases
// bend that twist across the window, and two ripple harmonics (one per axis)
// travel through it mid-flight -- that per-row AND per-column variation is
// the non-affine part that needs the dense kVNMeshSwirlW x kVNMeshSwirlH
// grid. Every extra term scales with t, so t = 0 is still the identity.
static void vn_anim_swirl(VNPointWarp *mesh, CGRect bounds, double t) {
    double s = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;
    double half = hypot(bounds.size.width, bounds.size.height) * 0.5;
    if (half < 1.0) half = 1.0;

    for (unsigned row = 0; row < kVNMeshSwirlH; row++) {
        for (unsigned col = 0; col < kVNMeshSwirlW; col++) {
            double u  = (double)col / (double)(kVNMeshSwirlW - 1);
            double v  = (double)row / (double)(kVNMeshSwirlH - 1);
            double lx = bounds.size.width  * u;
            double ly = bounds.size.height * v;
            double dx = bounds.origin.x + lx - cx;
            double dy = bounds.origin.y + ly - cy;

            // Twist scales with distance from the centre, so the middle
            // barely turns and the corners sweep a full ~185 degrees; the
            // u/v phases fold that sweep across the window.
            double r    = hypot(dx, dy) / half;
            double ang  = t * 3.2 * r
                        + t * 1.1 * sin(v * M_PI)
                        + t * 0.6 * sin(u * M_PI * 2.0);
            double ca   = cos(ang), sa = sin(ang);

            // Ripples travelling along each axis, ramped in by t and scaled
            // by r so the centre stays put. Die with s as the window
            // collapses.
            double ripple_x = (sin(v * M_PI * 2.0 + t * 9.42477796076938)
                             + 0.5 * sin(v * M_PI * 4.0 - t * 6.283185307179586))
                            * t * 0.05 * half * r;
            double ripple_y = cos(u * M_PI * 2.0 + t * 6.283185307179586)
                            * t * 0.04 * half * r;

            VNPointWarp *pt = &mesh[row * kVNMeshSwirlW + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + (dx * ca - dy * sa) * s + ripple_x * s);
            pt->global.y = (float)(cy + (dx * sa + dy * ca) * s + ripple_y * s);
        }
    }
}

// Half-turn about the centre while shrinking. Rotation plus uniform scale
// is affine, so 2x2 is exact.
static void vn_anim_flip(VNPointWarp *mesh, CGRect bounds, double t) {
    double s = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double ang = t * M_PI;
    double ca = cos(ang), sa = sin(ang);
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;

    for (unsigned row = 0; row < 2; row++) {
        for (unsigned col = 0; col < 2; col++) {
            double lx = bounds.size.width  * (double)col;
            double ly = bounds.size.height * (double)row;
            double dx = bounds.origin.x + lx - cx;
            double dy = bounds.origin.y + ly - cy;

            VNPointWarp *pt = &mesh[row * 2 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + (dx * ca - dy * sa) * s);
            pt->global.y = (float)(cy + (dx * sa + dy * ca) * s);
        }
    }
}

// Shear sideways with magnitude growing down the window, plus a shrink.
// Shear plus scale is affine: 2x2 exact.
static void vn_anim_tilt(VNPointWarp *mesh, CGRect bounds, double t) {
    double s = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double sh = t * 0.6;
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;

    for (unsigned row = 0; row < 2; row++) {
        for (unsigned col = 0; col < 2; col++) {
            double lx = bounds.size.width  * (double)col;
            double ly = bounds.size.height * (double)row;
            double dx = bounds.origin.x + lx - cx;
            double dy = bounds.origin.y + ly - cy;

            VNPointWarp *pt = &mesh[row * 2 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + (dx + sh * dy) * s);
            pt->global.y = (float)(cy + dy * s);
        }
    }
}

// Sink straight down while shrinking. Translation plus scale is affine
// (2x2 exact), but the clone leaves its frame -- if the server clips the
// warp to the window's frame the bottom is cut off. Same caveat as fall.
static void vn_anim_slide(VNPointWarp *mesh, CGRect bounds, double t) {
    double s = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double drop = t * t * bounds.size.height * 1.2;
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;

    for (unsigned row = 0; row < 2; row++) {
        for (unsigned col = 0; col < 2; col++) {
            double lx = bounds.size.width  * (double)col;
            double ly = bounds.size.height * (double)row;
            double dx = bounds.origin.x + lx - cx;
            double dy = bounds.origin.y + ly - cy;

            VNPointWarp *pt = &mesh[row * 2 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + dx * s);
            pt->global.y = (float)(cy + dy * s + drop);
        }
    }
}

// Suck into the red-button corner (top-left). The shrink is anchored there
// with a radius-dependent lag, so far points travel most -- that radial
// variation is non-affine and needs the 6x10 grid.
static void vn_anim_genie(VNPointWarp *mesh, CGRect bounds, double t) {
    double s = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double ax = bounds.origin.x;
    double ay = bounds.origin.y;
    double half = hypot(bounds.size.width, bounds.size.height) * 0.5;
    if (half < 1.0) half = 1.0;

    for (unsigned row = 0; row < 10; row++) {
        for (unsigned col = 0; col < 6; col++) {
            double u  = (double)col / 5.0;
            double v  = (double)row / 9.0;
            double lx = bounds.size.width  * u;
            double ly = bounds.size.height * v;
            double gx = bounds.origin.x + lx;
            double gy = bounds.origin.y + ly;

            double r = hypot(gx - ax, gy - ay) / half;
            double k = s * (1.0 - t * 0.5 * r);

            VNPointWarp *pt = &mesh[row * 6 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(ax + (gx - ax) * k);
            pt->global.y = (float)(ay + (gy - ay) * k);
        }
    }
}

// Travelling wave across the width while shrinking. The phase varies column
// to column, which is what needs the 8-wide grid; amplitude ramps with t so
// t = 0 is identity.
static void vn_anim_flag(VNPointWarp *mesh, CGRect bounds, double t) {
    double s = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;

    for (unsigned row = 0; row < 6; row++) {
        for (unsigned col = 0; col < 8; col++) {
            double u  = (double)col / 7.0;
            double v  = (double)row / 5.0;
            double lx = bounds.size.width  * u;
            double ly = bounds.size.height * v;
            double dx = bounds.origin.x + lx - cx;
            double dy = bounds.origin.y + ly - cy;

            double wob = sin(u * M_PI * 4.0 + t * M_PI * 4.0)
                       * t * 0.08 * bounds.size.height;

            VNPointWarp *pt = &mesh[row * 8 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + dx * s);
            pt->global.y = (float)(cy + dy * s + wob * s);
        }
    }
}

// A turn and a half about the centre while shrinking. Same family as flip,
// but the multi-turn version reads completely differently. Uniform rotation
// plus scale is affine: 2x2 exact.
static void vn_anim_spin(VNPointWarp *mesh, CGRect bounds, double t) {
    double s = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double ang = t * M_PI * 3.0;
    double ca = cos(ang), sa = sin(ang);
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;

    for (unsigned row = 0; row < 2; row++) {
        for (unsigned col = 0; col < 2; col++) {
            double lx = bounds.size.width  * (double)col;
            double ly = bounds.size.height * (double)row;
            double dx = bounds.origin.x + lx - cx;
            double dy = bounds.origin.y + ly - cy;

            VNPointWarp *pt = &mesh[row * 2 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + (dx * ca - dy * sa) * s);
            pt->global.y = (float)(cy + (dx * sa + dy * ca) * s);
        }
    }
}

// Roll up like a scroll: each row's collapse runs on a delayed clock so the
// fold travels from top to bottom. That per-row timing is the non-affine
// part and wants the tall 6x12 grid.
static void vn_anim_roll(VNPointWarp *mesh, CGRect bounds, double t) {
    double hs = 1.0 - t * 0.1;
    double cx = bounds.origin.x + bounds.size.width * 0.5;

    for (unsigned row = 0; row < 12; row++) {
        for (unsigned col = 0; col < 6; col++) {
            double u  = (double)col / 5.0;
            double v  = (double)row / 11.0;
            double lx = bounds.size.width  * u;
            double ly = bounds.size.height * v;
            double gx = bounds.origin.x + lx;

            double m = (t * 1.4 - v * 0.4);
            if (m < 0.0) m = 0.0; if (m > 1.0) m = 1.0;
            double fv = v * (1.0 - 0.995 * m);

            VNPointWarp *pt = &mesh[row * 6 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + (gx - cx) * hs);
            pt->global.y = (float)(bounds.origin.y + fv * bounds.size.height);
        }
    }
}

// Lens-style pincushion that swells mid-flight, then collapses with the
// window. The r-squared radial factor is non-affine: 8x8.
static void vn_anim_barrel(VNPointWarp *mesh, CGRect bounds, double t) {
    double s = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;
    double half = hypot(bounds.size.width, bounds.size.height) * 0.5;
    if (half < 1.0) half = 1.0;

    for (unsigned row = 0; row < 8; row++) {
        for (unsigned col = 0; col < 8; col++) {
            double u  = (double)col / 7.0;
            double v  = (double)row / 7.0;
            double lx = bounds.size.width  * u;
            double ly = bounds.size.height * v;
            double dx = bounds.origin.x + lx - cx;
            double dy = bounds.origin.y + ly - cy;

            double r = hypot(dx, dy) / half;
            double k = 1.0 + t * 0.8 * r * r;

            VNPointWarp *pt = &mesh[row * 8 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + dx * k * s);
            pt->global.y = (float)(cy + dy * k * s);
        }
    }
}

// An asymmetric angular sweep: each point turns by an amount set by its
// polar angle, so the window wrings itself out. The offset uses 1 - cos so
// it is periodic -- no seam at the branch cut -- and zero at t = 0.
// Non-affine: 8x8.
static void vn_anim_clock(VNPointWarp *mesh, CGRect bounds, double t) {
    double s = 1.0 - t;
    if (s < 0.005) s = 0.005;
    double cx = bounds.origin.x + bounds.size.width  * 0.5;
    double cy = bounds.origin.y + bounds.size.height * 0.5;

    for (unsigned row = 0; row < 8; row++) {
        for (unsigned col = 0; col < 8; col++) {
            double u  = (double)col / 7.0;
            double v  = (double)row / 7.0;
            double lx = bounds.size.width  * u;
            double ly = bounds.size.height * v;
            double dx = bounds.origin.x + lx - cx;
            double dy = bounds.origin.y + ly - cy;

            double theta = atan2(dy, dx);
            double extra = t * (1.0 - cos(theta)) * 1.2;
            double ca = cos(extra), sa = sin(extra);

            VNPointWarp *pt = &mesh[row * 8 + col];
            pt->local.x  = (float)lx;
            pt->local.y  = (float)ly;
            pt->global.x = (float)(cx + (dx * ca - dy * sa) * s);
            pt->global.y = (float)(cy + (dx * sa + dy * ca) * s);
        }
    }
}

/// Frame interval for the running animation, sampled once at start. See the
/// note in vn_anim_tick for why this is not recomputed per frame.
static double gAnimFrameInterval = 1.0 / 120.0;

/// When the next frame is due, in absolute time. The timer fires roughly
/// 0.8ms late every frame; rescheduling from "now" folded that lateness into
/// the period (measured: 9.17ms per frame against an 8.33ms target, ~109Hz
/// on a 120Hz display, 7.6% of frames landing a whole vsync late). Advancing
/// a fixed deadline instead keeps the cadence locked to the display.
///
/// Measured before/after: gap p50 9.17ms -> 8.30ms against an 8.33ms target,
/// and frames landing a whole vsync late 7.6% -> 1.3%.
///
/// Kept, with a caveat: the user-visible "hang" turned out to be a collision
/// with the system's transpose-into-icon animation, not a frame-pacing
/// problem, so this is not what fixed that. It stands on its own measurement
/// rather than on that symptom, and it may or may not be doing perceptible
/// work now. Removing it would need a fresh measurement, not an assumption.
static double gAnimNextDeadline = 0.0;

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
                    .anim = gActiveAnims[i].anim,
                };
            }
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    for (int i = 0; i < finished_count; i++) {
        vn_finish_animation_for_id(finished[i]);
    }

    for (int i = 0; i < snapshot_count; i++) {
        const VNAnimation *anim = snapshots[i].anim ? snapshots[i].anim : &gAnimations[0];
        if (!snapshots[i].clone_win) continue;

        // Dispatch on kind rather than assuming a mesh, so a shader animation
        // can be added here without disturbing this path.
        switch (anim->kind) {
        case VN_ANIM_MESH:
            if (vn_resolved_set_mesh_warp && anim->mesh.fill) {
                VNPointWarp mesh[kVNMeshMaxCount];
                anim->mesh.fill(mesh, snapshots[i].bounds, snapshots[i].p);
                vn_resolved_set_mesh_warp(snapshots[i].clone_win, NULL,
                                          anim->mesh.w, anim->mesh.h, (const float *)mesh);
            }
            break;
        case VN_ANIM_FILTER:
            if (anim->filter.fill_params) {
                float params[5] = {0};
                anim->filter.fill_params(params, snapshots[i].bounds, snapshots[i].p);
                vn_filter_update(snapshots[i].clone_win, params);
            }
            break;
        case VN_ANIM_SHADER:
            break;   // nothing implements this yet
        }
    }

#if ENABLE_LOGS && VN_LOG_LEVEL >= VN_LOG_LEVEL_TRACE
    // Late frames are what a dropped frame looks like from in here: the gap
    // since the previous tick, and how long this tick's own work took.
    {
        static double s_prev_tick = 0.0;
        double spent = (SLSCurrentRealTime() - now) * 1000.0;
        double gap   = s_prev_tick > 0.0 ? (now - s_prev_tick) * 1000.0 : 0.0;
        s_prev_tick = now;
        VN_TRACE("anim_tick: %d clone(s) gap=%.2fms work=%.2fms", snapshot_count, gap, spent);
    }
#endif

    if (more && vn_resolved_schedule_callback) {
        // Deliberately not vn_get_refresh_interval() here. That reads the
        // prefs, which stat()s the plist and re-parses it when the mtime
        // moves -- a syscall, and potentially a file read and CFPropertyList
        // parse, on the compositor's timer thread once per frame. The refresh
        // rate cannot meaningfully change inside a 240ms animation, so it is
        // sampled once at the start and reused.
        double interval = gAnimFrameInterval;
        if (interval <= 0.0) interval = 1.0 / 120.0;

        // Advance the deadline rather than measuring from now, so the timer's
        // own lateness does not compound frame over frame. If we have fallen
        // more than a whole frame behind -- a real stall, not ordinary
        // jitter -- resync instead of firing a burst of catch-up frames.
        double after = SLSCurrentRealTime();
        gAnimNextDeadline += interval;
        if (gAnimNextDeadline < after) {
            gAnimNextDeadline = after + interval;
        }
        vn_resolved_schedule_callback(vn_anim_tick, NULL, gAnimNextDeadline);
    } else {
        atomic_store_explicit(&gAnimTimerRunning, false, memory_order_release);
    }
}

static void vn_start_clone_animation(CGXWindow *clone_win, uint32_t orig_wid, CGRect frame,
                                     const VNAnimation *anim,
                                     pid_t pid, uint64_t psn, bool is_wa, const char *app_name) {
    if (!clone_win) return;
    uint32_t clone_wid = vn_resolved_window_get_id ? vn_resolved_window_get_id(clone_win) : 0;
    float dur = vn_duration();

    if ((pid == 0 || psn == 0) && orig_wid != 0 && vn_resolved_window_by_id) {
        CGXWindow *orig_win = vn_resolved_window_by_id(orig_wid);
        if (orig_win) {
            if (pid == 0 && vn_resolved_window_get_owning_pid) pid = vn_resolved_window_get_owning_pid(orig_win);
            CGXConnection *c = vn_window_connection(orig_win);
            if (c) {
                if (pid == 0) pid = vn_conn_get_pid(c);
                if (psn == 0) psn = vn_conn_get_psn(c);
            }
        }
    }

    if (!is_wa && app_name && (strcasecmp(app_name, "WhatsApp") == 0)) {
        is_wa = true;
    }

    if (frame.size.width < 1.0 || frame.size.height < 1.0) {
        VNPreferences prefs = vn_get_prefs();
        CGRect b = vn_resolved_clipped_frame_bounds ? vn_resolved_clipped_frame_bounds(clone_win) : CGRectZero;
        CGRect p = vn_resolved_screen_rect ? vn_resolved_screen_rect(clone_win) : CGRectZero;
        if (!prefs.shadows && p.size.width >= 1.0 && p.size.height >= 1.0) {
            frame = p;
        } else if (b.size.width >= 1.0 && b.size.height >= 1.0) {
            frame = b;
        } else if (p.size.width >= 1.0 && p.size.height >= 1.0) {
            frame = p;
        } else if (vn_resolved_screen_rect_from_rect) {
            frame = vn_resolved_screen_rect_from_rect(clone_win, CGRectMake(0.0, 0.0, 1.0, 1.0));
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
    if (!anim) anim = &gAnimations[0];
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
        .anim = anim,
    };
    if (app_name && app_name[0] != '\0') {
        strlcpy(gActiveAnims[slot].app, app_name, sizeof(gActiveAnims[slot].app));
    }
    os_unfair_lock_unlock(&gAnimsLock);

    if (clone_wid != 0) {
        CGSOrderOp op = lowest_clone_wid != 0 ? kVNOrderBelow : kVNOrderAbove;
        uint32_t rel = lowest_clone_wid;
        vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false);
        VN_DEBUG("anim: clone wid=%u ordered %s rel=%u (anim_id=%llu)",
               clone_wid, op == kVNOrderBelow ? "below" : "above", rel, anim_id);
    }

    double interval = vn_get_refresh_interval(clone_win);
    double hz = interval > 0.0 ? (1.0 / interval) : 120.0;
    gAnimFrameInterval = interval;

    VN_INFO("starting fade animation for clone wid=%u (orig=%u, app='%s' pid=%d psn=0x%llx is_wa=%d) win=%p anim='%s' duration=%.2fs interval=%.2fms (%.0fHz) (anim_id=%llu)",
            clone_wid, orig_wid, app_name ? app_name : "", pid, psn, is_wa, clone_win, anim ? anim->key : "?", dur, interval * 1000.0, hz, anim_id);

    if (vn_resolved_schedule_callback) {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(&gAnimTimerRunning, &expected, true,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            gAnimNextDeadline = SLSCurrentRealTime() + interval;
            vn_resolved_schedule_callback(vn_anim_tick, NULL, gAnimNextDeadline);
        } else {
            VN_DEBUG("anim_tick timer loop already active -- clone wid=%u animating concurrently", clone_wid);
        }
    } else {
        vn_finish_animation_for_id(anim_id);
    }
}


#pragma mark - System close-animation detection

// Some windows come with the system's own close animation, and running ours
// on top of it looks broken. The clearest case: double-click a file in
// Finder, and the app transposes the window back into the file's icon when
// it closes.
//
// Measured, from an unfiltered order-op trace of the same file opened both
// ways (log timestamps, one close each):
//
//   Finder double-click              open -a Preview <file>
//   13.034 wid=77 in  16x11   <-icon 21.176 wid=93 in 895x358  <- full size
//   15.368 mouse up on red           22.710 mouse up on red
//   15.428 wid=82 in  458x458 <-proxy   (nothing)
//   15.429 wid=81 in 1025x476 <-proxy
//   15.433 our animation starts      22.770 our animation starts
//   15.626 wid=81 out  14x9   <-icon    (nothing)
//
// So the app spawns brand-new proxy windows that fly into the icon while our
// clone shrinks in place. Nothing about the closing window differs between
// the two cases -- not its state, not its geometry, not our frame delivery
// (both closes measured 32 frames, zero late) -- so the proxies are the only
// thing that separates them.
//
// They appear ~60ms after the release and ~4ms before we animate, which is
// why this cannot be decided at pre-clone time and has to be checked at the
// moment we would start.
#define kVNProxyWindowWindowMs 250

static _Atomic uint64_t gLastNewWindowMs  = 0;
static _Atomic int      gLastNewWindowPid = 0;
static _Atomic uint32_t gLastNewWindowWid = 0;

/// Records a window that genuinely appeared (not a restack of one already on
/// screen). Called from the ordering hook, which the proxies do pass through.
static void vn_note_new_window(uint32_t wid, pid_t pid) {
    if (wid == 0 || pid == 0) return;
    atomic_store_explicit(&gLastNewWindowWid, wid, memory_order_relaxed);
    atomic_store_explicit(&gLastNewWindowPid, (int)pid, memory_order_relaxed);
    atomic_store_explicit(&gLastNewWindowMs, vn_now_ms(), memory_order_relaxed);
}

/// True when the closing window's app has just put a new window on screen --
/// i.e. it is mid-transpose and our animation would collide with it.
static bool vn_system_close_animation_in_flight(uint32_t closing_wid, pid_t pid) {
    if (pid == 0) return false;
    if (atomic_load_explicit(&gLastNewWindowPid, memory_order_relaxed) != (int)pid) return false;

    uint32_t nwid = atomic_load_explicit(&gLastNewWindowWid, memory_order_relaxed);
    if (nwid == 0 || nwid == closing_wid) return false;

    uint64_t at = atomic_load_explicit(&gLastNewWindowMs, memory_order_relaxed);
    uint64_t now = vn_now_ms();
    return (now >= at) && ((now - at) < kVNProxyWindowWindowMs);
}

// The proxy-window signal above misses the other half of the same behaviour.
// Double-click a *folder* on the desktop and Finder spawns nothing: it zooms
// the real window out of the folder icon, and on close it flies that same
// window back into it. From an unfiltered order-op trace of one such open
// (log timestamps, ms):
//
//   37.801  wid=81 out  296,127 920x492   <- created at its final geometry
//   37.855  wid=81 in   933,376 193x130   <- moved onto the icon, zoom starts
//   38.815  wid=81 in   296,127 920x492   <- arrived
//
// so the whole transpose happens on one window id. vn_system_close_animation_
// in_flight() cannot see it: there is no second window, and its own guard
// (nwid == closing_wid) rejects the only one there is. The 193x130 order-in
// is also invisible to everything downstream, because vn_is_target_window()
// drops Finder windows under 450pt wide.
//
// The fingerprint is the pair of wildly different frames within a few tens of
// ms of the window first being seen. Nothing a settled window does looks like
// that: a user resize is gradual and arrives long after birth, and the Dock's
// genie (which also flashes a 52x52 frame near the Dock) only ever happens to
// a window that has been on screen for a while. Hence the birth window --
// it is what separates "opened by zooming" from "minimised at some point".
//
// A window flagged here never gets a pre-clone, so the close runs stock and
// the system's own transpose plays alone. Suppressing *their* animation and
// keeping ours is the better end state, but it needs a way to cancel a
// transpose already committed to; this is the half that stops the collision.
#define kVNZoomBirthWindowMs 750
#define kVNZoomAreaRatio     4.0
#define MAX_ZOOM_TRACKED     128

typedef struct {
    uint32_t wid;
    uint64_t birth_ms;
    double   ref_area;   // largest frame seen so far, the one to compare against
    bool     zoom_opened;
} VNZoomRecord;

static VNZoomRecord   gZoomWindows[MAX_ZOOM_TRACKED] = {0};
static os_unfair_lock gZoomLock = OS_UNFAIR_LOCK_INIT;

/// Feeds one observed frame for a window. Called for every window the ordering
/// hook sees -- including the ones vn_is_target_window() rejects, which is
/// where the small transpose frames live.
static void vn_note_window_frame(uint32_t wid, CGRect frame) {
    if (wid == 0) return;
    double area = frame.size.width * frame.size.height;
    if (area <= 0.0) return;

    uint64_t now = vn_now_ms();

    os_unfair_lock_lock(&gZoomLock);
    int slot = -1, empty = -1, oldest = 0;
    for (int i = 0; i < MAX_ZOOM_TRACKED; i++) {
        if (gZoomWindows[i].wid == wid) { slot = i; break; }
        if (empty < 0 && gZoomWindows[i].wid == 0) empty = i;
        if (gZoomWindows[i].birth_ms < gZoomWindows[oldest].birth_ms) oldest = i;
    }
    if (slot < 0) {
        slot = (empty >= 0) ? empty : oldest;
        gZoomWindows[slot] = (VNZoomRecord){ .wid = wid, .birth_ms = now, .ref_area = area };
        os_unfair_lock_unlock(&gZoomLock);
        return;
    }

    VNZoomRecord *rec = &gZoomWindows[slot];
    if (rec->zoom_opened || (now - rec->birth_ms) > kVNZoomBirthWindowMs) {
        os_unfair_lock_unlock(&gZoomLock);
        return;
    }

    bool zoomed = (area * kVNZoomAreaRatio <= rec->ref_area) ||
                  (rec->ref_area * kVNZoomAreaRatio <= area);
    if (zoomed) {
        rec->zoom_opened = true;
    } else if (area > rec->ref_area) {
        rec->ref_area = area;
    }
    double   ref = rec->ref_area;
    uint64_t age = now - rec->birth_ms;
    os_unfair_lock_unlock(&gZoomLock);

    if (zoomed) {
        VN_INFO(">>> wid=%u opened by zooming out of an icon (%.0fx%.0f vs ref area %.0f, %llums after first seen) -- its close is the system's to animate",
                wid, frame.size.width, frame.size.height, ref, age);
    }
}

static bool vn_window_was_zoom_opened(uint32_t wid) {
    if (wid == 0) return false;
    bool flagged = false;
    os_unfair_lock_lock(&gZoomLock);
    for (int i = 0; i < MAX_ZOOM_TRACKED; i++) {
        if (gZoomWindows[i].wid == wid) { flagged = gZoomWindows[i].zoom_opened; break; }
    }
    os_unfair_lock_unlock(&gZoomLock);
    return flagged;
}

/// Window ids are recycled, so a record must not outlive its window.
static void vn_zoom_forget(uint32_t wid) {
    if (wid == 0) return;
    os_unfair_lock_lock(&gZoomLock);
    for (int i = 0; i < MAX_ZOOM_TRACKED; i++) {
        if (gZoomWindows[i].wid == wid) {
            memset(&gZoomWindows[i], 0, sizeof(VNZoomRecord));
            break;
        }
    }
    os_unfair_lock_unlock(&gZoomLock);
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
    if (vn_resolved_window_is_ordered_in && win) {
        return vn_resolved_window_is_ordered_in(win);
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
    vn_zoom_forget(wid);
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
    if (pid == 0 && win && vn_resolved_window_get_owning_pid) {
        pid = vn_resolved_window_get_owning_pid(win);
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
                VN_DEBUG("Window wid=%u was already ordered in (AppKit sibling restack/focus) -> NOT canceling clone wid=%u",
                       wid, gActiveAnims[i].clone_wid);
            } else {
                if (other_visible == 0) {
                    VN_DEBUG("Process pid=%d had 0 other visible windows and wid=%u is newly ordering in -> Dock/launch reopen, canceling clone wid=%u",
                           pid, wid, gActiveAnims[i].clone_wid);
                    match = true;
                } else {
                    VN_DEBUG("Process pid=%d still has %d other visible window(s); wid=%u is a sibling -> NOT canceling clone wid=%u",
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
            gActiveAnims[i].anim = NULL;
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    for (int j = 0; j < release_count; j++) {
        CGXWindow *clone_to_release = clones_to_release[j];
        uint32_t clone_wid = clone_wids[j];

        VN_DEBUG("Target window wid=%u (pid=%d) ordered back in while clone wid=%u was animating; ordering out and destroying clone",
               wid, pid, clone_wid);
        if (vn_resolved_update_ca_visibility && clone_to_release) {
            vn_resolved_update_ca_visibility(clone_to_release, false);
        }
        if (clone_wid != 0) {
            CGSOrderOp op = kVNOrderOut;
            uint32_t rel = 0;
            vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false);
        }
        if (clone_to_release) {
            if (vn_resolved_schedule_callback) {
                vn_resolved_schedule_callback(vn_delayed_clone_release, clone_to_release, SLSCurrentRealTime() + 0.1);
            } else {
                vn_release_clone(clone_to_release);
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

/// Defined further down with the early-hide state it clears.
static void vn_early_hidden_forget(uint32_t wid);

static void vn_hook_release_window(CGXConnection *conn, CGXWindow *win) {
    uint32_t rel_wid = (win && vn_resolved_window_get_id) ? vn_resolved_window_get_id(win) : 0;

    if (rel_wid != 0) {
        vn_track_window_release(rel_wid);
        vn_early_hidden_forget(rel_wid);
    }

    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (!gActiveAnims[i].is_animating) continue;

        bool match_self = (gActiveAnims[i].clone_win == win) || (rel_wid != 0 && gActiveAnims[i].clone_wid == rel_wid);

        if (match_self) {
            VN_DEBUG("release_window arrived for animating clone wid=%u win=%p; removing from animation table",
                   gActiveAnims[i].clone_wid, win);
            gActiveAnims[i].is_animating = false;
            gActiveAnims[i].clone_wid = 0;
            gActiveAnims[i].orig_wid = 0;
            gActiveAnims[i].clone_win = NULL;
            gActiveAnims[i].anim = NULL;
            break;
        } else if (rel_wid != 0 && gActiveAnims[i].orig_wid == rel_wid) {
            VN_DEBUG("release_window arrived for orig_wid=%u while clone wid=%u is animating (normal AppKit teardown); clone continues",
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
    const VNAnimation *anim_desc = NULL;

    os_unfair_lock_lock(&gPreCloneLock);
    for (int i = 0; i < MAX_PRECLONES; i++) {
        if (gPreClones[i].clone == win) {
            memset(&gPreClones[i], 0, sizeof(VNPreClone));
            break;
        }
        if (gPreClones[i].orig_wid != 0 &&
            ((rel_wid != 0 && gPreClones[i].orig_wid == rel_wid) ||
             (vn_resolved_window_by_id && vn_resolved_window_by_id(gPreClones[i].orig_wid) == win))) {
            orig_wid = gPreClones[i].orig_wid;
            clone_to_animate = vn_preclone_take_locked(orig_wid, &clone_wid, &clone_frame, &anim_desc, &anim_pid, &anim_psn, &anim_is_wa, anim_app, sizeof(anim_app));
            break;
        }
    }
    os_unfair_lock_unlock(&gPreCloneLock);

    vn_orig_release_window(conn, win);

    if (clone_to_animate) {
        VN_INFO(">>> Close seen at release_window for wid=%u (app='%s' pid=%d psn=0x%llx is_wa=%d) -- animating pre-clone wid=%u",
               orig_wid, anim_app, anim_pid, anim_psn, anim_is_wa, clone_wid);
        vn_start_clone_animation(clone_to_animate, orig_wid, clone_frame, anim_desc, anim_pid, anim_psn, anim_is_wa, anim_app);
    }
}

#pragma mark - Deferred (mouse-up) pre-cloning

// A red-button press that has not been released yet.
//
// The clone is deliberately NOT made at mouse-down. Vanish sees the press
// before AppKit has decided whether the gesture is a click or the start of a
// window drag, so a mouse-down clone can be left sitting at the original's
// old position while the user drags the real window out from over it --
// exposing both at once. No amount of hitbox precision fixes that, because
// the ambiguity is in the timing, not the geometry. Waiting for mouse-up
// means no clone exists during a drag at all. There is plenty of lead time:
// the app's real order-out lands 40-300ms after mouse-up, far longer than a
// clone takes to build.
//
// Only one mouse gesture can be in flight at a time, so one record is enough.
typedef struct {
    uint32_t orig_wid;
    CGPoint  down_screen_pt;
    CGPoint  down_local_pt;
    bool     active;
    bool     invalidated;   // a drag, or a move off the button, disqualified it
} VNPendingRedClick;

static VNPendingRedClick gPendingRed = {0};
static os_unfair_lock    gPendingRedLock = OS_UNFAIR_LOCK_INIT;

static void vn_pending_red_clear(void) {
    os_unfair_lock_lock(&gPendingRedLock);
    gPendingRed = (VNPendingRedClick){0};
    os_unfair_lock_unlock(&gPendingRedLock);
}

static void vn_pending_red_invalidate(uint32_t wid, const char *why) {
    os_unfair_lock_lock(&gPendingRedLock);
    bool hit = (gPendingRed.active && gPendingRed.orig_wid == wid && !gPendingRed.invalidated);
    if (hit) gPendingRed.invalidated = true;
    os_unfair_lock_unlock(&gPendingRedLock);
    if (hit) {
        VN_DEBUG("pre-clone: pending red click on wid=%u invalidated (%s) -- not cloning on release", wid, why);
    }
}

// Starting the animation on the app's fade, not on its order-out.
//
// Measured: an AppKit/Chromium window does not vanish when closed -- the app
// fades its alpha 1.0 -> 0.0 over ~250ms and only calls order-out once that
// fade has finished. So the order-out Vanish used to trigger on marks the
// END of the close, not the start of it, and waiting for it meant sitting
// through the whole fade first (~290ms for Chrome/Mail/Claude/Antigravity,
// vs ~60ms for apps that destroy the window outright and never fade).
//
// The fade itself is the real signal, and it is visible server-side as the
// window's alpha dropping below where it sat at mouse-up. Note this is not
// the server's own timed fade: CGXWindow::fade_begin is never involved (the
// fade-state pointer stays NULL throughout), the client pushes one alpha per
// frame. So there is nothing to hook -- only something to watch.
static uint32_t gAlphaProbeWid      = 0;
static double   gAlphaProbeStart    = 0.0;
static float    gAlphaProbeBaseline = 1.0f;

// The original is hidden the moment we take over, so it cannot show through
// behind the shrinking clone while it finishes its own fade. If the close
// somehow never lands, this is what puts it back.
//
// The window pointer is kept alongside the id because ids are recycled: by
// the time the check runs that id may belong to an entirely different
// window, and forcing a CA visibility update on a live window that was never
// ours is simply wrong.
//
// This was found while chasing a one-frame flash during window resizes. That
// flash turned out NOT to be ours -- it reproduces with Vanish fully
// unloaded -- so this is not a fix for it. It is kept because touching a
// window we no longer own is a real bug on its own, whatever it does or does
// not render.
//
// Matching on both is not airtight (an allocation could reuse the address
// too) but combined with the short window it is far tighter than the id
// alone, and the normal path clears the record long before this runs.
static uint32_t   gEarlyHiddenWid = 0;
static CGXWindow *gEarlyHiddenWin = NULL;

/// Called once the real close lands, so the restore below never fires for a
/// window that closed normally.
static void vn_early_hidden_forget(uint32_t wid) {
    if (wid != 0 && gEarlyHiddenWid == wid) {
        gEarlyHiddenWid = 0;
        gEarlyHiddenWin = NULL;
    }
}

static void vn_early_restore_check(void *ctx, double when) {
    (void)ctx; (void)when;
    uint32_t   wid = gEarlyHiddenWid;
    CGXWindow *was = gEarlyHiddenWin;
    if (wid == 0) return;
    gEarlyHiddenWid = 0;
    gEarlyHiddenWin = NULL;

    CGXWindow *win = vn_resolved_window_by_id ? vn_resolved_window_by_id(wid) : NULL;
    if (win && win == was && vn_window_is_ordered_in(win) && vn_resolved_update_ca_visibility) {
        VN_ERROR("early-close: wid=%u never ordered out -- restoring visibility", wid);
        vn_resolved_update_ca_visibility(win, true);
    }
}

static void vn_close_watch_tick(void *ctx, double when) {
    (void)ctx; (void)when;
    uint32_t wid = gAlphaProbeWid;
    if (wid == 0) return;

    double elapsed = SLSCurrentRealTime() - gAlphaProbeStart;
    CGXWindow *win = vn_resolved_window_by_id ? vn_resolved_window_by_id(wid) : NULL;
    if (!win) { gAlphaProbeWid = 0; return; }

    // Only while this window's pre-clone is still waiting to be used. If the
    // order-out beat us to it, the normal path already consumed it.
    os_unfair_lock_lock(&gPreCloneLock);
    bool pending = (vn_preclone_find_slot_locked(wid) >= 0);
    os_unfair_lock_unlock(&gPreCloneLock);
    if (!pending) { gAlphaProbeWid = 0; return; }

    // Relative to the alpha at mouse-up, not to 1.0: plenty of windows are
    // legitimately translucent, and only a *drop* means a close.
    float alpha = vn_window_alpha(win);
    if (alpha < gAlphaProbeBaseline - 0.01f) {
        gAlphaProbeWid = 0;

        uint32_t clone_wid = 0;
        CGRect   clone_frame = CGRectZero;
        pid_t    anim_pid = 0;
        uint64_t anim_psn = 0;
        bool     anim_is_wa = false;
        char     anim_app[64] = {0};

        // Checked before taking the pre-clone, so discarding it goes through
        // the normal path and nothing has to be released by hand.
        const VNAnimation *anim_desc = NULL;
        pid_t owner = vn_resolved_window_get_owning_pid ? vn_resolved_window_get_owning_pid(win) : 0;
        if (vn_system_close_animation_in_flight(wid, owner)) {
            VN_INFO(">>> Deferring to the system's close animation for wid=%u (pid=%d) -- app just spawned a new window, ours would collide",
                    wid, owner);
            vn_preclone_discard_wid(wid);
            return;
        }

        os_unfair_lock_lock(&gPreCloneLock);
        CGXWindow *clone = vn_preclone_take_locked(wid, &clone_wid, &clone_frame, &anim_desc,
                                                   &anim_pid, &anim_psn, &anim_is_wa,
                                                   anim_app, sizeof(anim_app));
        os_unfair_lock_unlock(&gPreCloneLock);

        if (!clone || clone_wid == 0) return;

        VN_INFO(">>> Close detected via fade for wid=%u (app='%s' alpha=%.3f from %.3f) at +%.0fms -- animating now, %.0fms before order-out would have arrived",
               wid, anim_app, (double)alpha, (double)gAlphaProbeBaseline,
               elapsed * 1000.0, 250.0);

        if (vn_resolved_update_ca_visibility) {
            vn_resolved_update_ca_visibility(win, false);
            gEarlyHiddenWid = wid;
            gEarlyHiddenWin = win;
            if (vn_resolved_schedule_callback) {
                vn_resolved_schedule_callback(vn_early_restore_check, NULL, SLSCurrentRealTime() + 2.0);
            }
        }

        vn_start_clone_animation(clone, wid, clone_frame, anim_desc, anim_pid, anim_psn, anim_is_wa, anim_app);
        return;
    }

    // Give up after a second: apps that destroy the window outright never
    // fade, and their order-out arrives on its own well inside that.
    if (elapsed < 1.0 && vn_resolved_schedule_callback) {
        vn_resolved_schedule_callback(vn_close_watch_tick, NULL, SLSCurrentRealTime() + 0.008);
    } else {
        gAlphaProbeWid = 0;
    }
}

// Builds the pre-clone for a red-button press that has now been confirmed by
// a release still inside the button.
static bool vn_create_preclone(CGXWindow *win, CGXConnection *conn,
                               CGPoint down_screen_pt, CGPoint down_local_pt) {
    const uint32_t wid = win->window_id;
    char app[256] = {0};
    pid_t pid = 0;
    vn_get_window_app_name(win, app, sizeof(app), &pid);

    if (vn_window_was_zoom_opened(wid)) {
        VN_INFO(">>> Skipping pre-clone for wid=%u (app='%s') -- it zoomed out of an icon and will transpose back into one; ours would collide",
                wid, app);
        return false;
    }

    CGXConnection *c = conn ? conn : vn_window_connection(win);
    uint64_t psn = vn_conn_get_psn(c);

    vn_preclone_discard_wid(wid);
    atomic_store_explicit(&gNonCloseWid, 0, memory_order_relaxed);

    uint32_t clone_wid = 0;
    CGRect clone_frame = CGRectZero;
    const VNAnimation *anim = NULL;
    CGXWindow *clone = vn_make_snapshot(win, conn, &clone_wid, &clone_frame, &anim, kVNOrderBelow);
    if (!clone || clone_wid == 0) {
        VN_ERROR("pre-clone: failed to create clone for wid=%u", wid);
        return false;
    }

    os_unfair_lock_lock(&gPreCloneLock);
    int slot = vn_preclone_find_slot_locked(wid);
    if (slot < 0) slot = vn_preclone_find_empty_slot_locked();
    gPreClones[slot] = (VNPreClone){
        .orig_wid = wid,
        .clone_wid = clone_wid,
        .clone = clone,
        .frame = clone_frame,
        .created_at = SLSCurrentRealTime(),
        .mouseDownScreenPt = down_screen_pt,
        .mouseDownLocalPt  = down_local_pt,
        // Set now, not on a later mouse-up: the clone is born at release, so
        // the cleanup timer's "released but no close ever came" 1s window
        // starts here.
        .mouseUpTime = SLSCurrentRealTime(),
        .pid = pid,
        .psn = psn,
        .is_whatsapp = (strcasecmp(app, "WhatsApp") == 0),
        .anim = anim,
    };
    strlcpy(gPreClones[slot].app, app, sizeof(gPreClones[slot].app));
    os_unfair_lock_unlock(&gPreCloneLock);

    VN_INFO(">>> Mouse up on RED CLOSE box of '%s' (pid=%d, psn=0x%llx, wid=%u) -- pre-clone ready! clone_wid=%u slot=%d",
           app, pid, psn, wid, clone_wid, slot);

    if (vn_resolved_schedule_callback) {
        vn_resolved_schedule_callback(vn_preclone_cleanup_timer, NULL, SLSCurrentRealTime() + 1.0);
    }

    gAlphaProbeWid      = wid;
    gAlphaProbeStart    = SLSCurrentRealTime();
    gAlphaProbeBaseline = vn_window_alpha(win);
    if (vn_resolved_schedule_callback) {
        vn_resolved_schedule_callback(vn_close_watch_tick, NULL, SLSCurrentRealTime() + 0.008);
    }
    return true;
}

static inline bool vn_is_in_red_hitbox(double lx, double ly) {
    bool in_c1 = (lx >= 9.0 && lx <= 26.0 && ly >= 4.5 && ly <= 25.0);
    bool in_c2 = (lx >= 18.0 && lx <= 33.5 && ly >= 18.0 && ly <= 33.5);
    return in_c1 || in_c2;
}

static inline bool vn_is_in_yellow_or_green_hitbox(double lx, double ly) {
    if (lx >= 26.5 && lx <= 80.0 && ly >= 4.5 && ly <= 25.0) {
        return true;
    }
    if (lx >= 34.0 && lx <= 88.0 && ly >= 18.0 && ly <= 33.5) {
        return true;
    }
    return false;
}

static uint64_t gLastMouseDownTimeMs = 0;
static CGPoint  gLastMouseDownPt     = {0};
static uint32_t gLastMouseDownWid    = 0;
static uint64_t gDoubleClickSuppressUntilMs = 0;

static void vn_hook_post_event(CGXConnection *conn, VNEvent *event) {
    if (!event) {
        if (vn_orig_post_event) vn_orig_post_event(conn, event);
        return;
    }

    uint32_t type = event->type;

    if (type == 1) {
        uint32_t wid = event->window_id;
        if (wid != 0 && vn_resolved_window_by_id) {
            CGXWindow *win = vn_resolved_window_by_id(wid);
            if (win && vn_is_target_window(win) && !vn_is_window_animating(wid, win)) {
                const CGPoint *screen_pt = &event->screen_pt;
                const CGPoint *local_pt  = &event->local_pt;
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
                    VN_DEBUG("hit-test: double-click detected on wid=%u (pt=%.1f,%.1f) -- discarding pre-clone and suppressing for 600ms",
                           wid, lx, ly);
                    vn_pending_red_clear();
                    vn_preclone_discard_wid(wid);
                    gDoubleClickSuppressUntilMs = now_ms + 600;
                    goto dispatch;
                }

                if (suppressed_by_double_click) {
                    VN_DEBUG("hit-test: click on wid=%u suppressed due to active double-click window", wid);
                    vn_pending_red_clear();
                    vn_preclone_discard_wid(wid);
                    goto dispatch;
                }

                bool is_red = vn_is_in_red_hitbox(lx, ly);
                bool is_yellow_or_green = vn_is_in_yellow_or_green_hitbox(lx, ly);

                if (lx <= 150.0 && ly <= 60.0) {
                    char hit_app[256] = {0};
                    pid_t hit_pid = 0;
                    vn_get_window_app_name(win, hit_app, sizeof(hit_app), &hit_pid);
                    VN_TRACE("hit-test: app='%s' pid=%d wid=%u pt=(%.1f, %.1f) lvl=%d -> %s",
                           hit_app, hit_pid, wid, lx, ly, vn_window_level(win),
                           is_red ? "RED (close)"
                                  : is_yellow_or_green ? "yellow/green (no animation)"
                                                       : "no traffic light -- NO PRE-CLONE");
                }

                if (is_red) {
                    // Record the press only. The clone is built on release
                    // (see vn_create_preclone), so a press that turns into a
                    // drag never leaves a clone behind to be exposed.
                    VN_DEBUG(">>> Mouse down in RED CLOSE box of wid=%u (pt=(%.1f, %.1f)) -- waiting for release before cloning",
                           wid, lx, ly);

                    vn_preclone_discard_wid(wid);

                    os_unfair_lock_lock(&gPendingRedLock);
                    gPendingRed = (VNPendingRedClick){
                        .orig_wid = wid,
                        .down_screen_pt = *screen_pt,
                        .down_local_pt  = *local_pt,
                        .active = true,
                        .invalidated = false,
                    };
                    os_unfair_lock_unlock(&gPendingRedLock);
                } else if (is_yellow_or_green) {
                    VN_DEBUG(">>> Mouse down in YELLOW/GREEN button of wid=%u (pt=(%.1f, %.1f)) -- ignoring close animation",
                           wid, lx, ly);
                    vn_pending_red_clear();
                    vn_preclone_discard_wid(wid);
                    atomic_store_explicit(&gNonCloseWid, wid, memory_order_relaxed);
                    atomic_store_explicit(&gNonCloseTimeMs, vn_now_ms(), memory_order_relaxed);
                } else {
                    vn_pending_red_clear();
                    vn_preclone_discard_wid(wid);
                }
            }
        }
    }
    else if (type == 2) {
        uint32_t wid = event->window_id;
        // The release is what commits a red-button press: consume the pending
        // record (always -- a gesture ends here whatever its outcome) and, if
        // it survived, build the clone now.
        VNPendingRedClick pend;
        os_unfair_lock_lock(&gPendingRedLock);
        pend = gPendingRed;
        gPendingRed = (VNPendingRedClick){0};
        os_unfair_lock_unlock(&gPendingRedLock);

        if (pend.active && wid != 0 && pend.orig_wid == wid && vn_resolved_window_by_id) {
            const CGPoint *local_pt = &event->local_pt;
            double lx = local_pt->x;
            double ly = local_pt->y;

            if (pend.invalidated) {
                VN_DEBUG("pre-clone: release on wid=%u ignored -- press was invalidated (drag)", wid);
            } else if (!vn_is_in_red_hitbox(lx, ly)) {
                // Released off the button: AppKit's own button tracking would
                // not close either, so not cloning is the correct outcome.
                VN_DEBUG("pre-clone: release on wid=%u outside red hitbox (pt=(%.1f, %.1f)) -- not cloning",
                       wid, lx, ly);
            } else {
                CGXWindow *win = vn_resolved_window_by_id(wid);
                if (win && vn_is_target_window(win) && !vn_is_window_animating(wid, win)) {
                    // Kept with the same caveat as gAnimNextDeadline, though this
                    // one is input latency rather than frame pacing, so it is the
                    // least likely of the four to be redundant.
                    //
                    // Forward the release BEFORE building the clone. Building
                    // it costs 2ms on a light window but 16ms on a heavy one
                    // (Preview), and doing that first put the whole cost
                    // between the click and the app hearing about it.
                    //
                    // The app is a different process, so it cannot have acted
                    // on the release by the time this returns, and its close
                    // signal is at minimum 40ms out against a 16ms build. If
                    // a request somehow did overtake us on another server
                    // thread it would find no pre-clone and pass through as
                    // an ordinary close -- the same graceful fallback as any
                    // close we did not pre-clone.
                    if (vn_orig_post_event) vn_orig_post_event(conn, event);
                    vn_create_preclone(win, conn, pend.down_screen_pt, pend.down_local_pt);
                    return;
                }
            }
        }
    }
    else if (type == 6) {
        uint32_t wid = event->window_id;

        // A drag while the button is still held disqualifies the press, so no
        // clone is ever built for it.
        uint32_t pend_wid = 0;
        CGPoint  pend_down = CGPointZero;
        bool     pend_live = false;
        os_unfair_lock_lock(&gPendingRedLock);
        pend_live = (gPendingRed.active && !gPendingRed.invalidated);
        pend_wid  = gPendingRed.orig_wid;
        pend_down = gPendingRed.down_screen_pt;
        os_unfair_lock_unlock(&gPendingRedLock);

        if (pend_live && wid != 0 && wid == pend_wid) {
            const CGPoint *local_pt  = &event->local_pt;
            const CGPoint *screen_pt = &event->screen_pt;

            if (!vn_is_in_red_hitbox(local_pt->x, local_pt->y)) {
                vn_pending_red_invalidate(wid, "moved off red button");
            } else {
                // Local coordinates barely move during a window drag (the
                // window follows the cursor), so screen distance is what
                // actually distinguishes a drag from a held click.
                double d_scr = hypot(screen_pt->x - pend_down.x, screen_pt->y - pend_down.y);
                if (d_scr >= 4.0) {
                    vn_pending_red_invalidate(wid, "window drag detected");
                }
            }
        }

        // Defensive: a clone already exists only between release and the real
        // close (40-300ms). A drag starting in that window could still expose
        // it, so drop it if one somehow appears.
        int slot = -1;
        os_unfair_lock_lock(&gPreCloneLock);
        if (wid != 0) slot = vn_preclone_find_slot_locked(wid);
        os_unfair_lock_unlock(&gPreCloneLock);
        if (slot >= 0) {
            VN_DEBUG("pre-clone: drag on wid=%u while a clone exists -- discarding it", wid);
            vn_preclone_discard_wid(wid);
        }
    }

dispatch:
    if (vn_orig_post_event) {
        vn_orig_post_event(conn, event);
    }
}

static void vn_hook_order_window_list(CGXConnection *conn, const uint32_t *wids,
                                 const CGSOrderOp *ops, const uint32_t *relativeTo,
                                 unsigned count, bool spaceSwitch) {
    // Every window the server orders, not just the ones vn_is_target_window()
    // keeps: the frames that give away a zoom-out-of-an-icon open belong to
    // windows that filter rejects.
    for (unsigned pi = 0; ops && pi < count; pi++) {
        uint32_t pwid = wids ? wids[pi] : 0;
        CGXWindow *pwin = pwid ? vn_resolved_window_by_id(pwid) : NULL;
        if (!pwin || vn_is_clone_wid(pwid, pwin)) continue;
        CGRect pr = vn_resolved_screen_rect ? vn_resolved_screen_rect(pwin) : CGRectZero;
        vn_note_window_frame(pwid, pr);
#if ENABLE_LOGS && VN_LOG_LEVEL >= VN_LOG_LEVEL_TRACE
        char papp[256] = {0};
        pid_t ppid = 0;
        vn_get_window_app_name(pwin, papp, sizeof(papp), &ppid);
        VN_TRACE("raw-order: app='%s' pid=%d wid=%u op=%d lvl=%d type=%u frame=%.0f,%.0f %.0fx%.0f target=%d",
                 papp, ppid, pwid, ops[pi], vn_window_level(pwin), pwin->window_type,
                 pr.origin.x, pr.origin.y, pr.size.width, pr.size.height,
                 vn_is_target_window(pwin));
#endif
    }
    if (ops && count >= 1) {
        if (count == 1) {
            uint32_t wid = wids ? wids[0] : 0;
            CGXWindow *win = vn_resolved_window_by_id(wid);
            if (win && vn_is_target_window(win)) {
                char app[256] = {0};
                pid_t pid = 0;
                vn_get_window_app_name(win, app, sizeof(app), &pid);

                if (ops[0] == kVNOrderOut) {
                    vn_track_window_order(wid, pid, kVNOrderOut, win);
                    vn_early_hidden_forget(wid);

                    bool animating = vn_is_window_animating(wid, win);
                    if (animating) {
                        VN_DEBUG("Window wid=%u (%p, app: '%s') already animating; dropping duplicate order-out",
                               wid, win, app);
                        return;
                    }

                    uint64_t now_ms = vn_now_ms();
                    uint64_t non_close_time = atomic_load_explicit(&gNonCloseTimeMs, memory_order_relaxed);
                    uint32_t non_close_wid = atomic_load_explicit(&gNonCloseWid, memory_order_relaxed);
                    if (non_close_wid == wid && (now_ms - non_close_time) < 2000) {
                        VN_DEBUG("Window wid=%u orderOut is from Miniaturize/Fullscreen (non-close recorded %llu ms ago) -- passing through without animation",
                               wid, (now_ms - non_close_time));
                        vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
                        return;
                    }

                    uint32_t clone_wid = 0;
                    CGXWindow *clone = NULL;
                    CGRect clone_frame = CGRectZero;
                    pid_t anim_pid = 0;
                    uint64_t anim_psn = 0;
                    bool anim_is_wa = false;
                    char anim_app[64] = {0};
                    const VNAnimation *anim_desc = NULL;
                
                    if (vn_system_close_animation_in_flight(wid, pid)) {
                        VN_INFO(">>> Deferring to the system's close animation for wid=%u (pid=%d) -- app just spawned a new window, ours would collide",
                                wid, pid);
                        vn_preclone_discard_wid(wid);
                    }

                    os_unfair_lock_lock(&gPreCloneLock);
                    clone = vn_preclone_take_locked(wid, &clone_wid, &clone_frame, &anim_desc, &anim_pid, &anim_psn, &anim_is_wa, anim_app, sizeof(anim_app));
                    os_unfair_lock_unlock(&gPreCloneLock);

                    if (!clone || clone_wid == 0) {
                        VN_DEBUG("Window wid=%u orderOut has no pre-clone (not closed via red button) -- passing through directly", wid);
                        vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
                        return;
                    }

                    const char *final_app = anim_app[0] ? anim_app : app;
                    if (!anim_is_wa && (strcasecmp(final_app, "WhatsApp") == 0)) {
                        anim_is_wa = true;
                    }
                    VN_INFO(">>> Intercepted close for target window %u (%p, app: '%s' pid=%d psn=0x%llx is_wa=%d)",
                           wid, win, final_app, anim_pid ? anim_pid : pid, anim_psn, anim_is_wa);

                    vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
                    vn_start_clone_animation(clone, wid, clone_frame, anim_desc, anim_pid ? anim_pid : pid, anim_psn, anim_is_wa, final_app);
                    return;
                } else {
                    VN_TRACE("Target order op: app='%s' pid=%d count=1 wid=%u op=%d lvl=%d",
                           app, pid, wid, ops[0], vn_window_level(win));

                    bool was_already_ordered_in = vn_check_window_is_ordered_in(wid, win);
                    if (!was_already_ordered_in) vn_note_new_window(wid, pid);
                    vn_track_window_order(wid, pid, ops[0], win);
                    vn_cancel_window_animation_if_ordering_in(wid, win, pid, was_already_ordered_in);
                }
            }

            vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
            return;
        }

        uint32_t pass_wids[count];
        CGSOrderOp pass_ops[count];
        uint32_t pass_rel[count];
        unsigned pass_count = 0;

        for (unsigned i = 0; i < count; i++) {
            uint32_t wid = wids ? wids[i] : 0;
            CGXWindow *win = vn_resolved_window_by_id(wid);

            if (win && vn_is_target_window(win)) {
                char app[256] = {0};
                pid_t pid = 0;
                vn_get_window_app_name(win, app, sizeof(app), &pid);

                VN_TRACE("Target order op: app='%s' pid=%d count=%u i=%u wid=%u op=%d lvl=%d",
                       app, pid, count, i, wid, ops[i], vn_window_level(win));

                if (ops[i] == kVNOrderOut) {
                    vn_track_window_order(wid, pid, kVNOrderOut, win);
                    vn_early_hidden_forget(wid);

                    if (vn_is_window_animating(wid, win)) {
                        VN_DEBUG("Window wid=%u (%p, app: '%s') already animating; dropping duplicate order-out",
                               wid, win, app);
                        continue;
                    }

                    uint64_t now_ms = vn_now_ms();
                    uint64_t non_close_time = atomic_load_explicit(&gNonCloseTimeMs, memory_order_relaxed);
                    uint32_t non_close_wid = atomic_load_explicit(&gNonCloseWid, memory_order_relaxed);
                    if (non_close_wid == wid && (now_ms - non_close_time) < 2000) {
                        VN_DEBUG("Window wid=%u multi-orderOut is from Miniaturize/Fullscreen -- passing through without animation", wid);
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
                    const VNAnimation *anim_desc = NULL;
                
                    if (vn_system_close_animation_in_flight(wid, pid)) {
                        VN_INFO(">>> Deferring to the system's close animation for wid=%u (pid=%d) -- app just spawned a new window, ours would collide",
                                wid, pid);
                        vn_preclone_discard_wid(wid);
                    }

                    os_unfair_lock_lock(&gPreCloneLock);
                    clone = vn_preclone_take_locked(wid, &clone_wid, &clone_frame, &anim_desc, &anim_pid, &anim_psn, &anim_is_wa, anim_app, sizeof(anim_app));
                    os_unfair_lock_unlock(&gPreCloneLock);

                    if (!clone || clone_wid == 0) {
                        VN_DEBUG("Window wid=%u multi-orderOut has no pre-clone (not closed via red button) -- passing through", wid);
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
                        VN_INFO(">>> Intercepted close for target window %u (%p, app: '%s' pid=%d psn=0x%llx is_wa=%d)",
                               wid, win, final_app, anim_pid ? anim_pid : pid, anim_psn, anim_is_wa);
                        vn_start_clone_animation(clone, wid, clone_frame, anim_desc, anim_pid ? anim_pid : pid, anim_psn, anim_is_wa, final_app);
                    }
                    pass_wids[pass_count] = wid;
                    pass_ops[pass_count] = ops[i];
                    pass_rel[pass_count] = relativeTo ? relativeTo[i] : 0;
                    pass_count++;
                    continue;
                } else {
                    bool was_already_ordered_in = vn_check_window_is_ordered_in(wid, win);
                    if (!was_already_ordered_in) vn_note_new_window(wid, pid);
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
            vn_orig_order(conn, pass_wids, pass_ops, pass_rel, pass_count, spaceSwitch);
        }
        return;
    }

    vn_orig_order(conn, wids, ops, relativeTo, count, spaceSwitch);
}

#pragma mark - Shader composite hook

// Where a shader animation will draw. A type-1 filter on the clone makes the
// compositor route its layer here (see kVNSymMetalCompositeRipple), so this
// runs only for windows carrying a filter -- in practice only ours, since
// nothing on the system sets one: the sole route is SLSNewCIFilterByName,
// a private client API for a two-effect legacy set.
//
// It draws nothing yet. Right now it only proves the hook fires where the
// static analysis says it does, and, just as importantly, that it stays quiet
// the rest of the time: a line per frame while a clone animates and not one
// otherwise is what says this is safe to build on.
static void vn_hook_metal_composite_ripple(void *ctx, void *layer, void *dest) {
#if ENABLE_LOGS && VN_LOG_LEVEL >= VN_LOG_LEVEL_TRACE
    if (layer) {
        const uint32_t type = *(const uint32_t *)((const uint8_t *)layer + kVNLayerFilterType);
        const float   *p    = (const float *)((const uint8_t *)layer + kVNLayerFilterParams);
        VN_TRACE("composite_ripple: layer=%p type=%u params=[%.3f %.3f %.3f %.3f %.3f] ctx=%p dest=%p",
                 layer, type, p[0], p[1], p[2], p[3], p[4], ctx, dest);
    }
#endif

    if (vn_orig_metal_composite_ripple) {
        vn_orig_metal_composite_ripple(ctx, layer, dest);
    }
}

#pragma mark - Entry

extern void MSHookFunction(void *symbol, void *replace, void **result);

__attribute__((constructor))
static void vanish_init(void) {
    char self[1024] = {0};
    uint32_t len = (uint32_t)sizeof(self);
    if (_NSGetExecutablePath(self, &len) != 0) return;
    if (strstr(self, "WindowServer") == NULL) {
        VN_INFO("not WindowServer (%{public}s) -- doing nothing", self);
        return;
    }

    void *targetOrder               = vn_skylight_symbol(kVNSymOrderWindowList);
    void *targetRelease             = vn_skylight_symbol(kVNSymReleaseWindow);
    void *targetPostEvent           = vn_skylight_symbol(kVNSymPostEventByConnection);
    vn_resolved_window_by_id                 = (VNWindowByIDFn)vn_skylight_symbol(kVNSymWindowByID);
    vn_resolved_window_get_owning_pid        = (VNWindowGetOwningPIDFn)vn_skylight_symbol(kVNSymWSWindowGetOwningPID);
    vn_resolved_get_connection_app_name      = (VNGetConnectionAppNameFn)vn_skylight_symbol(kVNSymGetConnectionAppName);
    vn_resolved_schedule_callback            = (VNScheduleCallbackFn)vn_skylight_symbol(kVNSymScheduleCallback);
    vn_resolved_set_mesh_warp                = (VNSetMeshWarpFn)vn_skylight_symbol(kVNSymSetMeshWarp);
    vn_resolved_window_get_filter            = (VNWindowGetFilterFn)vn_skylight_symbol(kVNSymWindowGetFilter);
    vn_resolved_window_set_filter            = (VNWindowSetFilterFn)vn_skylight_symbol(kVNSymWindowSetFilter);
    vn_resolved_update_window                = (VNUpdateWindowFn)vn_skylight_symbol(kVNSymUpdateWindow);
    vn_resolved_create_clone                 = (VNCreateCloneFn)vn_skylight_symbol(kVNSymCreateCloneOfWindow);
    vn_resolved_system_window_release        = (VNSystemWindowReleaseFn)vn_skylight_symbol(kVNSymSystemWindowRelease);
    vn_resolved_window_get_display           = (VNWindowGetDisplayFn)vn_skylight_symbol(kVNSymWindowGetDisplay);
    vn_resolved_screen_rect_from_rect        = (VNScreenRectFromRectFn)vn_skylight_symbol(kVNSymScreenRectFromRect);
    vn_resolved_screen_rect                  = (VNScreenRectFn)vn_skylight_symbol(kVNSymScreenRect);
    vn_resolved_window_get_id                = (VNWindowGetIDFn)vn_skylight_symbol(kVNSymWindowGetID);
    vn_resolved_clipped_frame_bounds         = (VNClippedFrameBoundsFn)vn_skylight_symbol(kVNSymClippedFrameBounds);
    vn_resolved_update_ca_visibility         = (VNUpdateCAVisibilityFn)vn_skylight_symbol(kVNSymUpdateCAVisibility);
    vn_resolved_clear_shadow_density         = (VNClearShadowDensityFn)vn_skylight_symbol(kVNSymClearShadowDensity);
    vn_resolved_window_set_shadow_enable     = (VNWSWindowSetShadowEnableFn)vn_skylight_symbol(kVNSymWSWindowSetShadowEnable);
    vn_resolved_window_release_shadow_resources = (VNWSWindowReleaseShadowResourcesFn)vn_skylight_symbol(kVNSymWSWindowReleaseShadowResources);
    vn_resolved_set_window_shadow_parameters = (VNSLSSetWindowShadowParametersFn)vn_skylight_symbol(kVNSymSLSSetWindowShadowParameters);

    void *targetEligible = vn_skylight_symbol(kVNSymIsProcessEligibleForSetFront);

    vn_resolved_window_is_ordered_in = (VNDynWindowIsOrderedInFn)vn_skylight_symbol("CGXWindowIsOrderedIn");
    if (!vn_resolved_window_is_ordered_in) {
        vn_resolved_window_is_ordered_in = (VNDynWindowIsOrderedInFn)vn_skylight_symbol("_CGXWindowIsOrderedIn");
    }

    if (!targetOrder || !targetRelease || !vn_resolved_window_by_id ||
        !vn_resolved_schedule_callback || !vn_resolved_clipped_frame_bounds) {
        VN_ERROR("symbol resolution failed -- inert (order=%p release=%p win_by_id=%p mesh_warp=%p)",
               targetOrder, targetRelease, vn_resolved_window_by_id, (void *)vn_resolved_set_mesh_warp);
        return;
    }

    void *rawRelease = ptrauth_strip(targetRelease, ptrauth_key_function_pointer);
    MSHookFunction(rawRelease, (void *)vn_hook_release_window, (void **)&vn_orig_release_window);

    void *rawOrder = ptrauth_strip(targetOrder, ptrauth_key_function_pointer);
    MSHookFunction(rawOrder, (void *)vn_hook_order_window_list, (void **)&vn_orig_order);

    if (targetPostEvent) {
        void *rawPost = ptrauth_strip(targetPostEvent, ptrauth_key_function_pointer);
        MSHookFunction(rawPost, (void *)vn_hook_post_event, (void **)&vn_orig_post_event);
        VN_DEBUG("hooked CGXPostEventByConnection -> orig %p", vn_orig_post_event);
    } else {
        VN_ERROR("WARNING: CGXPostEventByConnection unresolved");
    }

    void *targetRipple = vn_skylight_symbol(kVNSymMetalCompositeRipple);
    if (targetRipple) {
        void *rawRipple = ptrauth_strip(targetRipple, ptrauth_key_function_pointer);
        MSHookFunction(rawRipple, (void *)vn_hook_metal_composite_ripple,
                       (void **)&vn_orig_metal_composite_ripple);
        VN_DEBUG("hooked metal_composite_ripple -> orig %p", (void *)vn_orig_metal_composite_ripple);
    } else {
        VN_ERROR("WARNING: metal_composite_ripple unresolved -- shader animations cannot draw");
    }

    if (targetEligible) {
        void *rawEligible = ptrauth_strip(targetEligible, ptrauth_key_function_pointer);
        MSHookFunction(rawEligible, (void *)vn_hook_is_process_eligible, (void **)&vn_orig_is_process_eligible);
        VN_DEBUG("hooked isProcessEligibleForSetFront -> orig %p", (void *)vn_orig_is_process_eligible);
    } else {
        VN_ERROR("WARNING: isProcessEligibleForSetFront unresolved");
    }

    CFNotificationCenterAddObserver(
        CFNotificationCenterGetDarwinNotifyCenter(),
        NULL,
        vn_prefs_changed_callback,
        CFSTR("com.doraorak.vanish/prefsChanged"),
        NULL,
        CFNotificationSuspensionBehaviorDeliverImmediately);
    VN_DEBUG("Registered Darwin notification observer for com.doraorak.vanish/prefsChanged");

    VN_INFO("Vanish loaded successfully! Target window close hook active (duration: %.2fs)", (double)vn_duration());
}
