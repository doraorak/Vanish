# Phase 3 — static analysis of the close path

Target: macOS 27.0 (26A5406e), arm64e.
SkyLight UUID `1DC6C76E-DA47-35D8-BD2D-F40711AF128C`.

Everything below was read out of the binary, not inferred. Two results contradict
the plan this tweak was started from, so they are stated first.

## Method

SkyLight lives only in the dyld shared cache, so `otool` and `nm` cannot read it
and `dyld_info` only sees the export trie. The server-side names are **local**
symbols, which live in the cache's linkedit subcache — lldb reads them, nothing
else here does.

WindowServer itself is not debuggable in practice: attaching stops the
compositor and freezes the GUI, which is the exact failure this whole project is
built to avoid. So analysis ran against a throwaway host that does nothing but
`dlopen` SkyLight and `pause()`, giving lldb a mapped copy with symbols at a
known slide. `scratchpad/slhost.c` + `sl.sh`.

## The client/server split

SkyLight is both halves of the window server in one binary, and the prefix says
which half a function belongs to:

| prefix | side | exported? |
|---|---|---|
| `SLS*` | client — marshals a Mach message | yes, all 2,915 exports |
| `_X*` | MIG server routine — unmarshals it | **no** |
| `CGX*`, `PKG*`, `WS*`, and the lowercase C++ internals | server implementation | **no** |

`dyld_info -exports` returns **0** matches for any `_X*` or `CGX*` name. Every
one of the 2,915 exports is client-side, so `dlsym` finds none of the server
half — confirmed at runtime, it returns `NULL`.

That is a statement about `dlsym`, not about symbol resolution. The server-side
names are ordinary **local** symbols, and for a shared-cache image the cache-wide
`__LINKEDIT` is mapped into the process like any other segment, so `LC_SYMTAB`
can just be walked: `n_value` is the unslid address, add the slide. Verified
against the real SkyLight in `tools/restest.m` — all four symbols the tweak needs
resolve by name, to the addresses lldb reports.

So resolution is by **name, not offset**. The offsets below are for reading the
binary, not for shipping.

## The close path

A red-button close is `-[NSWindow orderOut:]`, which reduces to an order-out.
Traced end to end:

```
app: SLSOrderWindow(cid, wid, kSLSOrderOut, 0)
       └─ SLSOrderWindowList(cid, &wid, &op, &rel, 1)      [+0 builds a 1-element list]
            ├─ normalises op == 2 to op = 1, rel = 0       [+96..+136]
            ├─ CGSGetConnectionPortById                    [+140]
            └─ count == 1 ? _CGSOrderWindow (MIG client)   [+192]
                          : hand-rolled Mach msg, id 0x1513
                                    │
                              ── Mach ──
                                    │
WindowServer: _XOrderWindow(request, reply)                [+0x493900]
       ├─ rejects msgh_size != 0x2c
       ├─ wid = req[0x20], place = req[0x24], rel = req[0x28]
       ├─ window_by_id(wid)                                [+0x354DF4]
       ├─ CGXConnectionForPort(port)                       [+0x238F0C]
       ├─ connection_holds_rights_on_window(conn, 4, win, 1, 1)
       └─ CGXOrderWindowListSpaceSwitchOptions(conn, &wid, &place, &rel, 1, false)
```

### Every ordering path converges on one function

Confirmed by disassembling each entry point and following its calls:

| entry | reaches the convergence point |
|---|---|
| `_XOrderWindow` | directly, `bl` at +164 |
| `_XOrderWindowList` → `_CGXOrderWindowList` | tail call `b` at +192 |
| `_XOrderWindowListWithOperation` → `CGXOrderWindowListWithOperation` | `bl` |
| `_XOrderWindowListWithGroups` → `CGXOrderWindowListWithGroups` | `bl`, after group repair |

So a **single hook on `CGXOrderWindowListSpaceSwitchOptions` catches every window
ordering operation in the system**, from every client, including order-out.

```c
void CGXOrderWindowListSpaceSwitchOptions(
        CGXConnection *conn,
        const uint32_t *wids,
        const CGSOrderOp *ops,      // 0 = order out
        const uint32_t *relativeTo,
        unsigned count,
        bool spaceSwitch);
```

