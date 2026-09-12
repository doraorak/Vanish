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

#pragma mark - FlippedView Helper

@interface VNPFlippedView : NSView
@end

@implementation VNPFlippedView
- (BOOL)isFlipped {
    return YES;
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
    NSPopUpButton *_animPopUp;
    NSTextField *_targetAppField;
    NSSlider *_refreshRateSlider;
    NSTextField *_refreshRateLabel;
    NSSlider *_globalDurationSlider;
    NSTextField *_globalDurationLabel;
    
    // Per-Animation Duration Controls
    NSSlider *_animSliders[kAnimationCount];
    NSTextField *_animLabels[kAnimationCount];
    
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
    // Commit any in-flight text edits
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
    
    // Explicitly post Darwin notification so hooked processes reload immediately
    NSString *notif = [NSString stringWithFormat:@"%@/prefsChanged", _preferencesDomain];
    CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
                                         (__bridge CFStringRef)notif, NULL, NULL, true);
}

#pragma mark - View Lifecycle

- (void)loadView {
    NSRect initialFrame = NSMakeRect(0, 0, 520, 700);
    
    _scrollView = [[NSScrollView alloc] initWithFrame:initialFrame];
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
    if (currentW < 320.0) currentW = 500.0;
    
    if (_contentView && _totalContentHeight > 0) {
        _contentView.frame = NSMakeRect(0, 0, currentW, _totalContentHeight);
    }
    
    if (self.view.enclosingScrollView != nil) {
        // Hosted inside an outer scroll view (e.g. System Settings TweaksPrefPane)
        self.preferredContentSize = NSMakeSize(currentW, _totalContentHeight);
        _scrollView.hasVerticalScroller = NO;
    } else {
        // Hosted in a standalone representable (e.g. TweakInjectApp)
        self.preferredContentSize = NSMakeSize(currentW, 600);
        _scrollView.hasVerticalScroller = YES;
    }
}

#pragma mark - UI Building

