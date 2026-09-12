//
//  VanishPrefsViewController.m
//  VanishPrefs
//
//  Custom NSViewController conforming to PSPreferenceController for Vanish.
//  Ultra-compact, responsive design tailored specifically for macOS System Settings.
//

#import "VanishPrefsViewController.h"
#import <notify.h>

#pragma mark - PSPreferences Declarations

extern CFPropertyListRef _Nullable PSPreferencesCopyAppValue(CFStringRef key, CFStringRef domain);
extern void PSPreferencesSetAppValue(CFStringRef key, CFPropertyListRef _Nullable value, CFStringRef domain);
extern Boolean PSPreferencesAppSynchronize(CFStringRef domain);

#pragma mark - FlippedView

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
    double defaultDuration;
} VNAnimationMeta;

static const VNAnimationMeta kAnimations[] = {
    { "shrink",   "Shrink",   0.25 },
    { "squish",   "Squish",   0.25 },
    { "fall",     "Fall",     0.25 },
    { "swirl",    "Swirl",    0.30 },
    { "flip",     "Flip",     0.25 },
    { "tilt",     "Tilt",     0.25 },
    { "slide",    "Slide",    0.25 },
    { "genie",    "Genie",    0.35 },
    { "flag",     "Flag",     0.30 },
    { "spin",     "Spin",     0.25 },
    { "roll",     "Roll",     0.30 },
    { "barrel",   "Barrel",   0.35 },
    { "clock",    "Clock",    0.35 },
    { "dissolve", "Dissolve", 0.25 },
    { "crt",      "CRT Off",  0.30 },
    { "shatter",  "Shatter",  0.40 },
};
#define kAnimationCount (sizeof(kAnimations) / sizeof(kAnimations[0]))

#pragma mark - VanishPrefsViewController

@interface VanishPrefsViewController () <NSTextFieldDelegate> {
    VNPFlippedView *_contentView;
    
    // Card 1: General Settings
    NSTextField *_genHeader;
    VNPFlippedView *_genCard;
    VNPFlippedView *_rowEnable;
    NSTextField *_enabledTitle;
    NSSwitch *_enabledSwitch;
    NSBox *_sep1;
    
    VNPFlippedView *_rowShadows;
    NSTextField *_shadowsTitle;
    NSSwitch *_shadowsSwitch;
    NSBox *_sep2;
    
    VNPFlippedView *_rowTarget;
    NSTextField *_targetTitle;
    NSTextField *_targetAppField;
    
    // Card 2: Animation & Timing
    NSTextField *_animHeader;
    VNPFlippedView *_animCard;
    VNPFlippedView *_rowAnim;
    NSTextField *_animTitle;
    NSPopUpButton *_animPopUp;
    NSBox *_sep3;
    
    VNPFlippedView *_rowDuration;
    NSTextField *_durationTitle;
    NSTextField *_durationLabel;
    NSButton *_resetDurationBtn;
    NSTextField *_durationSubtitle;
    NSSlider *_durationSlider;
    NSBox *_sep4;
    
    VNPFlippedView *_rowRefresh;
    NSTextField *_refreshTitle;
    NSTextField *_refreshRateLabel;
    NSTextField *_refreshSubtitle;
    NSSlider *_refreshRateSlider;
    
    CGFloat _totalHeight;
}
@end

@implementation VanishPrefsViewController

