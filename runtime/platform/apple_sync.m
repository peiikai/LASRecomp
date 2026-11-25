// macOS / iOS: the sync folder's bundle (runtime/saves.h plat_sync_fetch / plat_sync_put) through NSFileCoordinator,
// so the cloud drive (iCloud Drive, a File Provider such as Google Drive) takes part: a file not on the device is
// downloaded before the read, and a write is one the provider sees whole (written next to it, then replaced in one
// step inside the coordinated write). On iOS the folder is one the player picked, under its bookmark's scope
// (runtime/saves.c resolves it).
#import <Foundation/Foundation.h>
#include <stdbool.h>
#include <stdio.h>
#include "../saves.h"

int plat_sync_fetch(const char *where, const char *name, const char *local) {
    @autoreleasepool {
        NSURL *u = [[NSURL fileURLWithPath:@(where) isDirectory:YES] URLByAppendingPathComponent:@(name)];
        NSURL *dst = [NSURL fileURLWithPath:@(local)];
        NSFileManager *fm = NSFileManager.defaultManager;
        // (iCloud: a file not on the device is no file here yet; ask for it, the coordinated read waits for it)
        NSNumber *ubiq = nil;
        [u getResourceValue:&ubiq forKey:NSURLIsUbiquitousItemKey error:nil];
        if (ubiq.boolValue) [fm startDownloadingUbiquitousItemAtURL:u error:nil];
        __block int r = 0;
        NSError *cerr = nil;
        NSFileCoordinator *c = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
        [c coordinateReadingItemAtURL:u options:NSFileCoordinatorReadingWithoutChanges error:&cerr byAccessor:^(NSURL *at) {
            if (![fm fileExistsAtPath:at.path]) { r = 0; return; }
            [fm removeItemAtURL:dst error:nil];
            NSError *e = nil;
            r = [fm copyItemAtURL:at toURL:dst error:&e] ? 1 : -1;
            if (r < 0) fprintf(stderr, "[saves] sync read %s: %s\n", at.path.UTF8String, e.localizedDescription.UTF8String);
        }];
        if (cerr) {
            if ([cerr.domain isEqualToString:NSCocoaErrorDomain] && cerr.code == NSFileReadNoSuchFileError) return 0;
            fprintf(stderr, "[saves] sync read: %s\n", cerr.localizedDescription.UTF8String);
            return -1;
        }
        return r;
    }
}

bool plat_sync_put(const char *where, const char *name, const char *local) {
    @autoreleasepool {
        NSURL *dir = [NSURL fileURLWithPath:@(where) isDirectory:YES];
        NSURL *u = [dir URLByAppendingPathComponent:@(name)];
        NSURL *src = [NSURL fileURLWithPath:@(local)];
        NSFileManager *fm = NSFileManager.defaultManager;
        __block bool ok = false;
        NSError *cerr = nil;
        NSFileCoordinator *c = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
        [c coordinateWritingItemAtURL:u options:NSFileCoordinatorWritingForReplacing error:&cerr byAccessor:^(NSURL *at) {
            NSURL *part = [[at URLByDeletingLastPathComponent]
                URLByAppendingPathComponent:[NSString stringWithFormat:@".%@.part", at.lastPathComponent]];
            [fm removeItemAtURL:part error:nil];
            NSError *e = nil;
            if (![fm copyItemAtURL:src toURL:part error:&e]) {
                fprintf(stderr, "[saves] sync write %s: %s\n", part.path.UTF8String, e.localizedDescription.UTF8String);
                return;
            }
            if ([fm fileExistsAtPath:at.path])
                ok = [fm replaceItemAtURL:at withItemAtURL:part backupItemName:nil options:0 resultingItemURL:nil error:&e];
            else ok = [fm moveItemAtURL:part toURL:at error:&e];
            if (!ok) {
                fprintf(stderr, "[saves] sync write %s: %s\n", at.path.UTF8String, e.localizedDescription.UTF8String);
                [fm removeItemAtURL:part error:nil];
            }
        }];
        if (cerr) fprintf(stderr, "[saves] sync write: %s\n", cerr.localizedDescription.UTF8String);
        return ok && !cerr;
    }
}
