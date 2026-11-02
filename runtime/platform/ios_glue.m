// iOS: the few Foundation / UIKit calls the C host layer needs (runtime/platform/ios.c, runtime/paths.c).
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>

#include "../paths.h"
#include "../rando.h"
#include "../settings.h"

// ---- Home Screen quick actions (Info.plist UIApplicationShortcutItems, tools/bundle_ios.sh, plus the seeds)
// "Metal HUD": MTL_HUD_ENABLED for this run - Metal reads it when the device is created, which SDL_main does later,
// so SDL's scene delegate is wrapped to see the launch's shortcut first (an app already running gets no HUD).
// "Play Vanilla" and one item per seed (play-seed, the seed's name in userInfo): a play request, taken by the start
// screen (as its PLAY: the first-launch questions, the shader preparation) or, in game, by ui_run (a reboot on it).
static pthread_mutex_t g_qa_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_qa_seed[128];
static bool g_qa_pending;
static void qa_request(UIApplicationShortcutItem *item) {
    NSString *seed = nil;
    if ([item.type isEqualToString:@"play-vanilla"]) seed = @"";
    else if ([item.type isEqualToString:@"play-seed"] && [(id)item.userInfo[@"seed"] isKindOfClass:NSString.class])
        seed = (NSString *)item.userInfo[@"seed"];
    if (!seed) return;
    pthread_mutex_lock(&g_qa_lock);
    snprintf(g_qa_seed, sizeof g_qa_seed, "%s", seed.UTF8String);
    g_qa_pending = true;
    pthread_mutex_unlock(&g_qa_lock);
}
bool plat_ios_take_play_request(char *seed, size_t n) {
    pthread_mutex_lock(&g_qa_lock);
    bool had = g_qa_pending;
    if (had) snprintf(seed, n, "%s", g_qa_seed), g_qa_pending = false;
    pthread_mutex_unlock(&g_qa_lock);
    if (had) fprintf(stderr, "[ios] quick action: play %s\n", seed[0] ? seed : "vanilla");  // (the log is open by now)
    return had;
}

static IMP g_sdl_will_connect;
static void will_connect(id self, SEL sel, UIScene *scene, UISceneSession *session, UISceneConnectionOptions *opts) {
    if ([opts.shortcutItem.type isEqualToString:@"metal-hud"]) setenv("MTL_HUD_ENABLED", "1", 1);
    else if (opts.shortcutItem) qa_request(opts.shortcutItem);
    ((void (*)(id, SEL, UIScene *, UISceneSession *, UISceneConnectionOptions *))g_sdl_will_connect)(self, sel, scene, session, opts);
}
// (the app already running: SDL's delegate has no such method)
static void perform_shortcut(id self, SEL sel, UIWindowScene *scene, UIApplicationShortcutItem *item,
                             void (^done)(BOOL)) {
    qa_request(item);
    done(YES);
}
__attribute__((constructor)) static void hook_shortcut_launch(void) {
    Class cls = objc_getClass("SDLUIKitSceneDelegate");
    Method m = class_getInstanceMethod(cls, @selector(scene:willConnectToSession:options:));
    if (m) g_sdl_will_connect = method_setImplementation(m, (IMP)will_connect);
    class_addMethod(cls, @selector(windowScene:performActionForShortcutItem:completionHandler:), (IMP)perform_shortcut,
                    "v@:@@@?");
}

// The dynamic items, rebuilt from rando/ whenever its list may have changed (the start screen opening, the
// Randomizer tab's list re-read after a seed is made, renamed or deleted, the app going to the background): the
// seed PLAY would start first, then the newest, then Metal HUD. iOS shows four items in all, static ones first and
// the first nearest the icon, so a static Metal HUD would sit between Play Vanilla and the seeds.
static void shortcuts_apply(void) {
    NSString *dir = [@(rt_data_root()) stringByAppendingPathComponent:@"rando"];
    NSMutableArray<NSDictionary *> *seeds = [NSMutableArray array];
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) {
        if (!rando_seed_there(name.UTF8String)) continue;
        NSString *p = [[dir stringByAppendingPathComponent:name] stringByAppendingPathComponent:@"settings.json"];
        NSDate *t = [NSFileManager.defaultManager attributesOfItemAtPath:p error:nil].fileModificationDate;
        [seeds addObject:@{@"name": name, @"t": t ?: NSDate.distantPast}];
    }
    NSString *cur = @(g_settings.rando_seed);
    [seeds sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
        if ([a[@"name"] isEqualToString:cur] != [b[@"name"] isEqualToString:cur]) return [a[@"name"] isEqualToString:cur] ? NSOrderedAscending : NSOrderedDescending;
        return [b[@"t"] compare:a[@"t"]];
    }];
    NSUInteger room = 3 - MIN((NSUInteger)3, [[NSBundle.mainBundle objectForInfoDictionaryKey:@"UIApplicationShortcutItems"] count]);
    NSMutableArray<UIApplicationShortcutItem *> *items = [NSMutableArray array];
    for (NSDictionary *s in seeds) {
        if (items.count == room) break;
        [items addObject:[[UIApplicationShortcutItem alloc] initWithType:@"play-seed" localizedTitle:s[@"name"]
                                                       localizedSubtitle:nil
                                                                    icon:[UIApplicationShortcutIcon iconWithSystemImageName:@"dice"]
                                                                userInfo:@{@"seed": s[@"name"]}]];
    }
    [items addObject:[[UIApplicationShortcutItem alloc] initWithType:@"metal-hud" localizedTitle:@"Metal HUD"
                                                   localizedSubtitle:nil
                                                                icon:[UIApplicationShortcutIcon iconWithSystemImageName:@"gauge.with.dots.needle.67percent"]
                                                            userInfo:nil]];
    UIApplication *app = UIApplication.sharedApplication;
    if ([app.shortcutItems isEqualToArray:items]) return;
    app.shortcutItems = items;
    fprintf(stderr, "[ios] quick actions: %lu of %lu seeds\n", (unsigned long)items.count - 1, (unsigned long)seeds.count);
}
void plat_ios_shortcuts_sync(void) {
    static atomic_bool queued;
    if (NSThread.isMainThread) { shortcuts_apply(); return; }
    if (atomic_exchange(&queued, true)) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        atomic_store(&queued, false);
        shortcuts_apply();
    });
}

// A tablet (iPad): the UI's phone enlargement isn't wanted on its screen.
bool plat_ios_tablet(void) { return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad; }

// The app's Documents folder (shared through Files / Finder with UIFileSharingEnabled): the data root.
const char *plat_ios_documents(void) {
    static char path[1024];
    if (!path[0]) {
        NSString *d = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        snprintf(path, sizeof path, "%s", d.fileSystemRepresentation);
    }
    return path;
}

// The iOS version ("26.0.1"), for the log's first line.
const char *plat_ios_os_version(void) {
    static char v[32];
    if (!v[0]) snprintf(v, sizeof v, "%s", UIDevice.currentDevice.systemVersion.UTF8String);
    return v;
}

// The .app bundle's folder: shipped resources (icon, fonts, the game code when linked separately).
const char *plat_ios_bundle_dir(void) {
    static char path[1024];
    if (!path[0]) snprintf(path, sizeof path, "%s", NSBundle.mainBundle.bundlePath.fileSystemRepresentation);
    return path;
}
