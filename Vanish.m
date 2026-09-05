//
//  Vanish.m
//  A close animation for macOS windows.
//
//  This loads into WindowServer. Everything below is written for that fact.
//
//  Architecture:
//  1. Target Isolation: All non-target windows pass straight through to orig
//     immediately. During startup/login, system windows (Dock, Finder, Notification
//     Center, Activity Monitor, etc.) are never touched, ensuring 100% stability.
//     Target process name is configurable via `/tmp/vanish_target` (defaults to "VanishTest").
//
//  2. Close Synchronization: When our target window closes, AppKit calls orderOut
//     and immediately follows with release_window (_XTerminateWindow). We intercept
//     orderOut to run our custom animation, and defer release_window until the animation
//     finishes, ensuring CGXWindow remains valid throughout the animation.
//

#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <string.h>
#import <stdbool.h>
#import <math.h>
#import <dlfcn.h>
#import <ptrauth.h>
#import <time.h>
#import <libproc.h>
#import <os/lock.h>
#import <os/log.h>
#import <mach-o/loader.h>
#import <mach-o/nlist.h>

#import "SLSPrivate.h"
#import "SkyLightServer.h"

#import <sys/time.h>
#import <sys/stat.h>
#import <stdatomic.h>

#pragma mark - Logging

#define ENABLE_LOGS 1

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

    // Milliseconds matter: the behaviour being chased happens inside the first
    // 100-300 ms of a close, which second resolution cannot show at all.
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
#define VN_LOG(fmt, ...) vn_log_to_file(fmt, ##__VA_ARGS__)
#else
#define VN_LOG(fmt, ...) do {} while (0)
#endif

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
static VNSetWindowAlphasFn          vn_orig_set_window_alphas;  // set when tracing hooks it
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

#pragma mark - Preferences

typedef struct {
    bool  enabled;
    bool  shadows;
    float duration;
    char  targetApp[256];
} VNPreferences;

static VNPreferences  gPrefs = { .enabled = true, .shadows = true, .duration = 0.25f, .targetApp = "all" };
static os_unfair_lock gPrefsLock = OS_UNFAIR_LOCK_INIT;
static struct timespec gPrefsMtime = {0};
static bool           gPrefsValid = false;

static void vn_reload_prefs_locked(void) {
    gPrefs.enabled = true;
    gPrefs.shadows = true;
    gPrefs.duration = 0.25f;
    strlcpy(gPrefs.targetApp, "all", sizeof(gPrefs.targetApp));

    // 1. Read from /Library/TweakInject/Preferences/Defaults/com.doraorak.vanish.plist
    NSString *path = @"/Library/TweakInject/Preferences/Defaults/com.doraorak.vanish.plist";
    struct stat st;
    if (stat([path UTF8String], &st) == 0) {
        gPrefsMtime = st.st_mtimespec;
        gPrefsValid = true;

        NSDictionary *dict = [NSDictionary dictionaryWithContentsOfFile:path];
        if (dict) {
            if (dict[@"enabled"] != nil) {
                gPrefs.enabled = [dict[@"enabled"] boolValue];
            }
            if (dict[@"shadows"] != nil) {
                gPrefs.shadows = [dict[@"shadows"] boolValue];
            }
            if (dict[@"duration"] != nil) {
                float dur = [dict[@"duration"] floatValue];
                if (dur >= 0.05f && dur <= 60.0f) {
                    gPrefs.duration = dur;
                }
            }
            if (dict[@"targetApp"] != nil) {
                NSString *target = dict[@"targetApp"];
                if ([target isKindOfClass:[NSString class]]) {
                    const char *s = [target UTF8String];
                    if (s && s[0] != '\0') {
                        strlcpy(gPrefs.targetApp, s, sizeof(gPrefs.targetApp));
                    }
                }
            }
        }
    } else {
        gPrefsValid = false;
    }

    // 2. Developer command-line overrides (if present and non-empty):
    // /tmp/vanish_duration overrides duration
    FILE *f_dur = fopen("/tmp/vanish_duration", "r");
    if (f_dur) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), f_dur)) {
            float val = strtof(buf, NULL);
            if (val >= 0.05f && val <= 60.0f) {
                gPrefs.duration = val;
            }
        }
        fclose(f_dur);
    }

    // /tmp/vanish_target overrides targetApp
    FILE *f_tgt = fopen("/tmp/vanish_target", "r");
    if (f_tgt) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), f_tgt)) {
            buf[strcspn(buf, "\r\n")] = '\0';
            if (buf[0] != '\0') {
                strlcpy(gPrefs.targetApp, buf, sizeof(gPrefs.targetApp));
            }
        }
        fclose(f_tgt);
    }

    // /tmp/vanish_shadows overrides shadows (0 or 1)
    FILE *f_shd = fopen("/tmp/vanish_shadows", "r");
    if (f_shd) {
        char buf[16] = {0};
        if (fgets(buf, sizeof(buf), f_shd)) {
            int val = atoi(buf);
            gPrefs.shadows = (val != 0);
        }
        fclose(f_shd);
    }
}

static VNPreferences vn_get_prefs(void) {
    struct stat st;
    bool have_stat = (stat("/Library/TweakInject/Preferences/Defaults/com.doraorak.vanish.plist", &st) == 0);
    struct stat st_tmp_dur, st_tmp_tgt, st_tmp_shd;
    bool have_tmp_dur = (stat("/tmp/vanish_duration", &st_tmp_dur) == 0);
    bool have_tmp_tgt = (stat("/tmp/vanish_target", &st_tmp_tgt) == 0);
    bool have_tmp_shd = (stat("/tmp/vanish_shadows", &st_tmp_shd) == 0);

    os_unfair_lock_lock(&gPrefsLock);
    bool fresh = gPrefsValid && have_stat &&
                 st.st_mtimespec.tv_sec  == gPrefsMtime.tv_sec &&
                 st.st_mtimespec.tv_nsec == gPrefsMtime.tv_nsec &&
                 !have_tmp_dur && !have_tmp_tgt && !have_tmp_shd;
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

/// Animation duration in seconds.
static float vn_duration(void) {
    VNPreferences prefs = vn_get_prefs();
    return prefs.duration;
}

#pragma mark - Target Filtering

/// Safely retrieves the application name for `win`.
/// Inside WindowServer, CGXGetConnectionAppName reads from the server's internal
/// connection table (ProcessRecord), bypassing kernel proc_pidinfo EPERM restrictions.
static bool vn_get_window_app_name(CGXWindow *win, char *out_name, size_t maxlen, pid_t *out_pid) {
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

    // 1. In WindowServer, connection ID is at win + 0x50.
    // CGXGetConnectionAppName queries WindowServer's internal table (ProcessRecord),
    // which works across all UIDs without kernel EPERM issues!
    if (vn_get_connection_app_name) {
        uint32_t cid = *(const uint32_t *)((const char *)win + 0x50);
        if (cid != 0) {
            char buf[256] = {0};
            if (vn_get_connection_app_name(cid, buf, sizeof(buf)) == 0 && buf[0] != '\0') {
                strncpy(out_name, buf, maxlen - 1);
                return true;
            }
        }
    }

    // 2. Fallbacks using pid: proc_pidpath, proc_name
    if (pid > 0) {
        char path[1024] = {0};
        if (proc_pidpath(pid, path, sizeof(path)) > 0) {
            char *slash = strrchr(path, '/');
            if (slash && slash[1] != '\0') {
                strncpy(out_name, slash + 1, maxlen - 1);
                return true;
            }
        }
        if (proc_name(pid, out_name, (uint32_t)maxlen) > 0 && out_name[0] != '\0') {
            return true;
        }
    }

    return false;
}

/// Returns true ONLY if `win` belongs to our designated target application.
static bool vn_is_target_window(CGXWindow *win) {
    if (!win) return false;

    VNPreferences prefs = vn_get_prefs();
    if (!prefs.enabled) return false;

    char name[256] = {0};
    pid_t pid = 0;
    bool has_name = vn_get_window_app_name(win, name, sizeof(name), &pid);

    // 1. If targetApp is "all" or empty, match all regular application windows
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

            // Exclude Finder Get Info inspector panels (fixed width 400pt).
            // Regular Finder folder browsing windows have min-width 510pt.
            // This preserves Finder's native desktop icon-zoom close transition!
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
        // Exclude system/special window levels (e.g. desktop level -2147483628, menu level 24, status level 25)
        int32_t lvl = vn_window_level(win);
        if (lvl != 0 && lvl != 3) {
            return false;
        }
        return true;
    }

    // 2. Always match VanishTest
    if (has_name && strcasecmp(name, "VanishTest") == 0) return true;

    // 3. Match specific configured application name
    if (has_name && strcasecmp(name, prefs.targetApp) == 0) return true;

    // 4. Match specific PID if targetApp is numeric
    pid_t target_pid = (pid_t)atoi(prefs.targetApp);
    if (target_pid > 0 && pid == target_pid) return true;

    return false;
}

