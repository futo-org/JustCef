#include "client_util.h"

#import <Cocoa/Cocoa.h>
#include <dispatch/dispatch.h>

#include "include/cef_browser.h"

#if __has_include(<UniformTypeIdentifiers/UniformTypeIdentifiers.h>)
  #import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
  #define HAS_UTTYPE 1
#else
  #define HAS_UTTYPE 0
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static inline NSView* BrowserNSView(CefRefPtr<CefBrowser> browser) {
  return (__bridge NSView*)browser->GetHost()->GetWindowHandle();
}

static inline NSWindow* BrowserNSWindow(CefRefPtr<CefBrowser> browser) {
  NSView* v = BrowserNSView(browser);
  return v ? v.window : nil;
}


static inline NSArray<NSString*>* ExtensionsFromFilters(const std::vector<std::pair<std::string,std::string>>& filters) {
  NSMutableArray<NSString*>* exts = [NSMutableArray array];
  for (const auto& f : filters) {
    NSString* s = [NSString stringWithUTF8String:f.second.c_str()];
    if (s.length == 0) continue;
    if ([s hasPrefix:@"."]) s = [s substringFromIndex:1];
    if ([s containsString:@"/"]) continue;
    [exts addObject:s];
  }
  return exts;
}

#if HAS_UTTYPE
static inline NSArray<UTType*>* ContentTypesFromFilters(const std::vector<std::pair<std::string,std::string>>& filters) API_AVAILABLE(macos(11.0)) {
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  for (const auto& f : filters) {
    NSString* tag = [NSString stringWithUTF8String:f.second.c_str()];
    if (tag.length == 0) continue;

    UTType* t = nil;
    if ([tag containsString:@"/"]) {
      t = [UTType typeWithMIMEType:tag];
    } else {
      if ([tag hasPrefix:@"."]) tag = [tag substringFromIndex:1];
      t = [UTType typeWithFilenameExtension:tag];
    }
    if (t) [types addObject:t];
  }
  return types;
}
#endif

