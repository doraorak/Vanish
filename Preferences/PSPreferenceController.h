//
//  PSPreferenceController.h
//  Vanish
//

#import <AppKit/AppKit.h>

@protocol PSPreferenceController <NSObject>
@optional
/// The defaults domain this tweak's preferences live in. Called once, after
/// init, before the view is first requested.
- (void)setPreferencesDomain:(NSString *)domain;
- (void)preferencesDidAppear;
- (void)preferencesDidDisappear;
@end