#pragma mark - Animation State & Multi-Window Tracking

static void vn_clear_warp(CGXWindow *win, CGXConnection *conn, CGRect bounds);

typedef struct {
    uint32_t       wid;             // clone wid if is_clone, else target wid
    uint32_t       orig_wid;        // original target wid being closed
    CGXWindow     *win;
    CGXConnection *conn;
    uint64_t       anim_id;
    bool           is_animating;
    bool           probe_only;      // warp for its own sake: never order out
    bool           is_clone;        // we made this window; destroy it when done
    CGRect         bounds;          // sampled once; see vn_start_window_animation
    double         start_time;   // SLSCurrentRealTime() when the fade began
    double         duration;
} VNWindowAnim;

#define MAX_ACTIVE_ANIMS 32
static VNWindowAnim   gActiveAnims[MAX_ACTIVE_ANIMS];
static os_unfair_lock gAnimsLock = OS_UNFAIR_LOCK_INIT;
static uint64_t       gNextAnimId = 1;

static bool vn_is_window_animating(uint32_t wid, CGXWindow *win) {
    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (gActiveAnims[i].is_animating) {
            if ((wid != 0 && (gActiveAnims[i].wid == wid || gActiveAnims[i].orig_wid == wid)) ||
                (win != NULL && gActiveAnims[i].win == win)) {
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
    uint32_t wid = 0;
    CGXWindow *win = NULL;
    CGXConnection *conn = NULL;
    bool found = false;
    bool probe_only = false;
    bool is_clone = false;
    CGRect bounds = CGRectZero;
    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (gActiveAnims[i].is_animating && gActiveAnims[i].anim_id == anim_id) {
            wid = gActiveAnims[i].wid;
            win = gActiveAnims[i].win;
            conn = gActiveAnims[i].conn;
            probe_only = gActiveAnims[i].probe_only;
            is_clone = gActiveAnims[i].is_clone;
            bounds = gActiveAnims[i].bounds;
            gActiveAnims[i].is_animating = false;
            gActiveAnims[i].wid = 0;
            gActiveAnims[i].orig_wid = 0;
            gActiveAnims[i].win = NULL;
            gActiveAnims[i].conn = NULL;
            found = true;
            break;
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    if (!found) return;

    VN_LOG("finish_animation_on_main: wid=%u win=%p", wid, win);

    // Re-validate window through WindowServer's internal table before operating (for real windows)
    if (!is_clone && wid != 0 && vn_window_by_id) {
        CGXWindow *live_win = vn_window_by_id(wid);
        if (!live_win || live_win != win) {
            VN_LOG("Window wid=%u is no longer valid in WindowServer, skipping order-out", wid);
            return;
        }
    }

    // A clone is ours:
    // 1. Immediately hide its CoreAnimation layer so CA stops drawing it completely.
    // 2. Order it out using server-internal context (conn = NULL), bypassing client ownership checks.
    // 3. Defer destruction by 100ms so the compositor commits the hide and orderOut
    //    before the mesh warp is deallocated. This guarantees zero 1-frame unwarped flash.
    if (is_clone) {
        VN_LOG("finish: hiding and ordering out clone wid=%u win=%p (release deferred 100ms)", wid, win);
        if (win && vn_update_ca_visibility) {
            vn_update_ca_visibility(win, false);
        }
        if (wid != 0) {
            CGSOrderOp op = kVNOrderOut;
            uint32_t rel = 0;
            vn_orig_order(NULL, &wid, &op, &rel, 1, false);
        }
        if (vn_schedule_callback && win) {
            vn_schedule_callback(vn_delayed_clone_release, win, SLSCurrentRealTime() + 0.1);
        } else if (vn_system_window_release && win) {
            vn_system_window_release(win);
        }
        return;
    }

    // 1. Commit the actual order-out to the display server.
    if (probe_only) {
        VN_LOG("probe finished for wid=%u -- restoring shape, not ordering out", wid);
        vn_clear_warp(win, conn, bounds);
        return;
    }
    if (conn && wid != 0) {
        CGSOrderOp op = kVNOrderOut;
        uint32_t rel = 0;
        VN_LOG("finish_animation_on_main: committing order-out for wid=%u win=%p", wid, win);
        vn_orig_order(conn, &wid, &op, &rel, 1, false);
    }

    // 2. Restore alpha to 1.0f in case the window is re-used or ordered back in later.
    if (win && conn) {
        vn_clear_warp(win, conn, bounds);
    }
}

#pragma mark - Pre-Cloning (Option A)

/// Pre-cloning captures the window at mouse-down time on the close button,
/// long before the client receives mouse-up or executes performClose:/orderOut:.
/// At mouse-down (t=0), the window is 100% alive, opaque, and undamaged.
/// Creating and ordering the clone above the window gives the compositor ~100ms
/// (physical click duration) to ingest and composite the clone. When orderOut arrives,
/// the clone is ALREADY visible and composited on screen, eliminating any 1-frame gap.
typedef struct {
    uint32_t       orig_wid;
    uint32_t       clone_wid;
    CGXWindow     *clone;
    CGXConnection *conn;
    CGRect         frame;
    double         created_at;
    CGPoint        mouseDownScreenPt;
    CGPoint        mouseDownLocalPt;
    double         mouseUpTime;
} VNPreClone;

static VNPreClone     gPreClone = {0};
static os_unfair_lock gPreCloneLock = OS_UNFAIR_LOCK_INIT;

// Clears gPreClone under lock and returns the clone pointer to release, if any.
// MUST be called with gPreCloneLock held.
// NEVER calls any external or system functions.
static CGXWindow *vn_preclone_take_locked(uint32_t *out_clone_wid, uint32_t *out_orig_wid, CGRect *out_frame) {
    CGXWindow *clone = gPreClone.clone;
    if (out_clone_wid) *out_clone_wid = gPreClone.clone_wid;
    if (out_orig_wid) *out_orig_wid = gPreClone.orig_wid;
    if (out_frame) *out_frame = gPreClone.frame;
    gPreClone.orig_wid = 0;
    gPreClone.clone_wid = 0;
    gPreClone.clone = NULL;
    gPreClone.conn = NULL;
    gPreClone.frame = CGRectZero;
    gPreClone.created_at = 0.0;
    gPreClone.mouseDownScreenPt = CGPointZero;
    gPreClone.mouseDownLocalPt  = CGPointZero;
    gPreClone.mouseUpTime = 0.0;
    return clone;
}

// Discards the pre-clone safely: extracts clone under lock, unlocks,
// suppresses CA visibility, orders out the clone, and releases it.
static void vn_preclone_discard(void) {
    uint32_t clone_wid = 0, orig_wid = 0;
    os_unfair_lock_lock(&gPreCloneLock);
    CGXWindow *clone = vn_preclone_take_locked(&clone_wid, &orig_wid, NULL);
    os_unfair_lock_unlock(&gPreCloneLock);

    if (clone) {
        VN_LOG("pre-clone: discarding and hiding unused clone wid=%u for orig=%u", clone_wid, orig_wid);
        if (vn_update_ca_visibility) {
            vn_update_ca_visibility(clone, false);
        }
        if (clone_wid != 0) {
            CGSOrderOp op = kVNOrderOut;
            uint32_t rel = 0;
            vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false);
        }
        if (vn_system_window_release) {
            vn_system_window_release(clone);
        }
    }
}

static void vn_preclone_cleanup_timer(void *ctx, double when) {
    (void)ctx; (void)when;
    double now = SLSCurrentRealTime();
    CGXWindow *clone = NULL;
    uint32_t clone_wid = 0, orig_wid = 0;

    os_unfair_lock_lock(&gPreCloneLock);
    if (gPreClone.orig_wid != 0) {
        bool expired = false;
        if (gPreClone.mouseUpTime > 0.0) {
            // Mouse was released: if AppKit/SwiftUI didn't close the window within 1.0s, clean up
            if ((now - gPreClone.mouseUpTime) >= 1.0) {
                expired = true;
            }
        } else {
            // Mouse is still held down: do not time out unless held for an extreme failsafe duration (30s)
            if ((now - gPreClone.created_at) >= 30.0) {
                expired = true;
            }
        }
        if (expired) {
            VN_LOG("pre-clone: timed out (age=%.2fs, since_up=%.2fs) without close -- cleaning up",
                   now - gPreClone.created_at,
                   gPreClone.mouseUpTime > 0.0 ? (now - gPreClone.mouseUpTime) : -1.0);
            clone = vn_preclone_take_locked(&clone_wid, &orig_wid, NULL);
        }
    }
    os_unfair_lock_unlock(&gPreCloneLock);

    if (clone) {
        if (vn_update_ca_visibility) {
            vn_update_ca_visibility(clone, false);
        }
        if (clone_wid != 0) {
            CGSOrderOp op = kVNOrderOut;
            uint32_t rel = 0;
            vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false);
        }
        if (vn_system_window_release) {
            vn_system_window_release(clone);
        }
    }
}

#pragma mark - Snapshot

/// Builds a clone of `win` and orders it `place` relative to the original.
///
/// This is the whole point of the snapshot approach: the clone is a window we
/// own and that is being composited, so everything already proven on live
/// windows -- the mesh warp above all -- applies to it. The original can then be
/// ordered out and torn down by its app without taking the animation with it.
///
/// `CreateCloneOfWindow` does all the construction; there is nothing to
/// reproduce here beyond finding the display and the frame.
static CGXWindow *vn_make_snapshot(CGXWindow *win, CGXConnection *conn,
                                   uint32_t orig_wid, uint32_t *out_wid,
                                   CGRect *out_frame,
                                   CGSOrderOp place) {
    if (access("/tmp/vanish_snapshot_off", F_OK) == 0) {
        VN_LOG("snapshot disabled by /tmp/vanish_snapshot_off");
        return NULL;
    }
    if (!vn_create_clone || !vn_window_get_display || !vn_screen_rect_from_rect ||
        !vn_window_get_id) {
        VN_LOG("snapshot: symbols unresolved (clone=%p disp=%p rect=%p id=%p)",
               (void *)vn_create_clone, (void *)vn_window_get_display,
               (void *)vn_screen_rect_from_rect, (void *)vn_window_get_id);
        return NULL;
    }

    const void *display = vn_window_get_display(win);
    if (!display) { VN_LOG("snapshot: no display for wid=%u", orig_wid); return NULL; }

    // vn_clipped_frame_bounds(win) returns the window frame INCLUDING the full drop shadow
    // in screen coordinates (e.g. bounds starts 56pt to the left and 38pt above the window content).
    // It is ALREADY in screen coordinates. Do NOT add probe.origin or pass through screen_rect_from_rect,
    // which would double the screen origin offset and push the clone hundreds of pixels into the middle of the app!
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

    CGXWindow *clone = vn_create_clone(win, frame, display, true);
    if (!clone) { VN_LOG("snapshot: CreateCloneOfWindow returned NULL"); return NULL; }

    uint32_t wid = vn_window_get_id(clone);
    if (wid != 0) {
        CGSOrderOp op  = place;
        uint32_t   rel = orig_wid;
        vn_orig_order(conn, &wid, &op, &rel, 1, false);
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

    float shadow_l = (content.size.width >= 1.0) ? (content.origin.x - frame.origin.x) : 0.0f;
    float shadow_t = (content.size.height >= 1.0) ? (content.origin.y - frame.origin.y) : 0.0f;
    float shadow_r = (content.size.width >= 1.0) ? ((frame.origin.x + frame.size.width) - (content.origin.x + content.size.width)) : 0.0f;
    float shadow_b = (content.size.height >= 1.0) ? ((frame.origin.y + frame.size.height) - (content.origin.y + content.size.height)) : 0.0f;
    float extra_w  = (content.size.width >= 1.0) ? (frame.size.width - content.size.width) : 0.0f;
    float extra_h  = (content.size.height >= 1.0) ? (frame.size.height - content.size.height) : 0.0f;

    VN_LOG("snapshot: clone=%p wid=%u %s %u frame=(%.1f,%.1f %.1fx%.1f) display=%p",
           clone, wid, place == kVNOrderBelow ? "below" : "above", orig_wid,
           frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
           display);
    VN_LOG("  [SHADOW TRACE] orig_content=(%.1f,%.1f %.1fx%.1f) clone_frame=(%.1f,%.1f %.1fx%.1f) shadow=[L=%.1f T=%.1f R=%.1f B=%.1f] extra=[+%.1fw, +%.1fh]",
           content.origin.x, content.origin.y, content.size.width, content.size.height,
           frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
           shadow_l, shadow_t, shadow_r, shadow_b, extra_w, extra_h);
    return clone;
}

#pragma mark - Animations

// Tracing lives further down, next to the hooks it belongs to.
static bool vn_trace_win(CGXWindow *win);
static long vn_trace_ms(void);


/// Mesh resolution. 2x2 would do for a plain scale, but the grid is the whole
/// point: anything that can move a vertex can be drawn here, so start with
/// enough vertices to bend.
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

    return (1.0 / 120.0); // High-refresh ProMotion default (8.33ms)
}

/// Fills the mesh for one frame.
///
/// `t` runs 0 -> 1. `bounds` is the window's frame in screen coordinates. The
/// local coordinates stay put -- they say which part of the window a vertex is
/// -- and only the global ones move. Every future animation is a new function
/// of exactly this shape.
static void vn_anim_shrink(VNPointWarp *mesh, CGRect bounds, double t) {
    double s  = 1.0 - t;
    if (s < 0.005) s = 0.005;  // Keep non-zero positive area to avoid GPU shader singularity
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

/// Puts the window back on its own shape, so one that is ordered back in is not
/// left holding the last frame of an animation.
static void vn_clear_warp(CGXWindow *win, CGXConnection *conn, CGRect bounds) {
    if (!vn_set_mesh_warp || !win || !conn) return;
    VNPointWarp mesh[kVNMeshCount];
    vn_anim_shrink(mesh, bounds, 0.0);   // t = 0 is identity
    vn_set_mesh_warp(win, conn, kVNMeshW, kVNMeshH, (const float *)mesh);
}

static void vn_finish_animation_for_id(uint64_t anim_id);
/// Draws one frame of every running animation, from the server's timer pass.
///
/// The order-out is issued from here rather than from any completion callback:
/// `start_order_window` force-finishes pending fades, so a completion runs
/// *inside* the ordering machinery, and re-entering it from there corrupts the
/// window list. The timer pass is a context the server itself orders windows
/// from.
static void vn_anim_tick(void *ctx, double when) {
    (void)ctx; (void)when;

    double now = SLSCurrentRealTime();
    uint64_t finished[MAX_ACTIVE_ANIMS];
    int finished_count = 0;
    bool more = false;

    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (!gActiveAnims[i].is_animating) continue;

        uint64_t aid = gActiveAnims[i].anim_id;
        double dur = gActiveAnims[i].duration > 0.0 ? gActiveAnims[i].duration : 0.25;
        double p = (now - gActiveAnims[i].start_time) / dur;

        (void)aid;
        if (p >= 1.0) {
            finished[finished_count++] = gActiveAnims[i].anim_id;
            continue;
        } else {
            more = true;
        }

        CGXWindow *win = gActiveAnims[i].win;
        CGXConnection *conn = gActiveAnims[i].conn;
        bool is_clone = gActiveAnims[i].is_clone;
        CGRect b = gActiveAnims[i].bounds;
        os_unfair_lock_unlock(&gAnimsLock);

        // Outside the lock on purpose: this re-enters the server.
        if (vn_set_mesh_warp) {
            VNPointWarp mesh[kVNMeshCount];
            vn_anim_shrink(mesh, b, p);
            CGXConnection *warp_conn = is_clone ? NULL : conn;
            vn_set_mesh_warp(win, warp_conn, kVNMeshW, kVNMeshH, (const float *)mesh);

            if (vn_trace_win(win)) {
                VN_LOG("  +%4ldms  warp t=%.3f bounds=(%.0f,%.0f %.0fx%.0f) topleft=(%.0f,%.0f)",
                       vn_trace_ms(), p, b.origin.x, b.origin.y, b.size.width, b.size.height,
                       (double)mesh[0].global.x, (double)mesh[0].global.y);
            }
        }
        os_unfair_lock_lock(&gAnimsLock);
    }
    os_unfair_lock_unlock(&gAnimsLock);

    for (int i = 0; i < finished_count; i++) vn_finish_animation_for_id(finished[i]);

    if (more && vn_schedule_callback) {
        CGXWindow *active_win = NULL;
        os_unfair_lock_lock(&gAnimsLock);
        for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
            if (gActiveAnims[i].is_animating && gActiveAnims[i].win) {
                active_win = gActiveAnims[i].win;
                break;
            }
        }
        os_unfair_lock_unlock(&gAnimsLock);
        vn_schedule_callback(vn_anim_tick, NULL, SLSCurrentRealTime() + vn_get_display_refresh_interval(active_win));
    }
}


