// iOS: SDL's file dialogs and SDL_OpenURL as UIKit's (ios_sdl.h renames the calls; SDL 3 has no dialog backend on
// iOS). A UIDocumentPickerViewController over SDL's window; the callbacks on the main thread, with SDL's list form
// (NULL-terminated paths, empty for a cancel, NULL when no window could be shown).
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "ios_sdl.h"
#include <stdio.h>

#undef SDL_ShowOpenFolderDialog  // (this file defines them)
#undef SDL_ShowOpenFileDialog
#undef SDL_OpenURL

static UIViewController *root_controller(SDL_Window *win) {
    UIWindow *w = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    return w.rootViewController;
}

// The folder picker: Files (On My iPhone, iCloud Drive, other apps' providers, external drives). The folder is kept
// under security-scoped access until the next pick or exit, so the setup can read it and runtime/paths.c can make its
// bookmark (ios_bookmark.m holds the data folder's own scope past that).
@interface LASFolderPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) SDL_DialogFileCallback cb;
@property(nonatomic) void *ud;
@property(nonatomic, strong) NSURL *scoped;  // (the folder under security-scoped access)
@end

@implementation LASFolderPicker
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *u = urls.firstObject;
    if (self.scoped) [self.scoped stopAccessingSecurityScopedResource];
    self.scoped = nil;
    if (u && [u startAccessingSecurityScopedResource]) self.scoped = u;
    else if (u) fprintf(stderr, "[picker] no security-scoped access to %s (reading it anyway)\n", u.path.UTF8String);
    const char *files[] = {u ? u.path.fileSystemRepresentation : NULL, NULL};
    if (self.cb) self.cb(self.ud, files, -1);
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c {
    const char *none[] = {NULL};
    if (self.cb) self.cb(self.ud, none, -1);
}
@end

static LASFolderPicker *g_picker;

void ios_ShowOpenFolderDialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win, const char *default_location, bool allow_many) {
    (void)default_location, (void)allow_many;
    UIViewController *root = root_controller(win);
    if (!root) { fprintf(stderr, "[picker] no root view controller\n"); SDL_SetError("no window to show the picker over"); cb(ud, NULL, -1); return; }
    if (!g_picker) g_picker = [LASFolderPicker new];
    g_picker.cb = cb, g_picker.ud = ud;
    UIDocumentPickerViewController *p = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder]];
    p.delegate = g_picker;
    p.allowsMultipleSelection = NO;
    p.shouldShowFileExtensions = YES;
    [root presentViewController:p animated:YES completion:nil];
}

// Files (several) with the filters' extensions: picked as copies in the app's tmp folder, so no security-scoped
// access is needed.
@interface LASFilePicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) SDL_DialogFileCallback cb;
@property(nonatomic) void *ud;
@end

@implementation LASFilePicker
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    const char *paths[urls.count + 1];
    NSMutableArray *keep = [NSMutableArray array];  // (the C strings' owners)
    for (NSUInteger i = 0; i < urls.count; i++) {
        NSString *p = urls[i].path;
        [keep addObject:p];
        paths[i] = p.fileSystemRepresentation;
    }
    paths[urls.count] = NULL;
    if (self.cb) self.cb(self.ud, paths, -1);
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c {
    const char *none[] = {NULL};
    if (self.cb) self.cb(self.ud, none, -1);
}
@end

static LASFilePicker *g_file_picker;

void ios_ShowOpenFileDialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win, const SDL_DialogFileFilter *filters,
                            int nfilters, const char *default_location, bool allow_many) {
    (void)default_location;
    UIViewController *root = root_controller(win);
    if (!root) { fprintf(stderr, "[picker] no root view controller\n"); SDL_SetError("no window to show the picker over"); cb(ud, NULL, -1); return; }
    if (!g_file_picker) g_file_picker = [LASFilePicker new];
    g_file_picker.cb = cb, g_file_picker.ud = ud;
    // (.bfttf has no system type: it gets a dynamic one, which Files gives such files too)
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (int i = 0; i < nfilters; i++)
        for (NSString *e in [@(filters[i].pattern) componentsSeparatedByString:@";"]) {
            UTType *t = [e isEqualToString:@"*"] ? UTTypeData : [UTType typeWithFilenameExtension:e];
            if (t) [types addObject:t];
        }
    if (!types.count) [types addObject:UTTypeData];
    UIDocumentPickerViewController *p = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
    p.delegate = g_file_picker;
    p.allowsMultipleSelection = allow_many;
    p.shouldShowFileExtensions = YES;
    [root presentViewController:p animated:YES completion:nil];
}

// A file:// URL to a folder: the Files app there ("Show in Finder" is "Open in Files"); the rest as SDL does it.
bool ios_OpenURL(const char *url) {
    @autoreleasepool {
        NSURL *u = [NSURL URLWithString:@(url)];
        BOOL dir = NO;
        if (u.isFileURL && [NSFileManager.defaultManager fileExistsAtPath:u.path isDirectory:&dir] && dir) {
            // (the URL's own percent-encoding kept: a path with a space in it is no URL before iOS 17)
            NSURLComponents *c = [NSURLComponents componentsWithURL:u resolvingAgainstBaseURL:NO];
            c.scheme = @"shareddocuments";
            NSURL *files = c.URL;
            [UIApplication.sharedApplication openURL:files options:@{} completionHandler:^(BOOL ok) {
                if (!ok) fprintf(stderr, "[picker] Files app: couldn't open %s\n", files.absoluteString.UTF8String);
            }];
            return true;
        }
        return SDL_OpenURL(url);
    }
}
