//
//  VanishPrefsViewController.h
//  VanishPrefs
//

#import <AppKit/AppKit.h>
#import "PSPreferenceController.h"

@interface VanishPrefsViewController : NSViewController <PSPreferenceController>

@property (nonatomic, copy) NSString *preferencesDomain;

- (void)setPreferencesDomain:(NSString *)domain;
- (void)preferencesDidAppear;
- (void)preferencesDidDisappear;

@end