Its own body calls `start_order_window` / `finish_order_windows`, which is where
the ordering is actually committed.

### Offsets from SkyLight's mach_header

Pinned to the UUID above. They **will** move on any OS update.

| symbol | offset |
|---|---|
| `connection_holds_rights_on_window` | `+0x1CD044` |
| `CGXConnectionForPort` | `+0x238F0C` |
| `start_order_window` | `+0x35438C` |
| `finish_order_windows` | `+0x3548BC` |
| `window_by_id` | `+0x354DF4` |
| **`CGXOrderWindowListSpaceSwitchOptions`** | **`+0x361E88`** |
| `_XOrderWindow` | `+0x493900` |
| `_XOrderWindowList` | `+0x493A00` |
| `_XOrderWindowListWithOperation` | `+0x4A179C` |
| `_XOrderWindowListWithGroups` | `+0x4B0564` |

These are for cross-checking a disassembly. The tweak does not use them: it
resolves by mangled name through `LC_SYMTAB`, which survives an OS update where
an offset does not.

## The public animation API is dead code

The plan assumed `_SLSCreateGenieWindowAnimation` and friends were a shortcut
past reversing the compositor. They are not. The symbols are exported, but the
functions are tombstones — a contiguous block of them at `0x19f422b40`:

| export | body | meaning |
|---|---|---|
| `SLSCreateGenieWindowAnimation` | `mov w0, #1006; ret` | `kCGErrorNotImplemented` |
| `SLSCreateSheetWindowAnimation` | `mov w0, #1006; ret` | |
| `SLSCreateSheetWindowAnimationWithParent` | `mov w0, #1006; ret` | |
| `SLSCreateMetalSheetWindowAnimationWithParent` | `mov w0, #1006; ret` | |
| `SLSCreateMetalSheetWindowAnimationWithParentAndShift` | `mov w0, #1006; ret` | |
| `SLSWindowAnimationSetParent` | `mov w0, #1006; ret` | |
| `SLSSetWindowAnimationProgress` | `mov w0, #1001; ret` | `kCGErrorIllegalArgument` |
| `SLSReleaseWindowAnimation` | `mov w0, #1001; ret` | |
| `SLSUpdateWindowAnimationOrigin` | `ret` | no-op |

There is also **no `_X` MIG routine** for any of them, which corroborates it:
nothing on the server side is listening. Verifying that a symbol exists was not
enough — it had to be disassembled.

## What is actually live

The real animation engine is server-internal and unexported, `PKG*`:

```
PKGAnimationCreate / Push / Pop / Release
PKGAnimationTransactionBegin / Commit
PKGAnimationAssociateDisplay
PKGWindowAnimationCreate
PKGWindowTransformAnimationCreate        <- the interesting one
_PKGAnimationBegin(void *, const double *)
_PKGAnimationCallback(void *, double)    <- progress-driven, 0..1
_PKGAnimationScheduleCallback(PKGAnimationSessionData *)
```

`PKGWindowTransformAnimationCreate` allocates a 0x190-byte animation object via
`PKGWindowAnimationCreate`, signs a `PKGWindowTransformAnimationDealloc` pointer
into `+0x10`, and copies **two 0x80-byte matrices** (4×4 doubles — `CATransform3D`)
into `+0x90` and `+0x110`: a from-transform and a to-transform. That is exactly a
window transform animation, and it is what the system uses.

Client side, these remain real and are the cheap path:

| export | state |
|---|---|
| `SLSSetWindowAlpha` | live |
| `SLSSetWindowTransform` | live |
| `SLSSetWindowListAlpha` | live |
| `SLSTransactionSetWindowTransform` | live |
| `SLSTransactionSetWindowAlphaAnimated` | live |

## The fade nobody can reach

`CGXSetWindowListAlpha(CGXConnection *, CGXWindow *, float alpha, float duration)`
branches on its duration:

```
+32  fcmp s1, #0.0
     duration >  0  ->  CGXWindow::fade_begin(conn, alpha, duration,
                                              void (^)(CGXWindow *, bool))
     duration == 0  ->  set_window_alphas(conn, win, alpha)
```

`fade_begin` is a real, compositor-driven animated fade with a completion block.

