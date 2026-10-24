// iOS: the few Foundation / UIKit calls the C host layer needs (runtime/platform/ios.c, runtime/paths.c).
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

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
