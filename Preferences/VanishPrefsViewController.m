//
//  VanishPrefsViewController.m
//  VanishPrefs
//
//  Custom NSViewController conforming to PSPreferenceController for Vanish.
//  Works seamlessly in both System Settings (PreferenceLoaderTI) and TweakInject.
//

#import "VanishPrefsViewController.h"
#import <notify.h>

#pragma mark - PSPreferences Declarations

extern CFPropertyListRef _Nullable PSPreferencesCopyAppValue(CFStringRef key, CFStringRef domain);
extern void PSPreferencesSetAppValue(CFStringRef key, CFPropertyListRef _Nullable value, CFStringRef domain);
extern Boolean PSPreferencesAppSynchronize(CFStringRef domain);

#pragma mark - FlippedView & ScrollView Helpers

@interface VNPFlippedView : NSView
@end

@implementation VNPFlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

@interface VNPScrollView : NSScrollView
@end

@implementation VNPScrollView
- (void)scrollWheel:(NSEvent *)event {
    if (self.enclosingScrollView != nil) {
        [[self nextResponder] scrollWheel:event];
        return;
    }
    [super scrollWheel:event];
}
@end

#pragma mark - Animation Metadata

typedef struct {
    const char *key;
    const char *name;
    const char *desc;
    double defaultDuration;
} VNAnimationMeta;

static const VNAnimationMeta kAnimations[] = {
    { "shrink",   "Shrink",   "Scale down smoothly toward window center",  0.25 },
    { "squish",   "Squish",   "Vertical compression with elastic rebound", 0.25 },
    { "fall",     "Fall",     "Gravity drop off-screen",                  0.25 },
    { "swirl",    "Swirl",    "Vortex spiral collapse into center",       0.30 },
    { "flip",     "Flip",     "3D perspective flip along horizontal axis",0.25 },
    { "tilt",     "Tilt",     "3D isometric tilt and perspective fade",   0.25 },
    { "slide",    "Slide",    "Slide smoothly off toward screen edge",    0.25 },
    { "genie",    "Genie",    "Classic macOS genie suck into dock/origin",0.35 },
    { "flag",     "Flag",     "Waving banner mesh oscillation",           0.30 },
    { "spin",     "Spin",     "Centroid rotation and scale-down",         0.25 },
    { "roll",     "Roll",     "Cylinder roll up like parchment",          0.30 },
    { "barrel",   "Barrel",   "Barrel roll perspective distortion",       0.35 },
    { "clock",    "Clock",    "Clock hand radial sweep wipe",             0.35 },
    { "dissolve", "Dissolve", "Grainy pixel dispersion fade",             0.25 },
    { "crt",      "CRT Off",  "Retro cathode-ray tube shutdown beam",     0.30 },
    { "shatter",  "Shatter",  "Glass fracture explosion into fragments",  0.40 },
};
#define kAnimationCount (sizeof(kAnimations) / sizeof(kAnimations[0]))

#pragma mark - VanishPrefsViewController

@interface VanishPrefsViewController () <NSTextFieldDelegate> {
    NSScrollView *_scrollView;
    VNPFlippedView *_contentView;
    
    // General Controls
    NSSwitch *_enabledSwitch;
    NSSwitch *_shadowsSwitch;
    NSTextField *_targetAppField;
    
    // Animation & Timing Controls
    NSPopUpButton *_animPopUp;
    NSSlider *_durationSlider;
    NSTextField *_durationLabel;
    NSTextField *_durationSubtitle;
    NSButton *_resetDurationBtn;
    NSSlider *_refreshRateSlider;
    NSTextField *_refreshRateLabel;
    
    CGFloat _totalContentHeight;
}
@end

@implementation VanishPrefsViewController

- (instancetype)init {
    self = [super initWithNibName:nil bundle:nil];
    if (self) {
        _preferencesDomain = @"com.doraorak.vanish";
    }
    return self;
}

- (void)setPreferencesDomain:(NSString *)domain {
    if (domain && domain.length > 0) {
        _preferencesDomain = [domain copy];
    }
    [self reloadAllValues];
}

- (void)preferencesDidAppear {
    [self reloadAllValues];
    [self updateLayoutSizing];
}

- (void)preferencesDidDisappear {
    if (_targetAppField && self.view.window) {
        [self.view.window makeFirstResponder:nil];
    }
}

#pragma mark - Preferences Accessors

