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
#include <mach/mach_time.h>
#include <sys/stat.h>
#include <stdatomic.h>

#include "SkyLightServer.h"
#include "WaterSim.h"
#include "TILicense.h"

#pragma mark - Logging

#ifndef ENABLE_LOGS
#define ENABLE_LOGS 1
#endif

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

/// Close animation styles are described by a registry further down; the
/// structs that carry a chosen one are defined before it.
typedef struct VNAnimationStyle VNAnimationStyle;

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
static VNCreateShaderFn             vn_resolved_create_shader;
static VNCreateSpecializedShaderFn  vn_orig_create_specialized_shader;
static VNUberCompositeFn            vn_orig_uber_composite;
static VNShapeWindowWithRectFn      vn_resolved_shape_window_with_rect;
static VNMetalCompositeLayerFn      vn_orig_metal_composite_layer;
static VNSetPipelineStateFn         vn_orig_set_pipeline_state;
static VNRenderEncoderFn            vn_resolved_render_encoder;
static VNStartCompositeFn           vn_orig_start_composite;
static VNEndEncodersFn              vn_resolved_end_encoders;
static VNRenderCommandBufferFn      vn_resolved_render_command_buffer;
static VNCopyPipelineStateFn        vn_orig_copy_pipeline_state;
static VNReevaluateHDRRequestFn     vn_resolved_reevaluate_hdr_request;

/// Non-zero while at least one clone carries a shader animation's tag. The
/// substitution hook tests this first: macOS's own Invert Colours accessibility
/// setting sets the same option bit on every layer, and without this we would
/// replace the shader for the entire screen.
static _Atomic int gShaderFilterCount;
static VNCreateCloneFn              vn_resolved_create_clone;
static VNSystemWindowReleaseFn      vn_resolved_system_window_release;
static VNWindowGetDisplayFn         vn_resolved_window_get_display;
static VNDisplayGetBoundsFn         vn_resolved_display_get_bounds;
static VNScreenRectFromRectFn       vn_resolved_screen_rect_from_rect;
static VNScreenRectFn               vn_resolved_screen_rect;
static VNWindowGetIDFn              vn_resolved_window_get_id;
static VNClippedFrameBoundsFn       vn_resolved_clipped_frame_bounds;
static VNCornerRadiusFn             vn_resolved_corner_radius;
static VNReleaseWindowFn            vn_orig_release_window;
static VNWindowGetOwningPIDFn       vn_resolved_window_get_owning_pid;
static VNGetConnectionAppNameFn     vn_resolved_get_connection_app_name;
static VNPostEventByConnectionFn    vn_orig_post_event;
static VNUpdateCAVisibilityFn       vn_resolved_update_ca_visibility;
static VNUnobscuredContentShapeFn        vn_resolved_unobscured_content_shape;
static VNGetRegionBoundsFn               vn_resolved_get_region_bounds;
static void                            **vn_resolved_session_control_ref;
static VNCopyScreenShapeFn               vn_resolved_copy_screen_frame_shape;
static VNCopyScreenShapeFn               vn_resolved_copy_screen_content_shape;
static int (*vn_region_union)(void *, void *, void **);
static int (*vn_region_diff)(void *, void *, void **);
static VNClearShadowDensityFn            vn_resolved_clear_shadow_density;
static VNWSWindowSetShadowEnableFn        vn_resolved_window_set_shadow_enable;
static VNWSWindowReleaseShadowResourcesFn vn_resolved_window_release_shadow_resources;
static VNSLSSetWindowShadowParametersFn   vn_resolved_set_window_shadow_parameters;
static VNIsProcessEligibleForSetFrontFn    vn_orig_is_process_eligible;

typedef bool (*VNDynWindowIsOrderedInFn)(const CGXWindow *win);
static VNDynWindowIsOrderedInFn     vn_resolved_window_is_ordered_in = NULL;

#pragma mark - Preferences

static const char * const kVNAnimationKeys[] = {
    "shrink", "squish", "fall", "swirl", "flip", "tilt", "slide", "genie",
    "flag", "spin", "roll", "barrel", "clock", "dissolve", "crt", "shatter",
    "burn", "water"
};
#define kVNAnimationKeyCount (sizeof(kVNAnimationKeys) / sizeof(kVNAnimationKeys[0]))

static const float kVNAnimationDefaultDurations[] = {
    0.25f, /* shrink */
    0.25f, /* squish */
    0.25f, /* fall */
    0.30f, /* swirl */
    0.25f, /* flip */
    0.25f, /* tilt */
    0.25f, /* slide */
    0.35f, /* genie */
    0.30f, /* flag */
    0.25f, /* spin */
    0.30f, /* roll */
    0.35f, /* barrel */
    0.35f, /* clock */
    0.30f, /* dissolve */
    0.30f, /* crt */
    0.35f, /* shatter */
    0.35f, /* burn */
    1.60f  /* water */
};
_Static_assert(sizeof(kVNAnimationDefaultDurations) / sizeof(kVNAnimationDefaultDurations[0]) == kVNAnimationKeyCount,
               "kVNAnimationDefaultDurations count mismatch");

typedef struct {
    bool  enabled;
    bool  shadows;
    float waterTint;      // 0..1, water only; see VNSimParams::tint
    uint32_t waterDrains; // water only; see kVNDrain*
    float refreshRate;
    float duration;
    float animDurations[kVNAnimationKeyCount];
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
    gPrefs.waterTint = kVNWaterTintDefault;
    gPrefs.waterDrains = kVNDrainAll;
    gPrefs.refreshRate = 120.0f;
    gPrefs.duration = 0.25f;
    for (size_t i = 0; i < kVNAnimationKeyCount; i++) {
        gPrefs.animDurations[i] = kVNAnimationDefaultDurations[i];
    }
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

                        CFTypeRef tintVal = CFDictionaryGetValue(dict, CFSTR("water_tint"));
                        if (tintVal && CFGetTypeID(tintVal) == CFNumberGetTypeID()) {
                            float tint = 0.0f;
                            if (CFNumberGetValue((CFNumberRef)tintVal, kCFNumberFloatType, &tint) &&
                                tint >= 0.0f && tint <= 1.0f) {
                                gPrefs.waterTint = tint;
                            }
                        }

                        static const struct { CFStringRef key; uint32_t bit; } kDrainKeys[] = {
                            { CFSTR("water_drain_left"),   kVNDrainLeft   },
                            { CFSTR("water_drain_middle"), kVNDrainMiddle },
                            { CFSTR("water_drain_right"),  kVNDrainRight  },
                        };
                        for (size_t di = 0; di < sizeof(kDrainKeys) / sizeof(kDrainKeys[0]); di++) {
                            // Boolean OR number: a switch's value has been
                            // written both ways, and a preference that is
                            // silently the wrong CFType reads as absent.
                            CFTypeRef v = CFDictionaryGetValue(dict, kDrainKeys[di].key);
                            bool on = true, got = false;
                            if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) {
                                on = CFBooleanGetValue((CFBooleanRef)v); got = true;
                            } else if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
                                int n = 0;
                                if (CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n)) { on = (n != 0); got = true; }
                            }
                            if (got) {
                                if (on) gPrefs.waterDrains |= kDrainKeys[di].bit;
                                else    gPrefs.waterDrains &= ~kDrainKeys[di].bit;
                            }
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

                        for (size_t i = 0; i < kVNAnimationKeyCount; i++) {
                            char keyBuf[64];
                            snprintf(keyBuf, sizeof(keyBuf), "duration_%s", kVNAnimationKeys[i]);
                            CFStringRef cfKey = CFStringCreateWithCString(kCFAllocatorDefault, keyBuf, kCFStringEncodingUTF8);
                            if (cfKey) {
                                CFTypeRef aDurVal = CFDictionaryGetValue(dict, cfKey);
                                if (aDurVal) {
                                    float aDur = 0.0f;
                                    if (CFGetTypeID(aDurVal) == CFNumberGetTypeID()) {
                                        CFNumberGetValue((CFNumberRef)aDurVal, kCFNumberFloatType, &aDur);
                                    } else if (CFGetTypeID(aDurVal) == CFStringGetTypeID()) {
                                        aDur = (float)CFStringGetDoubleValue((CFStringRef)aDurVal);
                                    }
                                    if (aDur >= 0.05f && aDur <= 60.0f) {
                                        gPrefs.animDurations[i] = aDur;
                                    }
                                }
                                CFRelease(cfKey);
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

static float vn_duration_for_key(const char *animKey) {
    VNPreferences prefs = vn_get_prefs();
    if (animKey && animKey[0]) {
        for (size_t i = 0; i < kVNAnimationKeyCount; i++) {
            if (strcasecmp(kVNAnimationKeys[i], animKey) == 0) {
                if (prefs.animDurations[i] >= 0.05f) {
                    return prefs.animDurations[i];
                }
                return kVNAnimationDefaultDurations[i];
            }
        }
    }
    return prefs.duration >= 0.05f ? prefs.duration : 0.25f;
}

static float vn_duration(void) {
    VNPreferences prefs = vn_get_prefs();
    return vn_duration_for_key(prefs.animation);
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

// A clone of a closing window, and the window and app it stands in for. Built
// on mouse-up and parked in gPendingClones, then moved whole into a
// VNCloneAnimation when the app's close arrives.
typedef struct {
    uint32_t       orig_wid;
    uint32_t       clone_wid;
    CGXWindow     *clone_win;
    CGRect         frame;
    pid_t          pid;
    uint64_t       psn;
    bool           is_whatsapp;
    char           app[64];
    // Resolved once, when the clone is built, and carried to the running
    // animation: no preference read or string compare per frame, and the
    // choice cannot change halfway through a close.
    const VNAnimationStyle *anim;
    // The per-close random value in a shader clone's params[0]. The draw hook
    // finds a layer's animation by it (see vn_clone_publish).
    float          seed;
    // When the clone was built, which is the mouse-up. A pending clone whose
    // close has not arrived a second later is discarded.
    double         created_at;
} VNClone;

// A clone on screen, animating. A slot is in use while is_animating is set.
typedef struct {
    uint64_t       anim_id;
    VNClone        clone;
    double         start_time;
    double         duration;
    bool           is_animating;
} VNCloneAnimation;

#define MAX_CLONE_ANIMATIONS 32
static VNCloneAnimation    gCloneAnimations[MAX_CLONE_ANIMATIONS];
static os_unfair_lock gCloneAnimationsLock = OS_UNFAIR_LOCK_INIT;

/// What the draw hook needs to know about each shader clone, readable from the
/// render thread without gCloneAnimationsLock: the hook runs inside the
/// compositor, and nothing that holds that lock may wait on the compositor.
///
/// Where the window sits inside the clone's quad, from the moment the clone is
/// built -- its very first frame is drawn with it -- and, once the animation
/// starts, when that was and how long it runs. Each composited frame takes its
/// phase from the clock at the moment it is drawn, rather than whatever the last
/// tick left on the window: the tick is a timer not locked to the display's
/// refresh, so a phase it sets can be drawn twice while the next is skipped.
///
/// A ring keyed by the clone's per-close seed. Entries are never cleared: a
/// seed is only looked up while its clone is being drawn, and a new close
/// brings a new seed.
typedef struct {
    _Atomic uint64_t key;       // (1 << 32) | seed bits; 0 while being written
    _Atomic double   start;     // INFINITY until the animation starts
    double           duration;
    float            offset_x;
    float            offset_y;
} VNCloneEntry;

static VNCloneEntry     gVNCloneEntries[MAX_CLONE_ANIMATIONS];
static _Atomic uint32_t gVNCloneEntryNext;

static uint64_t vn_clone_key(float seed) {
    uint32_t bits;
    memcpy(&bits, &seed, sizeof(bits));
    return (1ull << 32) | bits;
}

static VNCloneEntry *vn_clone_entry(float seed) {
    const uint64_t want = vn_clone_key(seed);
    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (atomic_load_explicit(&gVNCloneEntries[i].key, memory_order_acquire) == want) return &gVNCloneEntries[i];
    }
    return NULL;
}

/// Called when a shader clone is built.
static void vn_clone_publish(float seed, CGPoint offset) {
    VNCloneEntry *e = &gVNCloneEntries[atomic_fetch_add_explicit(&gVNCloneEntryNext, 1, memory_order_relaxed)
                                       % MAX_CLONE_ANIMATIONS];
    atomic_store_explicit(&e->key, 0, memory_order_relaxed);
    atomic_store_explicit(&e->start, INFINITY, memory_order_relaxed);
    e->duration = 0.25;
    e->offset_x = (float)offset.x;
    e->offset_y = (float)offset.y;
    atomic_store_explicit(&e->key, vn_clone_key(seed), memory_order_release);
}

/// Called when its animation starts, with the start time the tick also uses.
static void vn_clone_start(float seed, double start, double duration) {
    VNCloneEntry *e = vn_clone_entry(seed);
    if (!e) return;
    e->duration = duration > 0.0 ? duration : 0.25;
    atomic_store_explicit(&e->start, start, memory_order_release);
}

/// The draw-time state of the clone carrying `seed`: its phase at `now`, 0..1,
/// or -1 before it starts (the shader then uses the tick's), and the window's
/// offset in the quad, or -1s when the clone is unknown.
static void vn_clone_lookup(float seed, double now, float *phase, float *offset_x, float *offset_y) {
    *phase = -1.0f;
    *offset_x = *offset_y = -1.0f;
    VNCloneEntry *e = vn_clone_entry(seed);
    if (!e) return;
    *offset_x = e->offset_x;
    *offset_y = e->offset_y;
    const double start = atomic_load_explicit(&e->start, memory_order_acquire);
    if (!(start < INFINITY)) return;
    const double p = (now - start) / e->duration;
    *phase = (float)(p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p));
}

#pragma mark - Water simulation state

/// One simulation per water clone, each in a slot of its own: its own particles
/// and its own surface field on the GPU, and the close it belongs to here. Only
/// scratch -- the bin table, the neighbour list, the blur's intermediate -- is
/// shared, and the compute pass uses it one simulation after another.
///
/// Slots rather than one per clone without limit, because the GPU side is
/// allocated once and never released: a buffer can still be referenced by a
/// command buffer we handed it to and no longer track. A new water close fades
/// the ones still running out, so two are ever on screen for any length of time;
/// the third covers closes clicked faster than a fade takes. Past that the
/// oldest is taken over outright and its clone draws nothing more.
///
/// Written from the close path, read on the compositor's thread. Each slot is
/// published with a sequence counter rather than a lock, for the same reason
/// the draw hook uses a ring: the compositor's thread must never wait on the
/// close path. A torn read costs one frame of that simulation, so the reader
/// gives up and leaves the frame alone rather than retrying.
#define kVNWaterSlots 3

/// How long water takes to fade out: at the end of its duration, and when a
/// newer water close replaces it.
#define kVNWaterFadeSeconds 0.4

typedef struct {
    _Atomic uint32_t seq;         // odd while being written
    uint32_t         generation;  // unique per close; 0 = the slot is free
    CGXWindow       *clone_win;
    CGRect           window_pt;   // the closing window, in the display's points
    CGRect           display_pt;  // that display's bounds, in points
    // The other windows on screen, for the fluid to land on. Sampled once, when
    // the clone is built: a window that moves during the close keeps the shape
    // the fluid was told about, which is cheaper than re-reading the window
    // list on the compositor's thread every frame and wrong by less than a
    // window moves in a second.
    CGRect           obstacles[kVNMaxObstacles];
    uint32_t         obstacle_count;
    float            tint;        // read from prefs here, never on the render thread
    uint32_t         drains;      // likewise; see kVNDrain*
    /// The owning clone's per-close seed -- its filter params[0], the same value
    /// the draw path reads off each layer. It is how a layer finds its slot.
    float            seed;
    double           start;       // when the animation starts; INFINITY until then
    /// When the water has to be gone: the end of the close's duration, brought
    /// forward when a newer close replaces it or when all of it has drained.
    /// Atomic on its own, outside the sequence, because the compositor's thread
    /// brings it forward too.
    _Atomic double   end;
} VNWaterSlot;

static VNWaterSlot gVNWaterSlots[kVNWaterSlots];
static uint32_t    gVNWaterGeneration;   // close path only

/// How many slots hold a close. The compute hook tests this first and does
/// nothing else when it is zero, so every frame of every other close -- and
/// every frame of ordinary compositing -- pays one acquire load.
static _Atomic int gVNWaterClones;

typedef struct {
    bool     valid;
    uint32_t generation;
    CGRect   window_pt;
    CGRect   display_pt;
    CGRect   obstacles[kVNMaxObstacles];
    uint32_t obstacle_count;
    float    tint;
    uint32_t drains;
    float    seed;
    double   start;
} VNWaterSnapshot;

/// The windows the fluid should flow around. Defined with the window tracking
/// table it reads, further down.
static uint32_t vn_water_collect_obstacles(uint32_t closing_wid, CGRect closing_pt,
                                           CGRect display_pt, CGRect out[kVNMaxObstacles]);

static void vn_water_write_begin(VNWaterSlot *s) {
    const uint32_t seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

static void vn_water_write_end(VNWaterSlot *s) {
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&s->seq, atomic_load_explicit(&s->seq, memory_order_relaxed) + 1, memory_order_relaxed);
}

static void vn_water_count_clones(void) {
    int n = 0;
    for (int i = 0; i < kVNWaterSlots; i++) n += gVNWaterSlots[i].generation != 0;
    atomic_store_explicit(&gVNWaterClones, n, memory_order_release);
}

/// Brings a close's end forward to `when`, never back.
static void vn_water_end_by(VNWaterSlot *s, double when) {
    double cur = atomic_load_explicit(&s->end, memory_order_relaxed);
    while (when < cur && !atomic_compare_exchange_weak_explicit(&s->end, &cur, when,
                                                                memory_order_acq_rel, memory_order_relaxed)) {}
}

static VNWaterSlot *vn_water_slot_for_clone(CGXWindow *clone) {
    if (!clone) return NULL;
    for (int i = 0; i < kVNWaterSlots; i++) {
        if (gVNWaterSlots[i].generation != 0 && gVNWaterSlots[i].clone_win == clone) return &gVNWaterSlots[i];
    }
    return NULL;
}

/// `obstacles` is collected by the caller, BEFORE the clone exists. It has to
/// be: the clone is ordered above everything and, for water, shaped to the
/// whole display, so asking which windows are unobscured once it is on screen
/// answers "none of them" -- our own clone covers the lot.
static void vn_water_publish(CGXWindow *clone, CGRect window_pt, CGRect display_pt,
                             const CGRect *obstacles, uint32_t obstacle_count, float tint, float seed,
                             uint32_t drains) {
    // A free slot, or else the oldest close's -- which by now has been told to
    // fade by every close since, and whose clone draws nothing once its slot
    // belongs to someone else.
    VNWaterSlot *s = NULL;
    for (int i = 0; i < kVNWaterSlots && !s; i++) {
        if (gVNWaterSlots[i].generation == 0) s = &gVNWaterSlots[i];
    }
    if (!s) {
        s = &gVNWaterSlots[0];
        for (int i = 1; i < kVNWaterSlots; i++) {
            if (gVNWaterSlots[i].generation < s->generation) s = &gVNWaterSlots[i];
        }
        VN_INFO("water: every slot busy -- clone %p's water is taken over by clone %p", s->clone_win, clone);
    }

    vn_water_write_begin(s);
    if (++gVNWaterGeneration == 0) gVNWaterGeneration = 1;
    s->generation = gVNWaterGeneration;
    s->clone_win  = clone;
    s->window_pt  = window_pt;
    s->display_pt = display_pt;
    s->start      = INFINITY;
    atomic_store_explicit(&s->end, INFINITY, memory_order_relaxed);
    s->tint       = tint;
    s->drains     = drains;
    s->seed       = seed;
    s->obstacle_count = obstacle_count <= kVNMaxObstacles ? obstacle_count : kVNMaxObstacles;
    if (s->obstacle_count > 0 && obstacles) {
        memcpy(s->obstacles, obstacles, s->obstacle_count * sizeof(CGRect));
    }
    vn_water_write_end(s);
    vn_water_count_clones();

    VN_INFO("water: clone %p generation %u in slot %ld, %u obstacle(s), drains=%s%s%s, "
            "window=(%.0f,%.0f %.0fx%.0f) display=(%.0f,%.0f %.0fx%.0f)",
            clone, s->generation, (long)(s - gVNWaterSlots), s->obstacle_count,
            (drains & kVNDrainLeft) ? "L" : "-", (drains & kVNDrainMiddle) ? "M" : "-",
            (drains & kVNDrainRight) ? "R" : "-",
            window_pt.origin.x, window_pt.origin.y, window_pt.size.width, window_pt.size.height,
            display_pt.origin.x, display_pt.origin.y, display_pt.size.width, display_pt.size.height);
}

/// The close's animation has started. Every other water close still running
/// fades out from here: one body of water at a time, rather than two fluids
/// that cannot touch each other passing through one another.
static void vn_water_start(CGXWindow *clone, double start, double duration) {
    VNWaterSlot *s = vn_water_slot_for_clone(clone);
    if (!s) return;
    vn_water_write_begin(s);
    s->start = start;
    vn_water_write_end(s);
    atomic_store_explicit(&s->end, start + (duration > 0.0 ? duration : 1.0), memory_order_release);

    for (int i = 0; i < kVNWaterSlots; i++) {
        VNWaterSlot *o = &gVNWaterSlots[i];
        if (o == s || o->generation == 0) continue;
        vn_water_end_by(o, start + kVNWaterFadeSeconds);
    }
}

/// When a water clone's water has to be gone. The animation tick finishes a
/// water close here rather than at the end of its duration. A water clone
/// with no slot has had its water taken over by a newer close, and is done.
static double vn_water_end_for(CGXWindow *clone) {
    VNWaterSlot *s = vn_water_slot_for_clone(clone);
    return s ? atomic_load_explicit(&s->end, memory_order_acquire) : 0.0;
}

static void vn_particles_report(void);

/// Called from vn_release_clone, on whichever path releases the clone.
static void vn_water_retire(CGXWindow *clone) {
    VNWaterSlot *s = vn_water_slot_for_clone(clone);
    if (!s) return;
    vn_water_write_begin(s);
    s->generation = 0;
    s->clone_win  = NULL;
    vn_water_write_end(s);
    vn_water_count_clones();
    vn_particles_report();
}

/// A slot as the compositor's thread sees it. `valid` is false if the close
/// path was midway through publishing, or the slot is free.
static VNWaterSnapshot vn_water_snapshot(const VNWaterSlot *s) {
    VNWaterSnapshot out = {0};
    const uint32_t s1 = atomic_load_explicit(&s->seq, memory_order_relaxed);
    if (s1 & 1u) return out;
    atomic_thread_fence(memory_order_acquire);

    out.generation = s->generation;
    out.window_pt  = s->window_pt;
    out.display_pt = s->display_pt;
    out.start      = s->start;
    out.tint       = s->tint;
    out.drains     = s->drains;
    out.seed       = s->seed;
    out.obstacle_count = s->obstacle_count <= kVNMaxObstacles ? s->obstacle_count : 0;
    memcpy(out.obstacles, s->obstacles, sizeof(out.obstacles));

    atomic_thread_fence(memory_order_acquire);
    out.valid = out.generation != 0 && (atomic_load_explicit(&s->seq, memory_order_relaxed) == s1);
    return out;
}

static uint64_t       gNextAnimId = 1;
static _Atomic bool   gAnimTimerRunning = false;

static _Atomic(uint64_t) gLastWhatsAppClosedPSN = 0;
static _Atomic(uint64_t) gLastWhatsAppClosedTimeMs = 0;

static bool vn_is_whatsapp_closing_or_recently_closed(uint64_t psn) {
    if (psn == 0) return false;

    os_unfair_lock_lock(&gCloneAnimationsLock);
    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (gCloneAnimations[i].is_animating && gCloneAnimations[i].clone.is_whatsapp && gCloneAnimations[i].clone.psn == psn) {
            os_unfair_lock_unlock(&gCloneAnimationsLock);
            return true;
        }
    }
    os_unfair_lock_unlock(&gCloneAnimationsLock);

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

// One running animation's drawing work for one frame. vn_anim_tick copies
// these out under gCloneAnimationsLock and draws after releasing it, so the
// compositor calls never run while the close hooks wait on the lock.
typedef struct {
    CGXWindow *clone_win;
    CGRect     bounds;
    double     p;
    const VNAnimationStyle *anim;
} VNFrameJob;

static bool vn_is_window_animating(uint32_t wid, CGXWindow *win) {
    os_unfair_lock_lock(&gCloneAnimationsLock);
    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (gCloneAnimations[i].is_animating) {
            if ((wid != 0 && (gCloneAnimations[i].clone.clone_wid == wid || gCloneAnimations[i].clone.orig_wid == wid)) ||
                (win != NULL && gCloneAnimations[i].clone.clone_win == win)) {
                os_unfair_lock_unlock(&gCloneAnimationsLock);
                return true;
            }
        }
    }
    os_unfair_lock_unlock(&gCloneAnimationsLock);
    return false;
}

