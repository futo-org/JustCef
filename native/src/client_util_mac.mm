#include "client_util.h"

#import <Cocoa/Cocoa.h>
#include <dispatch/dispatch.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "include/cef_browser.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static inline NSView* BrowserNSView(CefRefPtr<CefBrowser> browser) {
  return (__bridge NSView*)browser->GetHost()->GetWindowHandle();
}

static inline NSWindow* BrowserNSWindow(CefRefPtr<CefBrowser> browser) {
  NSView* v = BrowserNSView(browser);
  return v ? v.window : nil;
}

static inline NSArray<UTType*>* ContentTypesFromFilters(const std::vector<std::pair<std::string,std::string>>& filters) {
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
}