- (instancetype)init {
    self = [super initWithNibName:nil bundle:nil];
    if (self) {
        _preferencesDomain = @"com.doraorak.vanish";
        _totalHeight = 302.0;
        self.preferredContentSize = NSMakeSize(260.0, _totalHeight);
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
    [self layoutAllSubviews];
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
    // Self.view is a clean flipped view, perfectly suited for embedding in System Settings
    _contentView = [[VNPFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 260, _totalHeight)];
    _contentView.autoresizingMask = NSViewWidthSizable;
    _contentView.wantsLayer = YES;
    
    [self buildUI];
    
    self.view = _contentView;
    self.preferredContentSize = NSMakeSize(260.0, _totalHeight);
    
    [self reloadAllValues];
    [self layoutAllSubviews];
}

- (void)viewDidLayout {
    [super viewDidLayout];
    [self layoutAllSubviews];
}

#pragma mark - UI Building

- (void)buildUI {
    // --- Section 1: General Settings ---
    _genHeader = [self createSectionHeader:@"GENERAL"];
    [_contentView addSubview:_genHeader];
    
    _genCard = [self createCardView];
    [_contentView addSubview:_genCard];
    
    // Row 1: Enable Vanish
    _rowEnable = [[VNPFlippedView alloc] init];
    _enabledTitle = [self createLabel:@"Enable Vanish"];
    [_rowEnable addSubview:_enabledTitle];
    _enabledSwitch = [self createSwitch];
    _enabledSwitch.target = self;
    _enabledSwitch.action = @selector(enabledToggled:);
    [_rowEnable addSubview:_enabledSwitch];
    [_genCard addSubview:_rowEnable];
    
    _sep1 = [self createSeparator];
    [_genCard addSubview:_sep1];
    
    // Row 2: Window Shadows
    _rowShadows = [[VNPFlippedView alloc] init];
    _shadowsTitle = [self createLabel:@"Window Shadows"];
    [_rowShadows addSubview:_shadowsTitle];
    _shadowsSwitch = [self createSwitch];
    _shadowsSwitch.target = self;
    _shadowsSwitch.action = @selector(shadowsToggled:);
    [_rowShadows addSubview:_shadowsSwitch];
    [_genCard addSubview:_rowShadows];
    
    _sep2 = [self createSeparator];
    [_genCard addSubview:_sep2];
    
    // Row 3: Target Application
    _rowTarget = [[VNPFlippedView alloc] init];
    _targetTitle = [self createLabel:@"Target Application"];
    [_rowTarget addSubview:_targetTitle];
    _targetAppField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 80, 20)];
    _targetAppField.placeholderString = @"all";
    _targetAppField.font = [NSFont monospacedSystemFontOfSize:10.5 weight:NSFontWeightRegular];
    _targetAppField.controlSize = NSControlSizeSmall;
    _targetAppField.delegate = self;
    [_rowTarget addSubview:_targetAppField];
    [_genCard addSubview:_rowTarget];
    
    // --- Section 2: Animation & Timing ---
    _animHeader = [self createSectionHeader:@"ANIMATION & TIMING"];
    [_contentView addSubview:_animHeader];
    
    _animCard = [self createCardView];
    [_contentView addSubview:_animCard];
    
    // Row 1: Active Animation
    _rowAnim = [[VNPFlippedView alloc] init];
    _animTitle = [self createLabel:@"Active Animation"];
    [_rowAnim addSubview:_animTitle];
    _animPopUp = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 110, 22) pullsDown:NO];
    _animPopUp.controlSize = NSControlSizeSmall;
    _animPopUp.font = [NSFont systemFontOfSize:11];
    for (size_t i = 0; i < kAnimationCount; i++) {
        [_animPopUp addItemWithTitle:[NSString stringWithUTF8String:kAnimations[i].name]];
        _animPopUp.lastItem.representedObject = [NSString stringWithUTF8String:kAnimations[i].key];
    }
    _animPopUp.target = self;
    _animPopUp.action = @selector(animationSelected:);
    [_rowAnim addSubview:_animPopUp];
    [_animCard addSubview:_rowAnim];
    
    _sep3 = [self createSeparator];
    [_animCard addSubview:_sep3];
    
    // Row 2: Single Animation Duration Slider
    _rowDuration = [[VNPFlippedView alloc] init];
    _durationTitle = [self createLabel:@"Animation Duration"];
    [_rowDuration addSubview:_durationTitle];
    
    _durationLabel = [self createBadgeLabel:@"0.25s"];
    [_rowDuration addSubview:_durationLabel];
    
    _resetDurationBtn = [NSButton buttonWithTitle:@"Reset" target:self action:@selector(resetDurationClicked:)];
    _resetDurationBtn.bezelStyle = NSBezelStyleInline;
    _resetDurationBtn.font = [NSFont systemFontOfSize:9.5 weight:NSFontWeightMedium];
    [_rowDuration addSubview:_resetDurationBtn];
    
    _durationSubtitle = [self createSubtitle:@"Duration for active animation"];
    [_rowDuration addSubview:_durationSubtitle];
    
    _durationSlider = [self createSliderWithMin:0.05 max:2.00 defaultVal:0.25];
    _durationSlider.target = self;
    _durationSlider.action = @selector(durationSliderChanged:);
    [_rowDuration addSubview:_durationSlider];
    [_animCard addSubview:_rowDuration];
    
    _sep4 = [self createSeparator];
    [_animCard addSubview:_sep4];
    
    // Row 3: Refresh Rate Slider
    _rowRefresh = [[VNPFlippedView alloc] init];
    _refreshTitle = [self createLabel:@"Refresh Rate"];
    [_rowRefresh addSubview:_refreshTitle];
    
    _refreshRateLabel = [self createBadgeLabel:@"120 Hz"];
    [_rowRefresh addSubview:_refreshRateLabel];
    
    _refreshSubtitle = [self createSubtitle:@"Animation FPS (0 follows ProMotion)"];
    [_rowRefresh addSubview:_refreshSubtitle];
    
    _refreshRateSlider = [self createSliderWithMin:0.0 max:144.0 defaultVal:120.0];
    _refreshRateSlider.target = self;
    _refreshRateSlider.action = @selector(refreshRateChanged:);
    [_rowRefresh addSubview:_refreshRateSlider];
    [_animCard addSubview:_rowRefresh];
}