- (void)buildUI {
    CGFloat width = 500.0;
    CGFloat y = 16.0;
    CGFloat pad = 16.0;
    CGFloat cardW = width - (pad * 2.0);
    
    // --- SECTION 1: General Settings Card ---
    NSView *generalCard = [self createCardView];
    CGFloat genY = 14.0;
    
    // Title
    NSTextField *genHeader = [self createSectionHeader:@"General Settings"];
    genHeader.frame = NSMakeRect(14, genY, cardW - 28, 20);
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
    genY += 48.0;
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
    genY += 48.0;
    [generalCard addSubview:[self createSeparatorAtY:genY width:cardW]];
    genY += 8.0;
    
    // 3. Active Animation PopUp
    _animPopUp = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 140, 26) pullsDown:NO];
    _animPopUp.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    for (size_t i = 0; i < kAnimationCount; i++) {
        [_animPopUp addItemWithTitle:[NSString stringWithUTF8String:kAnimations[i].name]];
        _animPopUp.lastItem.representedObject = [NSString stringWithUTF8String:kAnimations[i].key];
    }
    _animPopUp.target = self;
    _animPopUp.action = @selector(animationSelected:);
    [generalCard addSubview:[self createSettingRowWithTitle:@"Active Animation"
                                                   subtitle:@"Animation style to play on window close."
                                                    control:_animPopUp
                                                          y:genY
                                                      width:cardW]];
    genY += 48.0;
    [generalCard addSubview:[self createSeparatorAtY:genY width:cardW]];
    genY += 8.0;
    
    // 4. Target App Field
    _targetAppField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 140, 24)];
    _targetAppField.placeholderString = @"all";
    _targetAppField.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    _targetAppField.delegate = self;
    [generalCard addSubview:[self createSettingRowWithTitle:@"Target Application"
                                                   subtitle:@"'all' or specific app name (e.g. Mail, Slack)."
                                                    control:_targetAppField
                                                          y:genY
                                                      width:cardW]];
    genY += 48.0;
    [generalCard addSubview:[self createSeparatorAtY:genY width:cardW]];
    genY += 8.0;
    
    // 5. Refresh Rate Slider
    _refreshRateSlider = [self createSliderWithMin:0.0 max:144.0 defaultVal:120.0];
    _refreshRateSlider.target = self;
    _refreshRateSlider.action = @selector(refreshRateChanged:);
    _refreshRateLabel = [self createBadgeLabel:@"120 Hz"];
    [generalCard addSubview:[self createSliderRowWithTitle:@"Refresh Rate"
                                                  subtitle:@"Animation FPS (0 follows display ProMotion rate)."
                                                    slider:_refreshRateSlider
                                                valueLabel:_refreshRateLabel
                                                         y:genY
                                                     width:cardW]];
    genY += 56.0;
    [generalCard addSubview:[self createSeparatorAtY:genY width:cardW]];
    genY += 8.0;
    
    // 6. Global Fallback Duration Slider
    _globalDurationSlider = [self createSliderWithMin:0.05 max:2.0 defaultVal:0.25];
    _globalDurationSlider.target = self;
    _globalDurationSlider.action = @selector(globalDurationChanged:);
    _globalDurationLabel = [self createBadgeLabel:@"0.25s"];
    [generalCard addSubview:[self createSliderRowWithTitle:@"Default Duration"
                                                  subtitle:@"Fallback duration when no custom duration is set."
                                                    slider:_globalDurationSlider
                                                valueLabel:_globalDurationLabel
                                                         y:genY
                                                     width:cardW]];
    genY += 56.0;
    
    generalCard.frame = NSMakeRect(pad, y, cardW, genY + 6.0);
    generalCard.autoresizingMask = NSViewWidthSizable;
    [_contentView addSubview:generalCard];
    
    y += generalCard.frame.size.height + 20.0;
    
    // --- SECTION 2: Per-Animation Durations Card ---
    NSView *durationsCard = [self createCardView];
    CGFloat durY = 14.0;
    
    // Section Header & Reset All Button
    NSTextField *durHeader = [self createSectionHeader:@"Per-Animation Durations"];
    durHeader.frame = NSMakeRect(14, durY, cardW - 140, 20);
    [durationsCard addSubview:durHeader];
    
    NSButton *resetAllBtn = [NSButton buttonWithTitle:@"Reset All" target:self action:@selector(resetAllDurationsClicked:)];
    resetAllBtn.bezelStyle = NSBezelStyleInline;
    resetAllBtn.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
    resetAllBtn.frame = NSMakeRect(cardW - 100, durY - 2, 86, 22);
    resetAllBtn.autoresizingMask = NSViewMinXMargin;
    [durationsCard addSubview:resetAllBtn];
    durY += 24.0;
    
    NSTextField *durDesc = [NSTextField wrappingLabelWithString:@"Fine-tune duration individually for each close animation (0.05s to 2.00s)."];
    durDesc.font = [NSFont systemFontOfSize:11];
    durDesc.textColor = [NSColor secondaryLabelColor];
    durDesc.frame = NSMakeRect(14, durY, cardW - 28, 28);
    [durationsCard addSubview:durDesc];
    durY += 34.0;
    
    // 16 Sliders
    for (size_t i = 0; i < kAnimationCount; i++) {
        [durationsCard addSubview:[self createSeparatorAtY:durY width:cardW]];
        durY += 8.0;
        
        NSSlider *sl = [self createSliderWithMin:0.05 max:2.00 defaultVal:kAnimations[i].defaultDuration];
        sl.tag = (NSInteger)i;
        sl.target = self;
        sl.action = @selector(animDurationSliderChanged:);
        _animSliders[i] = sl;
        
        NSTextField *lbl = [self createBadgeLabel:[NSString stringWithFormat:@"%.2fs", kAnimations[i].defaultDuration]];
        _animLabels[i] = lbl;
        
        NSString *title = [NSString stringWithUTF8String:kAnimations[i].name];
        NSString *subtitle = [NSString stringWithUTF8String:kAnimations[i].desc];
        
        NSView *row = [self createSliderRowWithTitle:title
                                            subtitle:subtitle
                                              slider:sl
                                          valueLabel:lbl
                                                   y:durY
                                               width:cardW];
        [durationsCard addSubview:row];
        durY += 56.0;
    }
    
    durationsCard.frame = NSMakeRect(pad, y, cardW, durY + 10.0);
    durationsCard.autoresizingMask = NSViewWidthSizable;
    [_contentView addSubview:durationsCard];
    
    y += durationsCard.frame.size.height + 24.0;
    _totalContentHeight = y;
    _contentView.frame = NSMakeRect(0, 0, width, _totalContentHeight);
}

#pragma mark - UI Factory Helpers

- (NSView *)createCardView {
    VNPFlippedView *card = [[VNPFlippedView alloc] init];
    card.wantsLayer = YES;
    card.layer.cornerRadius = 10.0;
    if (@available(macOS 10.15, *)) {
        card.layer.cornerCurve = @"continuous";
    }
    card.layer.backgroundColor = [NSColor colorWithWhite:0.5 alpha:0.08].CGColor;
    card.layer.borderColor = [NSColor separatorColor].CGColor;
    card.layer.borderWidth = 0.5;
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
    VNPFlippedView *row = [[VNPFlippedView alloc] initWithFrame:NSMakeRect(14, y, w - 28, 44)];
    row.autoresizingMask = NSViewWidthSizable;
    
    NSTextField *titleLbl = [NSTextField labelWithString:title];
    titleLbl.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    titleLbl.textColor = [NSColor labelColor];
    titleLbl.frame = NSMakeRect(0, 2, w - 180, 18);
    titleLbl.autoresizingMask = NSViewWidthSizable;
    [row addSubview:titleLbl];
    
    NSTextField *subLbl = [NSTextField labelWithString:subtitle];
    subLbl.font = [NSFont systemFontOfSize:11];
    subLbl.textColor = [NSColor secondaryLabelColor];
    subLbl.frame = NSMakeRect(0, 22, w - 180, 16);
    subLbl.autoresizingMask = NSViewWidthSizable;
    [row addSubview:subLbl];
    
    CGFloat cW = control.frame.size.width;
    CGFloat cH = control.frame.size.height;
    control.frame = NSMakeRect((w - 28) - cW, (44 - cH) / 2.0, cW, cH);
    control.autoresizingMask = NSViewMinXMargin;
    [row addSubview:control];
    
    return row;
}