static void vn_filter_detach(CGXWindow *clone);

#pragma mark - EDR headroom

/// Where CGXWindow keeps its desired EDR headroom, decoded from the first
/// `ldr s<n>, [x0, #imm]` in reevaluate_hdr_request. -1 until found.
static intptr_t gVNHeadroomOffset = -1;

/// The headroom a Burn clone asks for. The display caps it at what the panel can
/// do (16 on this generation of built-in XDR panels), so asking for the most
/// costs nothing where there is less.
#define kVNBurnHeadroom 16.0f

/// How long the display takes to reach that headroom while a Burn clone exists,
/// in seconds. The system's own ramp is 2.0 s, which leaves the first close after
/// a quiet spell burning in SDR for most of its run.
#define kVNBurnHeadroomRamp 0.1f

/// SkyLight's ramp-duration override (see kVNSymForceEDRRampDuration). Held on
/// while any Burn clone exists, and the values found there put back when the
/// last one goes, so every other EDR request ramps as the system intends.
static uint8_t *gVNForceRamp;
static float   *gVNForcedRamp;
static _Atomic int gVNHDRClones;
static uint8_t gVNSavedForceRamp;
static float   gVNSavedForcedRamp;

static void vn_edr_ramp_override(bool on) {
    if (!gVNForceRamp || !gVNForcedRamp) return;
    if (on) {
        gVNSavedForceRamp  = *gVNForceRamp;
        gVNSavedForcedRamp = *gVNForcedRamp;
        *gVNForcedRamp = kVNBurnHeadroomRamp;
        *gVNForceRamp  = 1;
    } else {
        *gVNForceRamp  = gVNSavedForceRamp;
        *gVNForcedRamp = gVNSavedForcedRamp;
    }
    VN_INFO("edr: ramp override %s (force=%u duration=%.2fs)", on ? "on" : "restored",
            (unsigned)*gVNForceRamp, (double)*gVNForcedRamp);
}

static void vn_headroom_resolve(void) {
    if (!vn_resolved_reevaluate_hdr_request) return;
    const uint32_t *ins = ptrauth_strip((const void *)vn_resolved_reevaluate_hdr_request,
                                        ptrauth_key_function_pointer);
    for (int i = 0; i < 32; i++) {
        // LDR (immediate, SIMD&FP, 32-bit, unsigned offset) with base x0.
        if ((ins[i] & 0xFFC003E0u) == 0xBD400000u) {
            intptr_t off = (intptr_t)((ins[i] >> 10) & 0xFFF) * 4;
            if (off > 0 && off < 0x1000) gVNHeadroomOffset = off;
            break;
        }
    }
    VN_INFO("edr: reevaluate_hdr_request=%p headroom offset=0x%lx",
            (void *)vn_resolved_reevaluate_hdr_request, (long)gVNHeadroomOffset);
}

static float vn_window_headroom(CGXWindow *win) {
    if (!win || gVNHeadroomOffset < 0) return 0.0f;
    return *(float *)((char *)win + gVNHeadroomOffset);
}

/// Sets the headroom a clone asks the display for and has the server act on it.
/// 1.0 withdraws the request.
static void vn_window_request_headroom(CGXWindow *win, float headroom) {
    if (!win || gVNHeadroomOffset < 0 || !vn_resolved_reevaluate_hdr_request) return;
    float before = vn_window_headroom(win);
    *(float *)((char *)win + gVNHeadroomOffset) = headroom;
    vn_resolved_reevaluate_hdr_request(win);
    VN_INFO("edr: clone %p headroom %.2f -> %.2f", win, (double)before, (double)headroom);
    (void)before;
}

/// Every path that releases a clone goes through here, so neither a filter nor
/// an EDR headroom request can outlive the window it belongs to. There are four
/// of them -- the animation finishing, a pre-clone discarded, the pre-clone
/// cleanup timer, and the release-window hook -- and a cleanup missing from any
/// one leaks the object or leaves the display bright.
static void vn_release_clone(CGXWindow *clone) {
    if (!clone) return;
    if (vn_window_headroom(clone) > 1.0f) {
        // The override goes first, so the display eases back down at its own pace.
        if (atomic_fetch_sub_explicit(&gVNHDRClones, 1, memory_order_acq_rel) == 1) vn_edr_ramp_override(false);
        vn_window_request_headroom(clone, 1.0f);
    }
    vn_water_retire(clone);
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
    VNClone finished = {0};
    bool found = false;

    os_unfair_lock_lock(&gCloneAnimationsLock);
    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (gCloneAnimations[i].is_animating && gCloneAnimations[i].anim_id == anim_id) {
            finished = gCloneAnimations[i].clone;
            gCloneAnimations[i].is_animating = false;
            gCloneAnimations[i].clone = (VNClone){0};
            found = true;
            break;
        }
    }
    os_unfair_lock_unlock(&gCloneAnimationsLock);

    if (!found) return;

    uint32_t clone_wid = finished.clone_wid;
    CGXWindow *clone_win = finished.clone_win;

    if (finished.is_whatsapp && finished.psn != 0) {
        atomic_store_explicit(&gLastWhatsAppClosedPSN, finished.psn, memory_order_relaxed);
        atomic_store_explicit(&gLastWhatsAppClosedTimeMs, vn_now_ms(), memory_order_relaxed);
    }

    VN_DEBUG("finish_animation: hiding and ordering out clone wid=%u win=%p (app='%s' pid=%d psn=0x%llx is_wa=%d release deferred 100ms)",
           clone_wid, clone_win, finished.app, finished.pid, finished.psn, finished.is_whatsapp);
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

#define MAX_PENDING_CLONES 32

// Clones built on mouse-up, waiting for the app's real close. A slot is in
// use while orig_wid is non-zero.
static VNClone        gPendingClones[MAX_PENDING_CLONES] = {0};
static os_unfair_lock gPendingClonesLock = OS_UNFAIR_LOCK_INIT;

static int vn_pending_clone_find_slot_locked(uint32_t orig_wid) {
    if (orig_wid == 0) return -1;
    for (int i = 0; i < MAX_PENDING_CLONES; i++) {
        if (gPendingClones[i].orig_wid == orig_wid) return i;
    }
    return -1;
}

static int vn_pending_clone_find_empty_slot_locked(void) {
    for (int i = 0; i < MAX_PENDING_CLONES; i++) {
        if (gPendingClones[i].orig_wid == 0) return i;
    }
    return 0;
}

// Moves the pending clone for orig_wid out of its slot, which is freed.
static bool vn_pending_clone_take_locked(uint32_t orig_wid, VNClone *out) {
    int idx = vn_pending_clone_find_slot_locked(orig_wid);
    if (idx < 0) return false;

    *out = gPendingClones[idx];
    memset(&gPendingClones[idx], 0, sizeof(VNClone));
    return true;
}

