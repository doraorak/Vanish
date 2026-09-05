//
//  SkyLightServer.h
//  SkyLight's server side -- the half that runs inside WindowServer.
//
//  None of this is exported. `dlsym` returns NULL for every name below, because
//  all 2,915 of SkyLight's exports are the client API. These are local symbols,
//  resolved at runtime by walking LC_SYMTAB (see vn_skylight_symbol).
//
//  Signatures were read out of the corresponding MIG server routines, which
//  unmarshal a known message layout and then call straight through -- so the
//  argument order and types below are observed, not guessed. See ANALYSIS.md.
//

#ifndef SkyLightServer_h
#define SkyLightServer_h

#include <stdint.h>
#include <stdbool.h>
#include <CoreFoundation/CoreFoundation.h>

/// Opaque server-side types. Never dereferenced here: the field offsets are the
/// part of this that Apple can change without renaming anything.
typedef struct CGXConnection CGXConnection;
typedef struct CGXWindow CGXWindow;

/// Same values as CGSWindowOrderingMode. A close reduces to kVNOrderOut.
typedef int32_t CGSOrderOp;
enum { kVNOrderBelow = -1, kVNOrderOut = 0, kVNOrderAbove = 1 };

#pragma mark - Mangled names

//  Matched literally against the symbol table. Names, not offsets: offsets move
//  on every OS update, these strings have not. If Apple ever renames one, the
//  lookup fails, the hook is never installed, and the tweak is inert -- which is
//  the correct way for this to break.

/// Every window-ordering path in the server converges here -- `_XOrderWindow`,
/// `_XOrderWindowList`, `_XOrderWindowListWithOperation` and
/// `_XOrderWindowListWithGroups` all reach it. One hook covers every close.
#define kVNSymOrderWindowList \
    "__ZL36CGXOrderWindowListSpaceSwitchOptionsP13CGXConnectionPKjPK10CGSOrderOpS2_jb"

/// CGXWindow *window_by_id(uint32_t wid)
#define kVNSymWindowByID "__ZL12window_by_idj"

/// void CGXSetWindowListAlpha(CGXConnection *, CGXWindow *, float alpha, float duration)
#define kVNSymSetAlpha "_CGXSetWindowListAlpha"

/// void CGXWindow::release_window(CGXConnection *, CGXWindow *)
#define kVNSymReleaseWindow "__ZN9CGXWindow14release_windowEP13CGXConnectionPS_"

/// pid_t WSWindowGetOwningPID(CGXWindow *)
#define kVNSymWSWindowGetOwningPID "_WSWindowGetOwningPID"

/// int CGXGetConnectionAppName(uint32_t cid, char *buf, size_t buflen)
#define kVNSymGetConnectionAppName "_CGXGetConnectionAppName"

/// void CGXPostEventByConnection(CGXConnection *conn, void *event)
#define kVNSymPostEventByConnection "_CGXPostEventByConnection"

#pragma mark - Struct offsets

/// Window level at offset 0x20.
/// Read directly from `WSWindowGetLevel` (`ldr w0, [x0, #0x20]`).
/// Normal application document windows have level 0 (kCGSNormalWindowLevel).
#define kVNWindowLevelOffset 0x20

static inline int32_t vn_window_level(const CGXWindow *win) {
    if (!win) return -1;
    int32_t lvl = 0;
    __builtin_memcpy(&lvl, (const char *)win + kVNWindowLevelOffset, sizeof(lvl));
    return lvl;
}

/// Workspace data pointer at offset 0x258.
/// Initialized by `spaces_did_create_window_callback` and cleared to NULL by
/// `spaces_did_terminate_window_callback` upon window termination / destruction.
#define kVNWindowWorkspaceDataOffset 0x258

static inline void *vn_window_workspace_data(const CGXWindow *win) {
    if (!win) return NULL;
    void *ws = NULL;
    __builtin_memcpy(&ws, (const char *)win + kVNWindowWorkspaceDataOffset, sizeof(ws));
    return ws;
}

/// A window's current alpha, as a float.
///
/// `CGXWindow::fade_begin` reads exactly this at its `+36` (`ldr s0, [x0, #0x1e8]`).
#define kVNWindowAlphaOffset 0x1e8

static inline float vn_window_alpha(const CGXWindow *win) {
    if (!win) return 0.0f;
    float a = 0.0f;
    __builtin_memcpy(&a, (const char *)win + kVNWindowAlphaOffset, sizeof(a));
    return a;
}

/// Whether the window is currently ordered in (bit 6 of byte 0x965, or bit 46 of uint64 at 0x960).
/// Read directly from `_XWindowIsOrderedIn` and `start_order_window`.
#define kVNWindowOrderedInOffset 0x965
#define kVNWindowOrderedInMask   0x40

