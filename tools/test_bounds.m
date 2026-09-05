#import <Cocoa/Cocoa.h>
#import "../Vanish.m"

typedef int (*SLSGetWindowBoundsFn)(uint32_t cid, uint32_t wid, CGRect *outRect);

int main() {
    [NSApplication sharedApplication];
    NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 400, 300)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    [win orderFront:nil];
    uint32_t wid = (uint32_t)[win windowNumber];
    int cid = SLSMainConnectionID();
    printf("wid=%u cid=%d\n", wid, cid);

    SLSGetWindowBoundsFn getBounds = (SLSGetWindowBoundsFn)dlsym(RTLD_DEFAULT, "SLSGetWindowBounds");
    SLSGetWindowBoundsFn getFrameBounds = (SLSGetWindowBoundsFn)dlsym(RTLD_DEFAULT, "SLSGetWindowFrameBounds");

    CGRect r1 = CGRectZero, r2 = CGRectZero;
    if (getBounds) {
        getBounds(cid, wid, &r1);
        printf("SLSGetWindowBounds:      (%.1f, %.1f  %.1f x %.1f)\n", r1.origin.x, r1.origin.y, r1.size.width, r1.size.height);
    }
    if (getFrameBounds) {
        getFrameBounds(cid, wid, &r2);
        printf("SLSGetWindowFrameBounds: (%.1f, %.1f  %.1f x %.1f)\n", r2.origin.x, r2.origin.y, r2.size.width, r2.size.height);
    }

    CGXWindow *gxWin = vn_window_by_id ? vn_window_by_id(wid) : NULL;
    printf("CGXWindow: %p\n", gxWin);
    if (gxWin && vn_clipped_frame_bounds) {
        CGRect cfb = vn_clipped_frame_bounds(gxWin);
        printf("clipped_frame_bounds:    (%.1f, %.1f  %.1f x %.1f)\n", cfb.origin.x, cfb.origin.y, cfb.size.width, cfb.size.height);
    }

    typedef CGRect (*VNCGXFrameBoundsFn)(CGXWindow *);
    VNCGXFrameBoundsFn fbFn = (VNCGXFrameBoundsFn)vn_skylight_symbol("__ZNK9CGXWindow12frame_boundsEv");
    if (gxWin && fbFn) {
        CGRect fb = fbFn(gxWin);
        printf("frame_bounds:            (%.1f, %.1f  %.1f x %.1f)\n", fb.origin.x, fb.origin.y, fb.size.width, fb.size.height);
    }
    return 0;
}
