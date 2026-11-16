// macOS: Sparkle 2 (MIT, tools/fetch_sparkle.sh) is the app's updater, with its own windows. tools/bundle_macos.sh puts
// Sparkle.framework in the app with the feed of the build's channel (the releases' or the nightlies' appcast, made by
// tools/sparkle_appcast.sh) and the public key the appcast's EdDSA signatures are checked with: the same key as the
// Windows and Linux manifests (runtime/update.cpp). The framework is loaded when the start screen first comes up (a
// build from source has none: no Sparkle, the update dialog offers Download). It checks once, at launch, in the
// background: never again while the app runs. The update dialog's Update button asks it to check now, which shows
// Sparkle's own window with the update.
#include "sparkle.h"

#if defined(__APPLE__) && !PLAT_IOS
#import <Foundation/Foundation.h>

#include <stdio.h>

// (Sparkle's classes by name: the app doesn't link the framework, it loads it when it's there)
@interface NSObject (LASSparkle)
- (instancetype)initWithStartingUpdater:(BOOL)start updaterDelegate:(id)d userDriverDelegate:(id)u;
- (id)updater;
- (void)checkForUpdates:(id)sender;
- (void)checkForUpdatesInBackground;
- (BOOL)canCheckForUpdates;
@end

static id g_controller;

// (test) LARECOMP_SPARKLE_FEED=<url>: Sparkle reads this feed instead of the build's (a local server); with it Sparkle
// also runs in builds whose channel has no Update, and in a bundle without a feed in its Info.plist
@interface LASSparkleDelegate : NSObject
@end
@implementation LASSparkleDelegate
- (NSString *)feedURLStringForUpdater:(id)updater {
    (void)updater;
    const char *f = getenv("LARECOMP_SPARKLE_FEED");
    return f && *f ? [NSString stringWithUTF8String:f] : nil;
}
@end
static LASSparkleDelegate *g_delegate;

bool sparkle_start(bool check_now) {
    if (g_controller) return true;
    NSBundle *app = [NSBundle mainBundle];
    const char *test_feed = getenv("LARECOMP_SPARKLE_FEED");
    if ((!test_feed && ![app objectForInfoDictionaryKey:@"SUFeedURL"]) || ![app objectForInfoDictionaryKey:@"SUPublicEDKey"]) {
        fprintf(stderr, "[sparkle] not in this build (no feed or key)\n");
        return false;
    }
    NSString *path = [[app privateFrameworksPath] stringByAppendingPathComponent:@"Sparkle.framework"];
    NSBundle *fw = [NSBundle bundleWithPath:path];
    NSError *err = nil;
    if (!fw || ![fw loadAndReturnError:&err]) {
        fprintf(stderr, "[sparkle] can't load Sparkle.framework: %s\n", err ? err.localizedDescription.UTF8String : "missing");
        return false;
    }
    Class cls = NSClassFromString(@"SPUStandardUpdaterController");
    if (!cls) { fprintf(stderr, "[sparkle] no SPUStandardUpdaterController\n"); return false; }
    // (Sparkle's own schedule is off - SUEnableAutomaticChecks false in Info.plist - so it never asks for permission
    //  and never checks while the app runs; this is the one check at launch)
    g_delegate = [LASSparkleDelegate new];
    g_controller = [[cls alloc] initWithStartingUpdater:YES updaterDelegate:g_delegate userDriverDelegate:nil];
    if (!g_controller) { fprintf(stderr, "[sparkle] the updater didn't start\n"); return false; }
    fprintf(stderr, "[sparkle] started, feed %s\n", test_feed ? test_feed : [[app objectForInfoDictionaryKey:@"SUFeedURL"] UTF8String]);
    if (check_now) [[g_controller updater] checkForUpdatesInBackground];
    return true;
}

bool sparkle_available(void) { return g_controller != nil; }

void sparkle_check_now(void) {
    if (!g_controller) return;
    [g_controller checkForUpdates:nil];
}
#else
bool sparkle_start(bool) { return false; }
bool sparkle_available(void) { return false; }
void sparkle_check_now(void) {}
#endif