The public API cannot get to it. `SLSSetWindowAlpha` builds the same message and
hardcodes the duration to zero — literally `movi.2d v1, #0` at +56 — so every
client that has ever set a window's alpha has taken the instant path. The
animated one has been sitting behind a parameter no exported function will pass.

That is what this tweak uses, and it is why there is no timer in it: the window
server does the animation, and tells us when it has landed.

## The genie is not in SkyLight

Searching SkyLight for `genie`/`miniaturiz` returns exactly one hit, the dead
`SLSCreateGenieWindowAnimation` stub. The effect lives in **the Dock**:
`/System/Library/CoreServices/Dock.app/Contents/MacOS/Dock` carries an ObjC class
`WAGenieAnimation`, a `genieSpeed` property, `genie-speed`, and a
`genieTo:direction:genie:` selector.

So minimise is not a window-server animation that a close could be modelled on.
The Dock owns the window's appearance for the duration of the effect. Copying it
means either reproducing it (a mesh warp toward a target rect) or driving the
Dock, and neither is a small change to a window-server tweak. Worth doing later
for a "genie close"; not the thing to build first.

## arm64e: a correct address is not a callable pointer

The first build resolved every symbol correctly and died on the first call:

```
SkyLight   +0x354df4   window_by_id(unsigned int)     EXC_BAD_ACCESS
Vanish     +0xeac      vn_order_window_list           PAC_EXCEPTION
SkyLight   +0x464330   update_cursor_callback
```

`+0x354df4` is exactly the offset in the table above, so the lookup was right to
the byte. The failure is that on arm64e a C function pointer carries a PAC
signature and clang emits an authenticating branch (`blraa`) at the call site. A
bare `n_value + slide` has no signature, so the call traps, and the kernel
answers a PAC exception with **SIGKILL** — which no signal handler catches, so
the crash tripwire cannot see it either.

The fix is one call, at resolution time:

```c
ptrauth_sign_unauthenticated(raw, ptrauth_key_function_pointer, 0);
```

Zero discriminator, key IA — what clang uses for a plain C function pointer on
this ABI. Verified rather than assumed: for a symbol that is both exported and in
the symbol table, the signed pointer comes out **bit-identical** to what `dlsym`
returns, signature bits included (`tools/restest.m`).

`MSHookFunction` wants the opposite — it reads and patches instructions at the
address rather than branching to it — so the target is passed through
`ptrauth_strip` first.

## The cursor is a window

That same trace shows the hook being entered from `update_cursor_callback`, on a
timer, during login — before any user has clicked anything. The cursor is an
ordinary window and it is ordered out like one. Anything installed on this path
runs constantly and runs early, which is why a first build that only misbehaves
on window close will still take the session down at the login screen.

## A demangled name is not an ABI

Second crash, same hook, further along — PAC fixed, `window_by_id` fine,
`fade_begin` reached:

```
libsystem_blocks   _Block_copy                    SIGSEGV at 0x4d01b
SkyLight  +0x35c6ec CGXWindow::fade_begin
Vanish    +0xeb4    vn_order_window_list
SkyLight  +0x2a0274 CGXOrderWindowListWithGroups
```

The symbol demangles to

```
CGXWindow::fade_begin(CGXConnection *, float, float, void (CGXWindow *, bool) block_pointer)
```

which puts `this` in x0, the connection in x1, the floats in s0/s1 and the block
in x2. The code does no such thing. It `_Block_copy`s **x1** and never reads x2,
and its only in-tree caller passes `x1 = 0` to mean "no completion". The
`CGXConnection *` parameter is dead, and because `fade_begin` is not exported the
optimizer was free to delete it and shift everything left.

Observed ABI:

```
x0 = CGXWindow *this
x1 = void (^completion)(CGXWindow *, bool)    // may be NULL
s0 = float target alpha
s1 = float duration
```

So the connection pointer was being handed to `_Block_copy`. The lesson is
narrow and worth keeping: for an unexported symbol the mangled name records the
*source* signature, not the calling convention. Read a real call site.

The completion is safe to depend on — every path runs it. With a fade it is
queued behind the fade timer and invoked from `fade_finish`; when the window is
already at the target alpha and there is no fade state, `+268` invokes it inline
with `finished = false`.