- (NSView *)createSliderRowWithTitle:(NSString *)title subtitle:(NSString *)subtitle slider:(NSSlider *)slider valueLabel:(NSTextField *)valueLabel y:(CGFloat)y width:(CGFloat)w {
    VNPFlippedView *row = [[VNPFlippedView alloc] initWithFrame:NSMakeRect(14, y, w - 28, 52)];
    row.autoresizingMask = NSViewWidthSizable;
    
    NSTextField *titleLbl = [NSTextField labelWithString:title];
    titleLbl.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    titleLbl.textColor = [NSColor labelColor];
    titleLbl.frame = NSMakeRect(0, 4, 160, 18);
    [row addSubview:titleLbl];
    
    NSTextField *subLbl = [NSTextField labelWithString:subtitle];
    subLbl.font = [NSFont systemFontOfSize:11];
    subLbl.textColor = [NSColor secondaryLabelColor];
    subLbl.frame = NSMakeRect(0, 24, 160, 16);
    [row addSubview:subLbl];
    
    CGFloat rightW = 56.0;
    valueLabel.frame = NSMakeRect((w - 28) - rightW, 14, rightW, 18);
    valueLabel.autoresizingMask = NSViewMinXMargin;
    [row addSubview:valueLabel];
    
    CGFloat sliderW = 160.0;
    slider.frame = NSMakeRect((w - 28) - rightW - sliderW - 10, 14, sliderW, 20);
    slider.autoresizingMask = NSViewMinXMargin;
    [row addSubview:slider];
    
    return row;
}

#pragma mark - Reload Data

- (void)reloadAllValues {
    if (!_enabledSwitch) return;
    
    // 1. General
    BOOL enabled = [self readBool:@"enabled" defaultValue:YES];
    _enabledSwitch.state = enabled ? NSControlStateValueOn : NSControlStateValueOff;
    
    BOOL shadows = [self readBool:@"shadows" defaultValue:YES];
    _shadowsSwitch.state = shadows ? NSControlStateValueOn : NSControlStateValueOff;
    
    NSString *anim = [self readString:@"animation" defaultValue:@"shrink"];
    for (NSMenuItem *it in _animPopUp.itemArray) {
        if ([it.representedObject isEqualToString:anim]) {
            [_animPopUp selectItem:it];
            break;
        }
    }
    
    NSString *targetApp = [self readString:@"targetApp" defaultValue:@"all"];
    _targetAppField.stringValue = targetApp ?: @"all";
    
    double rr = [self readDouble:@"refreshRate" defaultValue:120.0];
    _refreshRateSlider.doubleValue = rr;
    _refreshRateLabel.stringValue = (rr <= 0.0) ? @"Auto" : [NSString stringWithFormat:@"%.0f Hz", rr];
    
    double globalDur = [self readDouble:@"duration" defaultValue:0.25];
    _globalDurationSlider.doubleValue = globalDur;
    _globalDurationLabel.stringValue = [NSString stringWithFormat:@"%.2fs", globalDur];
    
    // 2. Per-Animation Durations
    for (size_t i = 0; i < kAnimationCount; i++) {
        NSString *key = [NSString stringWithFormat:@"duration_%s", kAnimations[i].key];
        double d = [self readDouble:key defaultValue:kAnimations[i].defaultDuration];
        if (d < 0.05) d = globalDur;
        _animSliders[i].doubleValue = d;
        _animLabels[i].stringValue = [NSString stringWithFormat:@"%.2fs", d];
    }
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
    }
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

- (void)globalDurationChanged:(NSSlider *)sender {
    double val = round(sender.doubleValue * 100.0) / 100.0;
    _globalDurationLabel.stringValue = [NSString stringWithFormat:@"%.2fs", val];
    [self writePrefValue:@(val) forKey:@"duration"];
}

- (void)animDurationSliderChanged:(NSSlider *)sender {
    NSInteger idx = sender.tag;
    if (idx < 0 || idx >= (NSInteger)kAnimationCount) return;
    
    double val = round(sender.doubleValue * 100.0) / 100.0;
    _animLabels[idx].stringValue = [NSString stringWithFormat:@"%.2fs", val];
    
    NSString *key = [NSString stringWithFormat:@"duration_%s", kAnimations[idx].key];
    [self writePrefValue:@(val) forKey:key];
}

- (void)resetAllDurationsClicked:(NSButton *)sender {
    for (size_t i = 0; i < kAnimationCount; i++) {
        double def = kAnimations[i].defaultDuration;
        _animSliders[i].doubleValue = def;
        _animLabels[i].stringValue = [NSString stringWithFormat:@"%.2fs", def];
        
        NSString *key = [NSString stringWithFormat:@"duration_%s", kAnimations[i].key];
        [self writePrefValue:@(def) forKey:key];
    }
}

@end