static void vn_start_window_animation(CGXConnection *conn, uint32_t wid, CGXWindow *win,
                                      uint32_t orig_wid, CGRect frame, bool probe_only, bool is_clone) {
    float dur = vn_duration();

    if (frame.size.width < 1.0 || frame.size.height < 1.0) {
        VNPreferences prefs = vn_get_prefs();
        CGRect b = vn_clipped_frame_bounds ? vn_clipped_frame_bounds(win) : CGRectZero;
        CGRect p = vn_screen_rect ? vn_screen_rect(win) : CGRectZero;
        if (!prefs.shadows && p.size.width >= 1.0 && p.size.height >= 1.0) {
            frame = p;
        } else if (b.size.width >= 1.0 && b.size.height >= 1.0) {
            frame = b;
        } else if (p.size.width >= 1.0 && p.size.height >= 1.0) {
            frame = p;
        } else if (vn_screen_rect_from_rect) {
            frame = vn_screen_rect_from_rect(win, CGRectMake(0.0, 0.0, 1.0, 1.0));
        }
    }

    // Ensure clone is ordered on top so it animates cleanly above any underlying windows
    if (is_clone && wid != 0) {
        CGSOrderOp op = kVNOrderAbove;
        uint32_t rel = 0;
        vn_orig_order(NULL, &wid, &op, &rel, 1, false);
    }

    os_unfair_lock_lock(&gAnimsLock);
    int slot = -1;
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (!gActiveAnims[i].is_animating) {
            slot = i;
            break;
        }
    }
    if (slot == -1) slot = 0;

    uint64_t anim_id = gNextAnimId++;
    gActiveAnims[slot] = (VNWindowAnim){
        .wid = wid,
        .orig_wid = orig_wid,
        .win = win,
        .conn = conn,
        .anim_id = anim_id,
        .is_animating = true,
        .probe_only = probe_only,
        .is_clone = is_clone,
        .bounds = frame,
        .start_time = SLSCurrentRealTime(),
        .duration = (double)dur,
    };
    os_unfair_lock_unlock(&gAnimsLock);

    VN_LOG("starting fade animation for target wid=%u (orig=%u) win=%p duration=%.2fs (anim_id=%llu)",
           wid, orig_wid, win, dur, anim_id);

    double interval = vn_get_display_refresh_interval(win);

    if (vn_schedule_callback) {
        vn_schedule_callback(vn_anim_tick, NULL, SLSCurrentRealTime() + interval);
    } else {
        vn_finish_animation_for_id(anim_id);
    }
}