- (id)readPrefValue:(NSString *)key {
    if (!key || !_preferencesDomain) return nil;
    CFPropertyListRef val = PSPreferencesCopyAppValue((__bridge CFStringRef)key,
                                                      (__bridge CFStringRef)_preferencesDomain);
    return val ? (id)CFBridgingRelease(val) : nil;
}

- (BOOL)readBool:(NSString *)key defaultValue:(BOOL)def {
    id val = [self readPrefValue:key];
    return val ? [val boolValue] : def;
}

- (double)readDouble:(NSString *)key defaultValue:(double)def {
    id val = [self readPrefValue:key];
    return val ? [val doubleValue] : def;
}

- (NSString *)readString:(NSString *)key defaultValue:(NSString *)def {
    id val = [self readPrefValue:key];
    return val ? [val description] : def;
}

- (void)writePrefValue:(id)value forKey:(NSString *)key {
    if (!key || !_preferencesDomain) return;
    PSPreferencesSetAppValue((__bridge CFStringRef)key,
                             (__bridge CFPropertyListRef)value,
                             (__bridge CFStringRef)_preferencesDomain);
    
    NSString *notif = [NSString stringWithFormat:@"%@/prefsChanged", _preferencesDomain];
    CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
                                         (__bridge CFStringRef)notif, NULL, NULL, true);
}

#pragma mark - View Lifecycle

- (void)loadView {
    NSRect initialFrame = NSMakeRect(0, 0, 360, 520);
    
    _scrollView = [[VNPScrollView alloc] initWithFrame:initialFrame];
    _scrollView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _scrollView.drawsBackground = NO;
    _scrollView.hasVerticalScroller = YES;
    _scrollView.hasHorizontalScroller = NO;
    _scrollView.autohidesScrollers = YES;
    
    _contentView = [[VNPFlippedView alloc] initWithFrame:initialFrame];
    _contentView.autoresizingMask = NSViewWidthSizable;
    _contentView.wantsLayer = YES;
    
    [self buildUI];
    
    _scrollView.documentView = _contentView;
    self.view = _scrollView;
    
    [self reloadAllValues];
}

- (void)viewDidLayout {
    [super viewDidLayout];
    [self updateLayoutSizing];
}

- (void)updateLayoutSizing {
    CGFloat currentW = self.view.bounds.size.width;
    if (currentW < 240.0) currentW = 360.0;
    
    if (_contentView) {
        NSRect cvFrame = _contentView.frame;
        if (cvFrame.size.width != currentW || cvFrame.size.height != _totalContentHeight) {
            _contentView.frame = NSMakeRect(0, 0, currentW, _totalContentHeight);
        }
    }
    
    if (self.view.enclosingScrollView != nil) {
        self.preferredContentSize = NSMakeSize(currentW, _totalContentHeight);
        _scrollView.hasVerticalScroller = NO;
        _scrollView.hasHorizontalScroller = NO;
    } else {
        self.preferredContentSize = NSMakeSize(currentW, _totalContentHeight);
        _scrollView.hasVerticalScroller = YES;
        _scrollView.hasHorizontalScroller = NO;
    }
}

#pragma mark - UI Building

