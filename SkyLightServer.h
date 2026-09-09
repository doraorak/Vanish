//
//  SkyLightServer.h
//  SkyLight's server side -- the half that runs inside WindowServer.
//
//  None of this is exported. `dlsym` returns NULL for every name below, because
//  all 2,915 of SkyLight's exports are the client API. These are local symbols,
//  resolved at runtime by walking LC_SYMTAB (see vn_skylight_symbol).
//
//  Signatures are derived from the corresponding MIG server routines, which
//  unmarshal Mach messages and call internal implementations -- argument orders
//  and types below reflect observed WindowServer ABI.
//

#ifndef SkyLightServer_h
#define SkyLightServer_h

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int SLSConnectionID;
typedef uint32_t SLSWindowID;

/// The window server connection for this process.
extern SLSConnectionID SLSMainConnectionID(void);

/// The server's clock, in seconds. Exported by SkyLight.
extern double SLSCurrentRealTime(void);

/// Same layout as CATransform3D (16 doubles). Declared here so this header does
/// not have to pull in QuartzCore to describe fields Vanish never touches.
typedef struct { double m[16]; } VNTransform3D;

/// CGXConnection stays opaque: unlike CGXWindow there is no recovered layout
/// for it, and only one field is needed. Inventing a struct around a single
/// verified offset would look more authoritative than the evidence supports.
typedef struct CGXConnection CGXConnection;

/// The server's window object. Field names come from a partial type recovery;
/// every offset below is pinned by a _Static_assert, so a wrong padding size
/// is a build error rather than a memory corruption bug at runtime.
///
/// Four offsets were independently confirmed by disassembly before the
/// recovery existed, and all four line up with it exactly -- `level` against
/// WSWindowGetLevel (`ldr w0, [x0, #0x20]`), `connection` at 0x30, `alpha`
/// against CGXWindow::fade_begin (`ldr s0, [x0, #0x1e8]`), and the ordered-in
/// bit against _XWindowIsOrderedIn (bit 46 of `flags`).
typedef struct CGXWindow {
    uint32_t       window_id;                           // 0x000
    uint32_t       window_type;                         // 0x004
    uint8_t        _pad_008[0x8];
    void          *ca_backing;                          // 0x010
    uint8_t        _pad_018[0x8];
    int32_t        level;                               // 0x020
    uint8_t        _pad_024[0xc];
    CGXConnection *connection;                          // 0x030
    uint8_t        _pad_038[0x18];
    uint32_t       connection_id;                       // 0x050  cid for CGXGetConnectionAppName
    uint8_t        _pad_054[0x70];
    uint32_t       screen_geometry_seed;                // 0x0c4
    uint8_t        _pad_0c8[0x20];
    void          *clip_shape;                          // 0x0e8
    uint8_t        _pad_0f0[0x30];
    void          *global_clip_shape;                   // 0x120
    void          *layer_clip_shape;                    // 0x128
    uint8_t        _pad_130[0x50];
    void          *shape;                               // 0x180
    uint8_t        _pad_188[0x30];
    void          *shape_data;                          // 0x1b8
    uint8_t        _pad_1c0[0x28];
    float          alpha;                               // 0x1e8
    float          system_alpha;                        // 0x1ec
    uint8_t        _pad_1f0[0x30];
    float          backing_store_resolution;            // 0x220
    uint8_t        _pad_224[0x4];
    float          backing_store_override_resolution;   // 0x228
    uint8_t        _pad_22c[0x4];
    CGSize         backing_store_pixel_dimensions_hint; // 0x230
    uint8_t        _pad_240[0x10];
    void          *backing_store;                       // 0x250
    void          *workspace_data;                      // 0x258
    uint8_t        _pad_260[0x230];
    void          *window_mask;                         // 0x490
    void          *frozen_backing;                      // 0x498
    uint8_t        _pad_4a0[0x8];
    VNTransform3D  transform;                           // 0x4a8
    VNTransform3D  system_transform;                    // 0x528
    VNTransform3D  deformation_transform;               // 0x5a8
    uint8_t        _pad_628[0x180];
    void          *transform_stack;                     // 0x7a8
    uint8_t        _pad_7b0[0xc];
    uint32_t       event_mask;                          // 0x7bc
    uint8_t        _pad_7c0[0x10];
    double         sfx_corner_radius;                   // 0x7d0
    uint8_t        _pad_7d8[0x10];
    uint64_t       dominant_display_id;                 // 0x7e8
    uint8_t        _pad_7f0[0x78];
    double         corner_radius;                       // 0x868
    double         debug_corner_radius;                 // 0x870
    double         corner_radii[4];                     // 0x878
    uint8_t        _pad_898[0x8];
    void          *mask_path;                           // 0x8a0
    uint8_t        _pad_8a8[0x8];
    void          *mesh;                                // 0x8b0
    void          *frame_mesh_cache;                    // 0x8b8
    void          *fade_state;                          // 0x8c0
    uint8_t        _pad_8c8[0x18];
    int32_t        screen_state;                        // 0x8e0
    uint8_t        _pad_8e4[0x7c];
    uint64_t       flags;                               // 0x960
} CGXWindow;