static void vn_cancel_window_animation_if_ordering_in(uint32_t wid, CGXWindow *win, CGXConnection *conn) {
    CGXWindow *clone_to_release = NULL;
    uint32_t clone_wid = 0;
    CGXWindow *orig_win = NULL;
    CGXConnection *orig_conn = NULL;
    CGRect orig_bounds = CGRectZero;

    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (gActiveAnims[i].is_animating &&
            ((wid != 0 && (gActiveAnims[i].wid == wid || gActiveAnims[i].orig_wid == wid)) ||
             (win != NULL && gActiveAnims[i].win == win))) {
            
            if (gActiveAnims[i].is_clone) {
                clone_to_release = gActiveAnims[i].win;
                clone_wid = gActiveAnims[i].wid;
            } else {
                orig_win = gActiveAnims[i].win;
                orig_conn = gActiveAnims[i].conn ? gActiveAnims[i].conn : conn;
                orig_bounds = gActiveAnims[i].bounds;
            }

            gActiveAnims[i].is_animating = false;
            gActiveAnims[i].wid = 0;
            gActiveAnims[i].orig_wid = 0;
            gActiveAnims[i].win = NULL;
            gActiveAnims[i].conn = NULL;
            break;
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    if (clone_to_release) {
        VN_LOG("Target window wid=%u ordered back in while clone wid=%u was animating; ordering out and destroying clone",
               wid, clone_wid);
        if (vn_update_ca_visibility) {
            vn_update_ca_visibility(clone_to_release, false);
        }
        if (clone_wid != 0) {
            CGSOrderOp op = kVNOrderOut;
            uint32_t rel = 0;
            vn_orig_order(NULL, &clone_wid, &op, &rel, 1, false);
        }
        if (vn_schedule_callback) {
            vn_schedule_callback(vn_delayed_clone_release, clone_to_release, SLSCurrentRealTime() + 0.1);
        } else if (vn_system_window_release) {
            vn_system_window_release(clone_to_release);
        }
    } else if (orig_win && orig_conn) {
        VN_LOG("Target window wid=%u ordered back in while animating; restored alpha", wid);
        vn_clear_warp(orig_win, orig_conn, orig_bounds);
    }

    CGXWindow *clone = NULL;
    uint32_t c_wid = 0, o_wid = 0;
    os_unfair_lock_lock(&gPreCloneLock);
    if (wid != 0 && gPreClone.orig_wid == wid) {
        clone = vn_preclone_take_locked(&c_wid, &o_wid, NULL);
    }
    os_unfair_lock_unlock(&gPreCloneLock);

    if (clone) {
        VN_LOG("pre-clone: window wid=%u ordered in, discarding unused clone wid=%u", wid, c_wid);
        if (vn_update_ca_visibility) {
            vn_update_ca_visibility(clone, false);
        }
        if (c_wid != 0) {
            CGSOrderOp op = kVNOrderOut;
            uint32_t rel = 0;
            vn_orig_order(NULL, &c_wid, &op, &rel, 1, false);
        }
        if (vn_system_window_release) {
            vn_system_window_release(clone);
        }
    }
}

#pragma mark - Trace

/// Trace mode observes a STOCK close: every hook logs and calls straight
/// through, and no animation is started. The point is to find out what actually
/// makes a window disappear ~200 ms into a close while its alpha is still
/// animating, rather than to guess at it again.
///   0  off
///   1  observe a stock close -- pass through, change nothing
///   2  animate AND trace, which is the run that shows what removes the window
///      while the fade is still running
#define VN_TRACE 0

/// Warp probe.
///
/// Everything about the close path is now verified: the mesh is built
/// correctly, `set_mesh_warp` runs to completion and schedules its own redraw,
/// the window is never ordered out early, never released, and the app stays
/// alive. And still nothing moves on screen.
///
/// So the question is no longer about closing. It is whether a mesh warp draws
/// on an ordinary CoreAnimation-backed window at all. With this set, a target
/// window is warped when it is ordered IN, and closes are left completely alone
/// -- open a window and watch it shrink. If it shrinks, warp works and the
/// close path is hiding the window some other way. If it does not, warp is not
/// the lever for CA-backed windows and the CA layer is.
/// Probe mode is a runtime switch, not a rebuild: `touch /tmp/vanish_probe`
/// warps on window OPEN and leaves closes alone; remove it and closes animate.
/// Having both in one build is what lets the two traces be compared.
static bool vn_probe_mode(void) {
    return access("/tmp/vanish_probe", F_OK) == 0;
}

static uint64_t vn_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)(tv.tv_usec / 1000);
}

/// Several of the traced functions never see a window, so they cannot be
/// filtered by target. Instead the trace is armed when a target window is
/// ordered out and logs everything for a short while after.
static _Atomic(uint64_t) gTraceStartMs = 0;
static _Atomic(uint64_t) gTraceWin = 0;   // the CGXWindow * being closed

/// The closing window's content, learned from the first commit after arming, so
/// its destruction can be called out by name rather than guessed at.
static _Atomic(uint64_t) gTraceContent = 0;

#define kVNTraceWindowMs 4000

static void vn_trace_arm(CGXWindow *win) {
    atomic_store_explicit(&gTraceWin, (uint64_t)(uintptr_t)win, memory_order_relaxed);
    atomic_store_explicit(&gTraceContent, 0, memory_order_relaxed);
    atomic_store_explicit(&gTraceStartMs, vn_now_ms(), memory_order_relaxed);
}

static bool vn_trace_armed(void) {
    uint64_t start = atomic_load_explicit(&gTraceStartMs, memory_order_relaxed);
    return start != 0 && (vn_now_ms() - start) < kVNTraceWindowMs;
}

/// Milliseconds since the close that armed the trace. Only meaningful while
/// armed, which is the only time it is printed.
static long vn_trace_ms(void) {
    uint64_t start = atomic_load_explicit(&gTraceStartMs, memory_order_relaxed);
    if (start == 0) return -1;
    return (long)((int64_t)vn_now_ms() - (int64_t)start);
}

/// Only the window being closed is interesting. The last capture was 95% other
/// windows being updated at display refresh rate, which buried the four lines
/// that mattered.
static bool vn_trace_win(CGXWindow *win) {
    if (!vn_trace_armed()) return false;
    if (win == NULL) return true;   // context-free hooks: log for the whole window
    return (uint64_t)(uintptr_t)win == atomic_load_explicit(&gTraceWin, memory_order_relaxed);
}