static inline bool vn_window_is_ordered_in(const CGXWindow *win) {
    if (!win) return false;
    uint8_t flags = 0;
    __builtin_memcpy(&flags, (const char *)win + kVNWindowOrderedInOffset, sizeof(flags));
    return (flags & kVNWindowOrderedInMask) != 0;
}

/// Active fade state pointer at offset 0x8c0.
/// Non-NULL while an animation is running via `CGXWindow::fade_begin`.
/// Cleared to NULL by `CGXWindow::fade_finish`.
#define kVNWindowFadeOffset 0x8c0

static inline bool vn_window_is_fading(const CGXWindow *win) {
    if (!win) return false;
    const void *fade = NULL;
    __builtin_memcpy(&fade, (const char *)win + kVNWindowFadeOffset, sizeof(fade));
    return fade != NULL;
}

/// Owning connection pointer at offset 0x30.
#define kVNWindowConnectionOffset 0x30

static inline CGXConnection *vn_window_connection(const CGXWindow *win) {
    if (!win) return NULL;
    CGXConnection *conn = NULL;
    __builtin_memcpy(&conn, (const char *)win + kVNWindowConnectionOffset, sizeof(conn));
    return conn;
}

#pragma mark - Resolved function types

typedef void (*VNOrderWindowListFn)(CGXConnection *, const uint32_t *, const CGSOrderOp *,
                                    const uint32_t *, unsigned, bool);
typedef CGXWindow *(*VNWindowByIDFn)(uint32_t);
typedef void (*VNSetAlphaFn)(CGXConnection *, CGXWindow *, float, float);
typedef void (*VNReleaseWindowFn)(CGXConnection *, CGXWindow *);
typedef pid_t (*VNWindowGetOwningPIDFn)(CGXWindow *);
typedef int (*VNGetConnectionAppNameFn)(uint32_t, char *, size_t);


#pragma mark - Trace targets

//  Every signature below was read off a real call site, not off a demangled
//  name. That distinction has already cost one session: CGXWindow::fade_begin
//  demangles with a CGXConnection* it does not actually take.

/// void CGXWindow::update_ca_visibility(CGXWindow *self, bool visible)
/// x0 = this, x1 = the flag. Verified: it dereferences x0 at +0x10/+0x18/+0x960.
/// The compositor's own show/hide, independent of alpha -- prime suspect for a
/// window vanishing while its alpha is still animating.
#define kVNSymUpdateCAVisibility "__ZN9CGXWindow20update_ca_visibilityEb"

/// void set_window_alphas(CGXConnection *, CGXWindow *, float)
/// The alpha that actually reaches CoreAnimation. `CGXSetWindowListAlpha`
/// tail-calls it on the zero-duration path with x0=conn, x1=win, s0=alpha.
#define kVNSymSetWindowAlphas "__ZL17set_window_alphasP13CGXConnectionP9CGXWindowf"

/// void CGXWindow::fade_begin(CGXWindow *self, void (^)(CGXWindow *, bool),
///                            float alpha, float duration)
#define kVNSymFadeBegin \
    "__ZN9CGXWindow10fade_beginEP13CGXConnectionffU13block_pointerFvPS_bE"

/// void CGXWindow::fade_finish(CGXWindow *self, CGXConnection *)
#define kVNSymFadeFinish "__ZN9CGXWindow11fade_finishEP13CGXConnection"

/// void WS::CAWindowContent::commit_context_visibility(WS::CAWindowContent *self)
/// Reads bit 5 of the halfword at +0x840 and pushes it into CoreAnimation.
#define kVNSymCommitContextVisibility "__ZN2WS15CAWindowContent25commit_context_visibilityEv"

/// void start_order_window(CGXConnection *, CGXOrderWindowState *)
#define kVNSymStartOrderWindow "__ZL18start_order_windowP13CGXConnectionP19CGXOrderWindowState"

/// Where alpha actually reaches CoreAnimation. `set_window_alphas` reads two
/// more alpha channels off the window (+0x1ec, +0x1f0) and calls this with all
/// three, so a window has three alphas, not one.
///
///     x0 = this, x1 = conn, w2 = bool, s0/s1/s2 = the three alphas
#define kVNSymUpdateAlphas "__ZN9CGXWindow13update_alphasEP13CGXConnectionfffb"

/// void WS::CAWindowContent::~CAWindowContent(WS::CAWindowContent *self)
/// If the window's drawable content is destroyed while the fade is running,
/// there is nothing left to composite and no alpha can save it.
#define kVNSymCAWindowContentDtor "__ZN2WS15CAWindowContentD1Ev"

