// iOS: a system text prompt (UIAlertController with a text field) for our menus' text fields when a finger taps
// them: the system keyboard with its own Done / Cancel, paste and autocorrect - typed into an ImGui field the
// keyboard covered half the menu. Asked from the UI thread that draws the menu (any thread), shown on the main one;
// the answer is polled (plat_touch_prompt_poll).
#import <UIKit/UIKit.h>
#include <pthread.h>
#include <stdio.h>

#include "../platform.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_text[256];
static int g_result;  // 1 OK, -1 cancelled (taken once)
static int g_serial;  // (a prompt cancelled from our side: its late answer is dropped)
static UIAlertController *g_alert;

static void answer(int serial, int result, NSString *text) {
    pthread_mutex_lock(&g_lock);
    if (serial == g_serial) {
        g_result = result;
        snprintf(g_text, sizeof g_text, "%s", text ? text.UTF8String : "");
    }
    pthread_mutex_unlock(&g_lock);
}

void plat_touch_prompt(const char *title, const char *hint, const char *initial, int max_len, const char *ok, const char *cancel) {
    NSString *t = @(title ? title : ""), *h = @(hint ? hint : ""), *v = @(initial ? initial : "");
    NSString *okl = @(ok ? ok : "OK"), *cl = @(cancel ? cancel : "Cancel");
    pthread_mutex_lock(&g_lock);
    int serial = ++g_serial;
    g_result = 0;
    pthread_mutex_unlock(&g_lock);
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *root = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
            if ([s isKindOfClass:UIWindowScene.class])
                for (UIWindow *w in ((UIWindowScene *)s).windows)
                    if (w.isKeyWindow) root = w.rootViewController;
        if (!root) { fprintf(stderr, "[prompt] no window to show over\n"); answer(serial, -1, nil); return; }
        UIAlertController *a = [UIAlertController alertControllerWithTitle:t message:nil preferredStyle:UIAlertControllerStyleAlert];
        [a addTextFieldWithConfigurationHandler:^(UITextField *f) {
            f.placeholder = h;
            f.text = v;
            f.clearButtonMode = UITextFieldViewModeWhileEditing;
            f.autocorrectionType = UITextAutocorrectionTypeNo;
            f.autocapitalizationType = UITextAutocapitalizationTypeNone;
            f.returnKeyType = UIReturnKeyDone;
            f.enablesReturnKeyAutomatically = NO;
        }];
        UIAlertAction *okA = [UIAlertAction actionWithTitle:okl style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) {
            g_alert = nil;
            answer(serial, 1, a.textFields.firstObject.text);
        }];
        [a addAction:[UIAlertAction actionWithTitle:cl style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) {
            g_alert = nil;
            answer(serial, -1, nil);
        }]];
        [a addAction:okA];
        a.preferredAction = okA;
        if (max_len > 0) {  // (Return in the field: OK; the length kept as typed)
            UITextField *f = a.textFields.firstObject;
            [NSNotificationCenter.defaultCenter addObserverForName:UITextFieldTextDidChangeNotification object:f queue:nil
                                                        usingBlock:^(NSNotification *n) {
                if (f.markedTextRange) return;  // (mid-composition)
                if ((int)f.text.length > max_len) f.text = [f.text substringToIndex:(NSUInteger)max_len];
            }];
        }
        g_alert = a;
        [root presentViewController:a animated:YES completion:nil];
    });
}

int plat_touch_prompt_poll(char *out, size_t n) {
    pthread_mutex_lock(&g_lock);
    int r = g_result;
    if (r) { snprintf(out, n, "%s", g_text); g_result = 0; }
    pthread_mutex_unlock(&g_lock);
    return r;
}

void plat_touch_prompt_cancel(void) {
    pthread_mutex_lock(&g_lock);
    g_serial++, g_result = 0;
    pthread_mutex_unlock(&g_lock);
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_alert) [g_alert dismissViewControllerAnimated:YES completion:nil];
        g_alert = nil;
    });
}