static VNUpdateCAVisibilityFn      vn_orig_update_ca_visibility;
static VNFadeBeginFn               vn_orig_fade_begin;
static VNFadeFinishFn              vn_orig_fade_finish;
static VNCommitContextVisibilityFn vn_orig_commit_ctx_visibility;
static VNStartOrderWindowFn        vn_orig_start_order_window;
static VNUpdateAlphasFn            vn_orig_update_alphas;
static VNCAWindowContentDtorFn     vn_orig_ca_content_dtor;

static int vn_t_update_alphas(CGXWindow *win, CGXConnection *conn, bool flag,
                              float a0, float a1, float a2) {
    int mask = vn_orig_update_alphas(win, conn, flag, a0, a1, a2);
    if (vn_trace_win(win)) {
        // The mask must be returned, not swallowed: set_window_alphas compares
        // it against 4 to decide whether to commit the change to the screen.
        VN_LOG("  +%4ldms  update_alphas(win=%p, %.3f/%.3f/%.3f) -> mask=0x%x",
               vn_trace_ms(), win, (double)a0, (double)a1, (double)a2, mask);
    }
    return mask;
}

static void vn_t_ca_content_dtor(void *content) {
    if (vn_trace_armed()) {
        bool mine = ((uint64_t)(uintptr_t)content ==
                     atomic_load_explicit(&gTraceContent, memory_order_relaxed));
        VN_LOG("  +%4ldms  ~CAWindowContent(content=%p)%s",
               vn_trace_ms(), content, mine ? "   <<<< THE CLOSING WINDOW'S CONTENT" : "");
    }
    vn_orig_ca_content_dtor(content);
}

static void vn_t_update_ca_visibility(CGXWindow *win, bool visible) {
    if (vn_trace_win(win)) {
        VN_LOG("  +%4ldms  update_ca_visibility(win=%p, visible=%d) alpha=%.3f",
               vn_trace_ms(), win, (int)visible, vn_window_alpha(win));
    }
    vn_orig_update_ca_visibility(win, visible);
}

static void vn_t_set_window_alphas(CGXConnection *conn, CGXWindow *win, float alpha) {
    if (vn_trace_win(win)) {
        VN_LOG("  +%4ldms  set_window_alphas(win=%p, alpha=%.3f)", vn_trace_ms(), win, (double)alpha);
    }
    vn_orig_set_window_alphas(conn, win, alpha);
}

static void vn_t_fade_begin(CGXWindow *win, void (^done)(CGXWindow *, bool),
                            float alpha, float dur) {
    if (vn_trace_win(win)) {
        VN_LOG("  +%4ldms  fade_begin(win=%p, alpha=%.3f, dur=%.3f, block=%p)",
               vn_trace_ms(), win, (double)alpha, (double)dur, (__bridge void *)done);
    }
    vn_orig_fade_begin(win, done, alpha, dur);
}

static void vn_t_fade_finish(CGXWindow *win, CGXConnection *conn) {
    if (vn_trace_win(win)) {
        VN_LOG("  +%4ldms  fade_finish(win=%p) alpha=%.3f", vn_trace_ms(), win, vn_window_alpha(win));
    }
    vn_orig_fade_finish(win, conn);
}

static void vn_t_commit_ctx_visibility(void *content) {
    if (vn_trace_armed()) {
        uint64_t expected = 0;
        bool first = atomic_compare_exchange_strong_explicit(
            &gTraceContent, &expected, (uint64_t)(uintptr_t)content,
            memory_order_relaxed, memory_order_relaxed);
        VN_LOG("  +%4ldms  commit_context_visibility(content=%p)%s",
               vn_trace_ms(), content, first ? "   <-- assuming this is the closing window's" : "");
    }
    vn_orig_commit_ctx_visibility(content);
}

static void vn_t_start_order_window(CGXConnection *conn, void *state) {
    if (vn_trace_armed()) {
        VN_LOG("  +%4ldms  start_order_window(state=%p)", vn_trace_ms(), state);
    }
    vn_orig_start_order_window(conn, state);
}

#pragma mark - Hooks

static _Atomic(uint64_t) gCmdWRequestTimeMs = 0;
static _Atomic(pid_t)    gCmdWTargetPID = 0;
static _Atomic(uint32_t) gCmdWTargetWID = 0;

static _Atomic(uint64_t) gNonCloseTimeMs = 0;
static _Atomic(uint32_t) gNonCloseWid = 0;

static void vn_hooked_release_window(CGXConnection *conn, CGXWindow *win) {
    uint32_t rel_wid = (win && vn_window_get_id) ? vn_window_get_id(win) : 0;

    // If this window is currently in the active animation table, cancel the animation
    // entry so our timer doesn't touch the window later.
    // NOTE: When a clone is animating (is_clone == true), AppKit releasing the original window
    // (orig_wid) is expected and normal! Do NOT cancel the clone animation. Only cancel if the
    // window being released is the clone itself, OR if this is a live non-clone animation.
    os_unfair_lock_lock(&gAnimsLock);
    for (int i = 0; i < MAX_ACTIVE_ANIMS; i++) {
        if (!gActiveAnims[i].is_animating) continue;

        bool match_self = (gActiveAnims[i].win == win) || (rel_wid != 0 && gActiveAnims[i].wid == rel_wid);
        bool match_orig = (!gActiveAnims[i].is_clone) && (rel_wid != 0 && gActiveAnims[i].orig_wid == rel_wid);

        if (match_self || match_orig) {
            VN_LOG("release_window arrived for animating window wid=%u (orig=%u) win=%p; removing from animation table",
                   gActiveAnims[i].wid, gActiveAnims[i].orig_wid, win);
            gActiveAnims[i].is_animating = false;
            gActiveAnims[i].wid = 0;
            gActiveAnims[i].orig_wid = 0;
            gActiveAnims[i].win = NULL;
            gActiveAnims[i].conn = NULL;
            break;
        } else if (gActiveAnims[i].is_clone && rel_wid != 0 && gActiveAnims[i].orig_wid == rel_wid) {
            VN_LOG("release_window arrived for orig_wid=%u while clone wid=%u is animating (normal AppKit teardown); clone continues",
                   rel_wid, gActiveAnims[i].wid);
            gActiveAnims[i].orig_wid = 0;
            break;
        }
    }
    os_unfair_lock_unlock(&gAnimsLock);

    // A pre-clone held for this window means the user pressed the red button and
    // the window is now being destroyed. That IS the close.
    //
    // AppKit windows are ordered out first and released afterwards, so the
    // order-out hook gets there first and consumes the pre-clone. SwiftUI windows
    // arrive the other way round -- release first, order-out about a millisecond
    // later -- and this hook used to throw the pre-clone away, so by the time the
    // order-out ran there was nothing left to animate. That is the whole reason
    // System Settings, Calculator and TweakInject closed with no animation while
    // TextEdit and VanishTest were fine; the hit-test log shows the red button was
    // recognised and the clone built every time.
    //
    // So the clone is promoted here instead of discarded. The order-out that
    // follows finds the animation already running and drops itself as a duplicate.
    CGXWindow     *clone_to_animate = NULL;
    CGXConnection *clone_conn = NULL;
    uint32_t clone_wid = 0, orig_wid = 0;
    CGRect clone_frame = CGRectZero;

    os_unfair_lock_lock(&gPreCloneLock);
    if (gPreClone.clone == win) {
        // The clone itself is being released -- simply zero out without calling vn_system_window_release
        vn_preclone_take_locked(NULL, NULL, NULL);
    } else if (gPreClone.orig_wid != 0 && vn_window_by_id && vn_window_by_id(gPreClone.orig_wid) == win) {
        clone_conn = gPreClone.conn;
        clone_to_animate = vn_preclone_take_locked(&clone_wid, &orig_wid, &clone_frame);
    }
    os_unfair_lock_unlock(&gPreCloneLock);

    // Fallback: If no pre-clone was held, check whether Cmd+W was pressed recently for this window/process!
    // (Crucial for SwiftUI apps like Calculator, where release_window precedes orderOut upon Cmd+W)
    if (!clone_to_animate && rel_wid != 0 && vn_is_target_window(win)) {
        uint64_t now_ms = vn_now_ms();
        uint64_t cmdw_time = atomic_load_explicit(&gCmdWRequestTimeMs, memory_order_relaxed);
        pid_t cmdw_pid = atomic_load_explicit(&gCmdWTargetPID, memory_order_relaxed);
        uint32_t cmdw_wid = atomic_load_explicit(&gCmdWTargetWID, memory_order_relaxed);

        char app_name[256] = {0};
        pid_t win_pid = 0;
        vn_get_window_app_name(win, app_name, sizeof(app_name), &win_pid);

        bool is_valid_doc = false;
        CGRect probe = vn_screen_rect ? vn_screen_rect(win) : CGRectZero;
        if (probe.size.width < 1.0 || probe.size.height < 1.0) {
            if (vn_clipped_frame_bounds) probe = vn_clipped_frame_bounds(win);
        }
        if (probe.size.width >= 200.0 && probe.size.height >= 150.0 && vn_window_level(win) == 0) {
            is_valid_doc = true;
        }

        if (is_valid_doc && (now_ms - cmdw_time) < 400 && (cmdw_wid == rel_wid || (win_pid > 0 && cmdw_pid == win_pid))) {
            VN_LOG(">>> Cmd+W confirmed at release_window for '%s' wid=%u (pid=%d) -- cloning on the fly",
                   app_name, rel_wid, win_pid);
            atomic_store_explicit(&gCmdWRequestTimeMs, 0, memory_order_relaxed);
            uint32_t c_wid = 0;
            CGRect c_frame = CGRectZero;
            CGXWindow *clone = vn_make_snapshot(win, conn, rel_wid, &c_wid, &c_frame, kVNOrderAbove);
            if (clone && c_wid != 0) {
                clone_to_animate = clone;
                clone_wid = c_wid;
                clone_frame = c_frame;
                clone_conn = conn;
                orig_wid = rel_wid;
            }
        }
    }

    // Call original release_window IMMEDIATELY! Never defer it.
    // Deferring release_window causes deadlocks and use-after-free when a connection deallocates on quit.
    vn_orig_release_window(conn, win);

    // After the release, not before: the original has to be off the screen for the
    // clone sitting underneath it to be the thing the user sees warp.
    if (clone_to_animate && clone_wid != 0) {
        VN_LOG(">>> Close seen at release_window for wid=%u -- animating pre-clone wid=%u",
               orig_wid, clone_wid);
        vn_start_window_animation(clone_conn, clone_wid, clone_to_animate, orig_wid, clone_frame, false, true);
    }
}