- (void)buildUI {
    CGFloat width = 360.0;
    CGFloat y = 14.0;
    CGFloat pad = 12.0;
    CGFloat cardW = width - (pad * 2.0);
    
    // --- CARD 1: General Settings ---
    // Create card directly with real cardW to prevent any initial autoresizing scaling
    NSView *generalCard = [self createCardViewWithFrame:NSMakeRect(pad, y, cardW, 200)];
    CGFloat genY = 14.0;
    
    NSTextField *genHeader = [self createSectionHeader:@"General Settings"];
    genHeader.frame = NSMakeRect(14, genY, cardW - 28, 20);
    genHeader.autoresizingMask = NSViewWidthSizable;
    [generalCard addSubview:genHeader];
    genY += 26.0;
    
    // 1. Enable Switch
    _enabledSwitch = [self createSwitch];
    _enabledSwitch.target = self;
    _enabledSwitch.action = @selector(enabledToggled:);
    [generalCard addSubview:[self createSettingRowWithTitle:@"Enable Vanish"
                                                   subtitle:@"Play window close animations when closing apps."
                                                    control:_enabledSwitch
                                                          y:genY
                                                      width:cardW]];
    genY += 46.0;
    [generalCard addSubview:[self createSeparatorAtY:genY width:cardW]];
    genY += 8.0;
    
    // 2. Shadows Switch
    _shadowsSwitch = [self createSwitch];
    _shadowsSwitch.target = self;
    _shadowsSwitch.action = @selector(shadowsToggled:);
    [generalCard addSubview:[self createSettingRowWithTitle:@"Window Shadows"
                                                   subtitle:@"Include drop shadows during animations."
                                                    control:_shadowsSwitch
                                                          y:genY
                                                      width:cardW]];
    genY += 46.0;
    [generalCard addSubview:[self createSeparatorAtY:genY width:cardW]];
    genY += 8.0;
    
    // 3. Target App Field
    _targetAppField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 110, 22)];
    _targetAppField.placeholderString = @"all";
    _targetAppField.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _targetAppField.delegate = self;
    [generalCard addSubview:[self createSettingRowWithTitle:@"Target Application"
                                                   subtitle:@"'all' or specific app name (e.g. Mail, Slack)."
                                                    control:_targetAppField
                                                          y:genY
                                                      width:cardW]];
    genY += 46.0;
    
    // Finalize card 1 frame (width remains cardW, only height finalized)
    generalCard.frame = NSMakeRect(pad, y, cardW, genY + 10.0);
    [_contentView addSubview:generalCard];
    
    y += generalCard.frame.size.height + 16.0;
    
    // --- CARD 2: Animation & Timing ---
    NSView *animCard = [self createCardViewWithFrame:NSMakeRect(pad, y, cardW, 250)];
    CGFloat animY = 14.0;
    
    NSTextField *animHeader = [self createSectionHeader:@"Animation & Timing"];
    animHeader.frame = NSMakeRect(14, animY, cardW - 28, 20);
    animHeader.autoresizingMask = NSViewWidthSizable;
    [animCard addSubview:animHeader];
    animY += 26.0;
    
    // 1. Active Animation PopUp Button
    _animPopUp = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 130, 26) pullsDown:NO];
    _animPopUp.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    for (size_t i = 0; i < kAnimationCount; i++) {
        [_animPopUp addItemWithTitle:[NSString stringWithUTF8String:kAnimations[i].name]];
        _animPopUp.lastItem.representedObject = [NSString stringWithUTF8String:kAnimations[i].key];
    }
    _animPopUp.target = self;
    _animPopUp.action = @selector(animationSelected:);
    [animCard addSubview:[self createSettingRowWithTitle:@"Active Animation"
                                                subtitle:@"Animation style to play on window close."
                                                 control:_animPopUp
                                                       y:animY
                                                   width:cardW]];
    animY += 46.0;
    [animCard addSubview:[self createSeparatorAtY:animY width:cardW]];
    animY += 8.0;
    
    // 2. Single Animation Duration Slider (controls duration for the selected active animation)
    _durationSlider = [self createSliderWithMin:0.05 max:2.00 defaultVal:0.25];
    _durationSlider.target = self;
    _durationSlider.action = @selector(durationSliderChanged:);
    _durationLabel = [self createBadgeLabel:@"0.25s"];
    
    _durationSubtitle = [NSTextField labelWithString:@""];
    _durationSubtitle.font = [NSFont systemFontOfSize:11];
    _durationSubtitle.textColor = [NSColor secondaryLabelColor];
    
    _resetDurationBtn = [NSButton buttonWithTitle:@"Reset" target:self action:@selector(resetDurationClicked:)];
    _resetDurationBtn.bezelStyle = NSBezelStyleInline;
    _resetDurationBtn.font = [NSFont systemFontOfSize:10 weight:NSFontWeightMedium];
    
    [animCard addSubview:[self createDurationSliderRowWithTitle:@"Animation Duration"
                                                  subtitleLabel:_durationSubtitle
                                                         slider:_durationSlider
                                                     valueLabel:_durationLabel
                                                    resetButton:_resetDurationBtn
                                                              y:animY
                                                          width:cardW]];
    animY += 62.0;
    [animCard addSubview:[self createSeparatorAtY:animY width:cardW]];
    animY += 8.0;
    
    // 3. Refresh Rate Slider
    _refreshRateSlider = [self createSliderWithMin:0.0 max:144.0 defaultVal:120.0];
    _refreshRateSlider.target = self;
    _refreshRateSlider.action = @selector(refreshRateChanged:);
    _refreshRateLabel = [self createBadgeLabel:@"120 Hz"];
    [animCard addSubview:[self createSliderRowWithTitle:@"Refresh Rate"
                                               subtitle:@"Animation FPS (0 follows display ProMotion rate)."
                                                 slider:_refreshRateSlider
                                             valueLabel:_refreshRateLabel
                                                      y:animY
                                                  width:cardW]];
    animY += 62.0;
    
    // Finalize card 2 frame
    animCard.frame = NSMakeRect(pad, y, cardW, animY + 10.0);
    [_contentView addSubview:animCard];
    
    y += animCard.frame.size.height + 20.0;
    _totalContentHeight = y;
    _contentView.frame = NSMakeRect(0, 0, width, _totalContentHeight);
}

