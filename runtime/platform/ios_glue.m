// iOS: the few Foundation / UIKit calls the C host layer needs (runtime/platform/ios.c, runtime/paths.c).
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <objc/runtime.h>

// The "Metal HUD" Home Screen quick action (Info.plist UIApplicationShortcutItems, tools/bundle_ios.sh): a launch from
// it sets MTL_HUD_ENABLED for this run. Metal reads it when the device is created, which SDL_main does later, so SDL's
// scene delegate is wrapped to see the shortcut first. (An app already running only comes to the front: no HUD.)
static IMP g_sdl_will_connect;
static void will_connect(id self, SEL sel, UIScene *scene, UISceneSession *session, UISceneConnectionOptions *opts) {
    if ([opts.shortcutItem.type isEqualToString:@"metal-hud"]) setenv("MTL_HUD_ENABLED", "1", 1);
    ((void (*)(id, SEL, UIScene *, UISceneSession *, UISceneConnectionOptions *))g_sdl_will_connect)(self, sel, scene, session, opts);
}
__attribute__((constructor)) static void hook_shortcut_launch(void) {
    Method m = class_getInstanceMethod(objc_getClass("SDLUIKitSceneDelegate"), @selector(scene:willConnectToSession:options:));
    if (m) g_sdl_will_connect = method_setImplementation(m, (IMP)will_connect);
}

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