static inline bool vn_is_in_red_hitbox(double lx, double ly) {
    // Cluster 1: Compact / Standard titlebars (Antigravity, Urban VPN, TweakInject, Chrome, WhatsApp)
    // Red center ~ (17.0, 14.0), radius ~7.5pt
    bool in_c1 = (lx >= 9.0 && lx <= 26.0 && ly >= 4.5 && ly <= 25.0);

    // Cluster 2: Tall Unified toolbars (System Settings, Calculator, Notes, Messages, Mail)
    // Red center ~ (25.5, 26.0), radius ~7.5pt
    bool in_c2 = (lx >= 18.0 && lx <= 33.5 && ly >= 18.0 && ly <= 33.5);

    return in_c1 || in_c2;
}

static inline bool vn_is_in_yellow_or_green_hitbox(double lx, double ly) {
    // Cluster 1: Compact / Standard titlebars
    if (lx >= 26.5 && lx <= 80.0 && ly >= 4.5 && ly <= 25.0) {
        return true;
    }
    // Cluster 2: Tall Unified toolbars
    if (lx >= 34.0 && lx <= 88.0 && ly >= 18.0 && ly <= 33.5) {
        return true;
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

    // 1. Keyboard shortcut interception (Cmd+W, Cmd+M, Cmd+H, Ctrl+Cmd+F)
    if (type == 10) { // kCGEventKeyDown
        uint32_t flags = *(const uint32_t *)((const char *)event + 0x38);
        uint32_t wid = *(const uint32_t *)((const char *)event + 0x3c);
        uint16_t keycode = *(const uint16_t *)((const char *)event + 0x90);

        if ((flags & 0x00100000) != 0) { // Command key down
            if (keycode == 13) { // 'W' key -> Close Window
                VN_LOG(">>> KeyDown Cmd+W detected for wid=%u pid=%d", wid, pid);
                atomic_store_explicit(&gCmdWRequestTimeMs, vn_now_ms(), memory_order_relaxed);
                atomic_store_explicit(&gCmdWTargetPID, pid, memory_order_relaxed);
                atomic_store_explicit(&gCmdWTargetWID, wid, memory_order_relaxed);
                atomic_store_explicit(&gNonCloseWid, 0, memory_order_relaxed);
            } else if (keycode == 46) { // 'M' key -> Minimize
                VN_LOG(">>> KeyDown Cmd+M detected for wid=%u pid=%d (ignoring future orderOut)", wid, pid);
                atomic_store_explicit(&gNonCloseWid, wid, memory_order_relaxed);
                atomic_store_explicit(&gNonCloseTimeMs, vn_now_ms(), memory_order_relaxed);
            } else if (keycode == 4) { // 'H' key -> Hide App
                VN_LOG(">>> KeyDown Cmd+H detected for wid=%u pid=%d (ignoring future orderOut)", wid, pid);
                atomic_store_explicit(&gNonCloseWid, wid, memory_order_relaxed);
                atomic_store_explicit(&gNonCloseTimeMs, vn_now_ms(), memory_order_relaxed);
            } else if (keycode == 3 && (flags & 0x00040000) != 0) { // Ctrl+Cmd+F -> Fullscreen
                VN_LOG(">>> KeyDown Ctrl+Cmd+F detected for wid=%u pid=%d (ignoring future orderOut)", wid, pid);
                atomic_store_explicit(&gNonCloseWid, wid, memory_order_relaxed);
                atomic_store_explicit(&gNonCloseTimeMs, vn_now_ms(), memory_order_relaxed);
            }
        }
    }
    // 2. Mouse click interception (Traffic lights: Red vs Yellow vs Green)
    else if (type == 1) { // kCGEventLeftMouseDown
        uint32_t wid = *(const uint32_t *)((const char *)event + 0x3c);
        if (wid != 0 && vn_window_by_id) {
            CGXWindow *win = vn_window_by_id(wid);
            if (win && vn_is_target_window(win) && !vn_is_window_animating(wid, win)) {
                const CGPoint *screen_pt = (const CGPoint *)((const char *)event + 0x10);
                const CGPoint *local_pt  = (const CGPoint *)((const char *)event + 0x20);
                double lx = local_pt->x;
                double ly = local_pt->y;

                uint64_t now_ms = vn_now_ms();

                // Double-click check:
                // If a second click occurs within 450ms and 8pt on the same window,
                // it is a double-click gesture (e.g. zooming/maximizing via titlebar double-click).
                // In macOS, closing via the red button is NEVER a double-click.
                // Immediately discard any pre-clone, suppress pre-cloning for 600ms, and skip pre-cloning for this click.
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
                    vn_preclone_discard();
                    gDoubleClickSuppressUntilMs = now_ms + 600;
                    goto dispatch;
                }

                if (suppressed_by_double_click) {
                    VN_LOG("hit-test: click on wid=%u suppressed due to active double-click window", wid);
                    vn_preclone_discard();
                    goto dispatch;
                }

                bool is_red = vn_is_in_red_hitbox(lx, ly);
                bool is_yellow_or_green = vn_is_in_yellow_or_green_hitbox(lx, ly);

                if (lx <= 150.0 && ly <= 60.0) {
                    char hit_app[256] = {0};
                    pid_t hit_pid = 0;
                    vn_get_window_app_name(win, hit_app, sizeof(hit_app), &hit_pid);
                    VN_LOG("hit-test: app='%s' pid=%d wid=%u pt=(%.1f, %.1f) lvl=%d -> %s",
                           hit_app, hit_pid, wid, lx, ly, vn_window_level(win),
                           is_red ? "RED (close)"
                                  : is_yellow_or_green ? "yellow/green (no animation)"
                                                       : "no traffic light -- NO PRE-CLONE");
                }

                if (is_red) {
                    char app[256] = {0};
                    vn_get_window_app_name(win, app, sizeof(app), &pid);
                    VN_LOG(">>> Mouse down in RED CLOSE box of '%s' (wid=%u, pt=(%.1f, %.1f)) -- pre-cloning now!",
                           app, wid, lx, ly);

                    vn_preclone_discard();
                    atomic_store_explicit(&gNonCloseWid, 0, memory_order_relaxed);

                    uint32_t clone_wid = 0;
                    CGRect clone_frame = CGRectZero;
                    CGXWindow *clone = vn_make_snapshot(win, conn, wid, &clone_wid, &clone_frame, kVNOrderBelow);
                    if (clone && clone_wid != 0) {
                        os_unfair_lock_lock(&gPreCloneLock);
                        gPreClone.orig_wid = wid;
                        gPreClone.clone_wid = clone_wid;
                        gPreClone.clone = clone;
                        gPreClone.conn = conn;
                        gPreClone.frame = clone_frame;
                        gPreClone.created_at = SLSCurrentRealTime();
                        gPreClone.mouseDownScreenPt = *screen_pt;
                        gPreClone.mouseDownLocalPt  = *local_pt;
                        gPreClone.mouseUpTime = 0.0;
                        os_unfair_lock_unlock(&gPreCloneLock);

                        VN_LOG("pre-clone: ready! wid=%u clone_wid=%u (ordered below original)", wid, clone_wid);

                        if (vn_schedule_callback) {
                            vn_schedule_callback(vn_preclone_cleanup_timer, NULL, SLSCurrentRealTime() + 30.0);
                        }
                    } else {
                        VN_LOG("pre-clone: failed to create clone for wid=%u", wid);
                    }
                } else if (is_yellow_or_green) {
                    VN_LOG(">>> Mouse down in YELLOW/GREEN button of wid=%u (pt=(%.1f, %.1f)) -- ignoring close animation",
                           wid, lx, ly);
                    vn_preclone_discard();
                    atomic_store_explicit(&gNonCloseWid, wid, memory_order_relaxed);
                    atomic_store_explicit(&gNonCloseTimeMs, vn_now_ms(), memory_order_relaxed);
                } else {
                    // Clicked elsewhere on the window: discard any pending pre-clone
                    vn_preclone_discard();
                }
            }
        }
    }
    // 3. Mouse release interception (kCGEventLeftMouseUp)
    else if (type == 2) { // kCGEventLeftMouseUp
        os_unfair_lock_lock(&gPreCloneLock);
        bool has_preclone = (gPreClone.orig_wid != 0);
        uint32_t orig_wid = gPreClone.orig_wid;
        os_unfair_lock_unlock(&gPreCloneLock);

        if (has_preclone) {
            const CGPoint *local_pt = (const CGPoint *)((const char *)event + 0x20);
            double lx = local_pt->x;
            double ly = local_pt->y;

            // Wide tracking leeway for mouse release (allows standard AppKit release margin)
            bool near_red = (lx >= 6.0 && lx <= 42.0 && ly >= 3.0 && ly <= 40.0);
            if (!near_red) {
                VN_LOG("pre-clone: mouse up clearly outside button area (wid=%u pt=(%.1f, %.1f)) -- canceling pre-clone",
                       orig_wid, lx, ly);
                vn_preclone_discard();
            } else {
                os_unfair_lock_lock(&gPreCloneLock);
                if (gPreClone.orig_wid == orig_wid) {
                    gPreClone.mouseUpTime = SLSCurrentRealTime();
                }
                os_unfair_lock_unlock(&gPreCloneLock);

                if (vn_schedule_callback) {
                    vn_schedule_callback(vn_preclone_cleanup_timer, NULL, SLSCurrentRealTime() + 1.0);
                }
            }
        }
    }
    // 4. Mouse drag interception (kCGEventLeftMouseDragged)
    else if (type == 6) { // kCGEventLeftMouseDragged
        os_unfair_lock_lock(&gPreCloneLock);
        bool has_preclone = (gPreClone.orig_wid != 0);
        CGPoint down_scr = gPreClone.mouseDownScreenPt;
        uint32_t orig_wid = gPreClone.orig_wid;
        os_unfair_lock_unlock(&gPreCloneLock);

        if (has_preclone) {
            const CGPoint *local_pt  = (const CGPoint *)((const char *)event + 0x20);
            const CGPoint *screen_pt = (const CGPoint *)((const char *)event + 0x10);
            double lx = local_pt->x;
            double ly = local_pt->y;

            bool on_red = vn_is_in_red_hitbox(lx, ly);
            if (!on_red) {
                // Dragged off the red button! User either dragged away to cancel the close,
                // or is dragging/resizing the window.
                VN_LOG("pre-clone: dragged off red button pt=(%.1f, %.1f) -- aborting pre-clone for wid=%u",
                       lx, ly, orig_wid);
                vn_preclone_discard();
            } else {
                // Still within the red button hitbox.
                // Guard against someone dragging the whole window by the traffic light area:
                double d_scr = hypot(screen_pt->x - down_scr.x, screen_pt->y - down_scr.y);
                if (d_scr >= 12.0) {
                    VN_LOG("pre-clone: window drag detected (d_scr=%.1f) -- aborting pre-clone for wid=%u",
                           d_scr, orig_wid);
                    vn_preclone_discard();
                }
            }
        }
    }

dispatch:
    if (vn_orig_post_event) {
        vn_orig_post_event(conn, event);
    }
}