#pragma mark - UI Factory Helpers

- (NSView *)createCardViewWithFrame:(NSRect)frame {
    VNPFlippedView *card = [[VNPFlippedView alloc] initWithFrame:frame];
    card.wantsLayer = YES;
    card.layer.cornerRadius = 10.0;
    if (@available(macOS 10.15, *)) {
        card.layer.cornerCurve = @"continuous";
    }
    card.layer.backgroundColor = [NSColor colorWithWhite:0.5 alpha:0.08].CGColor;
    card.layer.borderColor = [NSColor separatorColor].CGColor;
    card.layer.borderWidth = 0.5;
    card.autoresizingMask = NSViewWidthSizable;
    return card;
}

- (NSTextField *)createSectionHeader:(NSString *)title {
    NSTextField *tf = [NSTextField labelWithString:title];
    tf.font = [NSFont systemFontOfSize:14 weight:NSFontWeightBold];
    tf.textColor = [NSColor labelColor];
    return tf;
}

- (NSView *)createSeparatorAtY:(CGFloat)y width:(CGFloat)w {
    NSBox *sep = [[NSBox alloc] initWithFrame:NSMakeRect(14, y, w - 28, 1)];
    sep.boxType = NSBoxSeparator;
    sep.autoresizingMask = NSViewWidthSizable;
    return sep;
}

- (NSSwitch *)createSwitch {
    NSSwitch *sw = [[NSSwitch alloc] initWithFrame:NSMakeRect(0, 0, 42, 22)];
    return sw;
}

- (NSSlider *)createSliderWithMin:(double)min max:(double)max defaultVal:(double)def {
    NSSlider *sl = [[NSSlider alloc] initWithFrame:NSMakeRect(0, 0, 140, 20)];
    sl.minValue = min;
    sl.maxValue = max;
    sl.doubleValue = def;
    sl.continuous = YES;
    return sl;
}

- (NSTextField *)createBadgeLabel:(NSString *)text {
    NSTextField *tf = [NSTextField labelWithString:text];
    tf.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightSemibold];
    tf.textColor = [NSColor labelColor];
    tf.alignment = NSTextAlignmentRight;
    return tf;
}

- (NSView *)createSettingRowWithTitle:(NSString *)title subtitle:(NSString *)subtitle control:(NSView *)control y:(CGFloat)y width:(CGFloat)w {
    VNPFlippedView *row = [[VNPFlippedView alloc] initWithFrame:NSMakeRect(0, y, w, 46)];
    row.autoresizingMask = NSViewWidthSizable;
    
    CGFloat cW = control.frame.size.width;
    CGFloat cH = control.frame.size.height;
    
    NSTextField *titleLbl = [NSTextField labelWithString:title];
    titleLbl.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    titleLbl.textColor = [NSColor labelColor];
    titleLbl.frame = NSMakeRect(14, 5, w - 28 - cW - 10, 18);
    titleLbl.autoresizingMask = NSViewWidthSizable;
    [row addSubview:titleLbl];
    
    if (subtitle && subtitle.length > 0) {
        NSTextField *subLbl = [NSTextField labelWithString:subtitle];
        subLbl.font = [NSFont systemFontOfSize:11];
        subLbl.textColor = [NSColor secondaryLabelColor];
        subLbl.frame = NSMakeRect(14, 24, w - 28 - cW - 10, 14);
        subLbl.autoresizingMask = NSViewWidthSizable;
        [row addSubview:subLbl];
    }
    
    control.frame = NSMakeRect(w - 14 - cW, (46 - cH) / 2.0, cW, cH);
    control.autoresizingMask = NSViewMinXMargin;
    [row addSubview:control];
    
    return row;
}