#define VN_ASSERT_OFFSET(field, off) \
    _Static_assert(offsetof(CGXWindow, field) == (off), \
                   "CGXWindow." #field " is not at " #off)

VN_ASSERT_OFFSET(window_id,           0x000);
VN_ASSERT_OFFSET(window_type,         0x004);
VN_ASSERT_OFFSET(ca_backing,          0x010);
VN_ASSERT_OFFSET(level,               0x020);
VN_ASSERT_OFFSET(connection,          0x030);
VN_ASSERT_OFFSET(connection_id,       0x050);
VN_ASSERT_OFFSET(screen_geometry_seed, 0x0c4);
VN_ASSERT_OFFSET(clip_shape,          0x0e8);
VN_ASSERT_OFFSET(global_clip_shape,   0x120);
VN_ASSERT_OFFSET(layer_clip_shape,    0x128);
VN_ASSERT_OFFSET(shape,               0x180);
VN_ASSERT_OFFSET(shape_data,          0x1b8);
VN_ASSERT_OFFSET(alpha,               0x1e8);
VN_ASSERT_OFFSET(system_alpha,        0x1ec);
VN_ASSERT_OFFSET(backing_store_resolution, 0x220);
VN_ASSERT_OFFSET(backing_store_pixel_dimensions_hint, 0x230);
VN_ASSERT_OFFSET(backing_store,       0x250);
VN_ASSERT_OFFSET(workspace_data,      0x258);
VN_ASSERT_OFFSET(window_mask,         0x490);
VN_ASSERT_OFFSET(frozen_backing,      0x498);
VN_ASSERT_OFFSET(transform,           0x4a8);
VN_ASSERT_OFFSET(system_transform,    0x528);
VN_ASSERT_OFFSET(deformation_transform, 0x5a8);
VN_ASSERT_OFFSET(transform_stack,     0x7a8);
VN_ASSERT_OFFSET(event_mask,          0x7bc);
VN_ASSERT_OFFSET(sfx_corner_radius,   0x7d0);
VN_ASSERT_OFFSET(dominant_display_id, 0x7e8);
VN_ASSERT_OFFSET(corner_radius,       0x868);
VN_ASSERT_OFFSET(debug_corner_radius, 0x870);
VN_ASSERT_OFFSET(corner_radii,        0x878);
VN_ASSERT_OFFSET(mask_path,           0x8a0);
VN_ASSERT_OFFSET(mesh,                0x8b0);
VN_ASSERT_OFFSET(frame_mesh_cache,    0x8b8);
VN_ASSERT_OFFSET(fade_state,          0x8c0);
VN_ASSERT_OFFSET(screen_state,        0x8e0);
VN_ASSERT_OFFSET(flags,               0x960);

/// The server's input event. Only the fields Vanish reads are named; the rest
/// is padding, so this deliberately does not claim to be the whole struct.
typedef struct {
    uint8_t  _pad_000[0x8];
    uint32_t type;       // 0x08  1 = mouse down, 2 = mouse up, 6 = mouse dragged
    uint32_t _pad_00c;
    CGPoint  screen_pt;  // 0x10
    CGPoint  local_pt;   // 0x20  window-local, top-left origin, y down
    uint8_t  _pad_030[0xc];
    uint32_t window_id;  // 0x3c
} VNEvent;

_Static_assert(offsetof(VNEvent, type)      == 0x08, "VNEvent.type moved");
_Static_assert(offsetof(VNEvent, screen_pt) == 0x10, "VNEvent.screen_pt moved");
_Static_assert(offsetof(VNEvent, local_pt)  == 0x20, "VNEvent.local_pt moved");
_Static_assert(offsetof(VNEvent, window_id) == 0x3c, "VNEvent.window_id moved");

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

/// bool isProcessEligibleForSetFront(uint32_t sessionID, CPSProcessSerNum psn, bool flag, bool *out)
#define kVNSymIsProcessEligibleForSetFront "__ZL28isProcessEligibleForSetFrontj16CPSProcessSerNumbPb"

#pragma mark - Window field access

/// Normal application document windows have level 0 (kCGSNormalWindowLevel).
static inline int32_t vn_window_level(const CGXWindow *win) {
    return win ? win->level : -1;
}

/// Set by spaces_did_create_window_callback, cleared to NULL by
/// spaces_did_terminate_window_callback when the window is torn down.
static inline void *vn_window_workspace_data(const CGXWindow *win) {
    return win ? win->workspace_data : NULL;
}

static inline float vn_window_alpha(const CGXWindow *win) {
    return win ? win->alpha : 0.0f;
}

/// Bit 46 of `flags`, read directly from _XWindowIsOrderedIn and
/// start_order_window.
#define kVNWindowOrderedInBit 46

static inline bool vn_window_is_ordered_in(const CGXWindow *win) {
    return win && (win->flags & (1ULL << kVNWindowOrderedInBit)) != 0;
}

/// Non-NULL while an animation is running via CGXWindow::fade_begin; cleared
/// by CGXWindow::fade_finish.
static inline bool vn_window_is_fading(const CGXWindow *win) {
    return win && win->fade_state != NULL;
}

static inline CGXConnection *vn_window_connection(const CGXWindow *win) {
    return win ? win->connection : NULL;
}

static inline pid_t vn_conn_get_pid(const CGXConnection *conn) {
    if (!conn) return 0;
    pid_t pid = 0;
    __builtin_memcpy(&pid, (const char *)conn + 0x268, sizeof(pid));
    return pid;
}

/// ProcessSerialNumber at offset 0x124 of CGXConnection. Still raw offset
/// math: CGXConnection has no recovered layout, and this is the only field
/// anything here needs.
static inline uint64_t vn_conn_get_psn(const CGXConnection *conn) {
    if (!conn) return 0;
    uint64_t psn = 0;
    __builtin_memcpy(&psn, (const char *)conn + 0x124, sizeof(psn));
    return psn;
}

#pragma mark - Resolved function types

typedef void (*VNOrderWindowListFn)(CGXConnection *, const uint32_t *, const CGSOrderOp *,
                                    const uint32_t *, unsigned, bool);
typedef CGXWindow *(*VNWindowByIDFn)(uint32_t);
typedef void (*VNSetAlphaFn)(CGXConnection *, CGXWindow *, float, float);
typedef void (*VNReleaseWindowFn)(CGXConnection *, CGXWindow *);
typedef pid_t (*VNWindowGetOwningPIDFn)(CGXWindow *);
typedef int (*VNGetConnectionAppNameFn)(uint32_t, char *, size_t);
typedef void (*VNUpdateCAVisibilityFn)(CGXWindow *, bool);
typedef bool (*VNIsProcessEligibleForSetFrontFn)(uint32_t, uint64_t, bool, bool *);


#pragma mark - Server-Internal Operations

/// void CGXWindow::update_ca_visibility(CGXWindow *self, bool visible)
#define kVNSymUpdateCAVisibility "__ZN9CGXWindow20update_ca_visibilityEb"

/// Runs a callback from the server's timer pass, on the main thread.
///     void WSScheduleCallbackOnCurrentSession(void (*cb)(void *ctx, double t),
///                                             void *ctx, double fireTime)
#define kVNSymScheduleCallback "_WSScheduleCallbackOnCurrentSession"

typedef void (*VNScheduleCallbackFn)(void (*)(void *, double), void *, double);

#pragma mark - Drawing a window differently

//  Alpha is a dead end on an ordinary window. `update_alphas` gates every real
//  effect on bit 2 of `[win + 0x967]`, which is clear: it stores the numbers,
//  returns mask 1, and `set_window_alphas` -- needing >= 4 -- never commits. A
//  scan of all instructions in __text found 67 reads of that byte and zero
//  writes, and Apple's own CGXSetWindowListAlpha fade is equally inactive on
//  such a window.
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

/// Window shadow management
#define kVNSymClearShadowDensity "__ZL20clear_shadow_densityP9CGXWindow"
#define kVNSymWSWindowSetShadowEnable "_WSWindowSetShadowEnable"
#define kVNSymWSWindowReleaseShadowResources "_WSWindowReleaseShadowResources"
#define kVNSymSLSSetWindowShadowParameters "_SLSSetWindowShadowParameters"

typedef void (*VNSystemWindowReleaseFn)(CGXWindow *);
typedef CGXWindow *(*VNCreateCloneFn)(CGXWindow *, CGRect, const void *, bool);
typedef const void *(*VNWindowGetDisplayFn)(CGXWindow *);
typedef CGRect      (*VNScreenRectFromRectFn)(CGXWindow *, CGRect);
typedef CGRect      (*VNScreenRectFn)(CGXWindow *);
typedef uint32_t    (*VNWindowGetIDFn)(CGXWindow *);

typedef void   (*VNSetMeshWarpFn)(CGXWindow *, CGXConnection *, unsigned, unsigned, const float *);
typedef CGRect (*VNClippedFrameBoundsFn)(CGXWindow *);

typedef void (*VNClearShadowDensityFn)(CGXWindow *);
typedef void (*VNWSWindowSetShadowEnableFn)(CGXWindow *);
typedef void (*VNWSWindowReleaseShadowResourcesFn)(CGXWindow *);
typedef CGError (*VNSLSSetWindowShadowParametersFn)(uint32_t cid, uint32_t wid, float density, float radius, float xOffset, float yOffset);
typedef void (*VNPostEventByConnectionFn)(CGXConnection *, VNEvent *);

#ifdef __cplusplus
}
#endif

#endif /* SkyLightServer_h */