static void vn_order_window_list(CGXConnection *conn, const uint32_t *wids,
                                 const CGSOrderOp *ops, const uint32_t *relativeTo,
                                 unsigned count, bool spaceSwitch) {
    if (ops && count >= 1) {
        // Fast path for single-window operations
        if (count == 1) {
            uint32_t wid = wids ? wids[0] : 0;
            CGXWindow *win = vn_window_by_id(wid);
            if (win && vn_is_target_window(win)) {
                if (ops[0] == kVNOrderOut) {
#if VN_TRACE == 1
                    // Stock close: pass through, change nothing.
                    vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
                    return;
#endif
                    bool animating = vn_is_window_animating(wid, win);
                    bool probe = vn_probe_mode();

                    char app[256] = {0};
                    pid_t pid = 0;
                    vn_get_window_app_name(win, app, sizeof(app), &pid);
                    VN_LOG("Target order op: app='%s' pid=%d count=1 wid=%u op=%d lvl=%d",
                           app, pid, wid, ops[0], vn_window_level(win));

                    if (probe) {
                        VN_LOG("probe mode: leaving the close alone for wid=%u", wid);
                        vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
                        return;
                    }
                    if (animating) {
                        VN_LOG("Window wid=%u (%p, app: '%s') already animating; dropping duplicate order-out",
                               wid, win, app);
                        return; // Dropped! Keep animation running undisturbed.
                    }

                    uint64_t now_ms = vn_now_ms();
                    uint64_t non_close_time = atomic_load_explicit(&gNonCloseTimeMs, memory_order_relaxed);
                    uint32_t non_close_wid = atomic_load_explicit(&gNonCloseWid, memory_order_relaxed);
                    if (non_close_wid == wid && (now_ms - non_close_time) < 2000) {
                        VN_LOG("Window wid=%u orderOut is from Miniaturize/Fullscreen (non-close recorded %llu ms ago) -- passing through without animation",
                               wid, (now_ms - non_close_time));
                        vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
                        return;
                    }

                    // 1. Check if we have an active pre-clone created at mouse-down time on the RED button
                    uint32_t clone_wid = 0;
                    CGXWindow *clone = NULL;
                    CGRect clone_frame = CGRectZero;

                    os_unfair_lock_lock(&gPreCloneLock);
                    if (gPreClone.orig_wid == wid && gPreClone.clone != NULL && gPreClone.clone_wid != 0) {
                        double age_ms = (SLSCurrentRealTime() - gPreClone.created_at) * 1000.0;
                        VN_LOG(">>> Using PRE-CLONE wid=%u clone_wid=%u (ready for %.1f ms -- zero gap!)",
                               wid, gPreClone.clone_wid, age_ms);
                        clone = vn_preclone_take_locked(&clone_wid, NULL, &clone_frame);
                    }
                    os_unfair_lock_unlock(&gPreCloneLock);

                    // 2. Fallback: ONLY if Cmd+W was pressed recently for this process/window
                    if (!clone) {
                        uint64_t cmdw_time = atomic_load_explicit(&gCmdWRequestTimeMs, memory_order_relaxed);
                        pid_t cmdw_pid = atomic_load_explicit(&gCmdWTargetPID, memory_order_relaxed);
                        uint32_t cmdw_wid = atomic_load_explicit(&gCmdWTargetWID, memory_order_relaxed);

                        bool is_valid_doc = false;
                        CGRect probe = vn_screen_rect ? vn_screen_rect(win) : CGRectZero;
                        if (probe.size.width < 1.0 || probe.size.height < 1.0) {
                            if (vn_clipped_frame_bounds) probe = vn_clipped_frame_bounds(win);
                        }
                        if (probe.size.width >= 200.0 && probe.size.height >= 150.0 && vn_window_level(win) == 0) {
                            is_valid_doc = true;
                        }

                        if (is_valid_doc && (now_ms - cmdw_time) < 400 && (cmdw_wid == wid || cmdw_pid == pid)) {
                            VN_LOG(">>> Cmd+W close confirmed for wid=%u (pid=%d, time delta %llu ms) -- cloning on the fly",
                                   wid, pid, (now_ms - cmdw_time));
                            atomic_store_explicit(&gCmdWRequestTimeMs, 0, memory_order_relaxed);
                            clone = vn_make_snapshot(win, conn, wid, &clone_wid, &clone_frame, kVNOrderAbove);
                        } else {
                            VN_LOG("Window wid=%u orderOut has no pre-clone and no Cmd+W intent -- not a close (passing through directly)", wid);
                            vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
                            return;
                        }
                    }

                    if (clone && clone_wid != 0) {
#if VN_TRACE
                        vn_trace_arm(win);
                        VN_LOG("=== CLOSE wid=%u (%p, '%s') alpha=%.3f lvl=%d mode=%d ===",
                               wid, win, app, vn_window_alpha(win), vn_window_level(win), VN_TRACE);
#endif
                        VN_LOG(">>> Intercepted close for target window %u (%p, app: '%s')", wid, win, app);

                        // Order out the original window immediately (reveals the pre-clone right behind it)
                        vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);

                        // Start shrink/warp animation on the clone, passing wid as orig_wid to block duplicates!
                        vn_start_window_animation(conn, clone_wid, clone, wid, clone_frame, false, true);
                    } else {
                        VN_LOG("no clone for wid=%u -- closing without an animation", wid);
                        vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
                    }
                    return;
                } else {
                    char app[256] = {0};
                    pid_t pid = 0;
                    vn_get_window_app_name(win, app, sizeof(app), &pid);
                    VN_LOG("Target order op: app='%s' pid=%d count=1 wid=%u op=%d lvl=%d",
                           app, pid, wid, ops[0], vn_window_level(win));

                    if (vn_probe_mode() && ops[0] == kVNOrderAbove &&
                        !vn_is_window_animating(wid, win)) {
                        vn_trace_arm(win);
                        VN_LOG(">>> PROBE: cloning wid=%u (%p, '%s') on order-in; "
                               "no animation, nothing destroyed, close path untouched",
                               wid, win, app);
                        vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);

                        uint32_t clone_wid = 0;
                        CGRect clone_frame = CGRectZero;
                        CGXWindow *clone = vn_make_snapshot(win, conn, wid, &clone_wid, &clone_frame, kVNOrderAbove);
                        VN_LOG(">>> PROBE result: %s",
                               clone ? "clone created -- look for a duplicate window on top"
                                     : "no clone; see the lines above for which step failed");
                        return;
                    }
                    // Window being ordered in: cancel animation if active
                    vn_cancel_window_animation_if_ordering_in(wid, win, conn);
                }
            }

            // Single non-target or non-close op: pass through
            vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
            return;
        }

        // Multi-window operations (count > 1)
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
                vn_get_window_app_name(win, app, sizeof(app), &pid);

                VN_LOG("Target order op: app='%s' pid=%d count=%u i=%u wid=%u op=%d lvl=%d",
                       app, pid, count, i, wid, ops[i], vn_window_level(win));

                if (ops[i] == kVNOrderOut) {
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

                    // Same as single-window path: check pre-clone or Cmd+W
                    uint32_t clone_wid = 0;
                    CGXWindow *clone = NULL;
                    CGRect clone_frame = CGRectZero;

                    os_unfair_lock_lock(&gPreCloneLock);
                    if (gPreClone.orig_wid == wid && gPreClone.clone != NULL && gPreClone.clone_wid != 0) {
                        clone = vn_preclone_take_locked(&clone_wid, NULL, &clone_frame);
                    }
                    os_unfair_lock_unlock(&gPreCloneLock);

                    if (!clone) {
                        uint64_t cmdw_time = atomic_load_explicit(&gCmdWRequestTimeMs, memory_order_relaxed);
                        pid_t cmdw_pid = atomic_load_explicit(&gCmdWTargetPID, memory_order_relaxed);
                        uint32_t cmdw_wid = atomic_load_explicit(&gCmdWTargetWID, memory_order_relaxed);

                        bool is_valid_doc = false;
                        CGRect probe = vn_screen_rect ? vn_screen_rect(win) : CGRectZero;
                        if (probe.size.width < 1.0 || probe.size.height < 1.0) {
                            if (vn_clipped_frame_bounds) probe = vn_clipped_frame_bounds(win);
                        }
                        if (probe.size.width >= 200.0 && probe.size.height >= 150.0 && vn_window_level(win) == 0) {
                            is_valid_doc = true;
                        }

                        if (is_valid_doc && (now_ms - cmdw_time) < 400 && (cmdw_wid == wid || cmdw_pid == pid)) {
                            VN_LOG(">>> Cmd+W multi-close confirmed for wid=%u (pid=%d) -- cloning on the fly", wid, pid);
                            atomic_store_explicit(&gCmdWRequestTimeMs, 0, memory_order_relaxed);
                            clone = vn_make_snapshot(win, conn, wid, &clone_wid, &clone_frame, kVNOrderAbove);
                        } else {
                            VN_LOG("Window wid=%u multi-orderOut has no pre-clone and no Cmd+W intent -- passing through", wid);
                            pass_wids[pass_count] = wid;
                            pass_ops[pass_count] = ops[i];
                            pass_rel[pass_count] = relativeTo ? relativeTo[i] : 0;
                            pass_count++;
                            continue;
                        }
                    }

                    if (clone && clone_wid != 0) {
                        VN_LOG(">>> Intercepted close for target window %u (%p, app: '%s')", wid, win, app);
                        vn_start_window_animation(conn, clone_wid, clone, wid, clone_frame, false, true);
                    }
                    pass_wids[pass_count] = wid;
                    pass_ops[pass_count] = ops[i];
                    pass_rel[pass_count] = relativeTo ? relativeTo[i] : 0;
                    pass_count++;
                    continue;
                } else {
                    vn_cancel_window_animation_if_ordering_in(wid, win, conn);
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

    // Default pass-through for all other windows.
    vn_orig_order(conn, wids, ops, relativeTo, count, spaceSwitch);
}