/// The alpha most recently *requested*, as opposed to the one being animated.
/// `CGXSetWindowListAlpha` writes the target here immediately, before the fade
/// has run at all.
#define kVNWindowRequestedAlphaOffset 0x1f4

static inline float vn_window_requested_alpha(const CGXWindow *win) {
    float a;
    __builtin_memcpy(&a, (const char *)win + kVNWindowRequestedAlphaOffset, sizeof(a));
    return a;
}

/// Runs a callback from the server's timer pass, on the main thread.
///
///     void WSScheduleCallbackOnCurrentSession(void (*cb)(void *ctx, double t),
///                                             void *ctx, double fireTime)
///
/// `fireTime` is absolute -- `SLSCurrentRealTime()` plus a delay. Read off its
/// body: it shuffles x0/x1 into x1/x2, fills in `gMainThreadQueue`, the current
/// session and two zero flags, and tail-calls `reschedule_callback_on_session`.
#define kVNSymScheduleCallback "_WSScheduleCallbackOnCurrentSession"

typedef void (*VNScheduleCallbackFn)(void (*)(void *, double), void *, double);
/// Returns a change mask, and the caller acts on it: `set_window_alphas` does
/// `cmp w0, #4` and only then calls into the CAManager that commits the change
/// to the screen. A hook that declares this void silently throws that away.
#pragma mark - Drawing a window differently

//  Alpha is a dead end on an ordinary window. `update_alphas` gates every real
//  effect on bit 2 of `[win + 0x967]`, which is clear: it stores the numbers,
//  returns mask 1, and `set_window_alphas` -- needing >= 4 -- never commits. A
//  scan of all 1.4M instructions in __text found 67 reads of that byte and ZERO
//  writes, and Apple's own CGXSetWindowListAlpha fade is equally invisible on
//  such a window. See ANALYSIS.md.
//
//  These two are what actually draw.

/// Arbitrary mesh deformation -- the general primitive. Genie, crumple, jelly,
/// fold: anything expressible as moving grid points.
///
///     void CGXWindow::set_mesh_warp(CGXWindow *self, CGXConnection *,
///                                   unsigned w, unsigned h, const float *mesh)
///
/// ABI read off `_XSetWindowWarp`'s call site (x0=win, x1=conn, x2=w, x3=h,
/// x4=mesh), not off the demangled name. That routine also validates
/// `float_count == w * h * 4` with `w, h <= 2048`, which pins the mesh layout to
/// the classic CGSSetWindowWarp one: w*h points, row-major, four floats each.
#define kVNSymSetMeshWarp "__ZN9CGXWindow13set_mesh_warpEP13CGXConnectionjjPKf"

/// The window's frame in screen coordinates, for building that mesh.
/// `CGXWindow::clipped_frame_bounds(CGXWindow *self)` -- x0 only, and a CGRect
/// is an HFA of four doubles, so it comes back in d0-d3.
#define kVNSymClippedFrameBounds "__ZN9CGXWindow20clipped_frame_boundsEv"

/// A 4x4 transform, for the cheap cases. Unlike the alpha path this schedules
/// its own redraw (`accumulate_window_display_updates`).
#define kVNSymWSSetWindowTransform "_WSSetWindowTransform"

/// One mesh vertex: where it is in the window, and where to draw it on screen.
typedef struct { float x, y; } VNMeshPoint;
typedef struct {
    VNMeshPoint local;    // window coordinates, 0..width / 0..height
    VNMeshPoint global;   // screen coordinates
} VNPointWarp;            // 16 bytes

/// Captures the window's current content into a backing the SERVER owns.
///
/// This is the piece every attempt so far has been missing. A warp deforms the
/// window's CoreAnimation content, and after the client calls `orderOut:` that
/// content stops being committed -- which is why the identical animation is
/// plainly visible on an open window and invisible on a closing one, whatever
/// the server-side ordering state.
///
/// `WSWindowFreezeContent(CGXWindow *win)` -- one pointer, verified by
/// disassembly (`mov x19, x0`, then real work: `WSWindowFreezeSizeIsValid`,
/// `WindowFreeze::Update(CGSize)`, and a scheduled
/// `frozen_backing_cleanup_callback`). Note `SLSWindowFreezeWithOptions` and its
/// MIG routine `_XWindowFreezeWithOptions` are stubs -- the latter validates the
/// message and returns 0 -- so the exported API is useless and this is not.
///
/// `layergen_frozen_window` is one of the functions that reads the mesh at
/// `[win + 0x8b0]`, so frozen content is warped like any other.
#define kVNSymFreezeContent "_WSWindowFreezeContent"