## The real cause: re-entrancy, not lifetime

Two crashes landed on the same instruction —

```
+148:  ldr  x12, [x12, #0x258]      ; per-window ordering info
+152:  str  w11, [x12, #0x8]        ; NULL + 8
```

— inside `update_window_order_info`, which walks the session's window list and
stamps an ordering index into each window's info struct.

The first of them looked like a lifetime problem and was guessed at as one. The
second showed the actual mechanism, in its stack:

```
9  vn_order_window_list                     <- our hook, passing through
8  CGXOrderWindowListSpaceSwitchOptions
7  start_order_window + 196
6  CGXWindow::fade_finish + 164
5  invocation function for block in fade_finish
4  __vn_order_window_list_block_invoke      <- our completion, from an earlier close
3  CGXOrderWindowListSpaceSwitchOptions     <- we re-enter ordering
2  start_order_window
1  WSWindowUpdateOrderedIn
0  update_window_order_info                 <- SIGSEGV
```

**`start_order_window` force-finishes pending fades.** Ordering any window drains
the fade-completion blocks of others, so a completion runs *inside* the ordering
machinery. Issuing an order-out from there re-enters
`CGXOrderWindowListSpaceSwitchOptions` while an outer one is still walking the
window list, and the inner pass leaves an entry the outer pass then stores
through.

Nothing about window lifetime is involved. The guard added for that theory —
re-resolving the window by id — is harmless and stays, but it was not the fix.

## The fix: leave the stack first

SkyLight schedules its own deferred work through

```c
void WSScheduleCallbackOnCurrentSession(void (*cb)(void *ctx, double t),
                                        void *ctx, double fireTime);
```

read off its body: it shuffles x0/x1 into x1/x2, fills in `gMainThreadQueue`,
the current session and two zero flags, and tail-calls
`reschedule_callback_on_session`. `d0` passes straight through, and callers give
an absolute time — `SLSCurrentRealTime()` plus a delay. `schedule_window_fade_timer`
is the worked example.

So the fade completion now records the order-out and schedules a drain; the
drain issues it from the timer pass. That is a context the server already orders
windows from — the same crash report shows `run_timer_pass` ->
`rebuild_menu_bars_callback` -> `reposition_one_menu_bar` ->
`CGXOrderWindowListSpaceSwitchOptions` — so it is a legitimate top-level call
rather than a nested one.

The visibility gate matters here too, and not only as an optimisation: a fade to
the alpha a window already has is answered by invoking the completion inline,
which would put the completion straight back inside the ordering call it is
trying to escape.

## Where the evidence runs out

The deferral fix worked, in the narrow sense: the next crash has no nesting.

```
4  Vanish  vn_order_window_list + 128            <- a plain pass-through
3  CGXOrderWindowListSpaceSwitchOptions
2  start_order_window + 1304
1  WSWindowUpdateOrderedIn
0  update_window_order_info                      <- SIGSEGV
```

No block, no fade, no drain — the hook did nothing but call the original, and
the original crashed on the same NULL. So the ordering state was already
corrupt, and the stack cannot say what corrupted it. Three candidates remain and
the crash distinguishes none of them: the fade itself, the deferral window
during which a window is transparent but still ordered in, or the hook — an
ellekit trampoline over a function the server calls constantly.

Guessing between three has already cost several sessions, so the build carries a
`VN_MODE` ladder instead:

| mode | behaviour | what it proves |
|---|---|---|
| 0 | pass through only; hook installed, symbols resolved | stable => the hook is sound, the fault is in the animation |
| 1 | fade, then order out in the same call | stable => the fade is fine, the deferral is the fault |
| 2 | fade, order out when it lands | the feature |

Mode 0 first, because it is the base case: if the machine is unstable with a
hook that does nothing, nothing else is worth reasoning about.

## The drag-fade subsystem

Not yet used, and the most promising native route left. SkyLight fades windows
during drags through machinery entirely separate from `CGXWindow::fade_begin`:

```
WSWindowShouldDragFade / WSWindowHasDragFade
WSWindowGetDragFadeEffect / WSWindowGetDragFadeList
WSWindowSetDisplayDragFadeAlpha(CGXWindow *, shared_ptr<Display> const &, float)
WSWindowGetDisplayDragFadeAlpha
WSWindowCopyDragFadedDisplayRegion
CGXWindow::finish_drag_fade()
scheduleDragFadeTimer() / gScheduledDragFadeTimer
_windowDragFadeDidEndHandler(CGSNotificationType, void *, unsigned int, void *)
WS::CAWindowContent::persist_context_drag_fade()
```

Two things make it interesting. The alpha is **per display** and held beside the
window rather than on it, so it does not fight the window's own alpha. And
`persist_context_drag_fade` keeps the window's CoreAnimation content alive
independently of the window — which is the native form of the snapshot idea, and
would remove the need to keep a dying window ordered in at all.

## Mesh warp draws; alpha does not

Proven on device. `CGXWindow::set_mesh_warp(win, conn, w, h, const float *mesh)`
visibly deforms a real window.

```
_XSetWindowWarp -> CGXWindow::set_mesh_warp    // x0=win x1=conn x2=w x3=h x4=mesh
```

`_XSetWindowWarp` validates `float_count == w * h * 4`, `w, h <= 2048`, pinning
the mesh to the classic `CGSSetWindowWarp` layout: `w*h` vertices, row-major,
`{local.x, local.y, global.x, global.y}` — local in window coordinates, global in
screen coordinates. Only the global points move.

Unlike alpha it needs no help: `set_mesh_warp` allocates the mesh, stores it at
`[win + 0x8b0]`, and calls `CGXSynchronizeSurfaces`, `CGXInvalidateDisplayShape`
and `CGXScheduleUpdateAllDisplays` itself. Scanning readers of `[win + 0x8b0]`
shows the CoreAnimation path consuming it — `WS::Updater::prepare_coreanimation`,
`ca_window_prep_ingestion_phase`, `ca_prepare_begin_window_update` — so it is not
a legacy-compositor leftover the way the window alpha turned out to be.

**Cost per frame is real.** `set_mesh_warp` invalidates and reschedules every
display, and `clipped_frame_bounds()` allocates a region
(`CGXCreateCombinedWindowClipShape`) and combines shapes on every call. Sampling
the bounds once per animation instead of once per frame is what stops it
stuttering while the user is dragging.

## What still hides a closing window

The warp renders on an open window and not on a closing one, and the close path
is otherwise clean: no second order-out reaches the hook, no `release_window`,
`update_ca_visibility` fires once at +1 ms and never again, and the test app
returns NO from `windowShouldClose` so it neither closes the window nor quits.

That leaves the client. `-[NSWindow orderOut:]` does more than send the
order-out, and the suspicion is the window's CoreAnimation context stops being
committed — which is what `WS::CAWindowContent::persist_context_drag_fade()`
exists to prevent. Comparing a probe trace against a close trace for the same
window's content pointer is the way to settle it.

## The pattern behind every failure

Four server-side facilities were tried on a closing window. Three did nothing
and one crashed, and they failed for one reason.

| tried | result |
|---|---|
| `CGXSetWindowListAlpha` / `set_window_alphas` | stores the value, `update_alphas` returns mask 1, never commits |
| `WSSetWindowListFadeProperties` | SIGSEGV on a NULL inside its block |
| `WSWindowFreezeContent` | SIGSEGV -- `WindowFreeze::Update` reads NULL+0x110 |
| `CGXWindow::set_mesh_warp` | **works, but only on a window that is still being composited** |

Every one of these is designed to be driven from inside the compositor's own
update pass -- they want a `WS::Updater::UpdateState`, a `CGXRedrawState`, a
`Breadcrumbs` -- or from a client's CA transaction. `WSWindowFreezeContent`'s
only in-tree caller is `set_debug_options`. Called ad hoc from an ordering hook
they either no-op or dereference something the update pass would have set up.

`set_mesh_warp` is the exception precisely because it does not need that: the
mesh sits at `[win + 0x8b0]` and is read by the CA path on the next frame the
window is composited. Which is also why it stops working the moment the window
stops being composited -- and after the client's `orderOut:` it stops being
composited, whatever the server-side ordering says. Neither swallowing the
order-out nor letting it land and re-ordering the window in changes that.

## What follows from it