static void vn_pending_clone_discard_wid(uint32_t orig_wid) {
    if (orig_wid == 0) return;
    VNClone taken = {0};
    os_unfair_lock_lock(&gPendingClonesLock);
    vn_pending_clone_take_locked(orig_wid, &taken);
    os_unfair_lock_unlock(&gPendingClonesLock);

    CGXWindow *clone = taken.clone_win;
    uint32_t clone_wid = taken.clone_wid;
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

static void vn_pending_clone_cleanup_timer(void *ctx, double when) {
    (void)ctx; (void)when;
    double now = SLSCurrentRealTime();
    CGXWindow *clones_to_free[MAX_PENDING_CLONES];
    uint32_t   wids_to_free[MAX_PENDING_CLONES];
    int        free_count = 0;

    os_unfair_lock_lock(&gPendingClonesLock);
    for (int i = 0; i < MAX_PENDING_CLONES; i++) {
        if (gPendingClones[i].orig_wid == 0) continue;

        if ((now - gPendingClones[i].created_at) >= 1.0) {
            VN_DEBUG("pre-clone: slot %d (wid=%u, age=%.2fs) timed out without close -- cleaning up",
                   i, gPendingClones[i].orig_wid, now - gPendingClones[i].created_at);
            clones_to_free[free_count] = gPendingClones[i].clone_win;
            wids_to_free[free_count]   = gPendingClones[i].clone_wid;
            free_count++;
            memset(&gPendingClones[i], 0, sizeof(VNClone));
        }
    }
    os_unfair_lock_unlock(&gPendingClonesLock);

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
//   lives). vn_make_clone primes the warp path with a t = 0 fill so the
//   first animated frame does not also have to realize the clone's surface,
//   and that priming is only invisible if t = 0 changes nothing.
//
//   Pick the smallest mesh that is exact. Points between mesh vertices are
//   interpolated, which reproduces an affine map (scale, rotation, shear)
//   exactly from its corners alone -- so a uniform shrink needs 2x2 and
//   gains nothing from more. Only genuinely non-affine motion, where
//   different parts of the window move on different curves, needs a denser
//   grid, and every extra vertex is per-frame work for the compositor.

// How far past the window a shader animation may draw, as a fraction of the
// window's size on each side.
//
// MUST match kVNShaderMargin in shaders/Vanish.metal. The shader subtracts it
// to put the window back at [0,1] after the widened shape moved the origin up
// and left; disagree here and every shader animation samples off by the
// difference.
//
// Each shader animation declares its own margin (VNAnimationStyle::shader.
// margin), and Shatter additionally spans the display's full height so its
// pieces can fall all the way off the screen. Where the window sits inside the
// quad reaches the shader per close (VNShaderExtra::offset).
//
// Mind the floor: a small margin exposes a one-or-two-frame artifact at the
// start of the close that has resisted five attempts to fix (a displaced copy
// of the window peeking out from behind the original). At half the window's
// size on each side the displaced frame lands entirely behind the original and
// is never seen. That is a workaround, not a fix: the mismatch is still there,
// it is merely covered -- so shrink an animation's margin only after watching
// its first frames.

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
    VN_ANIM_SHADER = 1,
} VNAnimKind;

struct VNAnimationStyle {
    const char *key;      // stored in prefs -- stable, do not rename
    const char *title;    // shown in the preferences pane
    VNAnimKind  kind;
    union {
        struct {
            unsigned w, h;
            void (*fill)(VNPointWarp *mesh, CGRect bounds, double t);
        } mesh;
        struct {
            uint32_t    type;   // kVNFilterType*, the tag that routes the clone
            const char *frag;   // fragment function in Vanish.metallib
            bool        hdr;    // asks the display for EDR headroom while it runs
            float       margin; // quad widened by this fraction of the frame on each side
            bool        full_height; // quad spans the display's full height as well
            bool        full_screen; // quad IS the display: the animation may draw anywhere
            bool        particles;   // stepped by the compute pass before each frame is drawn
        } shader;
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

// The first entry is the fallback for a missing or unrecognised preference.
static const VNAnimationStyle gAnimationStyles[] = {
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
    { "dissolve",  "Dissolve",  VN_ANIM_SHADER, .shader = { kVNFilterTypeShaderTag, "vn_uber_dissolve", false, 0.50f, false, false, false } },
    { "crt",       "CRT Off",   VN_ANIM_SHADER, .shader = { kVNFilterTypeShaderTag, "vn_uber_crt", false, 0.50f, false, false, false } },
    { "shatter",   "Shatter",   VN_ANIM_SHADER, .shader = { kVNFilterTypeShaderTag, "vn_uber_shatter", false, 0.50f, true, false, false } },
    { "burn",      "Burn",      VN_ANIM_SHADER, .shader = { kVNFilterTypeShaderTag, "vn_uber_burn", true, 0.50f, false, false, false } },
    { "water",     "Water",     VN_ANIM_SHADER, .shader = { kVNFilterTypeShaderTag, "vn_uber_water", false, 0.50f, false, true, true } },
};
#define kVNAnimationStyleCount (sizeof(gAnimationStyles) / sizeof(gAnimationStyles[0]))

static const VNAnimationStyle *vn_animation_style_for_key(const char *key) {
    if (key && key[0]) {
        for (size_t i = 0; i < kVNAnimationStyleCount; i++) {
            if (strcasecmp(gAnimationStyles[i].key, key) == 0) return &gAnimationStyles[i];
        }
    }
    return &gAnimationStyles[0];
}

static float vn_duration_for_anim(const VNAnimationStyle *anim) {
    return vn_duration_for_key(anim ? anim->key : NULL);
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

/// `params` reach our fragment functions as VNShaderExtra::params (see
/// kVNLayerFilterParamsOffset); NULL leaves them zero.
static bool vn_filter_attach(CGXWindow *clone, uint32_t type, bool is_shader, const float params[5]) {
    if (!clone || !vn_resolved_window_set_filter) return false;

    VNWindowFilter *f = calloc(1, sizeof(VNWindowFilter));
    if (!f) return false;

    f->refcount  = 1;     // so a server-side release frees it exactly once
    f->type      = type;
    f->filter_id = is_shader ? kVNFilterMarkerShader : 0;
    if (params) memcpy(f->params, params, sizeof(f->params));

    if (is_shader) atomic_fetch_add_explicit(&gShaderFilterCount, 1, memory_order_acq_rel);

    vn_resolved_window_set_filter(clone, f);
    return true;
}

// Widening where a shader animation may draw: an open problem.
//
// A fragment shader can only write inside the layer's draw shape, and
// generate_layers_for_window builds that from the WINDOW's own region -- so
// flecks that drift past the window's edge are cut off. Two things have been
// tried:
//
//   Inflating the clone's frame in CreateCloneOfWindow. No effect on the
//   bound (the region does not follow the frame) and it misplaced the content.
//
//   A mesh warp mapping local -m..size+m onto a correspondingly larger screen
//   rect. The destination shape IS built by running the region through the
//   mesh -- that is how a warped clone draws outside its rect today -- but
//   setting any mesh stopped the substituted shader from running: the close
//   rendered through the stock colour-invert instead. generate_layers_for_window
//   fills a layer down more than one branch, and the one a warped window takes
//   appears not to carry the filter tag.
//
// Next test, cheapest first: an IDENTITY mesh (local 0..size onto the frame
// unchanged). If that also breaks the shader, mesh and shader cannot coexist
// and the room must come from somewhere else. If it survives, the mesh is fine
// and the fault was in the margin mapping.

/// Widens where a shader animation may draw, by giving the clone a shape larger
/// than the window it copied. The draw shape the compositor clips us to is
/// built from this region, so growing it is what buys room for flecks that
/// drift past the window's edge.
///
/// The rect sizes and positions the clone, so its texture ends up stretched
/// across a quad larger than the window. The shader puts the content back at
/// 1:1 in the middle -- see vn_window_uv there, which has to agree with
/// kVNShaderMargin.
static CGPoint vn_shader_widen_bounds(CGXWindow *clone, CGRect frame, const VNAnimationStyle *anim,
                                      const void *display) {
    const float margin = anim ? anim->shader.margin : 0.5f;
    CGPoint offset = CGPointMake(margin, margin);
    if (!clone || !vn_resolved_shape_window_with_rect) return offset;

    const double mx = frame.size.width  * margin;
    const double my = frame.size.height * margin;

    // Screen coordinates, not window-local: the rect positions the window as
    // well as sizing it, so a (-mx, -my) origin does not widen the shape in
    // place, it teleports the clone to the top-left of the display.
    CGRect shape = CGRectMake(frame.origin.x - mx, frame.origin.y - my,
                              frame.size.width  + mx * 2.0,
                              frame.size.height + my * 2.0);

    // Water's particles fall to the bottom of the screen and bounce off its
    // sides, so its quad is the display: a shader may only write inside the
    // layer's draw shape, and anything narrower would clip the simulation at
    // the window's own margin.
    if (anim && anim->shader.full_screen && display && vn_resolved_display_get_bounds) {
        const CGRect screen = vn_resolved_display_get_bounds(display);
        // Union, not replacement: a window hanging off the edge of the display
        // still has to be inside the shape, or its own pixels are clipped
        // before the animation even starts.
        if (screen.size.width >= 1.0 && screen.size.height >= 1.0) shape = CGRectUnion(screen, frame);
    } else if (anim && anim->shader.full_height && display && vn_resolved_display_get_bounds) {
        const CGRect screen = vn_resolved_display_get_bounds(display);
        if (screen.size.height >= 1.0) {
            const double top    = fmin(CGRectGetMinY(screen), CGRectGetMinY(frame));
            const double bottom = fmax(CGRectGetMaxY(screen), CGRectGetMaxY(frame));
            shape.origin.y    = top;
            shape.size.height = bottom - top;
        }
    }

    // What the shader subtracts from its texture coordinates to put the window
    // at [0,1]: the window's offset inside the quad, in units of its own size.
    offset.x = (CGFloat)((frame.origin.x - shape.origin.x) / frame.size.width);
    offset.y = (CGFloat)((frame.origin.y - shape.origin.y) / frame.size.height);

    // Measure, rather than assume. Two attempts at this rect have moved the
    // clone instead of widening it in place, so log what the window actually
    // becomes: asked-for rect, and the screen rect and frame bounds the server
    // reports afterwards.
    CGRect before_screen = vn_resolved_screen_rect ? vn_resolved_screen_rect(clone) : CGRectZero;
    CGRect before_bounds = vn_resolved_clipped_frame_bounds ? vn_resolved_clipped_frame_bounds(clone) : CGRectZero;

    vn_resolved_shape_window_with_rect(clone, shape, 0);

    CGRect after_screen = vn_resolved_screen_rect ? vn_resolved_screen_rect(clone) : CGRectZero;
    CGRect after_bounds = vn_resolved_clipped_frame_bounds ? vn_resolved_clipped_frame_bounds(clone) : CGRectZero;

    VN_INFO("shader bounds: frame=(%.0f,%.0f %.0fx%.0f) asked=(%.0f,%.0f %.0fx%.0f) offset=(%.3f, %.3f)",
            frame.origin.x, frame.origin.y, frame.size.width, frame.size.height,
            shape.origin.x, shape.origin.y, shape.size.width, shape.size.height,
            offset.x, offset.y);
    VN_INFO("shader bounds: screen %.0f,%.0f %.0fx%.0f -> %.0f,%.0f %.0fx%.0f",
            before_screen.origin.x, before_screen.origin.y, before_screen.size.width, before_screen.size.height,
            after_screen.origin.x,  after_screen.origin.y,  after_screen.size.width,  after_screen.size.height);
    VN_INFO("shader bounds: bounds %.0f,%.0f %.0fx%.0f -> %.0f,%.0f %.0fx%.0f",
            before_bounds.origin.x, before_bounds.origin.y, before_bounds.size.width, before_bounds.size.height,
            after_bounds.origin.x,  after_bounds.origin.y,  after_bounds.size.width,  after_bounds.size.height);
    return offset;
}

/// The animation's progress, handed to the shader through the one per-window
/// float the compositor already plumbs into UberComposite_FragmentArgs:
/// CGXWindow::brightness -> layer->[0x224] -> args._brightness.
///
/// Runs 1 -> 0, so a clone that has been tagged but not yet started sits at 1,
/// which every shader here treats as identity.
static void vn_shader_set_phase(CGXWindow *clone, double t) {
    if (!clone) return;

    double b = 1.0 - t;
    if (b < 0.0) b = 0.0;
    if (b > 1.0) b = 1.0;
    clone->brightness = (float)b;

    // Writing the field leaves the window clean, and a clean window is never
    // re-composited, so the change would not reach the shader until something
    // else happened to dirty it.
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
    if (f->filter_id == kVNFilterMarkerShader) {
        atomic_fetch_sub_explicit(&gShaderFilterCount, 1, memory_order_acq_rel);
    }
    free(f);
}




// Clones win and places it relative to the original. Fills out's window,
// frame and style; the owner fields are the caller's.
static CGXWindow *vn_make_clone(CGXWindow *win, CGXConnection *conn, CGSOrderOp place, VNClone *out) {
    const uint32_t orig_wid = win->window_id;
    if (!vn_resolved_create_clone || !vn_resolved_window_get_display || !vn_resolved_screen_rect_from_rect ||
        !vn_resolved_window_get_id) {
        VN_ERROR("clone: symbols unresolved (clone=%p disp=%p rect=%p id=%p)",
               (void *)vn_resolved_create_clone, (void *)vn_resolved_window_get_display,
               (void *)vn_resolved_screen_rect_from_rect, (void *)vn_resolved_window_get_id);
        return NULL;
    }

    double t_snap0 = SLSCurrentRealTime();
    const void *display = vn_resolved_window_get_display(win);
    if (!display) { VN_ERROR("clone: no display for wid=%u", orig_wid); return NULL; }

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
        VN_ERROR("clone: no usable frame for wid=%u -- not cloning", orig_wid);
        return NULL;
    }

    const VNAnimationStyle *anim = vn_animation_style_for_key(prefs.animation);

    // Diagnostic: the selected animation silently falling back to the first
    // registry entry is indistinguishable, on screen, from the shader path
    // being broken. Log what prefs actually held and what it resolved to, so
    // the two can be told apart without a second build. The length is here
    // because a trailing space or newline in the stored value looks identical
    // in a plist dump but never matches a registry key.
    VN_INFO("animation select: prefs='%s' (len=%zu) -> key='%s' kind=%s",
            prefs.animation, strlen(prefs.animation),
            anim ? anim->key : "?",
            anim ? (anim->kind == VN_ANIM_SHADER ? "SHADER" : "MESH") : "?");

    // Before the clone exists, while the screen still looks the way the user
    // is looking at it.
    CGRect   water_obstacles[kVNMaxObstacles];
    uint32_t water_obstacle_count = 0;
    CGRect   water_screen = CGRectZero;
    if (anim->kind == VN_ANIM_SHADER && anim->shader.particles) {
        water_screen = vn_resolved_display_get_bounds ? vn_resolved_display_get_bounds(display) : CGRectZero;
        water_obstacle_count = vn_water_collect_obstacles(orig_wid,
                                                          content.size.width >= 1.0 ? content : frame,
                                                          water_screen, water_obstacles);
    }

    double t_clone0 = SLSCurrentRealTime();
    CGXWindow *clone = vn_resolved_create_clone(win, frame, display, true);
    double clone_ms = (SLSCurrentRealTime() - t_clone0) * 1000.0;
    if (!clone) { VN_ERROR("clone: CreateCloneOfWindow returned NULL"); return NULL; }

    uint32_t wid = vn_resolved_window_get_id(clone);
    if (wid != 0) {
        CGSOrderOp op  = place;
        uint32_t   rel = orig_wid;
        vn_orig_order(conn, &wid, &op, &rel, 1, false);
        out->orig_wid  = orig_wid;
        out->clone_wid = wid;
        out->clone_win = clone;
        out->frame     = frame;
        out->anim      = anim;
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
        VN_DEBUG("clone: disabled shadow property on clone wid=%u win=%p", wid, clone);
    }

    if (anim->kind == VN_ANIM_SHADER) {
        // Per-close values for the shader, in the window's unit square:
        //   params[0]  a fresh random value per close: Burn reads it as its
        //              ignition point, packed as whole thousandths of x plus y
        //              (y < 1), which a float holds to about 6e-5; Dissolve and
        //              Shatter seed their randomness with it
        //   params[1]  corner radius as a fraction of the window's height
        //   params[2]  inset of the window's left and right edges within the
        //              clone's frame, which also holds the shadow when shadows
        //              are on (the shadow is centred horizontally)
        //   params[3]  the window's top edge within the frame
        //   params[4]  the window's bottom edge within the frame
        float inset_x = 0.0f, top = 0.0f, bottom = 1.0f, radius = 0.0f;
        if (frame.size.width >= 1.0 && frame.size.height >= 1.0 &&
            content.size.width >= 1.0 && content.size.height >= 1.0) {
            const double left  = (CGRectGetMinX(content) - frame.origin.x) / frame.size.width;
            const double right = (CGRectGetMaxX(frame) - CGRectGetMaxX(content)) / frame.size.width;
            inset_x = (float)fmax(0.0, fmin(0.5, 0.5 * (left + right)));
            top     = (float)fmax(0.0, fmin(1.0, (CGRectGetMinY(content) - frame.origin.y) / frame.size.height));
            bottom  = (float)fmax(0.0, fmin(1.0, (CGRectGetMaxY(content) - frame.origin.y) / frame.size.height));
            const double radius_pt = vn_resolved_corner_radius ? vn_resolved_corner_radius(win) : 0.0;
            radius = (float)fmax(0.0, fmin(0.5, radius_pt / content.size.height));
            VN_INFO("clone: window within frame: left %.4f right %.4f top %.4f bottom %.4f corner radius %.1fpt "
                    "(window %.1f,%.1f %.1fx%.1f in frame %.1f,%.1f %.1fx%.1f)",
                    left, right, (double)top, (double)bottom, radius_pt,
                    content.origin.x, content.origin.y, content.size.width, content.size.height,
                    frame.origin.x, frame.origin.y, frame.size.width, frame.size.height);
        }
        const float params[5] = {
            (float)arc4random_uniform(1001) + (float)arc4random_uniform(1000) / 1000.0f,
            radius, inset_x, top, bottom,
        };
        out->seed = params[0];
        vn_filter_attach(clone, anim->shader.type, true, params);
        if (anim->shader.hdr) {
            if (atomic_fetch_add_explicit(&gVNHDRClones, 1, memory_order_acq_rel) == 0) vn_edr_ramp_override(true);
            vn_window_request_headroom(clone, kVNBurnHeadroom);
        }
        vn_clone_publish(params[0], vn_shader_widen_bounds(clone, frame, anim, display));
        if (anim->shader.particles) {
            // The particles need the window and the display in points; the
            // compute pass converts both into the render target's pixels once
            // it knows the destination it is stepping against. The obstacles
            // were collected above, before this clone went on screen.
            vn_water_publish(clone, content.size.width >= 1.0 ? content : frame,
                             water_screen, water_obstacles, water_obstacle_count,
                             prefs.waterTint, params[0], prefs.waterDrains);
        }
        vn_shader_set_phase(clone, 0.0);
    } else if (vn_resolved_set_mesh_warp && anim->kind == VN_ANIM_MESH && anim->mesh.fill) {
        VNPointWarp warm[kVNMeshMaxCount];
        anim->mesh.fill(warm, frame, 0.0);
        vn_resolved_set_mesh_warp(clone, NULL, anim->mesh.w, anim->mesh.h, (const float *)warm);
    }

    // The clone is built synchronously in the event hook, before the app is
    // even handed the mouse-up, so anything slow here is felt as a hitch at
    // the click itself rather than as a dropped animation frame.
    VN_DEBUG("clone: clone=%p wid=%u %s %u frame=(%.1f,%.1f %.1fx%.1f) display=%p build=%.2fms total=%.2fms",
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
    VNFrameJob jobs[MAX_CLONE_ANIMATIONS];
    int job_count = 0;
    uint64_t finished[MAX_CLONE_ANIMATIONS];
    int finished_count = 0;
    bool more = false;
    CGXWindow *first_active_win = NULL;

    os_unfair_lock_lock(&gCloneAnimationsLock);
    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (!gCloneAnimations[i].is_animating) continue;

        double dur = gCloneAnimations[i].duration > 0.0 ? gCloneAnimations[i].duration : 0.25;
        double p = (now - gCloneAnimations[i].start_time) / dur;

        // Water ends when its water does: faded out for a newer close, or
        // drained away, which can be well before the duration is up.
        const VNAnimationStyle *style = gCloneAnimations[i].clone.anim;
        const bool water_done = style && style->kind == VN_ANIM_SHADER && style->shader.particles &&
                                now >= vn_water_end_for(gCloneAnimations[i].clone.clone_win);

        if (p >= 1.0 || water_done) {
            finished[finished_count++] = gCloneAnimations[i].anim_id;
        } else {
            more = true;
            if (!first_active_win && gCloneAnimations[i].clone.clone_win) {
                first_active_win = gCloneAnimations[i].clone.clone_win;
            }
            if (job_count < MAX_CLONE_ANIMATIONS) {
                jobs[job_count++] = (VNFrameJob){
                    .clone_win = gCloneAnimations[i].clone.clone_win,
                    .bounds = gCloneAnimations[i].clone.frame,
                    .p = p,
                    .anim = gCloneAnimations[i].clone.anim,
                };
            }
        }
    }
    os_unfair_lock_unlock(&gCloneAnimationsLock);

    for (int i = 0; i < finished_count; i++) {
        vn_finish_animation_for_id(finished[i]);
    }

    for (int i = 0; i < job_count; i++) {
        const VNAnimationStyle *anim = jobs[i].anim ? jobs[i].anim : &gAnimationStyles[0];
        if (!jobs[i].clone_win) continue;

        // Dispatch on kind rather than assuming a mesh, so a shader animation
        // can be added here without disturbing this path.
        switch (anim->kind) {
        case VN_ANIM_MESH:
            if (vn_resolved_set_mesh_warp && anim->mesh.fill) {
                VNPointWarp mesh[kVNMeshMaxCount];
                anim->mesh.fill(mesh, jobs[i].bounds, jobs[i].p);
                vn_resolved_set_mesh_warp(jobs[i].clone_win, NULL,
                                          anim->mesh.w, anim->mesh.h, (const float *)mesh);
            }
            break;
        case VN_ANIM_SHADER:
            // The tag routes the clone to our substituted ubershader; all that
            // changes per frame is the progress it reads.
            vn_shader_set_phase(jobs[i].clone_win, jobs[i].p);
            break;
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
        VN_TRACE("anim_tick: %d clone(s) gap=%.2fms work=%.2fms", job_count, gap, spent);
    }
#endif

    if (more && vn_resolved_schedule_callback) {
        // Deliberately not vn_get_refresh_interval() here. That reads the
        // prefs, which stat()s the plist and re-parses it when the mtime
        // moves -- a syscall, and potentially a file read and CFPropertyList
        // parse, on the compositor's timer thread once per frame. The refresh
        // rate cannot meaningfully change inside a 240ms animation, so it is
        // sampled once at the start and reused.
        // Twice per refresh. The tick only marks the clone dirty -- each frame
        // reads its own phase when it is drawn -- but the compositor redraws a
        // window only once it is dirty, and a timer that is not locked to the
        // display beats against it: at one tick per refresh, some refreshes
        // get two and the next gets none, and that frame repeats. At two per
        // refresh every refresh has one to draw. The tick's work is ~0.01ms.
        double interval = gAnimFrameInterval * 0.5;
        if (interval <= 0.0) interval = 1.0 / 240.0;

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

static void vn_start_clone_animation(VNClone clone) {
    CGXWindow *clone_win = clone.clone_win;
    if (!clone_win) return;
    uint32_t clone_wid = vn_resolved_window_get_id ? vn_resolved_window_get_id(clone_win) : 0;
    clone.clone_wid = clone_wid;
    float dur = vn_duration_for_anim(clone.anim);

    if ((clone.pid == 0 || clone.psn == 0) && clone.orig_wid != 0 && vn_resolved_window_by_id) {
        CGXWindow *orig_win = vn_resolved_window_by_id(clone.orig_wid);
        if (orig_win) {
            if (clone.pid == 0 && vn_resolved_window_get_owning_pid) clone.pid = vn_resolved_window_get_owning_pid(orig_win);
            CGXConnection *c = vn_window_connection(orig_win);
            if (c) {
                if (clone.pid == 0) clone.pid = vn_conn_get_pid(c);
                if (clone.psn == 0) clone.psn = vn_conn_get_psn(c);
            }
        }
    }

    if (!clone.is_whatsapp && strcasecmp(clone.app, "WhatsApp") == 0) {
        clone.is_whatsapp = true;
    }

    CGRect frame = clone.frame;
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

    os_unfair_lock_lock(&gCloneAnimationsLock);
    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (gCloneAnimations[i].is_animating && gCloneAnimations[i].clone.clone_wid != 0 && gCloneAnimations[i].clone.clone_wid != clone_wid) {
            if (gCloneAnimations[i].anim_id > max_anim_id) {
                max_anim_id = gCloneAnimations[i].anim_id;
                lowest_clone_wid = gCloneAnimations[i].clone.clone_wid;
            }
        }
    }

    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (!gCloneAnimations[i].is_animating) {
            slot = i;
            break;
        }
    }
    if (slot == -1) slot = 0;

    anim_id = gNextAnimId++;
    if (!clone.anim) clone.anim = &gAnimationStyles[0];
    clone.frame = frame;
    const double start_time = SLSCurrentRealTime();
    gCloneAnimations[slot] = (VNCloneAnimation){
        .anim_id = anim_id,
        .clone = clone,
        .is_animating = true,
        .start_time = start_time,
        .duration = (double)dur,
    };
    if (clone.anim->kind == VN_ANIM_SHADER) {
        vn_clone_start(clone.seed, start_time, (double)dur);
        vn_water_start(clone_win, start_time, (double)dur);
    }
    os_unfair_lock_unlock(&gCloneAnimationsLock);

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

    VN_INFO("starting fade animation for clone wid=%u (orig=%u, app='%s' pid=%d psn=0x%llx is_wa=%d) win=%p anim='%s' duration=%.2fs interval=%.2fms (%.0fHz) headroom=%.2f (anim_id=%llu)",
            clone_wid, clone.orig_wid, clone.app, clone.pid, clone.psn, clone.is_whatsapp, clone_win, clone.anim->key, dur, interval * 1000.0, hz,
            (double)vn_window_headroom(clone_win), anim_id);

    if (vn_resolved_schedule_callback) {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(&gAnimTimerRunning, &expected, true,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            gAnimNextDeadline = SLSCurrentRealTime() + interval * 0.5;
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

    os_unfair_lock_lock(&gCloneAnimationsLock);
    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (gCloneAnimations[i].is_animating) {
            if ((wid != 0 && gCloneAnimations[i].clone.clone_wid == wid) ||
                (win != NULL && gCloneAnimations[i].clone.clone_win == win)) {
                os_unfair_lock_unlock(&gCloneAnimationsLock);
                return true;
            }
        }
    }
    os_unfair_lock_unlock(&gCloneAnimationsLock);

    os_unfair_lock_lock(&gPendingClonesLock);
    for (int i = 0; i < MAX_PENDING_CLONES; i++) {
        if (gPendingClones[i].orig_wid != 0) {
            if ((wid != 0 && gPendingClones[i].clone_wid == wid) ||
                (win != NULL && gPendingClones[i].clone_win == win)) {
                os_unfair_lock_unlock(&gPendingClonesLock);
                return true;
            }
        }
    }
    os_unfair_lock_unlock(&gPendingClonesLock);

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

/// The windows the fluid should flow around: everything ordered in on this
/// display except the window that is closing and our own clones. Read from the
/// tracking table the order hook already maintains, on the close path, so the
/// compositor's thread never has to take that lock.
/// `closing_pt` is the closing window's OWN rect -- screen_rect, not the
/// clipped frame bounds that carry its drop shadow, and not the display-sized
/// shape the shader is allowed to draw into. It has to be the tight one: an
/// inflated rect would overlap windows that merely sit near the closing one,
/// drop them from the list, and the water would fall straight through the first
/// thing under it.
/// Whether `w` is one of ours: a water clone in a slot, any clone the
/// animation table knows, or the window being closed -- which is on its way
/// out, and whose shadow would otherwise hide the edges of its neighbours.
static bool vn_water_ignores_as_cover(CGXWindow *w, uint32_t closing_wid) {
    const uint32_t wid = vn_resolved_window_get_id ? vn_resolved_window_get_id(w) : 0;
    if (wid != 0 && wid == closing_wid) return true;
    for (int i = 0; i < kVNWaterSlots; i++) {
        if (gVNWaterSlots[i].generation != 0 && gVNWaterSlots[i].clone_win == w) return true;
    }
    return vn_is_clone_wid(wid, w);
}

/// The bounds of the part of `w` you can see, leaving our own windows out of
/// what covers it. The same computation as
/// CGXCreateScreenUnobscuredContentShapeForWindow -- content shape minus the
/// frame shapes of every window above -- over the same window stack, with
/// vn_water_ignores_as_cover skipped. See kVNSymSessionControlRef.
///
/// 1 with `out` set when some of it is visible, 0 when none is, and -1 when
/// the stack could not be read, which sends the caller back to SkyLight's own
/// answer.
static int vn_water_visible_bounds(CGXWindow *w, uint32_t closing_wid, CGRect *out) {
    if (!vn_resolved_session_control_ref || !vn_resolved_copy_screen_frame_shape ||
        !vn_resolved_copy_screen_content_shape || !vn_region_union || !vn_region_diff ||
        !vn_resolved_get_region_bounds) return -1;

    const char *session = *(const char **)vn_resolved_session_control_ref;
    if (!session) return -1;
    const char *windows = *(const char * const *)(session + kVNSessionWindowsOuterOffset);
    if (!windows) return -1;
    windows = *(const char * const *)(windows + kVNSessionWindowsInnerOffset);
    if (!windows) return -1;
    const struct { CGXWindow **items; int32_t count; } *stack = (const void *)(windows + kVNSessionWindowStackOffset);
    if (!stack->items || stack->count <= 0 || stack->count > 16384) return -1;

    // Front to back: everything before `w` is above it. Not finding it at all
    // means this is not the stack the window lives in.
    int32_t index = -1;
    for (int32_t i = 0; i < stack->count; i++) {
        if (stack->items[i] == w) { index = i; break; }
    }
    if (index < 0) return -1;

    void *visible = vn_resolved_copy_screen_content_shape(w, 0);
    if (!visible) return 0;
    for (int32_t i = 0; i < index; i++) {
        CGXWindow *above = stack->items[i];
        if (!above || vn_water_ignores_as_cover(above, closing_wid)) continue;
        void *frame = vn_resolved_copy_screen_frame_shape(above, 0);
        if (!frame) continue;
        void *rest = NULL;
        vn_region_diff(visible, frame, &rest);
        CFRelease(frame);
        CFRelease(visible);
        visible = rest;
        if (!visible) return 0;
    }

    CGRect bounds = CGRectZero;
    vn_resolved_get_region_bounds(visible, &bounds);
    CFRelease(visible);
    if (bounds.size.width < 1.0 || bounds.size.height < 1.0) return 0;
    *out = bounds;
    return 1;
}

static uint32_t vn_water_collect_obstacles(uint32_t closing_wid, CGRect closing_pt,
                                           CGRect display_pt, CGRect out[kVNMaxObstacles]) {
    const double t0 = SLSCurrentRealTime();
    uint32_t wids[MAX_TRACKED_WINDOWS];
    int n = 0;

    os_unfair_lock_lock(&gTrackedLock);
    for (int i = 0; i < MAX_TRACKED_WINDOWS && n < MAX_TRACKED_WINDOWS; i++) {
        if (gTrackedWindows[i].wid != 0 && gTrackedWindows[i].is_ordered_in &&
            gTrackedWindows[i].wid != closing_wid) {
            wids[n++] = gTrackedWindows[i].wid;
        }
    }
    os_unfair_lock_unlock(&gTrackedLock);

    uint32_t count = 0;
    for (int i = 0; i < n && count < kVNMaxObstacles; i++) {
        if (!vn_resolved_window_by_id) break;
        CGXWindow *w = vn_resolved_window_by_id(wids[i]);
        if (!w || vn_is_clone_wid(wids[i], w)) continue;

        CGRect r = vn_resolved_screen_rect ? vn_resolved_screen_rect(w) : CGRectZero;
        if (r.size.width < 32.0 || r.size.height < 32.0) continue;
        // Only what is actually on this display, and only what is big enough to
        // be a surface rather than a tooltip.
        if (display_pt.size.width >= 1.0 && !CGRectIntersectsRect(r, display_pt)) continue;

        // Not the desktop. The wallpaper is a window like any other and it is
        // ordered in and covers the screen, so it would take a slot and be
        // solid to every drop -- the fluid is meant to run over the desktop,
        // not be contained by it. Anything covering nearly the whole display
        // is one of those surfaces rather than a ledge to land on.
        if (display_pt.size.width >= 1.0 && display_pt.size.height >= 1.0) {
            const double screen_area = display_pt.size.width * display_pt.size.height;
            if (r.size.width * r.size.height >= screen_area * 0.85) {
                VN_DEBUG("water: obstacle wid=%u (%.0f,%.0f %.0fx%.0f) covers the display -- skipped",
                         wids[i], r.origin.x, r.origin.y, r.size.width, r.size.height);
                continue;
            }
        }

        // Not something the fluid is born on top of.
        //
        // A window overlapping the closing one is underneath where the liquid
        // starts, so some of the liquid begins inside it. Left in the list it
        // is solid to the particles born above its top edge and permeable to
        // the ones born below -- one window behaving two ways at once, which
        // pins half the fluid along a dead straight line at its top edge and
        // pours the other half through it. That is the rectangle that became
        // visible. The question is about the window, so it is answered once
        // here rather than per particle.
        if (closing_pt.size.width >= 1.0 && CGRectIntersectsRect(r, closing_pt)) {
            VN_DEBUG("water: obstacle wid=%u (%.0f,%.0f %.0fx%.0f) overlaps the closing window "
                     "(%.0f,%.0f %.0fx%.0f) -- skipped",
                     wids[i], r.origin.x, r.origin.y, r.size.width, r.size.height,
                     closing_pt.origin.x, closing_pt.origin.y, closing_pt.size.width, closing_pt.size.height);
            continue;
        }

        // Only the part of it you can actually see.
        //
        // A window buried behind another is still a window as far as the
        // tracking table is concerned, and making it solid stops the water in
        // mid-air on a rectangle that is not on screen. SkyLight already knows
        // the answer: the unobscured content shape is the window's content
        // minus everything stacked above it, and it is NULL outright when the
        // window is not visible.
        //
        // The bounding box of that shape becomes the obstacle, so a window
        // half-covered from one side only blocks along the half you can see.
        //
        // Our own windows are left out of what covers it. SkyLight's answer
        // counts every window above, and a water clone still fading from the
        // last close is the whole display: asked while one is up, every
        // window is buried and the water falls through all of them.
        CGRect visible = CGRectZero;
        int seen_state = vn_water_visible_bounds(w, closing_wid, &visible);
        if (seen_state < 0 && vn_resolved_unobscured_content_shape && vn_resolved_get_region_bounds) {
            void *shape = vn_resolved_unobscured_content_shape(w);
            seen_state = shape ? 1 : 0;
            if (shape) {
                vn_resolved_get_region_bounds(shape, &visible);
                CFRelease(shape);
            }
            static bool s_reported;
            if (!s_reported) {
                s_reported = true;
                VN_ERROR("water: the window stack could not be read -- visibility counts our own clones as cover");
            }
        }
        if (seen_state == 0) {
            VN_DEBUG("water: obstacle wid=%u (%.0f,%.0f %.0fx%.0f) is not visible -- skipped",
                     wids[i], r.origin.x, r.origin.y, r.size.width, r.size.height);
            continue;
        }
        if (seen_state > 0) {
            // Mostly buried counts as buried: landing on a sliver reads as
            // landing on nothing.
            const double full = r.size.width * r.size.height;
            const double seen = visible.size.width * visible.size.height;
            if (visible.size.width < 32.0 || visible.size.height < 32.0 ||
                (full > 0.0 && seen < full * 0.2)) {
                VN_DEBUG("water: obstacle wid=%u (%.0f,%.0f %.0fx%.0f) is %.0f%% covered -- skipped",
                         wids[i], r.origin.x, r.origin.y, r.size.width, r.size.height,
                         full > 0.0 ? (1.0 - seen / full) * 100.0 : 100.0);
                continue;
            }
            if (!CGRectEqualToRect(visible, r)) {
                VN_DEBUG("water: obstacle wid=%u (%.0f,%.0f %.0fx%.0f) -> visible part "
                         "(%.0f,%.0f %.0fx%.0f)",
                         wids[i], r.origin.x, r.origin.y, r.size.width, r.size.height,
                         visible.origin.x, visible.origin.y, visible.size.width, visible.size.height);
            }
            r = visible;
        }

        VN_DEBUG("water: obstacle wid=%u (%.0f,%.0f %.0fx%.0f)",
                 wids[i], r.origin.x, r.origin.y, r.size.width, r.size.height);
        out[count++] = r;
    }

    // This runs on the mouse-up, between the click and the app hearing about
    // it, and the shape query is the one expensive thing in it -- so it is
    // measured rather than assumed.
    VN_INFO("water: %u obstacle(s) from %d window(s) in %.2fms",
            count, n, (SLSCurrentRealTime() - t0) * 1000.0);
    return count;
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

    bool has_pending_clone = false;
    if (wid != 0) {
        os_unfair_lock_lock(&gPendingClonesLock);
        has_pending_clone = (vn_pending_clone_find_slot_locked(wid) >= 0);
        os_unfair_lock_unlock(&gPendingClonesLock);
    }

    int other_visible = vn_count_visible_windows_for_pid(pid, wid);

    CGXWindow *clones_to_release[MAX_CLONE_ANIMATIONS] = {0};
    uint32_t clone_wids[MAX_CLONE_ANIMATIONS] = {0};
    int release_count = 0;

    os_unfair_lock_lock(&gCloneAnimationsLock);
    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (!gCloneAnimations[i].is_animating) continue;

        bool match = false;
        if ((wid != 0 && (gCloneAnimations[i].clone.clone_wid == wid || gCloneAnimations[i].clone.orig_wid == wid)) ||
            (win != NULL && gCloneAnimations[i].clone.clone_win == win)) {
            match = true;
        }
        else if (pid != 0 && gCloneAnimations[i].clone.pid == pid && !has_pending_clone) {
            if (was_already_ordered_in) {
                VN_DEBUG("Window wid=%u was already ordered in (AppKit sibling restack/focus) -> NOT canceling clone wid=%u",
                       wid, gCloneAnimations[i].clone.clone_wid);
            } else {
                if (other_visible == 0) {
                    VN_DEBUG("Process pid=%d had 0 other visible windows and wid=%u is newly ordering in -> Dock/launch reopen, canceling clone wid=%u",
                           pid, wid, gCloneAnimations[i].clone.clone_wid);
                    match = true;
                } else {
                    VN_DEBUG("Process pid=%d still has %d other visible window(s); wid=%u is a sibling -> NOT canceling clone wid=%u",
                           pid, other_visible, wid, gCloneAnimations[i].clone.clone_wid);
                }
            }
        }

        if (match) {
            clones_to_release[release_count] = gCloneAnimations[i].clone.clone_win;
            clone_wids[release_count] = gCloneAnimations[i].clone.clone_wid;
            release_count++;

            gCloneAnimations[i].is_animating = false;
            gCloneAnimations[i].clone = (VNClone){0};
        }
    }
    os_unfair_lock_unlock(&gCloneAnimationsLock);

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
        vn_pending_clone_discard_wid(wid);
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

    os_unfair_lock_lock(&gCloneAnimationsLock);
    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (!gCloneAnimations[i].is_animating) continue;

        bool match_self = (gCloneAnimations[i].clone.clone_win == win) || (rel_wid != 0 && gCloneAnimations[i].clone.clone_wid == rel_wid);

        if (match_self) {
            VN_DEBUG("release_window arrived for animating clone wid=%u win=%p; removing from animation table",
                   gCloneAnimations[i].clone.clone_wid, win);
            gCloneAnimations[i].is_animating = false;
            gCloneAnimations[i].clone = (VNClone){0};
            break;
        } else if (rel_wid != 0 && gCloneAnimations[i].clone.orig_wid == rel_wid) {
            VN_DEBUG("release_window arrived for orig_wid=%u while clone wid=%u is animating (normal AppKit teardown); clone continues",
                   rel_wid, gCloneAnimations[i].clone.clone_wid);
            gCloneAnimations[i].clone.orig_wid = 0;
            break;
        }
    }
    os_unfair_lock_unlock(&gCloneAnimationsLock);

    VNClone taken = {0};

    os_unfair_lock_lock(&gPendingClonesLock);
    for (int i = 0; i < MAX_PENDING_CLONES; i++) {
        if (gPendingClones[i].clone_win == win) {
            memset(&gPendingClones[i], 0, sizeof(VNClone));
            break;
        }
        if (gPendingClones[i].orig_wid != 0 &&
            ((rel_wid != 0 && gPendingClones[i].orig_wid == rel_wid) ||
             (vn_resolved_window_by_id && vn_resolved_window_by_id(gPendingClones[i].orig_wid) == win))) {
            vn_pending_clone_take_locked(gPendingClones[i].orig_wid, &taken);
            break;
        }
    }
    os_unfair_lock_unlock(&gPendingClonesLock);

    vn_orig_release_window(conn, win);

    if (taken.clone_win) {
        VN_INFO(">>> Close seen at release_window for wid=%u (app='%s' pid=%d psn=0x%llx is_wa=%d) -- animating pre-clone wid=%u",
               taken.orig_wid, taken.app, taken.pid, taken.psn, taken.is_whatsapp, taken.clone_wid);
        vn_start_clone_animation(taken);
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
    os_unfair_lock_lock(&gPendingClonesLock);
    bool pending = (vn_pending_clone_find_slot_locked(wid) >= 0);
    os_unfair_lock_unlock(&gPendingClonesLock);
    if (!pending) { gAlphaProbeWid = 0; return; }

    // Relative to the alpha at mouse-up, not to 1.0: plenty of windows are
    // legitimately translucent, and only a *drop* means a close.
    float alpha = vn_window_alpha(win);
    if (alpha < gAlphaProbeBaseline - 0.01f) {
        gAlphaProbeWid = 0;

        // Checked before taking the pre-clone, so discarding it goes through
        // the normal path and nothing has to be released by hand.
        pid_t owner = vn_resolved_window_get_owning_pid ? vn_resolved_window_get_owning_pid(win) : 0;
        if (vn_system_close_animation_in_flight(wid, owner)) {
            VN_INFO(">>> Deferring to the system's close animation for wid=%u (pid=%d) -- app just spawned a new window, ours would collide",
                    wid, owner);
            vn_pending_clone_discard_wid(wid);
            return;
        }

        VNClone taken = {0};
        os_unfair_lock_lock(&gPendingClonesLock);
        vn_pending_clone_take_locked(wid, &taken);
        os_unfair_lock_unlock(&gPendingClonesLock);

        if (!taken.clone_win || taken.clone_wid == 0) return;

        VN_INFO(">>> Close detected via fade for wid=%u (app='%s' alpha=%.3f from %.3f) at +%.0fms -- animating now, %.0fms before order-out would have arrived",
               wid, taken.app, (double)alpha, (double)gAlphaProbeBaseline,
               elapsed * 1000.0, 250.0);

        if (vn_resolved_update_ca_visibility) {
            vn_resolved_update_ca_visibility(win, false);
            gEarlyHiddenWid = wid;
            gEarlyHiddenWin = win;
            if (vn_resolved_schedule_callback) {
                vn_resolved_schedule_callback(vn_early_restore_check, NULL, SLSCurrentRealTime() + 2.0);
            }
        }

        vn_start_clone_animation(taken);
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
static bool vn_create_pending_clone(CGXWindow *win, CGXConnection *conn) {
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

    vn_pending_clone_discard_wid(wid);
    atomic_store_explicit(&gNonCloseWid, 0, memory_order_relaxed);

    VNClone made = {0};
    CGXWindow *clone = vn_make_clone(win, conn, kVNOrderBelow, &made);
    if (!clone || made.clone_wid == 0) {
        VN_ERROR("pre-clone: failed to create clone for wid=%u", wid);
        return false;
    }

    os_unfair_lock_lock(&gPendingClonesLock);
    int slot = vn_pending_clone_find_slot_locked(wid);
    if (slot < 0) slot = vn_pending_clone_find_empty_slot_locked();
    made.pid = pid;
    made.psn = psn;
    made.is_whatsapp = (strcasecmp(app, "WhatsApp") == 0);
    strlcpy(made.app, app, sizeof(made.app));
    made.created_at = SLSCurrentRealTime();
    gPendingClones[slot] = made;
    os_unfair_lock_unlock(&gPendingClonesLock);

    VN_INFO(">>> Mouse up on RED CLOSE box of '%s' (pid=%d, psn=0x%llx, wid=%u) -- pre-clone ready! clone_wid=%u slot=%d",
           app, pid, psn, wid, made.clone_wid, slot);

    if (vn_resolved_schedule_callback) {
        vn_resolved_schedule_callback(vn_pending_clone_cleanup_timer, NULL, SLSCurrentRealTime() + 1.0);
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
                    vn_pending_clone_discard_wid(wid);
                    gDoubleClickSuppressUntilMs = now_ms + 600;
                    goto dispatch;
                }

                if (suppressed_by_double_click) {
                    VN_DEBUG("hit-test: click on wid=%u suppressed due to active double-click window", wid);
                    vn_pending_red_clear();
                    vn_pending_clone_discard_wid(wid);
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
                    // (see vn_create_pending_clone), so a press that turns into a
                    // drag never leaves a clone behind to be exposed.
                    VN_DEBUG(">>> Mouse down in RED CLOSE box of wid=%u (pt=(%.1f, %.1f)) -- waiting for release before cloning",
                           wid, lx, ly);

                    vn_pending_clone_discard_wid(wid);

                    os_unfair_lock_lock(&gPendingRedLock);
                    gPendingRed = (VNPendingRedClick){
                        .orig_wid = wid,
                        .down_screen_pt = *screen_pt,
                        .active = true,
                        .invalidated = false,
                    };
                    os_unfair_lock_unlock(&gPendingRedLock);
                } else if (is_yellow_or_green) {
                    VN_DEBUG(">>> Mouse down in YELLOW/GREEN button of wid=%u (pt=(%.1f, %.1f)) -- ignoring close animation",
                           wid, lx, ly);
                    vn_pending_red_clear();
                    vn_pending_clone_discard_wid(wid);
                    atomic_store_explicit(&gNonCloseWid, wid, memory_order_relaxed);
                    atomic_store_explicit(&gNonCloseTimeMs, vn_now_ms(), memory_order_relaxed);
                } else {
                    vn_pending_red_clear();
                    vn_pending_clone_discard_wid(wid);
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
                    vn_create_pending_clone(win, conn);
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
        os_unfair_lock_lock(&gPendingClonesLock);
        if (wid != 0) slot = vn_pending_clone_find_slot_locked(wid);
        os_unfair_lock_unlock(&gPendingClonesLock);
        if (slot >= 0) {
            VN_DEBUG("pre-clone: drag on wid=%u while a clone exists -- discarding it", wid);
            vn_pending_clone_discard_wid(wid);
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

        // Cleared here rather than only in the target-window branches below.
        // The restore backstop fires 2s after a fade-path close if no order-out
        // was seen, and "seen" used to mean "seen for a window that still passes
        // vn_is_target_window()". A window that stops matching the filter on the
        // way out -- which a closing Chromium window can, its properties change
        // as it goes -- ordered out perfectly well and was still counted as
        // never having done so, so two seconds later the backstop made it
        // visible again. This loop sees every ordering op before any filtering.
        if (ops[pi] == kVNOrderOut) vn_early_hidden_forget(pwid);

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
                        // Forwarded, not dropped. What this guard is for is
                        // "do not start a SECOND animation" -- but it used to
                        // skip vn_orig_order too, and on the fade path that is
                        // the only order-out the window ever gets.
                        //
                        // The fade path starts the animation from
                        // vn_close_watch_tick, before the app has asked for
                        // anything. The original is hidden there by forcing its
                        // CA visibility off, which is not the same as being
                        // ordered out: the server still has it ordered in. So
                        // swallowing the app's real order-out left the window
                        // alive and ordered in, invisible only for as long as
                        // nothing recomputed its visibility. Anything that did
                        // -- the app reusing the window, a space or display
                        // change -- brought it straight back, after a close
                        // that had looked perfect.
                        //
                        // Apps that destroy the window right after hid this:
                        // release_window tore it down before anything noticed.
                        // Chromium keeps its NSWindows and reuses them, which
                        // is why Chrome and Discord are where it shows up.
                        //
                        // A genuinely duplicate order-out is harmless here: the
                        // window is already out, and ordering it out again is a
                        // no-op.
                        VN_DEBUG("Window wid=%u (%p, app: '%s') already animating; forwarding the order-out, not starting a second animation",
                               wid, win, app);
                        vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
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

                    VNClone taken = {0};
                
                    if (vn_system_close_animation_in_flight(wid, pid)) {
                        VN_INFO(">>> Deferring to the system's close animation for wid=%u (pid=%d) -- app just spawned a new window, ours would collide",
                                wid, pid);
                        vn_pending_clone_discard_wid(wid);
                    }

                    os_unfair_lock_lock(&gPendingClonesLock);
                    vn_pending_clone_take_locked(wid, &taken);
                    os_unfair_lock_unlock(&gPendingClonesLock);

                    if (!taken.clone_win || taken.clone_wid == 0) {
                        VN_DEBUG("Window wid=%u orderOut has no pre-clone (not closed via red button) -- passing through directly", wid);
                        vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
                        return;
                    }

                    if (!taken.app[0]) strlcpy(taken.app, app, sizeof(taken.app));
                    if (!taken.pid) taken.pid = pid;
                    if (!taken.is_whatsapp && (strcasecmp(taken.app, "WhatsApp") == 0)) {
                        taken.is_whatsapp = true;
                    }
                    VN_INFO(">>> Intercepted close for target window %u (%p, app: '%s' pid=%d psn=0x%llx is_wa=%d)",
                           wid, win, taken.app, taken.pid, taken.psn, taken.is_whatsapp);

                    vn_orig_order(conn, wids, ops, relativeTo, 1, spaceSwitch);
                    vn_start_clone_animation(taken);
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
                        // Passed along rather than dropped from the batch, for
                        // the reason given in the single-window branch above.
                        VN_DEBUG("Window wid=%u (%p, app: '%s') already animating; forwarding the order-out, not starting a second animation",
                               wid, win, app);
                        pass_wids[pass_count] = wid;
                        pass_ops[pass_count] = ops[i];
                        pass_rel[pass_count] = relativeTo ? relativeTo[i] : 0;
                        pass_count++;
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

                    VNClone taken = {0};
                
                    if (vn_system_close_animation_in_flight(wid, pid)) {
                        VN_INFO(">>> Deferring to the system's close animation for wid=%u (pid=%d) -- app just spawned a new window, ours would collide",
                                wid, pid);
                        vn_pending_clone_discard_wid(wid);
                    }

                    os_unfair_lock_lock(&gPendingClonesLock);
                    vn_pending_clone_take_locked(wid, &taken);
                    os_unfair_lock_unlock(&gPendingClonesLock);

                    if (!taken.clone_win || taken.clone_wid == 0) {
                        VN_DEBUG("Window wid=%u multi-orderOut has no pre-clone (not closed via red button) -- passing through", wid);
                        pass_wids[pass_count] = wid;
                        pass_ops[pass_count] = ops[i];
                        pass_rel[pass_count] = relativeTo ? relativeTo[i] : 0;
                        pass_count++;
                        continue;
                    }

                    if (!taken.app[0]) strlcpy(taken.app, app, sizeof(taken.app));
                    if (!taken.pid) taken.pid = pid;
                    if (!taken.is_whatsapp && (strcasecmp(taken.app, "WhatsApp") == 0)) {
                        taken.is_whatsapp = true;
                    }
                    VN_INFO(">>> Intercepted close for target window %u (%p, app: '%s' pid=%d psn=0x%llx is_wa=%d)",
                           wid, win, taken.app, taken.pid, taken.psn, taken.is_whatsapp);
                    vn_start_clone_animation(taken);
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

#pragma mark - Shader substitution

/// The fragment the running shader animation wants, or the registry's first
/// shader entry if none is running. Only consulted when a pipeline is actually
/// being built, which is once per process.
static const char *vn_shader_fragment_name(void) {
    const char *name = NULL;

    os_unfair_lock_lock(&gCloneAnimationsLock);
    for (int i = 0; i < MAX_CLONE_ANIMATIONS; i++) {
        if (gCloneAnimations[i].is_animating && gCloneAnimations[i].clone.anim &&
            gCloneAnimations[i].clone.anim->kind == VN_ANIM_SHADER) {
            name = gCloneAnimations[i].clone.anim->shader.frag;
            break;
        }
    }
    os_unfair_lock_unlock(&gCloneAnimationsLock);

    if (!name) {
        for (size_t i = 0; i < kVNAnimationStyleCount; i++) {
            if (gAnimationStyles[i].kind == VN_ANIM_SHADER) { name = gAnimationStyles[i].shader.frag; break; }
        }
    }
    return name ? name : "vn_uber_dissolve";
}

/// The ubershader's own library and vertex descriptor, taken from the first
/// create_specialized_shader the compositor runs. Our pipelines are built
/// against the same descriptor so Apple's quad and attribute layout still fit.
static void *gUberLibrary;
static void *gUberVertexDescriptor;

/// Our compiled shaders, loaded once, on the device of the library Apple is
/// building from -- a pipeline state is bound to the device that made it, and
/// taking the device from the library we were handed removes the question.
static void *vn_shader_library(void *their_lib) {
    static void *s_library = NULL;
    static bool  s_tried   = false;
    if (s_tried) return s_library;
    s_tried = true;

    void *(*msg)(void *, void *)                   = (void *(*)(void *, void *))dlsym(RTLD_DEFAULT, "objc_msgSend");
    void *(*msg2)(void *, void *, void *, void **) = (void *(*)(void *, void *, void *, void **))dlsym(RTLD_DEFAULT, "objc_msgSend");
    void *(*sel)(const char *)                     = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    if (!msg || !sel || !their_lib) return NULL;

    void *device = msg(their_lib, sel("device"));
    if (!device) return NULL;

    Dl_info di;
    char path[4096];
    if (!dladdr((void *)vn_shader_library, &di) || !di.dli_fname) return NULL;
    if (strlcpy(path, di.dli_fname, sizeof(path)) >= sizeof(path)) return NULL;
    char *macos = strstr(path, "/Contents/MacOS/");
    if (!macos) return NULL;
    *macos = '\0';
    if (strlcat(path, "/Contents/Resources/Vanish.metallib", sizeof(path)) >= sizeof(path)) return NULL;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path,
                                                           (CFIndex)strlen(path), false);
    if (!url) return NULL;

    double t0 = SLSCurrentRealTime();
    void *err = NULL;
    s_library = msg2(device, sel("newLibraryWithURL:error:"), (void *)url, &err);
    CFRelease(url);

    if (!s_library) VN_ERROR("shader: failed to load %s on device %p", path, device);
    else VN_INFO("shader: loaded %s on device %p in %.1fms", path, device,
                 (SLSCurrentRealTime() - t0) * 1000.0);
    return s_library;
}

/// One built pipeline per fragment function, ours to keep.
///
/// Apple's own cache in UberComposite is keyed on the options value, and every
/// shader animation we tag produces the same options -- so letting it cache our
/// shader would lock the first effect in for the life of the process and make
/// switching animations impossible. Keying on the fragment name instead means
/// each effect is built once and switching between them is free.
static struct { const char *frag; void *shader; } gVNShaders[8];

static void *vn_shader_for_fragment(const char *frag_name) {

    if (!frag_name || !vn_resolved_create_shader || !gUberLibrary || !gUberVertexDescriptor) {
        return NULL;
    }

    // Registry strings are static, so identity is enough and costs no strcmp on
    // the frames that hit.
    for (size_t i = 0; i < sizeof(gVNShaders) / sizeof(gVNShaders[0]); i++) {
        if (gVNShaders[i].frag == frag_name) return gVNShaders[i].shader;
    }

    void *lib = vn_shader_library(gUberLibrary);
    if (!lib) return NULL;

    CFStringRef frag_str = CFStringCreateWithCString(NULL, frag_name, kCFStringEncodingUTF8);
    void *shader = frag_str ? vn_resolved_create_shader(lib,
                                                        (void *)CFSTR("vn_uber_vertex"),
                                                        (void *)frag_str,
                                                        gUberVertexDescriptor) : NULL;
    if (frag_str) CFRelease(frag_str);

    if (!shader) {
        VN_ERROR("shader: create_shader refused '%s' -- the stock ubershader will run instead", frag_name);
        return NULL;
    }
    VN_INFO("shader: built pipeline for '%s' -> %p", frag_name, shader);

    for (size_t i = 0; i < sizeof(gVNShaders) / sizeof(gVNShaders[0]); i++) {
        if (!gVNShaders[i].frag) { gVNShaders[i].frag = frag_name; gVNShaders[i].shader = shader; break; }
    }
    return shader;
}

#pragma mark - Particle simulation

// A pass of our own inside the compositor's frame.
//
// Every other animation here is a closed form: give a pixel its phase and the
// fragment works out its colour without knowing anything about the frame
// before. A fluid cannot be written that way -- a particle's position this
// frame is its position last frame plus what the forces did in between -- so
// the state has to live on the GPU and be advanced exactly once per frame,
// before anything reads it.
//
// The frame that advances it is the compositor's own. MetalContext::
// StartComposite closes whatever is encoding and asks RenderCommandBuffer for
// the command buffer the whole frame is being built on (see
// kVNSymMetalContextStartComposite for the disassembly that says so), so
// hooking it gives us a point where that command buffer exists, nothing is
// encoding, and the render pass that will draw our clone has not started. A
// compute encoder opened there, used and ended before the original runs, puts
// the simulation inside the frame the compositor was already assembling:
//
//   * one queue and one command buffer, so there is no second timeline to keep
//     in step with and nothing to synchronise between them;
//   * Metal's own hazard tracking orders the render pass after the compute
//     pass, because the buffers are ordinary tracked resources in the same
//     command buffer -- the fragment reads what this frame's step wrote, with
//     no fences of ours;
//   * nothing runs at all when no water close is in flight.
//
// The simulation works in RENDER-TARGET PIXELS, the space a fragment's
// [[position]] arrives in, so the kernels and the fragment need no mapping
// between them. The destination gives the domain: its bounds are in points and
// scaling them by its own scale is what StartComposite hands to
// FillOrtho2DFromBounds, which is the pixel grid.

/// Kernel buffer indices; must match Water.metal.
#define kVNParticleBufferIndex 0
#define kVNBinBufferIndex      1
#define kVNSimParamsIndex      2
#define kVNObstacleIndex       3
#define kVNNeighbourIndex      4
#define kVNCensusIndex         5

/// Fragment buffer and texture indices, ours. The compositor binds only
/// buffer(0) and texture(0) on this path, and VNShaderExtra sits at 8.
#define kVNFragSimParamsIndex 11
#define kVNFragFieldTexture    1

typedef struct { unsigned long width, height, depth; } VNMTLSize;
typedef struct { unsigned long location, length; } VNNSRange;

/// objc_msgSend, cast per call shape rather than called variadically: a
/// variadic cast gets struct and float arguments wrong on arm64, and MTLSize is
/// a 24-byte struct passed by value.
static void  *(*vn_msg_id)(void *, void *);
static void  *(*vn_msg_id_p)(void *, void *, void *);
static void  *(*vn_msg_id_pp)(void *, void *, void *, void **);
static void  *(*vn_msg_id_u)(void *, void *, unsigned long);
static void  *(*vn_msg_id_uu)(void *, void *, unsigned long, unsigned long);
static void  *(*vn_msg_id_range)(void *, void *, VNNSRange);
static void   (*vn_msg_v_p)(void *, void *, void *);
static void   (*vn_msg_v_u)(void *, void *, unsigned long);
static void   (*vn_msg_v_puu)(void *, void *, void *, unsigned long, unsigned long);
static void   (*vn_msg_v_cuu)(void *, void *, const void *, unsigned long, unsigned long);
static bool   (*vn_msg_b_p)(void *, void *, void *);
static bool   (*vn_msg_b_u)(void *, void *, unsigned long);
static void  *(*vn_msg_id_uuub)(void *, void *, unsigned long, unsigned long, unsigned long, bool);
static void   (*vn_msg_v_pu)(void *, void *, void *, unsigned long);
static void   (*vn_msg_v_size2)(void *, void *, VNMTLSize, VNMTLSize);
static void   (*vn_msg_v)(void *, void *);
static double (*vn_msg_d)(void *, void *);
static unsigned long (*vn_msg_u)(void *, void *);
static void  *(*vn_sel_reg)(const char *);
static void  *(*vn_get_class)(const char *);
static void  *(*vn_pool_push)(void);
static void   (*vn_pool_pop)(void *);

static bool vn_objc_ready(void) {
    static bool s_tried, s_ok;
    if (s_tried) return s_ok;
    s_tried = true;

    void *send = dlsym(RTLD_DEFAULT, "objc_msgSend");
    vn_sel_reg   = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    vn_get_class = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    vn_pool_push = (void *(*)(void))dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPush");
    vn_pool_pop  = (void (*)(void *))dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPop");
    if (!send || !vn_sel_reg || !vn_get_class || !vn_pool_push || !vn_pool_pop) return false;

    vn_msg_id       = (void *(*)(void *, void *))send;
    vn_msg_id_p     = (void *(*)(void *, void *, void *))send;
    vn_msg_id_pp    = (void *(*)(void *, void *, void *, void **))send;
    vn_msg_id_u     = (void *(*)(void *, void *, unsigned long))send;
    vn_msg_id_uu    = (void *(*)(void *, void *, unsigned long, unsigned long))send;
    vn_msg_id_range = (void *(*)(void *, void *, VNNSRange))send;
    vn_msg_v_p      = (void (*)(void *, void *, void *))send;
    vn_msg_v_u      = (void (*)(void *, void *, unsigned long))send;
    vn_msg_v_puu    = (void (*)(void *, void *, void *, unsigned long, unsigned long))send;
    vn_msg_v_cuu    = (void (*)(void *, void *, const void *, unsigned long, unsigned long))send;
    vn_msg_b_p      = (bool (*)(void *, void *, void *))send;
    vn_msg_b_u      = (bool (*)(void *, void *, unsigned long))send;
    vn_msg_id_uuub  = (void *(*)(void *, void *, unsigned long, unsigned long, unsigned long, bool))send;
    vn_msg_v_pu     = (void (*)(void *, void *, void *, unsigned long))send;
    vn_msg_v_size2  = (void (*)(void *, void *, VNMTLSize, VNMTLSize))send;
    vn_msg_v        = (void (*)(void *, void *))send;
    vn_msg_d        = (double (*)(void *, void *))send;
    vn_msg_u        = (unsigned long (*)(void *, void *))send;
    s_ok = true;
    return true;
}

#define VN_SEL(name) ({ static void *s_sel; if (!s_sel) s_sel = vn_sel_reg(name); s_sel; })

/// Everything the simulation owns, created once on the compositor's device and
/// kept for the life of the process. Nothing here is ever released: a buffer or
/// pipeline can still be referenced by a command buffer we handed it to and no
/// longer track, and the whole allocation is under two megabytes.
/// The kernels, in the order Algorithm 1 runs them. One table rather than one
/// global apiece, so a kernel that fails to build is reported by name and the
/// dispatch sequence reads as the algorithm does.
enum {
    kVNKPredict, kVNKBinClear, kVNKBinFill, kVNKNeighbours,
    kVNKLambda, kVNKDelta, kVNKApply,
    kVNKVelocity, kVNKVorticity, kVNKVelPost, kVNKVelCommit,
    kVNKCensus, kVNKField, kVNKBlurX, kVNKBlurY,
    kVNKernelCount
};

static const char * const kVNKernelNames[kVNKernelCount] = {
    "vn_pbf_predict", "vn_pbf_bin_clear", "vn_pbf_bin_fill", "vn_pbf_neighbours",
    "vn_pbf_lambda", "vn_pbf_delta", "vn_pbf_apply",
    "vn_pbf_velocity", "vn_pbf_vorticity", "vn_pbf_vel_post", "vn_pbf_vel_commit",
    "vn_pbf_census", "vn_field_build", "vn_field_blur_x", "vn_field_blur_y",
};

/// Everything the simulation owns, created once on the compositor's device and
/// kept for the life of the process. Nothing here is ever released: a buffer,
/// texture or pipeline can still be referenced by a command buffer we handed it
/// to and no longer track, and the whole allocation is a few megabytes.
static void *gVNSimDevice;
static void *gVNSimPipes[kVNKernelCount];
static void *gVNSimBins;        // MTLBuffer, the bin table -- scratch, shared by every simulation
static void *gVNSimNeighbours;  // MTLBuffer, the flat neighbour list and its counts -- likewise
static void *gVNSimScratch;     // MTLTexture, the blurs' intermediate -- likewise
static void *gVNSimTimestamps;  // MTLCounterSampleBuffer, or NULL when unavailable
static uint32_t gVNSimSampleSlot;
static bool  gVNSimBroken;      // a failure that retrying would only repeat

/// Frames of census results in flight per simulation. Each frame counts into
/// its own entry and the CPU reads it back when that frame's command buffer
/// completes, long before the ring comes round to it again.
#define kVNCensusRing 4u

/// What one water slot has on the GPU, and what the draw path has to know
/// about its last dispatch.
///
/// The draw path is handed constants plus the few values that vary, never a
/// copy of a whole struct. That is not tidiness: the step writes on the
/// compositor's thread and the draw reads on it, and a torn read of `field_w`
/// or `iso` would let the fragment sample outside the field, which on this GPU
/// means faulting inside WindowServer. Constants cannot tear, and each value
/// that varies is published whole in one atomic.
typedef struct {
    void *particles;                  // MTLBuffer, kVNParticleCountMax x VNParticle
    void *field;                      // MTLTexture, RGBA16Float
    void *census;                     // MTLBuffer, shared, kVNCensusRing x uint32
    uint32_t census_next;
    _Atomic uint32_t census_live;     // particles still on screen, as last read back
    _Atomic uint32_t census_generation;   // the close that count belongs to
    _Atomic uint32_t stepped_generation;  // the close whose water the buffers hold
    _Atomic(void *)  stepped_dest;
    _Atomic uint64_t domain_bits;
    _Atomic uint64_t seeded_at_bits;  // double: when the fluid was seeded
} VNSimSlot;

static VNSimSlot gVNSims[kVNWaterSlots];
static void         *gVNSimSteppedCmdBuf;
static double        gVNSimLastStepTime;
static os_unfair_lock gVNSimLock = OS_UNFAIR_LOCK_INIT;

/// GPU time for the compute pass, resolved on the command buffer's completion
/// and summarised when the close ends.
static struct {
    uint64_t steps, sum_ns, min_ns, max_ns, unavailable;
    double   frame_sum_ms;
} gVNSimStats;
static os_unfair_lock gVNSimStatsLock = OS_UNFAIR_LOCK_INIT;

static void vn_particles_report(void) {
    os_unfair_lock_lock(&gVNSimStatsLock);
    const uint64_t n = gVNSimStats.steps;
    const uint64_t sum = gVNSimStats.sum_ns, lo = gVNSimStats.min_ns, hi = gVNSimStats.max_ns;
    const uint64_t missing = gVNSimStats.unavailable;
    const double frame_sum = gVNSimStats.frame_sum_ms;
    memset(&gVNSimStats, 0, sizeof(gVNSimStats));
    os_unfair_lock_unlock(&gVNSimStatsLock);

    if (n == 0) {
        if (missing) VN_INFO("water: %llu step(s) completed, none with GPU timing", missing);
        return;
    }
    VN_INFO("water: %llu step(s) on the GPU -- min %.3fms avg %.3fms max %.3fms; "
            "whole frame avg %.3fms (%llu untimed)",
            n, lo / 1e6, (double)sum / (double)n / 1e6, hi / 1e6, frame_sum / (double)n, missing);
}

/// Resolves the two timestamps the compute pass sampled, and the whole frame's
/// GPU time alongside them.
///
/// The timestamps come back in NANOSECONDS, not in mach_absolute_time ticks.
///
/// This was the other way round to begin with, converted through the mach
/// timebase -- which on this machine is 125/3, so every step was reported 41.67
/// times too long: 78ms of compute inside a 2.8ms frame. That impossibility is
/// what the frame's own GPUStartTime/GPUEndTime are logged beside it for, and
/// they are kept for exactly that reason: they arrive in seconds and rest on no
/// assumption, so they are the check on the number that does.
/// Up to this many steps may be in flight with their timestamps unresolved.
/// The sample buffer holds a pair of slots for each: resolving a frame's pair
/// after the next frame had written over it is the one way to get numbers that
/// look plausible and mean nothing.
#define kVNTimestampSlots 4u

static void vn_particles_resolve_timing(void *cmdbuf, uint32_t slot) {
    void *pool = vn_pool_push();
    double frame_ms = 0.0;
    if (cmdbuf) {
        const double t0 = vn_msg_d(cmdbuf, VN_SEL("GPUStartTime"));
        const double t1 = vn_msg_d(cmdbuf, VN_SEL("GPUEndTime"));
        if (t1 > t0) frame_ms = (t1 - t0) * 1000.0;
    }

    uint64_t ns = 0;
    void *sample = gVNSimTimestamps;
    if (sample) {
        void *data = vn_msg_id_range(sample, VN_SEL("resolveCounterRange:"),
                                     (VNNSRange){ slot * 2u, 2 });
        const uint64_t *ts = data ? (const uint64_t *)vn_msg_id(data, VN_SEL("bytes")) : NULL;
        if (ts && ts[0] != UINT64_MAX && ts[1] != UINT64_MAX && ts[1] > ts[0]) {
            ns = ts[1] - ts[0];
        }
    }

    os_unfair_lock_lock(&gVNSimStatsLock);
    if (ns > 0) {
        if (gVNSimStats.steps == 0 || ns < gVNSimStats.min_ns) gVNSimStats.min_ns = ns;
        if (ns > gVNSimStats.max_ns) gVNSimStats.max_ns = ns;
        gVNSimStats.steps++;
        gVNSimStats.sum_ns += ns;
        gVNSimStats.frame_sum_ms += frame_ms;
    } else {
        gVNSimStats.unavailable++;
    }
    os_unfair_lock_unlock(&gVNSimStatsLock);

    VN_DEBUG("water: step %.3fms on the GPU, frame %.3fms", ns / 1e6, frame_ms);
    vn_pool_pop(pool);
}

/// The timestamp counter set, if this device samples one at encoder
/// boundaries. Timing is a diagnostic: everything here may return NULL and the
/// simulation runs untimed.
static void *vn_particles_make_timestamps(void *device) {
    void **name_ref = (void **)dlsym(RTLD_DEFAULT, "MTLCommonCounterSetTimestamp");
    void *wanted = name_ref ? *name_ref : NULL;
    if (!wanted) return NULL;

    // MTLCounterSamplingPointAtStageBoundary. A device that cannot sample there
    // may do more than return nil from the encoder call, so ask first.
    if (!vn_msg_b_u(device, VN_SEL("supportsCounterSampling:"), 0)) return NULL;

    void *sets = vn_msg_id(device, VN_SEL("counterSets"));
    if (!sets) return NULL;

    void *found = NULL;
    const unsigned long n = vn_msg_u(sets, VN_SEL("count"));
    for (unsigned long i = 0; i < n && !found; i++) {
        void *set = vn_msg_id_u(sets, VN_SEL("objectAtIndexedSubscript:"), i);
        void *name = set ? vn_msg_id(set, VN_SEL("name")) : NULL;
        if (name && vn_msg_b_p(name, VN_SEL("isEqualToString:"), wanted)) {
            found = set;
        }
    }
    if (!found) return NULL;

    void *cls = vn_get_class("MTLCounterSampleBufferDescriptor");
    void *desc = cls ? vn_msg_id(vn_msg_id(cls, VN_SEL("alloc")), VN_SEL("init")) : NULL;
    if (!desc) return NULL;

    vn_msg_v_p(desc, VN_SEL("setCounterSet:"), found);
    vn_msg_v_u(desc, VN_SEL("setSampleCount:"), kVNTimestampSlots * 2u);
    vn_msg_v_u(desc, VN_SEL("setStorageMode:"), 0 /* MTLStorageModeShared */);

    void *err = NULL;
    void *buf = vn_msg_id_pp(device, VN_SEL("newCounterSampleBufferWithDescriptor:error:"), desc, &err);
    vn_msg_v(desc, VN_SEL("release"));
    return buf;
}

static void *vn_particles_pipeline(void *device, void *library, const char *fn_name) {
    CFStringRef name = CFStringCreateWithCString(NULL, fn_name, kCFStringEncodingUTF8);
    if (!name) return NULL;
    void *fn = vn_msg_id_p(library, VN_SEL("newFunctionWithName:"), (void *)name);
    CFRelease(name);
    if (!fn) { VN_ERROR("water: Vanish.metallib has no kernel '%s'", fn_name); return NULL; }

    void *err = NULL;
    void *pipe = vn_msg_id_pp(device, VN_SEL("newComputePipelineStateWithFunction:error:"), fn, &err);
    vn_msg_v(fn, VN_SEL("release"));
    if (!pipe) VN_ERROR("water: no compute pipeline for '%s'", fn_name);
    return pipe;
}

/// A texture for the surface field: fixed size, so it is allocated once and no
/// display can force a resize -- a resize would mean releasing a texture that
/// may still be in flight in a command buffer we no longer own.
static void *vn_particles_field_texture(void *device) {
    void *cls = vn_get_class("MTLTextureDescriptor");
    if (!cls) return NULL;
    // 115 = MTLPixelFormatRGBA16Float. Half is the right width for a density
    // field normalised to rest: the values sit around 1.0, where half has far
    // more precision than a surface needs.
    void *desc = vn_msg_id_uuub(cls, VN_SEL("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"),
                                115, kVNFieldDim, kVNFieldDim, false);
    if (!desc) return NULL;
    vn_msg_v_u(desc, VN_SEL("setUsage:"), 1 | 2);          // ShaderRead | ShaderWrite
    vn_msg_v_u(desc, VN_SEL("setStorageMode:"), 2);        // Private
    return vn_msg_id_p(device, VN_SEL("newTextureWithDescriptor:"), desc);
}

/// Builds the pipelines, buffers and field textures, once, on the device the
/// frame is being built for. Returns false while the pieces are not there yet --
/// the shader library is captured from the compositor's own first ubershader
/// build, which on the very first water close happens a frame or two after this
/// first runs.
static bool vn_particles_ensure(void *device) {
    if (gVNSimBroken || !device) return false;
    if (gVNSimScratch && gVNSimDevice == device) return true;

    if (gVNSimDevice && gVNSimDevice != device) {
        VN_ERROR("water: a second Metal device (%p, had %p) -- not simulating on it", device, gVNSimDevice);
        gVNSimBroken = true;
        return false;
    }

    void *library = gUberLibrary ? vn_shader_library(gUberLibrary) : NULL;
    if (!library) {
        VN_DEBUG("water: no shader library yet -- skipping this frame's step");
        return false;
    }
    if (vn_msg_id(library, VN_SEL("device")) != device) {
        VN_ERROR("water: the shader library is not on the frame's device -- not simulating");
        gVNSimBroken = true;
        return false;
    }

    const double t0 = SLSCurrentRealTime();
    for (int i = 0; i < kVNKernelCount; i++) {
        gVNSimPipes[i] = vn_particles_pipeline(device, library, kVNKernelNames[i]);
        if (!gVNSimPipes[i]) { gVNSimBroken = true; return false; }
    }

    // Private storage for everything but the census: nothing on the CPU reads
    // or writes the rest. The particles are seeded by the predict kernel's own
    // spawn branch, which is why there is no upload here and no
    // synchronisation to go with it.
    const unsigned long particle_bytes  = (unsigned long)kVNParticleCountMax * sizeof(VNParticle);
    const unsigned long bin_bytes       = (unsigned long)kVNBinDimMax * kVNBinDimMax * kVNBinStride * sizeof(uint32_t);
    const unsigned long neighbour_bytes = ((unsigned long)kVNParticleCountMax * kVNMaxNeighbours
                                           + kVNParticleCountMax) * sizeof(uint32_t);
    gVNSimBins       = vn_msg_id_uu(device, VN_SEL("newBufferWithLength:options:"), bin_bytes, 0x20);
    gVNSimNeighbours = vn_msg_id_uu(device, VN_SEL("newBufferWithLength:options:"), neighbour_bytes, 0x20);
    void *scratch    = vn_particles_field_texture(device);
    bool ok = gVNSimBins && gVNSimNeighbours && scratch;
    for (int i = 0; i < kVNWaterSlots && ok; i++) {
        gVNSims[i].particles = vn_msg_id_uu(device, VN_SEL("newBufferWithLength:options:"), particle_bytes, 0x20);
        gVNSims[i].field     = vn_particles_field_texture(device);
        // Shared: the CPU reads it back. A new buffer is zeroed, which is the
        // state the ring expects every entry to be in before it is counted into.
        gVNSims[i].census    = vn_msg_id_uu(device, VN_SEL("newBufferWithLength:options:"),
                                            kVNCensusRing * sizeof(uint32_t), 0);
        ok = gVNSims[i].particles && gVNSims[i].field && gVNSims[i].census;
    }
    if (!ok) {
        VN_ERROR("water: could not allocate the simulation on device %p", device);
        gVNSimBroken = true;
        return false;
    }

    gVNSimTimestamps = vn_particles_make_timestamps(device);
    gVNSimDevice = device;
    gVNSimScratch = scratch;   // last: it is what says the rest exists
    VN_INFO("water: %d simulations of up to %u particles (%lu bytes and a %ux%u field each), "
            "%lu + %lu bytes of shared scratch, on device %p in %.1fms; GPU timing %s",
            kVNWaterSlots, kVNParticleCountMax, particle_bytes, kVNFieldDim, kVNFieldDim,
            bin_bytes, neighbour_bytes, device, (SLSCurrentRealTime() - t0) * 1000.0,
            gVNSimTimestamps ? "on" : "unavailable");
    return true;
}

/// Opens a compute encoder on the frame's command buffer, sampling the GPU's
/// timestamp counter around it when the device has one.
static void *vn_particles_encoder(void *cmdbuf, uint32_t slot) {
    if (gVNSimTimestamps) {
        void *cls = vn_get_class("MTLComputePassDescriptor");
        void *desc = cls ? vn_msg_id(cls, VN_SEL("computePassDescriptor")) : NULL;
        void *attachments = desc ? vn_msg_id(desc, VN_SEL("sampleBufferAttachments")) : NULL;
        void *attachment = attachments ? vn_msg_id_u(attachments, VN_SEL("objectAtIndexedSubscript:"), 0) : NULL;
        if (attachment) {
            vn_msg_v_p(attachment, VN_SEL("setSampleBuffer:"), gVNSimTimestamps);
            vn_msg_v_u(attachment, VN_SEL("setStartOfEncoderSampleIndex:"), slot * 2u);
            vn_msg_v_u(attachment, VN_SEL("setEndOfEncoderSampleIndex:"), slot * 2u + 1u);
            void *enc = vn_msg_id_p(cmdbuf, VN_SEL("computeCommandEncoderWithDescriptor:"), desc);
            if (enc) return enc;
        }
    }
    return vn_msg_id(cmdbuf, VN_SEL("computeCommandEncoder"));
}

static void vn_dispatch_1d(void *enc, int kernel, uint32_t threads) {
    if (threads == 0) return;
    const unsigned long group = 64;
    vn_msg_v_p(enc, VN_SEL("setComputePipelineState:"), gVNSimPipes[kernel]);
    vn_msg_v_size2(enc, VN_SEL("dispatchThreadgroups:threadsPerThreadgroup:"),
                   (VNMTLSize){ (threads + group - 1) / group, 1, 1 },
                   (VNMTLSize){ group, 1, 1 });
}

static void vn_dispatch_field(void *enc, int kernel, void *src, void *dst) {
    vn_msg_v_p(enc, VN_SEL("setComputePipelineState:"), gVNSimPipes[kernel]);
    if (src) vn_msg_v_pu(enc, VN_SEL("setTexture:atIndex:"), src, 0);
    if (dst) vn_msg_v_pu(enc, VN_SEL("setTexture:atIndex:"), dst, 1);
    const unsigned long g = 8, n = kVNFieldDim / 8;
    vn_msg_v_size2(enc, VN_SEL("dispatchThreadgroups:threadsPerThreadgroup:"),
                   (VNMTLSize){ n, n, 1 }, (VNMTLSize){ g, g, 1 });
}

static uint64_t vn_bits_d(double v) { uint64_t b; memcpy(&b, &v, sizeof(b)); return b; }
static double   vn_d_bits(uint64_t b) { double v; memcpy(&v, &b, sizeof(v)); return v; }

/// One simulation's frame of fluid, into an encoder that already has the
/// shared scratch bound. Everything it touches that other simulations also
/// touch -- the bins, the neighbour list, the blur's intermediate -- it is
/// done with before the next one's dispatches start: dispatches within a
/// compute encoder are ordered by Metal, the default dispatch type being
/// serial, so running simulations one after another is all the isolation
/// they need.
static void vn_particles_encode_slot(void *enc, int slot_index, const VNWaterSnapshot *water,
                                     const int32_t *b, float scale, double frame_dt, double now,
                                     void *destination) {
    VNSimSlot *sim = &gVNSims[slot_index];

    // The window, the display and the obstacles all arrive in points; the fluid
    // lives in the render target's pixels, which is the same rectangle scaled
    // about the destination's own origin.
    const double w_pt = (double)(b[2] - b[0]), h_pt = (double)(b[3] - b[1]);
    const double ox = (double)b[0], oy = (double)b[1];
    VNSimParams sp = {0};
    sp.domain_x = (float)(w_pt * scale);
    sp.domain_y = (float)(h_pt * scale);
    sp.spawn_min_x = (float)((CGRectGetMinX(water->window_pt) - ox) * scale);
    sp.spawn_min_y = (float)((CGRectGetMinY(water->window_pt) - oy) * scale);
    sp.spawn_max_x = (float)((CGRectGetMaxX(water->window_pt) - ox) * scale);
    sp.spawn_max_y = (float)((CGRectGetMaxY(water->window_pt) - oy) * scale);

    // A window larger than the destination, or off it entirely, would seed the
    // fluid outside the domain and leave it pinned to an edge.
    if (sp.spawn_min_x < 0.0f) sp.spawn_min_x = 0.0f;
    if (sp.spawn_min_y < 0.0f) sp.spawn_min_y = 0.0f;
    if (sp.spawn_max_x > sp.domain_x) sp.spawn_max_x = sp.domain_x;
    if (sp.spawn_max_y > sp.domain_y) sp.spawn_max_y = sp.domain_y;
    if (sp.spawn_max_x < sp.spawn_min_x + 16.0f) sp.spawn_max_x = sp.spawn_min_x + 16.0f;
    if (sp.spawn_max_y < sp.spawn_min_y + 16.0f) sp.spawn_max_y = sp.spawn_min_y + 16.0f;

    sp.dt      = (float)(frame_dt / (double)kVNSubSteps);
    sp.gravity = kVNGravityPx * scale;
    sp.seed    = (float)(water->generation % 977u) + 0.5f;
    sp.valid   = 1u;
    vn_water_derive(&sp, sp.spawn_max_x - sp.spawn_min_x, sp.spawn_max_y - sp.spawn_min_y);
    sp.tint    = water->tint;   // derive fills in the default; the preference wins
    sp.drains  = water->drains;
    sp.fade    = 1.0f;

    // The windows the fluid has to flow around, converted the same way.
    VNObstacle obstacles[kVNMaxObstacles] = {0};
    uint32_t nobs = water->obstacle_count <= kVNMaxObstacles ? water->obstacle_count : kVNMaxObstacles;
    for (uint32_t i = 0; i < nobs; i++) {
        const CGRect r = water->obstacles[i];
        obstacles[i].min_x  = (float)((CGRectGetMinX(r) - ox) * scale);
        obstacles[i].min_y  = (float)((CGRectGetMinY(r) - oy) * scale);
        obstacles[i].max_x  = (float)((CGRectGetMaxX(r) - ox) * scale);
        obstacles[i].max_y  = (float)((CGRectGetMaxY(r) - oy) * scale);
        obstacles[i].corner = (float)(12.0 * scale);
    }
    sp.obstacles = nobs;

    const bool seeding = water->generation != atomic_load_explicit(&sim->stepped_generation, memory_order_relaxed);
    if (seeding) {
        atomic_store_explicit(&sim->seeded_at_bits, vn_bits_d(now), memory_order_relaxed);
        if (now - water->start > 0.05) {
            VN_INFO("water: slot %d seeded %.0fms into its close", slot_index, (now - water->start) * 1000.0);
        }
    }

    vn_msg_v_puu(enc, VN_SEL("setBuffer:offset:atIndex:"), sim->particles, 0, kVNParticleBufferIndex);
    vn_msg_v_cuu(enc, VN_SEL("setBytes:length:atIndex:"), obstacles, sizeof(obstacles), kVNObstacleIndex);

    // Algorithm 1, substepped.
    for (uint32_t sub = 0; sub < kVNSubSteps; sub++) {
        sp.spawn = (seeding && sub == 0) ? 1u : 0u;
        vn_msg_v_cuu(enc, VN_SEL("setBytes:length:atIndex:"), &sp, sizeof(sp), kVNSimParamsIndex);

        vn_dispatch_1d(enc, kVNKPredict, sp.count);
        if (sp.spawn) continue;                     // the seeding step does no solve

        vn_dispatch_1d(enc, kVNKBinClear, sp.bin_w * sp.bin_h);
        vn_dispatch_1d(enc, kVNKBinFill, sp.count);
        vn_dispatch_1d(enc, kVNKNeighbours, sp.count);
        for (uint32_t it = 0; it < kVNSolverIterations; it++) {
            vn_dispatch_1d(enc, kVNKLambda, sp.count);
            vn_dispatch_1d(enc, kVNKDelta, sp.count);
            vn_dispatch_1d(enc, kVNKApply, sp.count);
        }
        vn_dispatch_1d(enc, kVNKVelocity, sp.count);
        vn_dispatch_1d(enc, kVNKVorticity, sp.count);
        vn_dispatch_1d(enc, kVNKVelPost, sp.count);
        vn_dispatch_1d(enc, kVNKVelCommit, sp.count);
    }

    // The field is built from final positions, so the bins have to describe
    // those -- the last solve binned predicted ones.
    sp.spawn = 0u;
    vn_msg_v_cuu(enc, VN_SEL("setBytes:length:atIndex:"), &sp, sizeof(sp), kVNSimParamsIndex);

    const uint32_t ring = sim->census_next++ % kVNCensusRing;
    vn_msg_v_puu(enc, VN_SEL("setBuffer:offset:atIndex:"), sim->census, ring * sizeof(uint32_t), kVNCensusIndex);
    vn_dispatch_1d(enc, kVNKCensus, sp.count);

    vn_dispatch_1d(enc, kVNKBinClear, sp.bin_w * sp.bin_h);
    vn_dispatch_1d(enc, kVNKBinFill, sp.count);
    vn_dispatch_field(enc, kVNKField, sim->field, NULL);
    vn_dispatch_field(enc, kVNKBlurX, sim->field, gVNSimScratch);
    vn_dispatch_field(enc, kVNKBlurY, gVNSimScratch, sim->field);

    atomic_store_explicit(&sim->stepped_generation, water->generation, memory_order_release);
    uint32_t dx, dy;
    memcpy(&dx, &sp.domain_x, sizeof(dx));
    memcpy(&dy, &sp.domain_y, sizeof(dy));
    atomic_store_explicit(&sim->domain_bits, ((uint64_t)dy << 32) | dx, memory_order_relaxed);
    atomic_store_explicit(&sim->stepped_dest, destination, memory_order_release);

    if (seeding) {
        VN_INFO("water: slot %d destination %p kind=%u bounds=(%d,%d %d,%d) scale=%.2f -> domain %.0fx%.0f px; "
                "%u particles spaced %.1fpx, h=%.1fpx, rho0=%.4g, %u bins of %.0fpx, %u obstacle(s)",
                slot_index, destination, *(const uint32_t *)((const char *)destination + kVNDestinationKindOffset),
                b[0], b[1], b[2], b[3], (double)scale, (double)sp.domain_x, (double)sp.domain_y,
                sp.count, (double)(sp.h / kVNSmoothingRatio), (double)sp.h, (double)sp.rho0,
                sp.bin_w * sp.bin_h, (double)sp.cell, nobs);
    }
}

/// Reads back one simulation's census once the frame that counted it is done.
static void vn_particles_read_census(int slot_index, uint32_t ring, uint32_t generation) {
    VNSimSlot *sim = &gVNSims[slot_index];
    uint32_t *counts = (uint32_t *)vn_msg_id(sim->census, VN_SEL("contents"));
    if (!counts) return;
    const uint32_t live = counts[ring];
    counts[ring] = 0;   // ready for the frame that next counts into it
    atomic_store_explicit(&sim->census_live, live, memory_order_relaxed);
    atomic_store_explicit(&sim->census_generation, generation, memory_order_release);
}

/// One frame of fluid for every water close on screen, encoded into the
/// command buffer StartComposite is about to open its render pass on.
static void vn_particles_step(void *context, void *destination) {
    if (!vn_objc_ready() || gVNSimBroken) return;
    if (!vn_resolved_end_encoders || !vn_resolved_render_command_buffer) return;

    // The destination has to be a display-sized one. The compositor opens
    // render passes into small offscreen surfaces as well, and their pixel grid
    // is not the one the clone is finally drawn in.
    const int32_t *b = (const int32_t *)((const char *)destination + kVNDestinationBoundsOffset);
    float scale = *(const float *)((const char *)destination + kVNDestinationScaleOffset);
    if (!(scale > 0.0f)) scale = 1.0f;
    const double w_pt = (double)(b[2] - b[0]), h_pt = (double)(b[3] - b[1]);
    if (w_pt < 512.0 || h_pt < 512.0) return;

    const double now = SLSCurrentRealTime();

    // Which slots have a frame to simulate. A clone is built on mouse-up and
    // may sit there a while before the app gets round to closing the window --
    // or be discarded without ever animating. Simulating through that would
    // land the fluid on the floor before the first frame anyone sees, so a slot
    // waits for its animation to start. Nor is a slot stepped past its end: it
    // is faded out by then and about to be released.
    VNWaterSnapshot water[kVNWaterSlots];
    bool due[kVNWaterSlots] = { false };
    bool any = false;
    for (int i = 0; i < kVNWaterSlots; i++) {
        water[i] = vn_water_snapshot(&gVNWaterSlots[i]);
        if (!water[i].valid || !(water[i].start < INFINITY)) continue;

        // All of it has drained: the close is over, whatever its duration said.
        VNSimSlot *sim = &gVNSims[i];
        if (atomic_load_explicit(&sim->census_generation, memory_order_acquire) == water[i].generation &&
            atomic_load_explicit(&sim->census_live, memory_order_relaxed) == 0u) {
            vn_water_end_by(&gVNWaterSlots[i], now);
            continue;
        }
        if (now >= atomic_load_explicit(&gVNWaterSlots[i].end, memory_order_acquire)) continue;
        due[i] = any = true;
    }
    if (!any) return;

    os_unfair_lock_lock(&gVNSimLock);

    // One step per DISPLAYED FRAME.
    //
    // A new command buffer is not a new frame. The compositor flushes and
    // starts another one several times per refresh, and the first build of
    // this gated on the command buffer alone: it stepped 613 times in a close
    // that should have had about 144, so the fluid lived four seconds for
    // every one on screen. It was on the floor and sloshed into the corners a
    // quarter of a second in, which is why the water had no relation left to
    // the window it came from.
    //
    // The clock is the gate instead, against the same refresh interval the
    // animation tick runs on.
    double min_interval = gAnimFrameInterval > 0.0 ? gAnimFrameInterval * 0.9 : (0.9 / 120.0);
    if (gVNSimLastStepTime > 0.0 && (now - gVNSimLastStepTime) < min_interval) {
        os_unfair_lock_unlock(&gVNSimLock);
        return;
    }

    void *cmdbuf = vn_resolved_render_command_buffer(context, (const void *[3]){ NULL, NULL, NULL });
    if (!cmdbuf || cmdbuf == gVNSimSteppedCmdBuf) {
        os_unfair_lock_unlock(&gVNSimLock);
        return;
    }

    void *pool = vn_pool_push();
    void *device = vn_msg_id(cmdbuf, VN_SEL("device"));
    if (!vn_particles_ensure(device)) {
        vn_pool_pop(pool);
        os_unfair_lock_unlock(&gVNSimLock);
        return;
    }

    double frame_dt = gVNSimLastStepTime > 0.0 ? now - gVNSimLastStepTime : 1.0 / 60.0;
    if (frame_dt < 1.0 / 1000.0) frame_dt = 1.0 / 1000.0;
    if (frame_dt > 1.0 / 30.0)   frame_dt = 1.0 / 30.0;   // a stall must not teleport the fluid

    // Nothing may be encoding when a compute encoder is opened on the command
    // buffer. StartComposite closes both encoders itself a moment from now, so
    // this costs the frame nothing it was not about to pay, and it is the very
    // call StartComposite makes.
    vn_resolved_end_encoders(context);

    const uint32_t ts_slot = gVNSimSampleSlot++ % kVNTimestampSlots;
    void *enc = vn_particles_encoder(cmdbuf, ts_slot);
    if (enc) {
        vn_msg_v_puu(enc, VN_SEL("setBuffer:offset:atIndex:"), gVNSimBins, 0, kVNBinBufferIndex);
        vn_msg_v_puu(enc, VN_SEL("setBuffer:offset:atIndex:"), gVNSimNeighbours, 0, kVNNeighbourIndex);

        uint32_t rings[kVNWaterSlots], gens[kVNWaterSlots];
        for (int i = 0; i < kVNWaterSlots; i++) {
            if (!due[i]) continue;
            rings[i] = gVNSims[i].census_next % kVNCensusRing;
            gens[i]  = water[i].generation;
            vn_particles_encode_slot(enc, i, &water[i], b, scale, frame_dt, now, destination);
        }
        vn_msg_v(enc, VN_SEL("endEncoding"));

        // The census, and the timing, are read back when the frame completes.
        const bool d0 = due[0], d1 = due[1], d2 = due[2];
        const uint32_t r0 = rings[0], r1 = rings[1], r2 = rings[2];
        const uint32_t g0 = gens[0], g1 = gens[1], g2 = gens[2];
        const bool timed = gVNSimTimestamps != NULL;
        _Static_assert(kVNWaterSlots == 3, "the completion handler captures one census per slot");
        vn_msg_v_p(cmdbuf, VN_SEL("addCompletedHandler:"), (void *)^(void *done) {
            if (d0) vn_particles_read_census(0, r0, g0);
            if (d1) vn_particles_read_census(1, r1, g1);
            if (d2) vn_particles_read_census(2, r2, g2);
            if (timed) vn_particles_resolve_timing(done, ts_slot);
        });

        gVNSimSteppedCmdBuf = cmdbuf;
        gVNSimLastStepTime  = now;
    } else {
        VN_ERROR("water: no compute encoder on command buffer %p", cmdbuf);
        gVNSimBroken = true;
    }
    vn_pool_pop(pool);

    os_unfair_lock_unlock(&gVNSimLock);
}

/// Hands the fragment the simulation it is about to draw. Bound on every draw
/// of one of our pipelines, whether or not it is the water one: the other
/// fragments do not declare the parameters, while a water fragment drawn with
/// them missing would read unbound memory.
///
/// `layer_seed` is the drawn layer's filter params[0], which is how it finds
/// its slot; `bound` says whether it came from a layer at all. `valid` keeps
/// the fragment away from a field before its close's first step has filled it,
/// and away from a frame whose step ran against a different destination --
/// where a particle's coordinates and a pixel's would not be in the same space.
static void vn_particles_bind_fragment(void *encoder, void *destination,
                                       float layer_seed, bool bound) {
    // vn_objc_ready() first, and on this path specifically: the draw arrives
    // here without going through the step, so nothing else has resolved the
    // message-send pointers. Resolving them at load makes this a load of a
    // static bool, but the guard stays -- getting it wrong once meant every
    // shader animation branched to address zero inside WindowServer.
    if (!encoder || !vn_objc_ready()) return;

    // Constants, plus the values that vary.
    VNSimParams sp = {0};
    sp.count     = kVNParticleCountMax;
    sp.bin_slots = kVNBinSlots;
    sp.field_w   = kVNFieldDim;
    sp.field_h   = kVNFieldDim;
    sp.iso       = kVNIsoLevel;
    sp.fade      = 1.0f;

    int found = -1;
    bool torn = false;
    VNWaterSnapshot water = {0};
    for (int i = 0; bound && i < kVNWaterSlots && found < 0; i++) {
        const VNWaterSnapshot w = vn_water_snapshot(&gVNWaterSlots[i]);
        if (!w.valid) { torn |= (atomic_load_explicit(&gVNWaterSlots[i].seq, memory_order_relaxed) & 1u) != 0; continue; }
        if (w.seed == layer_seed) { found = i; water = w; }
    }

    // A water layer whose slot a newer close has taken: its water is gone, and
    // drawing the untouched window instead would bring the window back. A slot
    // midway through being written is not that, and gets the window for the
    // one frame.
    void *field = NULL;
    if (bound && found < 0 && !torn && gVNSimScratch) {
        sp.valid = 2u;
    } else if (found >= 0 && gVNSimScratch) {
        VNSimSlot *sim = &gVNSims[found];
        field = sim->field;
        void *stepped = atomic_load_explicit(&sim->stepped_dest, memory_order_acquire);
        const bool fresh = atomic_load_explicit(&sim->stepped_generation, memory_order_acquire) == water.generation;
        if (fresh && stepped == destination) {
            const uint64_t d = atomic_load_explicit(&sim->domain_bits, memory_order_relaxed);
            const uint32_t dx = (uint32_t)d, dy = (uint32_t)(d >> 32);
            memcpy(&sp.domain_x, &dx, sizeof(dx));
            memcpy(&sp.domain_y, &dy, sizeof(dy));
            const double now = SLSCurrentRealTime();
            const double seeded = vn_d_bits(atomic_load_explicit(&sim->seeded_at_bits, memory_order_relaxed));
            const double end = atomic_load_explicit(&gVNWaterSlots[found].end, memory_order_acquire);
            const double fade = (end - now) / kVNWaterFadeSeconds;
            sp.age   = (float)fmax(0.0, now - seeded);
            sp.fade  = (float)(fade < 0.0 ? 0.0 : (fade > 1.0 ? 1.0 : fade));
            sp.tint  = water.tint;
            sp.valid = 1u;
        } else if (fresh && stepped && stepped != destination) {
            // A layer drawn into a destination the step did not run against. The
            // two pointers are the same object on the path a clone takes, so this
            // firing at all says the composite is reaching our layer some other
            // way -- worth one line, because the symptom is a water close that
            // simulates and draws nothing.
            static void *s_reported;
            if (s_reported != destination) {
                s_reported = destination;
                VN_INFO("water: layer destination %p is not the stepped one %p -- not drawing the fluid",
                        destination, stepped);
            }
        }
    }

    vn_msg_v_cuu(encoder, VN_SEL("setFragmentBytes:length:atIndex:"), &sp, sizeof(sp), kVNFragSimParamsIndex);
    if (!field && gVNSimScratch) field = gVNSims[0].field;   // something must be bound; valid says not to read it
    if (field) {
        vn_msg_v_pu(encoder, VN_SEL("setFragmentTexture:atIndex:"), field, kVNFragFieldTexture);
    }
}

/// The hook. Everything above it runs only for the frames of a water close;
/// every other frame pays this one load.
static void vn_hook_start_composite(void *context, void *destination, uint64_t load, uint64_t store) {
    if (destination && atomic_load_explicit(&gVNWaterClones, memory_order_acquire) > 0) {
        vn_particles_step(context, destination);
    }
    vn_orig_start_composite(context, destination, load, store);
}

#pragma mark - Per-window shader arguments

/// The fragment buffer index VNShaderExtra is bound at. Must match
/// kVNShaderExtraIndex in Common.metal. The compositor binds only buffer(0) on
/// this path, so this index is ours.
#define kVNShaderExtraIndex 8

/// Must match VNShaderExtra in Common.metal.
typedef struct {
    float params[5];   // the layer's filter params, set per close in vn_make_clone
    float bound;       // 1 when params came from a layer; 0 leaves the shader on its defaults
    float phase;       // the animation's phase when this frame is drawn; -1 = use the tick's
    float offset[2];   // the window's offset inside the quad, in window sizes; -1 = default
} VNShaderExtra;

/// True once every hook the argument buffer needs is in place. Our fragment
/// functions declare that buffer, and drawing one without it bound reads
/// unbound GPU memory, so without all of them we do not substitute at all.
static bool gVNShaderArgsReady;

/// The layer MetalCompositeLayer is drawing on this thread, so SetPipelineState
/// can find its params. The compositor draws on more than one thread.
static __thread void *tl_vn_layer;

/// The destination that layer is being drawn into, so the water fragment can be
/// told whether the simulation it would read was stepped in this same pixel
/// grid.
static __thread void *tl_vn_destination;

/// Pipeline states made from our MetalShaders, recorded as CopyPipelineState
/// hands them out.
static _Atomic(void *) gVNPipelines[16];

static bool vn_is_our_shader(void *shader) {
    for (size_t i = 0; i < sizeof(gVNShaders) / sizeof(gVNShaders[0]); i++) {
        if (gVNShaders[i].shader == shader) return shader != NULL;
    }
    return false;
}

static bool vn_is_our_pipeline(void *pipeline) {
    for (size_t i = 0; i < sizeof(gVNPipelines) / sizeof(gVNPipelines[0]); i++) {
        void *p = atomic_load_explicit(&gVNPipelines[i], memory_order_acquire);
        if (!p) return false;
        if (p == pipeline) return true;
    }
    return false;
}

static uint64_t vn_hook_metal_composite_layer(void *context, void *layer, void *destination, uint64_t flags) {
    void *outer = tl_vn_layer;
    void *outer_dest = tl_vn_destination;
    tl_vn_layer = layer;
    tl_vn_destination = destination;
    uint64_t result = vn_orig_metal_composite_layer(context, layer, destination, flags);
    tl_vn_layer = outer;
    tl_vn_destination = outer_dest;
    return result;
}

static void *vn_hook_copy_pipeline_state(void *shader, void *context, bool a, bool b) {
    void *pipeline = vn_orig_copy_pipeline_state(shader, context, a, b);
    if (!pipeline || !vn_is_our_shader(shader) || vn_is_our_pipeline(pipeline)) return pipeline;

    for (size_t i = 0; i < sizeof(gVNPipelines) / sizeof(gVNPipelines[0]); i++) {
        void *expected = NULL;
        if (atomic_compare_exchange_strong_explicit(&gVNPipelines[i], &expected, pipeline,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            VN_INFO("shader args: recorded pipeline %p from shader %p (slot %zu)", pipeline, shader, i);
            break;
        }
        if (expected == pipeline) break;
    }
    return pipeline;
}

/// Binds VNShaderExtra whenever one of our pipelines is set, whatever the path:
/// our fragment functions read it, so it must be bound on every draw of theirs.
static void vn_hook_set_pipeline_state(void *context, void *pipeline) {
    vn_orig_set_pipeline_state(context, pipeline);
    // Every layer of every window comes through here, every frame; only while
    // a shader clone exists can one of our pipelines be among them.
    if (atomic_load_explicit(&gShaderFilterCount, memory_order_acquire) <= 0) return;
    if (!pipeline || !vn_is_our_pipeline(pipeline)) return;

    static void (*msg)(void *, void *, const void *, unsigned long, unsigned long);
    static void *sel_set_bytes;
    if (!msg) {
        sel_set_bytes = ((void *(*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName"))("setFragmentBytes:length:atIndex:");
        msg = (void (*)(void *, void *, const void *, unsigned long, unsigned long))dlsym(RTLD_DEFAULT, "objc_msgSend");
    }

    VNShaderExtra extra = { .phase = -1.0f, .offset = { -1.0f, -1.0f } };
    void *layer = tl_vn_layer;
    if (layer && *(uint32_t *)((char *)layer + kVNLayerFilterTypeOffset) == kVNFilterTypeShaderTag) {
        memcpy(extra.params, (char *)layer + kVNLayerFilterParamsOffset, sizeof(extra.params));
        extra.bound = 1.0f;
        const double now = SLSCurrentRealTime();
        vn_clone_lookup(extra.params[0], now, &extra.phase, &extra.offset[0], &extra.offset[1]);
        VN_TRACE("draw: layer %p phase %.4f offset (%.3f, %.3f) at %.4f", layer, (double)extra.phase,
                 (double)extra.offset[0], (double)extra.offset[1], now);
    }

    void *encoder = vn_resolved_render_encoder(context);
    if (encoder && msg && sel_set_bytes) {
        msg(encoder, sel_set_bytes, &extra, sizeof(extra), kVNShaderExtraIndex);
        vn_particles_bind_fragment(encoder, tl_vn_destination, extra.params[0], extra.bound > 0.5f);
    }

    static _Atomic int s_logged;
    if (atomic_fetch_add_explicit(&s_logged, 1, memory_order_relaxed) < 6) {
        VN_INFO("shader args: bound on encoder %p for pipeline %p layer %p bound=%.0f params=(%.3f, %.3f)",
                encoder, pipeline, layer, (double)extra.bound, (double)extra.params[0], (double)extra.params[1]);
    }
}

/// The substitution, placed ahead of Apple's shader cache rather than inside it.
///
/// A clone tagged with filter type 2 makes DetermineShaderOptions set option bit
/// 0x10, which arrives here as `options`. Answering with our own shader means
/// the compositor's map is never consulted for our layers and never holds
/// anything of ours -- so switching animations takes effect on the next close,
/// with no cache to invalidate and no map internals to poke.
///
/// Everything else stays Apple's: their pixel formats, vertex descriptor, quad,
/// texture binds and uniform buffer. Only the two functions differ.
static void *vn_hook_uber_composite(void *composer, unsigned fmt, uint64_t options) {
    if ((options & kVNUberOptionShaderTag) && gVNShaderArgsReady &&
        atomic_load_explicit(&gShaderFilterCount, memory_order_acquire) > 0) {

        void *shader = vn_shader_for_fragment(vn_shader_fragment_name());

        if (!shader && vn_orig_uber_composite && (!gUberLibrary || !gUberVertexDescriptor)) {
            // Nothing to build against yet: the library and vertex descriptor
            // only arrive when the compositor builds an ubershader of its own,
            // and our capture hook is what notices. Run the original once to
            // make that happen, then build ours from what it left behind.
            //
            // Without this the first close of a session renders its opening
            // frames through the stock colour-invert -- a dark window flashing
            // white -- because the capture lands a few milliseconds after the
            // clone starts compositing.
            (void)vn_orig_uber_composite(composer, fmt, options);
            shader = vn_shader_for_fragment(vn_shader_fragment_name());
        }

        if (shader) return shader;
    }

    return vn_orig_uber_composite ? vn_orig_uber_composite(composer, fmt, options) : NULL;
}

/// The whole substitution, and it is one call.
///
/// A clone tagged with filter type 2 makes DetermineShaderOptions set option bit
/// 0x10, which reaches here as `options`. Answering with the same request
/// against OUR library gives a pipeline built by Apple's own code -- right pixel
/// formats, right vertex descriptor, their quad, their texture binds, their
/// uniform buffer -- running our functions. UberComposite caches the result per
/// options value, so this happens once and costs nothing per frame.
///
/// We call create_shader rather than passing the constants function through:
/// our shader declares none of the ubershader's function constants, so it needs
/// none supplied.
static void *vn_hook_create_specialized_shader(void *library, void *vtx, void *frag,
                                               void *constants_fn, uint64_t options, void *vdesc) {
    // Capture, do not substitute. Apple's cache is keyed on the options value
    // and every shader animation produces the same one, so a substitution here
    // would be cached and freeze the effect for the life of the process. The
    // swap happens in vn_hook_uber_composite instead, ahead of that cache; all
    // this hook is for is the library and vertex descriptor to build against,
    // taken from whatever the compositor builds first.
    if (library && vdesc && !gUberLibrary) {
        gUberLibrary = library;
        gUberVertexDescriptor = vdesc;
        VN_DEBUG("shader: captured ubershader library=%p vdesc=%p", library, vdesc);
    }

    return vn_orig_create_specialized_shader
         ? vn_orig_create_specialized_shader(library, vtx, frag, constants_fn, options, vdesc)
         : NULL;
}

#pragma mark - Entry

extern void MSHookFunction(void *symbol, void *replace, void **result);

static void vanish_init_payload(void) {
    // Resolved here rather than on first use: the draw path and the compute
    // path both send messages, and whichever runs first must not be the one
    // that decides whether the pointers exist.
    if (!vn_objc_ready()) {
        VN_ERROR("WARNING: objc_msgSend could not be resolved -- the water animation is unavailable");
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
    vn_resolved_create_shader                = (VNCreateShaderFn)vn_skylight_symbol(kVNSymCreateShader);
    vn_resolved_shape_window_with_rect       = (VNShapeWindowWithRectFn)vn_skylight_symbol(kVNSymShapeWindowWithRect);
    vn_resolved_create_clone                 = (VNCreateCloneFn)vn_skylight_symbol(kVNSymCreateCloneOfWindow);
    vn_resolved_system_window_release        = (VNSystemWindowReleaseFn)vn_skylight_symbol(kVNSymSystemWindowRelease);
    vn_resolved_window_get_display           = (VNWindowGetDisplayFn)vn_skylight_symbol(kVNSymWindowGetDisplay);
    vn_resolved_display_get_bounds = (VNDisplayGetBoundsFn)vn_skylight_symbol(kVNSymDisplayGetBounds);
    vn_resolved_screen_rect_from_rect        = (VNScreenRectFromRectFn)vn_skylight_symbol(kVNSymScreenRectFromRect);
    vn_resolved_screen_rect                  = (VNScreenRectFn)vn_skylight_symbol(kVNSymScreenRect);
    vn_resolved_window_get_id                = (VNWindowGetIDFn)vn_skylight_symbol(kVNSymWindowGetID);
    vn_resolved_clipped_frame_bounds         = (VNClippedFrameBoundsFn)vn_skylight_symbol(kVNSymClippedFrameBounds);
    vn_resolved_corner_radius                = (VNCornerRadiusFn)vn_skylight_symbol(kVNSymCornerRadius);
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
    TIL_HOOK("com.doraorak.vanish", rawRelease, vn_hook_release_window, &vn_orig_release_window);

    void *rawOrder = ptrauth_strip(targetOrder, ptrauth_key_function_pointer);
    TIL_HOOK("com.doraorak.vanish", rawOrder, vn_hook_order_window_list, &vn_orig_order);

    if (targetPostEvent) {
        void *rawPost = ptrauth_strip(targetPostEvent, ptrauth_key_function_pointer);
        TIL_HOOK("com.doraorak.vanish", rawPost, vn_hook_post_event, &vn_orig_post_event);
        VN_DEBUG("hooked CGXPostEventByConnection -> orig %p", vn_orig_post_event);
    } else {
        VN_ERROR("WARNING: CGXPostEventByConnection unresolved");
    }

    void *targetUber = vn_skylight_symbol(kVNSymUberComposite);
    if (targetUber && vn_resolved_create_shader) {
        void *rawUber = ptrauth_strip(targetUber, ptrauth_key_function_pointer);
        TIL_HOOK("com.doraorak.vanish", rawUber, vn_hook_uber_composite, &vn_orig_uber_composite);
        VN_DEBUG("hooked ShaderComposer::UberComposite -> orig %p", (void *)vn_orig_uber_composite);
    } else {
        VN_ERROR("WARNING: ShaderComposer::UberComposite unresolved -- shader animations will not draw");
    }

    void *targetSpecialized = vn_skylight_symbol(kVNSymCreateSpecializedShader);
    if (targetSpecialized && vn_resolved_create_shader) {
        void *rawSpecialized = ptrauth_strip(targetSpecialized, ptrauth_key_function_pointer);
        TIL_HOOK("com.doraorak.vanish", rawSpecialized, vn_hook_create_specialized_shader, &vn_orig_create_specialized_shader);
        VN_DEBUG("hooked ShaderComposer::create_specialized_shader -> orig %p",
                 (void *)vn_orig_create_specialized_shader);
    } else {
        VN_ERROR("WARNING: create_specialized_shader/create_shader unresolved -- shader animations will not draw");
    }

    // Per-window shader arguments. All three hooks or none: see gVNShaderArgsReady.
    void *targetCompositeLayer = vn_skylight_symbol(kVNSymMetalCompositeLayer);
    void *targetSetPipeline    = vn_skylight_symbol(kVNSymMetalContextSetPipelineState);
    void *targetCopyPipeline   = vn_skylight_symbol(kVNSymMetalShaderCopyPipelineState);
    vn_resolved_render_encoder = (VNRenderEncoderFn)vn_skylight_symbol(kVNSymMetalContextRenderEncoder);
    if (targetCompositeLayer && targetSetPipeline && targetCopyPipeline && vn_resolved_render_encoder) {
        TIL_HOOK("com.doraorak.vanish", ptrauth_strip(targetCopyPipeline, ptrauth_key_function_pointer),
                 vn_hook_copy_pipeline_state, &vn_orig_copy_pipeline_state);
        TIL_HOOK("com.doraorak.vanish", ptrauth_strip(targetSetPipeline, ptrauth_key_function_pointer),
                 vn_hook_set_pipeline_state, &vn_orig_set_pipeline_state);
        TIL_HOOK("com.doraorak.vanish", ptrauth_strip(targetCompositeLayer, ptrauth_key_function_pointer),
                 vn_hook_metal_composite_layer, &vn_orig_metal_composite_layer);
        gVNShaderArgsReady = vn_orig_copy_pipeline_state && vn_orig_set_pipeline_state &&
                             vn_orig_metal_composite_layer;
    }
    VN_INFO("shader args: composite_layer=%p set_pipeline=%p copy_pipeline=%p render_encoder=%p ready=%d",
            targetCompositeLayer, targetSetPipeline, targetCopyPipeline,
            (void *)vn_resolved_render_encoder, gVNShaderArgsReady);
    if (!gVNShaderArgsReady) {
        VN_ERROR("WARNING: shader argument hooks missing -- shader animations fall back to the stock invert");
    }

    // The compute pass. StartComposite is the point in the frame where the
    // command buffer exists and nothing is encoding; EndEncoders and
    // RenderCommandBuffer are the two calls it makes there itself, and the two
    // the pass needs to get in. Without all three the water animation simply
    // never steps -- its fragment then draws the untouched window, which is the
    // same thing it draws before an animation starts.
    // What makes the fluid land only on windows that are actually on screen.
    // Both optional: without them every tracked window is solid, which is the
    // behaviour this replaces rather than a failure.
    vn_resolved_unobscured_content_shape =
        (VNUnobscuredContentShapeFn)vn_skylight_symbol(kVNSymUnobscuredContentShape);
    // A data symbol: strip the function-pointer signature vn_skylight_symbol puts on.
    vn_resolved_session_control_ref = (void **)ptrauth_strip(vn_skylight_symbol(kVNSymSessionControlRef),
                                                             ptrauth_key_function_pointer);
    vn_resolved_copy_screen_frame_shape   = (VNCopyScreenShapeFn)vn_skylight_symbol(kVNSymCopyScreenFrameShape);
    vn_resolved_copy_screen_content_shape = (VNCopyScreenShapeFn)vn_skylight_symbol(kVNSymCopyScreenContentShape);
    vn_region_union = (int (*)(void *, void *, void **))dlsym(RTLD_DEFAULT, "CGSUnionRegion");
    vn_region_diff  = (int (*)(void *, void *, void **))dlsym(RTLD_DEFAULT, "CGSDiffRegion");
    VN_INFO("water: window stack %p, frame shape %p, content shape %p, union %p, diff %p",
            (void *)vn_resolved_session_control_ref, (void *)vn_resolved_copy_screen_frame_shape,
            (void *)vn_resolved_copy_screen_content_shape, (void *)vn_region_union, (void *)vn_region_diff);
    vn_resolved_get_region_bounds = (VNGetRegionBoundsFn)dlsym(RTLD_DEFAULT, "CGSGetRegionBounds");
    VN_INFO("water: unobscured_content_shape=%p get_region_bounds=%p",
            (void *)vn_resolved_unobscured_content_shape, (void *)vn_resolved_get_region_bounds);

    void *targetStartComposite        = vn_skylight_symbol(kVNSymMetalContextStartComposite);
    vn_resolved_end_encoders          = (VNEndEncodersFn)vn_skylight_symbol(kVNSymMetalContextEndEncoders);
    vn_resolved_render_command_buffer = (VNRenderCommandBufferFn)vn_skylight_symbol(kVNSymMetalContextRenderCommandBuffer);
    if (targetStartComposite && vn_resolved_end_encoders && vn_resolved_render_command_buffer) {
        TIL_HOOK("com.doraorak.vanish", ptrauth_strip(targetStartComposite, ptrauth_key_function_pointer),
                 vn_hook_start_composite, &vn_orig_start_composite);
    }
    VN_INFO("water: start_composite=%p end_encoders=%p render_command_buffer=%p hooked=%d",
            targetStartComposite, (void *)vn_resolved_end_encoders,
            (void *)vn_resolved_render_command_buffer, vn_orig_start_composite != NULL);
    if (!vn_orig_start_composite) {
        VN_ERROR("WARNING: the compute pass could not be installed -- the water animation will not simulate");
    }

    vn_resolved_reevaluate_hdr_request = (VNReevaluateHDRRequestFn)vn_skylight_symbol(kVNSymReevaluateHDRRequest);
    vn_headroom_resolve();
    // vn_skylight_symbol signs what it returns as a function pointer; these are
    // data, so they are used stripped.
    gVNForceRamp  = (uint8_t *)ptrauth_strip(vn_skylight_symbol(kVNSymForceEDRRampDuration),
                                             ptrauth_key_function_pointer);
    gVNForcedRamp = (float *)ptrauth_strip(vn_skylight_symbol(kVNSymForcedEDRRampDuration),
                                           ptrauth_key_function_pointer);
    VN_INFO("edr: ramp override force=%p duration=%p (currently %u, %.2fs)",
            (void *)gVNForceRamp, (void *)gVNForcedRamp,
            gVNForceRamp ? (unsigned)*gVNForceRamp : 0u, gVNForcedRamp ? (double)*gVNForcedRamp : 0.0);

    if (targetEligible) {
        void *rawEligible = ptrauth_strip(targetEligible, ptrauth_key_function_pointer);
        TIL_HOOK("com.doraorak.vanish", rawEligible, vn_hook_is_process_eligible, &vn_orig_is_process_eligible);
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

__attribute__((constructor))
static void vanish_init(void) {
    char self[1024] = {0};
    uint32_t len = (uint32_t)sizeof(self);
    if (_NSGetExecutablePath(self, &len) != 0) return;
    if (strstr(self, "WindowServer") == NULL) {
        VN_INFO("not WindowServer (%{public}s) -- doing nothing", self);
        return;
    }

    TIL_DISPATCH("com.doraorak.vanish", vanish_init_payload);
}