#pragma mark - Entry

extern void MSHookFunction(void *symbol, void *replace, void **result);

__attribute__((constructor))
static void vanish_init(void) {
    char self[1024] = {0};
    uint32_t len = (uint32_t)sizeof(self);
    extern int _NSGetExecutablePath(char *buf, uint32_t *bufsize);
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

    CFNotificationCenterAddObserver(
        CFNotificationCenterGetDarwinNotifyCenter(),
        NULL,
        vn_prefs_changed_callback,
        CFSTR("com.doraorak.vanish/prefsChanged"),
        NULL,
        CFNotificationSuspensionBehaviorDeliverImmediately);
    VN_LOG("Registered Darwin notification observer for com.doraorak.vanish/prefsChanged");

#if VN_TRACE
    // Best effort: a symbol that has moved should cost that one line of trace,
    // not the whole build.
    struct { const char *name; void *replace; void **orig; } traces[] = {
        { kVNSymUpdateCAVisibility,      (void *)vn_t_update_ca_visibility, (void **)&vn_orig_update_ca_visibility },
        { kVNSymSetWindowAlphas,         (void *)vn_t_set_window_alphas,    (void **)&vn_orig_set_window_alphas },
        { kVNSymFadeBegin,               (void *)vn_t_fade_begin,           (void **)&vn_orig_fade_begin },
        { kVNSymFadeFinish,              (void *)vn_t_fade_finish,          (void **)&vn_orig_fade_finish },
        { kVNSymCommitContextVisibility, (void *)vn_t_commit_ctx_visibility,(void **)&vn_orig_commit_ctx_visibility },
        { kVNSymStartOrderWindow,        (void *)vn_t_start_order_window,   (void **)&vn_orig_start_order_window },
        { kVNSymUpdateAlphas,            (void *)vn_t_update_alphas,        (void **)&vn_orig_update_alphas },
        { kVNSymCAWindowContentDtor,     (void *)vn_t_ca_content_dtor,      (void **)&vn_orig_ca_content_dtor },
    };
    for (size_t i = 0; i < sizeof(traces) / sizeof(traces[0]); i++) {
        void *sym = vn_skylight_symbol(traces[i].name);
        if (!sym) { VN_LOG("trace: UNRESOLVED %s", traces[i].name); continue; }
        MSHookFunction(ptrauth_strip(sym, ptrauth_key_function_pointer),
                       traces[i].replace, traces[i].orig);
        if (!*traces[i].orig) VN_LOG("trace: no trampoline for %s", traces[i].name);
    }
    VN_LOG("Vanish loaded in TRACE mode %d (1=stock close, 2=animate+trace)", VN_TRACE);
#else
    VN_LOG("Vanish loaded successfully! Target window close hook active (duration: %.2fs)", (double)vn_duration());
#endif
}

