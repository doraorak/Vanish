//
//  SLSPrivate.h
//  Header file for undocumented SkyLight SPI
//
//  Derived from CGSPrivate.h (kept beside this file), whose attribution is:
//
//      Arranged by Nicholas Jitkoff
//      Based on CGSPrivate.h by Richard Wareham
//
//      Contributors:
//        Austin Sarner: Shadows
//        Jason Harris: Filters, Shadows, Regions
//        Kevin Ballard: Warping
//        Steve Voida: Workspace notifications
//        Tony Arnold: Workspaces notifications enum filters
//        Ben Gertzfield: CGSRemoveConnectionNotifyProc
//
//  ---------------------------------------------------------------------------
//
//  Why this file exists rather than using CGSPrivate.h directly:
//
//  The window server moved from CoreGraphics to SkyLight, and the symbols were
//  renamed CGS* -> SLS* with it. Every window-management name in CGSPrivate.h is
//  gone on macOS 27. Checked against the live binaries with `dyld_info -exports`:
//
//      CGSSetWindowTransform   absent everywhere   ->  _SLSSetWindowTransform
//      CGSSetWindowAlpha       absent everywhere   ->  _SLSSetWindowAlpha
//      CGSOrderWindow          absent everywhere   ->  _SLSOrderWindow
//      CGSNewTransition        absent everywhere   ->  _SLSNewTransition
//      CGSDefaultConnection    absent everywhere   ->  _SLSMainConnectionID
//
//  CoreGraphics still exports 122 CGS* symbols, but they are geometry helpers
//  (CGSBoundingShape*) and none of the window API. SkyLight exports 1,480 SLS*.
//
//  So CGSPrivate.h remains correct about the SHAPES -- the structs, the enums,
//  the error conventions, the argument orders -- and is still the best
//  documentation of them in existence. It is kept as a reference. This file is
//  what the code actually compiles against.
//
//  Only what is needed is declared here. Add symbols as they are used, and
//  verify each one exists first:
//
//      dyld_info -exports /System/Library/PrivateFrameworks/SkyLight.framework/\
//          Versions/A/SkyLight | grep _SLSWhatever
//

#ifndef SLSPrivate_h
#define SLSPrivate_h

#include <CoreGraphics/CoreGraphics.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int SLSConnectionID;
typedef uint32_t SLSWindowID;

#pragma mark - Connection

/// The window server connection for this process.
///
/// CGSPrivate.h calls this `_CGSDefaultConnection`. That symbol no longer
/// exists; SkyLight exports `SLSMainConnectionID` (and `_SLSDefaultConnection`,
/// note the leading underscore in the C name itself).
extern SLSConnectionID SLSMainConnectionID(void);

/// The server's clock, in seconds. Exported, unlike most of what this tweak
/// touches, and the base for the absolute fire times the scheduler wants.
extern double SLSCurrentRealTime(void);

#pragma mark - Window geometry and appearance

extern CGError SLSGetWindowBounds(SLSConnectionID cid, SLSWindowID wid, CGRect *outBounds);
extern CGError SLSGetWindowLevel(SLSConnectionID cid, SLSWindowID wid, int *outLevel);

extern CGError SLSSetWindowAlpha(SLSConnectionID cid, SLSWindowID wid, float alpha);
extern CGError SLSGetWindowAlpha(SLSConnectionID cid, SLSWindowID wid, float *outAlpha);

extern CGError SLSSetWindowTransform(SLSConnectionID cid, SLSWindowID wid, CGAffineTransform transform);
extern CGError SLSGetWindowTransform(SLSConnectionID cid, SLSWindowID wid, CGAffineTransform *outTransform);

#pragma mark - Ordering

/// Same three values as CGSWindowOrderingMode in CGSPrivate.h. A window closing
/// reduces to an order-out.
typedef enum {
    kSLSOrderAbove =  1,
    kSLSOrderBelow = -1,
    kSLSOrderOut   =  0
} SLSWindowOrderingMode;

extern CGError SLSOrderWindow(SLSConnectionID cid, SLSWindowID wid,
                              SLSWindowOrderingMode place, SLSWindowID relativeToWID);

extern CGError SLSReleaseWindow(SLSConnectionID cid, SLSWindowID wid);

#pragma mark - Window animations

//  SkyLight exports a window-animation API. It does not work.
//
//  Every one of these symbols exists in the export trie and every one of them is
//  a tombstone -- a contiguous block of two-instruction bodies that return an
//  error without doing anything. Disassembled on 27.0 (26A5406e):
//
//      SLSCreateGenieWindowAnimation                   mov w0, #1006 ; ret
//      SLSCreateSheetWindowAnimation                   mov w0, #1006 ; ret
//      SLSCreateSheetWindowAnimationWithParent         mov w0, #1006 ; ret
//      SLSCreateMetalSheetWindowAnimationWithParent    mov w0, #1006 ; ret
//      SLSCreateMetalSheetWindowAnimation...AndShift   mov w0, #1006 ; ret
//      SLSWindowAnimationSetParent                     mov w0, #1006 ; ret
//      SLSSetWindowAnimationProgress                   mov w0, #1001 ; ret
//      SLSReleaseWindowAnimation                       mov w0, #1001 ; ret
//      SLSUpdateWindowAnimationOrigin                  ret
//
//  1006 is kCGErrorNotImplemented, 1001 kCGErrorIllegalArgument. There is also no
//  _X MIG server routine for any of them, so there is nothing on the other side
//  listening either.
//
//  They are deliberately not declared here. Many exported animation symbols
//  are stubs that return kCGErrorNotImplemented.
//
//  The live engine is server-internal and unexported (PKGWindowAnimationCreate,
//  PKGWindowTransformAnimationCreate, _PKGAnimationCallback). Reaching it means
//  matching an unexported ABI inside the window server. The functions declared
//  above -- SLSSetWindowTransform and SLSSetWindowAlpha -- are real, and driving
//  them frame by frame is the dull, safe way to animate.

#ifdef __cplusplus
}
#endif

#endif /* SLSPrivate_h */