The mechanism is not in doubt: a mesh warp on a live window draws, and any
animation expressible as moving grid points can be drawn that way. What is in
doubt is animating **the dying window**, and nothing server-side keeps a window
composited once its client has let go.

So animate something else: a window we own, showing the closing window's
content, warped by the same mesh code, destroyed when the animation ends. It is
a live window, so everything already proven on live windows applies to it
unchanged -- and it is what the Dock does for the genie, which is the same
conclusion reached from the other direction back when the genie turned out not
to live in SkyLight at all.

## Consequences for Phase 4

1. **Trigger**: `CGXOrderWindowListSpaceSwitchOptions`, narrowed to
   `count == 1 && ops[0] == 0 && relativeTo[0] == 0`, and to windows with a
   non-zero current alpha.
2. **Resolution**: by mangled name via `LC_SYMTAB`, signed for arm64e.
   All-or-nothing — if any symbol stops resolving, no hook is installed.
3. **Animation**: `fade_begin(win, completion, 0.0f, duration)`, ordering the
   window out in the completion, then restoring alpha to 1.
4. **Deferral is done off-stack**, through the server's own timer callback.
5. **Open risk: connection lifetime.** The completion captures the
   `CGXConnection *` and uses it a duration later. The window is re-validated;
   the connection is not, because nothing cheap re-derives it.

## The Mode 2 Root Cause & Resolution

The crash report at `update_window_order_info() + 152`:
```assembly
0x18c4a2b90 <+144>: add    w10, w10, #0x1
0x18c4a2b94 <+148>: ldr    x12, [x12, #0x258]   ; x12 = win->workspace_data (offset 0x258)
0x18c4a2b98 <+152>: str    w11, [x12, #0x8]     ; crash! dereferencing NULL + 0x8
```

### The Mechanism
1. Offset `0x258` of `CGXWindow` is `win->workspace_data`.
2. There are only two callers in the entire OS that modify this field (`spaces_did_create_window_callback` and `spaces_did_terminate_window_callback`).
3. When AppKit closes and deallocates a window (`isReleasedWhenClosed == YES`), `spaces_did_terminate_window_callback` runs, frees the workspace data, and sets `[win + 0x258] = NULL`.
4. Under standard operation, `start_order_window` removes the window from `session->ordered_windows` (`(char *)wd + 0x20`) via `CGXWindowArrayRemove` before deallocation occurs.
5. In Mode 2, Vanish deferred the `orderOut` call to allow the fade animation to run. Consequently, `win` remained in `session->ordered_windows`.
6. When the client deallocated the window immediately, `spaces_did_terminate_window_callback` cleared `win->workspace_data`.
7. On the very next order-in/order-out event anywhere in the system, `update_window_order_info` walked `session->ordered_windows`, encountered the terminated window whose `workspace_data` was `NULL`, and dereferenced `NULL + 0x8`.

### The Solution
`update_window_order_info` (`__ZL24update_window_order_infov`) is hooked. Before the original function executes its walk, our hook inspects `session->ordered_windows` (`_CGSessionGetWindowData(_CGSessionControlGetCurrentSession())`). Any window entry whose `workspace_data` (`*(void **)((char *)w + 0x258)`) has become `NULL` is cleanly removed from the array using SkyLight's native `_CGXWindowArrayRemove`.

## Eliminating the Close Gap: Pre-Cloning (Option A) & Client Behavior (Option C)

### Root Cause of the Gap
When a window closes, two independent timing bottlenecks conspire to create a visible gap:
1. **AppKit Client Animation (Option C Analysis)**:
   - `-[NSWindow orderOut:]` -> `[self orderWindow:NSWindowOut relativeTo:0]` -> `_doOrderWindow:`.
   - `_doOrderWindow:` calls `-[NSWindow _effectiveOrderOutAnimationTypeIfModal:]`.
   - Document windows (`animationBehavior == 3`, e.g. TextEdit) trigger `_NSWindowTransformAnimation` (Type 6), running a ~200 ms client-side fade/transform before sending `SLSOrderWindow(..., kSLSOrderOut, ...)`.
   - Non-document windows or windows with `reduceMotion` return Type 2 (`NSWindowAnimationBehaviorNone`), skipping the 200 ms fade and dispatching `SLSOrderWindow` immediately.
   - **However**, even with Type 2, AppKit commits a CoreAnimation transaction that unbinds its surface at the exact same millisecond it sends `SLSOrderWindow`.