typedef void   (*VNFreezeContentFn)(CGXWindow *);
#pragma mark - Cloning a window

//  SkyLight already implements the whole snapshot: CreateCloneOfWindow does
//  new_window, set_level_internal, WSWindowSetTitle, set_window_list_tags,
//  WSWindowSetDepth, WSWindowSetHasAlpha, WSWindowInternalSetSharedState,
//  WSWindowSetResolution, WSWindowGetShape and WSWindowSetCapturedContent, then
//  hands back the clone. Nothing here has to reproduce that.
//
//  ABI read off `CreateClonedContentForItem`'s call site, not the demangled
//  name: x0 = source window, d0-d3 = frame (a CGRect is an HFA of four
//  doubles), x1 = PKGDisplay *, w2 = flag (that caller passes 1). The result is
//  the clone, and the caller null-checks it.
#define kVNSymCreateCloneOfWindow \
    "__ZL19CreateCloneOfWindowP9CGXWindow6CGRectPK10PKGDisplayb"

/// The display a window belongs to. Falls back through
/// `window_lookup_first_active_managed_space_display` then
/// `window_lookup_best_pkg_display_for_geometry`, so it copes with a window
/// that is not on a managed space.
#define kVNSymWindowGetDisplay "_PKGWindowGetDisplay"

/// Maps a window-local rect to screen coordinates. The clone caller passes
/// CGRectZero to get the window's own frame.
#define kVNSymScreenRectFromRect "__ZNK9CGXWindow21screen_rect_from_rectE6CGRect"

/// Returns the window content bounding box in screen coordinates.
#define kVNSymScreenRect "__ZNK9CGXWindow11screen_rectEv"

/// `ldr w0, [x0]` -- the window id is the first field.
#define kVNSymWindowGetID "_WSWindowGetID"

/// Destroys a clone. `WSSystemWindowRelease(CGXWindow *win)` -- one argument:
/// it checks the system-window bit in `[win + 0x960]`, clears it, picks the
/// window's own connection out of `[win + 0x30]`, and tail-calls
/// `CGXWindow::release_window`. It is a no-op on a window that is not a system
/// window, which makes it safe to call on anything.
#define kVNSymSystemWindowRelease "_WSSystemWindowRelease"

typedef void (*VNSystemWindowReleaseFn)(CGXWindow *);
typedef CGXWindow *(*VNCreateCloneFn)(CGXWindow *, CGRect, const void *, bool);
typedef const void *(*VNWindowGetDisplayFn)(CGXWindow *);
typedef CGRect      (*VNScreenRectFromRectFn)(CGXWindow *, CGRect);
typedef CGRect      (*VNScreenRectFn)(CGXWindow *);
typedef uint32_t    (*VNWindowGetIDFn)(CGXWindow *);

typedef void   (*VNSetMeshWarpFn)(CGXWindow *, CGXConnection *, unsigned, unsigned, const float *);
typedef CGRect (*VNClippedFrameBoundsFn)(CGXWindow *);

typedef int (*VNUpdateAlphasFn)(CGXWindow *, CGXConnection *, bool, float, float, float);
typedef void (*VNCAWindowContentDtorFn)(void *);

//  CGXScheduleUpdateDisplaysIntersectingWindow takes TWO arguments and the
//  window is the SECOND one. Calling it as f(win) crashed the server:
//
//      CGXWindow::intersecting_displays() + 108   SIGSEGV at 0xac   (x1 = 0)
//      CGXScheduleUpdateDisplaysIntersectingWindow + 48
//      vn_apply_alpha
//
//  It does `mov x19, x0`, then `mov x0, sp` for the out-vector, and calls
//  `intersecting_displays` WITHOUT setting x1 -- so x1 arrives straight from
//  its own caller, and `intersecting_displays` reads it (`mov x20, x1` at its
//  +64) as the CGXWindow. x0 is something else entirely, forwarded to
//  `CGXScheduleUpdateDisplay`.
//
//  Not declared until a real call site is read. Inferring an arity from a name
//  is what this file already warns about twice.
typedef void (*VNUpdateCAVisibilityFn)(CGXWindow *, bool);
typedef void (*VNSetWindowAlphasFn)(CGXConnection *, CGXWindow *, float);
typedef void (*VNFadeBeginFn)(CGXWindow *, void (^)(CGXWindow *, bool), float, float);
typedef void (*VNFadeFinishFn)(CGXWindow *, CGXConnection *);
typedef void (*VNCommitContextVisibilityFn)(void *);
typedef void (*VNStartOrderWindowFn)(CGXConnection *, void *);
typedef void (*VNPostEventByConnectionFn)(CGXConnection *, void *);

#endif /* SkyLightServer_h */