#pragma mark - Explicit Dynamic Layout

- (void)layoutAllSubviews {
    CGFloat w = self.view.bounds.size.width;
    if (w < 120.0) return;
    
    CGFloat y = 4.0;
    
    // Section 1: General
    _genHeader.frame = NSMakeRect(8, y, w - 16, 14);
    y += 18.0;
    
    _genCard.frame = NSMakeRect(0, y, w, 110.0);
    
    // Switch sizing (fitting size or minimum 42x22)
    CGSize swFit = [_enabledSwitch fittingSize];
    CGFloat swW = MAX(swFit.width, 42.0);
    CGFloat swH = MAX(swFit.height, 22.0);
    
    // Row 1: Enable
    _rowEnable.frame = NSMakeRect(0, 0, w, 36.0);
    _enabledSwitch.frame = NSMakeRect(w - 12 - swW, (36.0 - swH) / 2.0, swW, swH);
    _enabledTitle.frame = NSMakeRect(12, (36.0 - 16.0) / 2.0, w - 24 - swW - 8, 16);
    _sep1.frame = NSMakeRect(12, 36, w - 24, 1);
    
    // Row 2: Shadows
    _rowShadows.frame = NSMakeRect(0, 37, w, 36.0);
    _shadowsSwitch.frame = NSMakeRect(w - 12 - swW, (36.0 - swH) / 2.0, swW, swH);
    _shadowsTitle.frame = NSMakeRect(12, (36.0 - 16.0) / 2.0, w - 24 - swW - 8, 16);
    _sep2.frame = NSMakeRect(12, 73, w - 24, 1);
    
    // Row 3: Target App
    _rowTarget.frame = NSMakeRect(0, 74, w, 36.0);
    CGFloat tfW = 75.0;
    _targetAppField.frame = NSMakeRect(w - 12 - tfW, (36.0 - 20.0) / 2.0, tfW, 20);
    _targetTitle.frame = NSMakeRect(12, (36.0 - 16.0) / 2.0, w - 24 - tfW - 8, 16);
    
    y += 110.0 + 14.0;
    
    // Section 2: Animation & Timing
    _animHeader.frame = NSMakeRect(8, y, w - 16, 14);
    y += 18.0;
    
    _animCard.frame = NSMakeRect(0, y, w, 142.0);
    // Row 1: Active Animation
    _rowAnim.frame = NSMakeRect(0, 0, w, 36.0);
    CGFloat popW = 104.0;
    _animPopUp.frame = NSMakeRect(w - 12 - popW, (36.0 - 22.0) / 2.0, popW, 22);
    _animTitle.frame = NSMakeRect(12, (36.0 - 16.0) / 2.0, w - 24 - popW - 8, 16);
    _sep3.frame = NSMakeRect(12, 36, w - 24, 1);
    
    // Row 2: Single Duration Slider
    _rowDuration.frame = NSMakeRect(0, 37, w, 52.0);
    CGFloat durLabelW = 42.0;
    CGSize rsz = [_resetDurationBtn fittingSize];
    CGFloat resetW = MAX(rsz.width + 6, 40.0);
    _durationLabel.frame = NSMakeRect(w - 12 - durLabelW, 5, durLabelW, 15);
    _resetDurationBtn.frame = NSMakeRect(w - 12 - durLabelW - 4 - resetW, 4, resetW, 18);
    _durationTitle.frame = NSMakeRect(12, 5, w - 24 - durLabelW - 4 - resetW - 6, 15);
    _durationSubtitle.frame = NSMakeRect(12, 20, w - 24, 12);
    _durationSlider.frame = NSMakeRect(12, 33, w - 24, 15);
    _sep4.frame = NSMakeRect(12, 89, w - 24, 1);
    
    // Row 3: Refresh Rate Slider
    _rowRefresh.frame = NSMakeRect(0, 90, w, 52.0);
    CGFloat rrLabelW = 48.0;
    _refreshRateLabel.frame = NSMakeRect(w - 12 - rrLabelW, 5, rrLabelW, 15);
    _refreshTitle.frame = NSMakeRect(12, 5, w - 24 - rrLabelW - 6, 15);
    _refreshSubtitle.frame = NSMakeRect(12, 20, w - 24, 12);
    _refreshRateSlider.frame = NSMakeRect(12, 33, w - 24, 15);
    
    y += 142.0 + 8.0;
    _totalHeight = y;
    self.preferredContentSize = NSMakeSize(w, _totalHeight);
}

