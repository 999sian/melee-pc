/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ios_dialog.h"

#if defined(__APPLE__) && defined(TARGET_OS_IPHONE)
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface MeleeDocPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property (nonatomic, assign) SDL_DialogFileCallback callback;
@property (nonatomic, assign) void* userdata;
@property (nonatomic, strong) MeleeDocPickerDelegate* selfRetain;
@property (nonatomic, strong) NSURL* securityScopedURL;
@end

static NSURL* s_activeSecurityScopedURL = nil;

@implementation MeleeDocPickerDelegate

- (void)finishWithFiles:(const char* const*)files filter:(int)filter {
    if (self.callback) {
        self.callback(self.userdata, files, filter);
        self.callback = NULL;
    }
    self.selfRetain = nil;
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (!urls || urls.count == 0) {
        const char* const files[] = { NULL };
        [self finishWithFiles:files filter:0];
        return;
    }

    NSURL *sourceURL = urls.firstObject;
    BOOL accessing = [sourceURL startAccessingSecurityScopedResource];

    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *docsDir = [paths firstObject];
    NSString *fileName = [sourceURL lastPathComponent];
    NSString *destPath = [docsDir stringByAppendingPathComponent:fileName];

    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *chosenPath = sourceURL.path;

    // If file is not already inside Documents, copy it into Documents for persistent local access
    if (![sourceURL.path hasPrefix:docsDir]) {
        BOOL needCopy = YES;
        if ([fm fileExistsAtPath:destPath]) {
            NSDictionary *srcAttr = [fm attributesOfItemAtPath:sourceURL.path error:nil];
            NSDictionary *dstAttr = [fm attributesOfItemAtPath:destPath error:nil];
            if ([srcAttr[NSFileSize] isEqualToNumber:dstAttr[NSFileSize]]) {
                needCopy = NO;
                chosenPath = destPath;
            } else {
                [fm removeItemAtPath:destPath error:nil];
            }
        }
        if (needCopy) {
            NSError *copyErr = nil;
            if ([fm copyItemAtPath:sourceURL.path toPath:destPath error:&copyErr]) {
                chosenPath = destPath;
            } else {
                // If copy fails (e.g. disk space), fallback to source path
                chosenPath = sourceURL.path;
            }
        }
    } else {
        chosenPath = sourceURL.path;
    }

    if (accessing) {
        if ([chosenPath isEqualToString:sourceURL.path]) {
            // Retain active security scope for external URL
            if (s_activeSecurityScopedURL && s_activeSecurityScopedURL != sourceURL) {
                [s_activeSecurityScopedURL stopAccessingSecurityScopedResource];
            }
            s_activeSecurityScopedURL = sourceURL;
        } else {
            [sourceURL stopAccessingSecurityScopedResource];
        }
    }

    const char* utf8 = [chosenPath UTF8String];
    const char* const files[] = { utf8, NULL };
    [self finishWithFiles:files filter:0];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    const char* const files[] = { NULL };
    [self finishWithFiles:files filter:0];
}

@end

static UIViewController* FindTopViewController(void) {
    UIWindow *keyWindow = nil;
    for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
        if (scene.activationState == UISceneActivationStateForegroundActive &&
            [scene isKindOfClass:[UIWindowScene class]]) {
            UIWindowScene *windowScene = (UIWindowScene *)scene;
            for (UIWindow *w in windowScene.windows) {
                if (w.isKeyWindow) {
                    keyWindow = w;
                    break;
                }
            }
            if (keyWindow) break;
        }
    }
    if (!keyWindow) {
        for (UIWindow *w in [UIApplication sharedApplication].windows) {
            if (w.isKeyWindow) {
                keyWindow = w;
                break;
            }
        }
    }
    if (!keyWindow) {
        keyWindow = [UIApplication sharedApplication].windows.firstObject;
    }
    UIViewController *top = keyWindow.rootViewController;
    while (top.presentedViewController) {
        top = top.presentedViewController;
    }
    return top;
}

void ios_show_open_file_dialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window) {
    (void)window;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *topVC = FindTopViewController();
        if (!topVC) {
            if (callback) callback(userdata, NULL, -1);
            return;
        }

        MeleeDocPickerDelegate *delegate = [[MeleeDocPickerDelegate alloc] init];
        delegate.callback = callback;
        delegate.userdata = userdata;
        delegate.selfRetain = delegate;

        UIDocumentPickerViewController *picker = nil;
        if (@available(iOS 14.0, *)) {
            NSMutableArray<UTType *> *types = [NSMutableArray array];
            NSArray<NSString *> *extensions = @[@"iso", @"gcm", @"ciso", @"rvz", @"gcz", @"wia"];
            for (NSString *ext in extensions) {
                UTType *t = [UTType typeWithFilenameExtension:ext];
                if (t) [types addObject:t];
            }
            [types addObject:UTTypeData];
            [types addObject:UTTypeItem];
            picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:NO];
        } else {
            picker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[@"public.data", @"public.item"]
                                                                            inMode:UIDocumentPickerModeOpen];
        }

        picker.delegate = delegate;
        picker.allowsMultipleSelection = NO;

        if (picker.popoverPresentationController) {
            picker.popoverPresentationController.sourceView = topVC.view;
            picker.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(topVC.view.bounds),
                                                                         CGRectGetMidY(topVC.view.bounds), 1, 1);
            picker.popoverPresentationController.permittedArrowDirections = 0;
        }

        [topVC presentViewController:picker animated:YES completion:nil];
    });
}

#endif
