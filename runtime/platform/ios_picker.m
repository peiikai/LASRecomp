// iOS: the folder picker the start screen's Game Files page uses (SDL 3 has no file dialogs on iOS). A
// UIDocumentPickerViewController for a folder - Files: On My iPhone, iCloud Drive, other apps' providers - over
// SDL's window; the picked folder is kept under security-scoped access until the next pick or exit, so the setup can
// read it (runtime/ui/launcher.mm copies it into Documents/extracted, APFS clones where it can). The callback is on
// the main thread, with the folder's path, "" for a cancel (as SDL's dialogs: an empty list).
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL3/SDL.h>
#include <stdio.h>

typedef void (*PlatIosPicked)(void *ud, const char *path);

@interface LASFolderPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) PlatIosPicked cb;
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
    if (self.cb) self.cb(self.ud, u ? u.path.fileSystemRepresentation : "");
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c {
    if (self.cb) self.cb(self.ud, "");
}
@end

static LASFolderPicker *g_picker;

void plat_ios_pick_folder(SDL_Window *win, PlatIosPicked cb, void *ud) {
    UIWindow *w = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    UIViewController *root = w.rootViewController;
    if (!root) { fprintf(stderr, "[picker] no root view controller\n"); cb(ud, ""); return; }
    if (!g_picker) g_picker = [LASFolderPicker new];
    g_picker.cb = cb, g_picker.ud = ud;
    UIDocumentPickerViewController *p = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder]];
    p.delegate = g_picker;
    p.allowsMultipleSelection = NO;
    p.shouldShowFileExtensions = YES;
    [root presentViewController:p animated:YES completion:nil];
}

// Files (several) with the extensions in exts ("bfttf;ttf", as SDL's filters): picked as copies in the app's tmp
// folder, so no security-scoped access is needed. The callback gets a NULL-terminated list of paths, empty for a cancel.
typedef void (*PlatIosPickedFiles)(void *ud, const char *const *paths);

@interface LASFilePicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) PlatIosPickedFiles cb;
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
    if (self.cb) self.cb(self.ud, paths);
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c {
    const char *none[] = {NULL};
    if (self.cb) self.cb(self.ud, none);
}
@end

static LASFilePicker *g_file_picker;

void plat_ios_pick_files(SDL_Window *win, const char *exts, PlatIosPickedFiles cb, void *ud) {
    UIWindow *w = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    UIViewController *root = w.rootViewController;
    const char *none[] = {NULL};
    if (!root) { fprintf(stderr, "[picker] no root view controller\n"); cb(ud, none); return; }
    if (!g_file_picker) g_file_picker = [LASFilePicker new];
    g_file_picker.cb = cb, g_file_picker.ud = ud;
    // (.bfttf has no system type: it gets a dynamic one, which Files gives such files too)
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *e in [@(exts) componentsSeparatedByString:@";"]) {
        UTType *t = [UTType typeWithFilenameExtension:e];
        if (t) [types addObject:t];
    }
    if (!types.count) [types addObject:UTTypeData];
    UIDocumentPickerViewController *p = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:YES];
    p.delegate = g_file_picker;
    p.allowsMultipleSelection = YES;
    p.shouldShowFileExtensions = YES;
    [root presentViewController:p animated:YES completion:nil];
}

// The Files app at LAS Recomp's own folder (Settings > Prelaunch: the data folder, as "Show in Finder")
void plat_ios_open_documents(void) {
    extern const char *plat_ios_documents(void);
    NSString *url = [@"shareddocuments://" stringByAppendingString:@(plat_ios_documents())];
    [UIApplication.sharedApplication openURL:[NSURL URLWithString:url] options:@{} completionHandler:^(BOOL ok) {
        if (!ok) fprintf(stderr, "[picker] Files app: couldn't open %s\n", url.UTF8String);
    }];
}