#pragma mark - UI Factory Helpers

- (VNPFlippedView *)createCardView {
    VNPFlippedView *card = [[VNPFlippedView alloc] init];
    card.wantsLayer = YES;
    card.layer.cornerRadius = 8.0;
    if (@available(macOS 10.15, *)) {
        card.layer.cornerCurve = @"continuous";
    }
    card.layer.backgroundColor = [NSColor colorWithWhite:0.5 alpha:0.07].CGColor;
    card.layer.borderColor = [NSColor separatorColor].CGColor;
    card.layer.borderWidth = 0.5;
    return card;
}

- (NSTextField *)createSectionHeader:(NSString *)title {
    NSTextField *tf = [NSTextField labelWithString:title];
    tf.font = [NSFont systemFontOfSize:10 weight:NSFontWeightBold];
    tf.textColor = [NSColor secondaryLabelColor];
    return tf;
}

- (NSTextField *)createLabel:(NSString *)text {
    NSTextField *tf = [NSTextField labelWithString:text];
    tf.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    tf.textColor = [NSColor labelColor];
    return tf;
}

- (NSTextField *)createSubtitle:(NSString *)text {
    NSTextField *tf = [NSTextField labelWithString:text];
    tf.font = [NSFont systemFontOfSize:9.5 weight:NSFontWeightRegular];
    tf.textColor = [NSColor secondaryLabelColor];
    return tf;
}

- (NSBox *)createSeparator {
    NSBox *sep = [[NSBox alloc] init];
    sep.boxType = NSBoxSeparator;
    return sep;
}

- (NSSwitch *)createSwitch {
    NSSwitch *sw = [[NSSwitch alloc] initWithFrame:NSMakeRect(0, 0, 40, 22)];
    sw.controlSize = NSControlSizeSmall;
    return sw;
}

- (NSSlider *)createSliderWithMin:(double)min max:(double)max defaultVal:(double)def {
    NSSlider *sl = [[NSSlider alloc] initWithFrame:NSMakeRect(0, 0, 140, 15)];
    sl.controlSize = NSControlSizeSmall;
    sl.minValue = min;
    sl.maxValue = max;
    sl.doubleValue = def;
    sl.continuous = YES;
    return sl;
}

- (NSTextField *)createBadgeLabel:(NSString *)text {
    NSTextField *tf = [NSTextField labelWithString:text];
    tf.font = [NSFont monospacedDigitSystemFontOfSize:10.5 weight:NSFontWeightSemibold];
    tf.textColor = [NSColor labelColor];
    tf.alignment = NSTextAlignmentRight;
    return tf;
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
        _durationSubtitle.stringValue = [NSString stringWithFormat:@"Duration for %s (default: %.2fs)", meta->name, meta->defaultDuration];
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
