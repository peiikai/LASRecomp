// iOS: a folder outside the sandbox the app may keep using - the data folder the player picked in the Files app
// (runtime/paths.c: the pointer file holds a bookmark instead of a path, since a path outside the sandbox grants
// nothing on the next launch). A bookmark is made while the picker's security scope is held (one made outside it
// reads back this run and resolves to nothing from the next launch on), and checked to resolve again before it is
// kept. Resolving one opens its security scope, held until the process ends (the game reads and writes there
// throughout); a bookmark the system calls stale is rewritten by the caller. Declared in runtime/platform.h.
#import <Foundation/Foundation.h>
#include <stdio.h>
#include "../platform.h"

static NSURL *g_held;  // the folder under security-scoped access (the data folder in use, or the last one tried)

static void hold(NSURL *u) {
    if (g_held && [g_held.path isEqualToString:u.path]) return;
    if (g_held) [g_held stopAccessingSecurityScopedResource];
    g_held = [u startAccessingSecurityScopedResource] ? u : nil;
}

// The bookmark of `path` (a folder the picker granted), base64 in out; false when the system refuses one or it
// doesn't resolve back.
bool plat_ios_bookmark_make(const char *path, char *out, size_t n) {
    @autoreleasepool {
        NSURL *u = [NSURL fileURLWithPath:@(path) isDirectory:YES];
        BOOL scoped = [u startAccessingSecurityScopedResource];
        NSError *err = nil;
        NSData *bm = [u bookmarkDataWithOptions:0 includingResourceValuesForKeys:nil relativeToURL:nil error:&err];
        if (scoped) [u stopAccessingSecurityScopedResource];
        if (!bm) { fprintf(stderr, "[bookmark] none for %s: %s\n", path, err.localizedDescription.UTF8String); return false; }
        BOOL stale = NO;
        NSURL *back = [NSURL URLByResolvingBookmarkData:bm options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:&err];
        if (!back) { fprintf(stderr, "[bookmark] %s: doesn't resolve back: %s\n", path, err.localizedDescription.UTF8String); return false; }
        NSString *b64 = [bm base64EncodedStringWithOptions:0];
        if (b64.length >= n) { fprintf(stderr, "[bookmark] %s: too long (%lu)\n", path, (unsigned long)b64.length); return false; }
        snprintf(out, n, "%s", b64.UTF8String);
        return true;
    }
}

// The folder a bookmark (base64) names, its security scope opened: 1 resolved, 0 not a bookmark / gone,
// -1 resolved but stale (the caller rewrites it with plat_ios_bookmark_make). An empty path: a plain path, not
// a bookmark (not ours).
int plat_ios_bookmark_resolve(const char *b64, char *path, size_t n) {
    @autoreleasepool {
        NSData *bm = [[NSData alloc] initWithBase64EncodedString:@(b64) options:NSDataBase64DecodingIgnoreUnknownCharacters];
        if (!bm.length) return 0;
        BOOL stale = NO;
        NSError *err = nil;
        NSURL *u = [NSURL URLByResolvingBookmarkData:bm options:0 relativeToURL:nil bookmarkDataIsStale:&stale error:&err];
        if (!u) { fprintf(stderr, "[bookmark] doesn't resolve: %s\n", err.localizedDescription.UTF8String); return 0; }
        // (a bookmark follows its folder, into Files' Recently Deleted too: deleted is gone, as on the desktop)
        if ([u.pathComponents containsObject:@".Trash"]) { fprintf(stderr, "[bookmark] %s: in the trash\n", u.path.UTF8String); return 0; }
        hold(u);
        snprintf(path, n, "%s", u.path.fileSystemRepresentation);
        return stale ? -1 : 1;
    }
}

// The last folder the picker granted stays readable (ios_picker.m drops its scope at the next pick): held here
// until the process ends, as the data folder's would be.
void plat_ios_bookmark_hold(const char *path) {
    @autoreleasepool { hold([NSURL fileURLWithPath:@(path) isDirectory:YES]); }
}

// A folder in iCloud Drive: its files may not be on the device, and a write there is a sync. The start screen
// warns, as it does for OneDrive on Windows. (Other providers - Google Drive, Dropbox - look just like On My iPhone
// from here: all of them are folders under some app group's File Provider Storage; no warning for those.)
const char *plat_cloud_synced(const char *dir) {
    @autoreleasepool {
        NSURL *u = [NSURL fileURLWithPath:@(dir) isDirectory:YES];
        NSNumber *ubi = nil;
        [u getResourceValue:&ubi forKey:NSURLIsUbiquitousItemKey error:nil];
        return ubi.boolValue ? "iCloud Drive" : NULL;
    }
}