- (NSView *)createDurationSliderRowWithTitle:(NSString *)title
                               subtitleLabel:(NSTextField *)subLbl
                                      slider:(NSSlider *)slider
                                  valueLabel:(NSTextField *)valueLabel
                                 resetButton:(NSButton *)resetBtn
                                           y:(CGFloat)y
                                       width:(CGFloat)w {
    VNPFlippedView *row = [[VNPFlippedView alloc] initWithFrame:NSMakeRect(0, y, w, 62)];
    row.autoresizingMask = NSViewWidthSizable;
    
    CGFloat rightItemsWidth = 56 + 6 + 50; // badge + spacing + reset button
    
    NSTextField *titleLbl = [NSTextField labelWithString:title];
    titleLbl.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    titleLbl.textColor = [NSColor labelColor];
    titleLbl.frame = NSMakeRect(14, 4, w - 28 - rightItemsWidth, 18);
    titleLbl.autoresizingMask = NSViewWidthSizable;
    [row addSubview:titleLbl];
    
    resetBtn.frame = NSMakeRect(w - 14 - 48, 3, 48, 20);
    resetBtn.autoresizingMask = NSViewMinXMargin;
    [row addSubview:resetBtn];
    
    valueLabel.frame = NSMakeRect(w - 14 - 48 - 6 - 54, 4, 54, 18);
    valueLabel.autoresizingMask = NSViewMinXMargin;
    [row addSubview:valueLabel];
    
    if (subLbl) {
        subLbl.frame = NSMakeRect(14, 22, w - 28, 14);
        subLbl.autoresizingMask = NSViewWidthSizable;
        [row addSubview:subLbl];
    }
    
    slider.frame = NSMakeRect(14, 40, w - 28, 18);
    slider.autoresizingMask = NSViewWidthSizable;
    [row addSubview:slider];
    
    return row;
}

- (NSView *)createSliderRowWithTitle:(NSString *)title subtitle:(NSString *)subtitle slider:(NSSlider *)slider valueLabel:(NSTextField *)valueLabel y:(CGFloat)y width:(CGFloat)w {
    VNPFlippedView *row = [[VNPFlippedView alloc] initWithFrame:NSMakeRect(0, y, w, 62)];
    row.autoresizingMask = NSViewWidthSizable;
    
    NSTextField *titleLbl = [NSTextField labelWithString:title];
    titleLbl.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    titleLbl.textColor = [NSColor labelColor];
    titleLbl.frame = NSMakeRect(14, 4, w - 28 - 60, 18);
    titleLbl.autoresizingMask = NSViewWidthSizable;
    [row addSubview:titleLbl];
    
    valueLabel.frame = NSMakeRect(w - 14 - 56, 4, 56, 18);
    valueLabel.autoresizingMask = NSViewMinXMargin;
    [row addSubview:valueLabel];
    
    if (subtitle && subtitle.length > 0) {
        NSTextField *subLbl = [NSTextField labelWithString:subtitle];
        subLbl.font = [NSFont systemFontOfSize:11];
        subLbl.textColor = [NSColor secondaryLabelColor];
        subLbl.frame = NSMakeRect(14, 22, w - 28, 14);
        subLbl.autoresizingMask = NSViewWidthSizable;
        [row addSubview:subLbl];
    }
    
    slider.frame = NSMakeRect(14, 40, w - 28, 18);
    slider.autoresizingMask = NSViewWidthSizable;
    [row addSubview:slider];
    
    return row;
}

#pragma mark - Reload Data

- (const VNAnimationMeta *)metaForAnimationKey:(NSString *)key {
    if (!key || key.length == 0) return &kAnimations[0];
    for (size_t i = 0; i < kAnimationCount; i++) {
        if (strcasecmp(kAnimations[i].key, [key UTF8String]) == 0) {
            return &kAnimations[i];
        }
    }
    return &kAnimations[0];
}

