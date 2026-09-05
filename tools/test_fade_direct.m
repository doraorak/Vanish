#import <Cocoa/Cocoa.h>

extern int SLSMainConnectionID(void);
extern CGError SLSOrderWindow(int cid, uint32_t wid, int mode, uint32_t relTo);
extern CGError SLSSetWindowAlpha(int cid, uint32_t wid, float alpha);

int main(int argc, char **argv) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        
        NSRect frame = NSMakeRect(400, 400, 400, 300);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:frame
                                                    styleMask:NSWindowStyleMaskTitled
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        [win setTitle:@"Direct Fade Test"];
        [win setBackgroundColor:[NSColor redColor]];
        [win makeKeyAndOrderFront:nil];
        
        uint32_t wid = (uint32_t)[win windowNumber];
        int cid = SLSMainConnectionID();
        NSLog(@"Window created wid=%u cid=%d", wid, cid);
        
        // After 2 seconds, call orderOut WITHOUT closing or destroying anything!
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            NSLog(@"Calling orderOut now...");
            [win orderOut:nil];
        });
        
        // Keep running for 8 seconds so we can observe the fade
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            NSLog(@"Test complete, exiting.");
            exit(0);
        });
        
        [app run];
    }
    return 0;
}
