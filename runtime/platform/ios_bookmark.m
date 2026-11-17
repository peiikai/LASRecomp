// iOS: a folder outside the sandbox the app may keep using - the data folder the player picked in the Files app
// (runtime/paths.c: the pointer file holds a bookmark instead of a path, since a path outside the sandbox grants
// nothing on the next launch). A bookmark is made while the picker's security scope is held (one made outside it
// reads back this run and resolves to nothing from the next launch on), and checked to resolve again before it is
// kept. Resolving one opens its security scope, held until the process ends (the game reads and writes there
// throughout); a bookmark the system calls stale is rewritten by the caller. Declared in runtime/platform.h.
#import <Foundation/Foundation.h>
#include <stdio.h>
#include "../platform.h"

// The folders resolved under security-scoped access, never given up: the data folder in use is read throughout
static NSMutableSet<NSString *> *g_held;

static void hold(NSURL *u) {
    if (!g_held) g_held = [NSMutableSet set];
    if (![g_held containsObject:u.path] && [u startAccessingSecurityScopedResource]) [g_held addObject:u.path];
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

static NSString *shown_name(NSURL *u) {
    NSString *name = nil;
    [u getResourceValue:&name forKey:NSURLLocalizedNameKey error:nil];
    return name.length ? name : u.lastPathComponent;
}

// Where `path` is, as the Files app names it: root "" for On My iPhone, else the place's own name (iCloud Drive,
// a drive, a server's share), and the folders below it joined by " › ". The names come from heuristics on the path
// (measured on iOS 26: only those places' roots have a localized name differing from the one on disk); false when
// none fits.
bool plat_ios_folder_place(const char *path, char *root, size_t rn, char *rest, size_t restn) {
    @autoreleasepool {
        char real[PATH_MAX], docs[PATH_MAX];
        if (!realpath(path, real)) snprintf(real, sizeof real, "%s", path);  // (a folder still to be made)
        if (!realpath(plat_ios_documents(), docs)) snprintf(docs, sizeof docs, "%s", plat_ios_documents());
        NSArray<NSString *> *parts = [NSURL fileURLWithPath:@(real)].pathComponents;
        NSArray<NSString *> *dparts = [NSURL fileURLWithPath:@(docs)].pathComponents;
        NSUInteger from = NSNotFound;  // the first part shown under the root
        NSString *top = nil;           // the root's name ("": this device)
        NSMutableArray *shown = [NSMutableArray array];
        NSUInteger fps = [parts indexOfObject:@"File Provider Storage"];
        if (parts.count >= dparts.count && [[parts subarrayWithRange:NSMakeRange(0, dparts.count)] isEqualToArray:dparts]) {
            // (the app's Documents: Files shows it as a folder named after the app)
            [shown addObject:NSBundle.mainBundle.infoDictionary[@"CFBundleDisplayName"] ?: @"LAS Recomp"];
            top = @"", from = dparts.count;
        } else if (fps != NSNotFound) {
            top = @"", from = fps + 1;
        } else {
            for (NSUInteger i = parts.count; i-- > 1;) {
                NSString *p = parts[i];
                if ([p isEqualToString:@"Library"] || [p isEqualToString:@"Containers"]) break;  // (the user's home: no place)
                NSString *dir = [NSString pathWithComponents:[parts subarrayWithRange:NSMakeRange(0, i + 1)]];
                NSString *name = nil;
                [[NSURL fileURLWithPath:dir isDirectory:YES] getResourceValue:&name forKey:NSURLLocalizedNameKey error:nil];
                if (name.length && ![name isEqualToString:p]) { top = name, from = i + 1; break; }
            }
        }
        if (!top) return false;
        for (NSUInteger i = from; i < parts.count; i++)
            [shown addObject:shown_name([NSURL fileURLWithPath:[NSString pathWithComponents:[parts subarrayWithRange:NSMakeRange(0, i + 1)]]])];
        snprintf(root, rn, "%s", top.UTF8String);
        snprintf(rest, restn, "%s", [shown componentsJoinedByString:@" \u203a "].UTF8String);
        return true;
    }
}