- (void)updateDurationSliderForActiveAnimation {
    if (!_animPopUp || !_durationSlider) return;
    
    NSString *animKey = _animPopUp.selectedItem.representedObject ?: @"shrink";
    const VNAnimationMeta *meta = [self metaForAnimationKey:animKey];
    
    NSString *durKey = [NSString stringWithFormat:@"duration_%s", meta->key];
    double globalDur = [self readDouble:@"duration" defaultValue:0.25];
    double dur = [self readDouble:durKey defaultValue:meta->defaultDuration];
    if (dur < 0.05) dur = (globalDur >= 0.05) ? globalDur : meta->defaultDuration;
    
    _durationSlider.doubleValue = dur;
    _durationLabel.stringValue = [NSString stringWithFormat:@"%.2fs", dur];
    if (_durationSubtitle) {
        _durationSubtitle.stringValue = [NSString stringWithFormat:@"Duration for %s animation (default: %.2fs).", meta->name, meta->defaultDuration];
    }
}

- (void)reloadAllValues {
    if (!_enabledSwitch) return;
    
    // 1. General
    BOOL enabled = [self readBool:@"enabled" defaultValue:YES];
    _enabledSwitch.state = enabled ? NSControlStateValueOn : NSControlStateValueOff;
    
    BOOL shadows = [self readBool:@"shadows" defaultValue:YES];
    _shadowsSwitch.state = shadows ? NSControlStateValueOn : NSControlStateValueOff;
    
    NSString *targetApp = [self readString:@"targetApp" defaultValue:@"all"];
    _targetAppField.stringValue = targetApp ?: @"all";
    
    // 2. Animation & Timing
    NSString *anim = [self readString:@"animation" defaultValue:@"shrink"];
    for (NSMenuItem *it in _animPopUp.itemArray) {
        if ([it.representedObject isEqualToString:anim]) {
            [_animPopUp selectItem:it];
            break;
        }
    }
    
    [self updateDurationSliderForActiveAnimation];
    
    double rr = [self readDouble:@"refreshRate" defaultValue:120.0];
    _refreshRateSlider.doubleValue = rr;
    _refreshRateLabel.stringValue = (rr <= 0.0) ? @"Auto" : [NSString stringWithFormat:@"%.0f Hz", rr];
}

#pragma mark - Control Actions

- (void)enabledToggled:(NSSwitch *)sender {
    BOOL val = (sender.state == NSControlStateValueOn);
    [self writePrefValue:@(val) forKey:@"enabled"];
}

- (void)shadowsToggled:(NSSwitch *)sender {
    BOOL val = (sender.state == NSControlStateValueOn);
    [self writePrefValue:@(val) forKey:@"shadows"];
}

- (void)animationSelected:(NSPopUpButton *)sender {
    NSString *key = sender.selectedItem.representedObject;
    if (key) {
        [self writePrefValue:key forKey:@"animation"];
        [self updateDurationSliderForActiveAnimation];
    }
}

- (void)durationSliderChanged:(NSSlider *)sender {
    double dur = sender.doubleValue;
    _durationLabel.stringValue = [NSString stringWithFormat:@"%.2fs", dur];
    
    NSString *animKey = _animPopUp.selectedItem.representedObject ?: @"shrink";
    NSString *durKey = [NSString stringWithFormat:@"duration_%@", animKey];
    [self writePrefValue:@(dur) forKey:durKey];
    [self writePrefValue:@(dur) forKey:@"duration"];
}

- (void)resetDurationClicked:(NSButton *)sender {
    NSString *animKey = _animPopUp.selectedItem.representedObject ?: @"shrink";
    const VNAnimationMeta *meta = [self metaForAnimationKey:animKey];
    
    double defDur = meta->defaultDuration;
    _durationSlider.doubleValue = defDur;
    _durationLabel.stringValue = [NSString stringWithFormat:@"%.2fs", defDur];
    
    NSString *durKey = [NSString stringWithFormat:@"duration_%s", meta->key];
    [self writePrefValue:@(defDur) forKey:durKey];
    [self writePrefValue:@(defDur) forKey:@"duration"];
}

- (void)controlTextDidEndEditing:(NSNotification *)obj {
    if (obj.object == _targetAppField) {
        NSString *val = _targetAppField.stringValue;
        if (val.length == 0) val = @"all";
        [self writePrefValue:val forKey:@"targetApp"];
    }
}

- (void)refreshRateChanged:(NSSlider *)sender {
    double val = round(sender.doubleValue);
    _refreshRateLabel.stringValue = (val <= 0.0) ? @"Auto" : [NSString stringWithFormat:@"%.0f Hz", val];
    [self writePrefValue:@(val) forKey:@"refreshRate"];
}

@end
