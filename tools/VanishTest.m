#import <Cocoa/Cocoa.h>

@interface VanishTestDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic, strong) NSWindow *window;
@end

@implementation VanishTestDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSRect frame = NSMakeRect(200, 200, 500, 340);
    NSUInteger style = NSWindowStyleMaskTitled |
                       NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable |
                       NSWindowStyleMaskResizable;
    
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    [self.window setTitle:@"Vanish Test Window"];
    [self.window setReleasedWhenClosed:NO];
    [self.window setDelegate:self];
    [self.window center];
    
    NSTextField *label = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 100, 460, 120)];
    [label setEditable:NO];
    [label setSelectable:NO];
    [label setBezeled:NO];
    [label setDrawsBackground:NO];
    [label setAlignment:NSTextAlignmentCenter];
    [label setFont:[NSFont systemFontOfSize:16 weight:NSFontWeightMedium]];
    [label setStringValue:@"Vanish Test Window\n\nClick the red close button to trigger the close animation."];
    
    [self.window.contentView addSubview:label];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)windowShouldClose:(NSWindow *)sender {
    float dur = 2.5f;
    FILE *f = fopen("/tmp/vanish_duration", "r");
    if (f) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            float val = strtof(buf, NULL);
            if (val >= 0.05f) dur = val;
        }
        fclose(f);
    }
    float delay = dur + 2.0f;
    if (delay < 4.0f) delay = 4.0f;
    NSLog(@"[VanishTest] Red button clicked. Ordering out window, delaying exit %.1fs...", delay);

    // Order out the window -- this sends SLSOrderWindow (op=0) to WindowServer, triggering Vanish!
    [sender orderOut:nil];

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        NSLog(@"[VanishTest] Delay finished, terminating app.");
        [NSApp terminate:nil];
    });

    // Return NO so AppKit does NOT execute [NSWindow close], preserving the window's backing store and view hierarchy during the WindowServer animation
    return NO;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return NO;
}

@end

int main(int argc, const char * argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        VanishTestDelegate *delegate = [[VanishTestDelegate alloc] init];
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}