namespace shared {
    void PlatformTitleChange(CefRefPtr<CefBrowser> browser, const std::string& title) 
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        std::string titleStr(title);
        NSString* str = [NSString stringWithUTF8String:titleStr.c_str()];
        [window setTitle:str];
    }

    void PlatformIconChange(CefRefPtr<CefBrowser> /*browser*/, const std::string& iconPath)
    {
        int w, h, channels;
        unsigned char* rgba = stbi_load(iconPath.c_str(), &w, &h, &channels, 4);
        if (!rgba) return;

        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                        pixelsWide:w
                        pixelsHigh:h
                    bitsPerSample:8
                    samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                    colorSpaceName:NSCalibratedRGBColorSpace
                        bytesPerRow:w*4
                        bitsPerPixel:32];

        memcpy([rep bitmapData], rgba, (size_t)w * (size_t)h * 4);

        NSImage* img = [[NSImage alloc] initWithSize:NSMakeSize(w, h)];
        [img addRepresentation:rep];

        [NSApp setApplicationIconImage:img];

        stbi_image_free(rgba);
    }
    
    bool PlatformGetFullscreen(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return false;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return false;

        return ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
    }

    void PlatformSetMinimumWindowSize(CefRefPtr<CefBrowser> browser, int minWidth, int minHeight)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        NSSize minSize;
        minSize.width = minWidth;
        minSize.height = minHeight;
        
        [window setContentMinSize:minSize];
    }

    void PlatformSetFrameless(CefRefPtr<CefBrowser> browser, bool frameless)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        NSWindowStyleMask styleMask = [window styleMask];
        if (frameless) {
            styleMask &= ~NSWindowStyleMaskTitled;
        } else {
            styleMask |= NSWindowStyleMaskTitled;
        }
        [window setStyleMask:styleMask];
    }

    void PlatformSetResizable(CefRefPtr<CefBrowser> browser, bool resizable)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        NSWindowStyleMask styleMask = [window styleMask];
        if (resizable) {
            styleMask |= NSWindowStyleMaskResizable;
        } else {
            styleMask &= ~NSWindowStyleMaskResizable;
        }
        [window setStyleMask:styleMask];
    }

    void PlatformSetFullscreen(CefRefPtr<CefBrowser> browser, bool fullscreen)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        bool isCurrentlyFullscreen = ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
        
        if (fullscreen != isCurrentlyFullscreen) {
            dispatch_async(dispatch_get_main_queue(), ^{
                [window toggleFullScreen:nil];
            });
        }
    }

    void PlatformMaximize(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        [window zoom:nil];
    }

    void PlatformMinimize(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        [window miniaturize:nil];
    }

    void PlatformRestore(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        [window deminiaturize:nil];
    }

    void PlatformShow(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        [window makeKeyAndOrderFront:nil];
    }

    void PlatformHide(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        [window orderOut:nil];
    }

    void PlatformActivate(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyAndOrderFront:nil];
    }

    void PlatformBringToTop(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        [window orderFrontRegardless];
    }

    void PlatformSetAlwaysOnTop(CefRefPtr<CefBrowser> browser, bool alwaysOnTop)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        if (alwaysOnTop) {
            [window setLevel:NSFloatingWindowLevel];
        } else {
            [window setLevel:NSNormalWindowLevel];
        }
    }

    CefSize PlatformGetWindowSize(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return CefSize(0, 0);

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return CefSize(0, 0);

        NSRect frame = [window frame];
        return CefSize(frame.size.width, frame.size.height);
    }

    void PlatformCenterWindow(CefRefPtr<CefBrowser> browser, const CefSize& size)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        NSRect screen = [[window screen] frame];
        NSRect frame = NSMakeRect((screen.size.width - size.width) / 2,
                                (screen.size.height - size.height) / 2,
                                size.width, size.height);
        [window setFrame:frame display:YES];
    }

    void PlatformSetWindowSize(CefRefPtr<CefBrowser> browser, const CefSize& size)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        NSRect frame = [window frame];
        frame.size = NSMakeSize(size.width, size.height);
        [window setFrame:frame display:YES];
    }

    CefPoint PlatformGetWindowPosition(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return CefPoint(0, 0);

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return CefPoint(0, 0);

        NSRect frame = [window frame];
        return CefPoint(frame.origin.x, frame.origin.y);
    }

    void PlatformSetWindowPosition(CefRefPtr<CefBrowser> browser, const CefPoint& position)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        NSRect frame = [window frame];
        frame.origin.x = position.x;
        frame.origin.y = position.y;
        [window setFrameOrigin:frame.origin];
    }

    void PlatformWindowRequestFocus(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = BrowserNSView(browser);
        if (!view) return;

        NSWindow* window = BrowserNSWindow(browser);
        if (!window) return;

        [window makeKeyWindow];
    }

    std::future<std::vector<std::string>> PlatformPickFiles(bool multiple, const std::vector<std::pair<std::string, std::string>>& filters)
    {
        auto promise = std::make_shared<std::promise<std::vector<std::string>>>();

        dispatch_async(dispatch_get_main_queue(), ^{
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.allowsMultipleSelection = multiple;
            panel.canChooseDirectories = NO;
            panel.canChooseFiles = YES;

        #if HAS_UTTYPE
            if (@available(macOS 11.0, *)) {
                panel.allowedContentTypes = ContentTypesFromFilters(filters);
            } else
        #endif
            {
                #pragma clang diagnostic push
                #pragma clang diagnostic ignored "-Wdeprecated-declarations"
                panel.allowedFileTypes = ExtensionsFromFilters(filters);
                #pragma clang diagnostic pop
            }

            [panel beginWithCompletionHandler:^(NSInteger result) {
                std::vector<std::string> files;
                if (result == NSModalResponseOK) {
                    for (NSURL* url in panel.URLs) {
                        const char* p = url.path.UTF8String;
                        if (p) files.emplace_back(p);
                    }
                }
                promise->set_value(std::move(files));
            }];
        });

        return promise->get_future();
    }

    std::future<std::string> PlatformPickDirectory()
    {
        auto promise = std::make_shared<std::promise<std::string>>();

        dispatch_async(dispatch_get_main_queue(), ^{
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.canChooseDirectories = YES;
            panel.canChooseFiles = NO;
            panel.allowsMultipleSelection = NO;

            [panel beginWithCompletionHandler:^(NSInteger result) {
            if (result == NSModalResponseOK) {
                NSURL* url = panel.URLs.firstObject;
                promise->set_value(url ? url.path.UTF8String : "");
            } else {
                promise->set_value("");
            }
            }];
        });

        return promise->get_future();
    }

    std::future<std::string> PlatformSaveFile(const std::string& default_name, const std::vector<std::pair<std::string, std::string>>& filters)
    {
        auto promise = std::make_shared<std::promise<std::string>>();

        dispatch_async(dispatch_get_main_queue(), ^{
            NSSavePanel* savePanel = [NSSavePanel savePanel];
            savePanel.nameFieldStringValue = [NSString stringWithUTF8String:default_name.c_str()];

        #if HAS_UTTYPE
            if (@available(macOS 11.0, *)) {
                savePanel.allowedContentTypes = ContentTypesFromFilters(filters);
            } else
        #endif
            {
                #pragma clang diagnostic push
                #pragma clang diagnostic ignored "-Wdeprecated-declarations"
                savePanel.allowedFileTypes = ExtensionsFromFilters(filters);
                #pragma clang diagnostic pop
            }

            [savePanel beginWithCompletionHandler:^(NSInteger result) {
                if (result == NSModalResponseOK) {
                    NSURL* url = savePanel.URL;
                    promise->set_value(url ? url.path.UTF8String : "");
                } else {
                    promise->set_value("");
                }
            }];
        });

        return promise->get_future();
    }

}