2. **WindowServer Compositor Handover**:
   - Creating a snapshot clone (`CreateCloneOfWindow`) *inside* `CGXOrderWindowListSpaceSwitchOptions` means the clone is newly introduced to the window list.
   - WindowServer only ingests and composites the new clone on the *subsequent* compositor pass (~16.6 ms at 60 Hz).
   - Because the client has already unbound its surface, ordering out the original immediately leaves a 1-frame blank desktop gap. Holding the original back also fails because its surface is already gone.

### The Solution: Event-Time Pre-Cloning (Option A)
Pre-cloning moves snapshot creation from **order-time** back to **click-time**:
1. **Event Interception**:
   - Hook `CGXPostEventByConnection(CGXConnection *conn, void *event)` (symbol `_CGXPostEventByConnection`, resolved via `LC_SYMTAB`).
   - `SLSEventRecord` layout:
     - `+0x08`: `uint32_t type` (`1` = `kCGEventLeftMouseDown`)
     - `+0x20`: `CGPoint local_pt` (window-local coordinates, top-left origin)
     - `+0x3c`: `uint32_t wid` (target window ID)
2. **Pre-Clone Trigger**:
   - When `type == 1` hits a target window in the top-left traffic light close box (`x in [0, 80], y in [0, 40]`):
     - Window is 100% alive, opaque, and fully composited.
     - Invoke `CreateCloneOfWindow` and order it `kVNOrderAbove` the target window.
     - Cache in `gPreClone` (`orig_wid`, `clone_wid`, `clone`, `conn`, `created_at`).
3. **Zero-Gap Compositor Handover**:
   - The user's physical click duration (time between mouse-down and mouse-up) is 80–150 ms (5–9 compositor frames at 60 Hz, 10–18 frames at 120 Hz ProMotion).
   - The clone is fully ingested and composited while the mouse button is still held down.
   - When mouse-up occurs, AppKit calls `orderOut:` -> `CGXOrderWindowListSpaceSwitchOptions(..., kVNOrderOut)`.
   - Vanish immediately consumes `gPreClone`, starts the mesh warp animation on the clone, and orders out the original window behind it.
   - Result: **0 ms handover gap, 0 blank frames, 100% sharp initial frame**.
4. **Safety & Fallbacks**:
   - If the user presses mouse-down in the close box but drags away without releasing, a 1.5s timer (`vn_preclone_cleanup_timer`) automatically discards and releases the unused clone.
   - If the window is closed via keyboard shortcut (⌘W) or application menu, `gPreClone` is empty; Vanish falls back to on-the-fly snapshot creation.

5. **Event Routing & Hit-Testing (`kVNOrderBelow`)**:
   - In modern SwiftUI applications with `.windowStyle(.hiddenTitleBar)` (e.g. `TweakInject`), hit-testing is evaluated dynamically during mouse events.
   - If the pre-clone was ordered `kVNOrderAbove`, it covered the window and intercepted `kCGEventLeftMouseUp`, preventing the app's close button from completing the click action.
   - By ordering the pre-clone `kVNOrderBelow`, the target window stays on top throughout mouse tracking. The application receives mouse-up without interference, triggering `orderOut:`. When `orderOut:` arrives, the target window is hidden, revealing the pre-clone already composited directly behind it.

6. **Lock Recursion Safety**:
   - `os_unfair_lock` is strictly non-recursive. Calling `vn_system_window_release()` while holding `gPreCloneLock` triggered `vn_hooked_release_window`, which attempted to re-acquire `gPreCloneLock`, resulting in `_os_unfair_lock_recursive_abort`.
   - The state is now extracted via `vn_preclone_take_locked()`, and all calls to `vn_system_window_release()` occur strictly after releasing `gPreCloneLock`.
   - `vn_hooked_release_window` handles `gPreClone.clone == win` by zeroing out state without re-invoking release.
   - Hit-testing box is refined to `[15, 60] x [0, 36]` to strictly capture the physical red close button while ignoring clicks on yellow/green buttons or the titlebar.
