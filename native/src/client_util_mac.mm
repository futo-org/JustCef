#include "client_util.h"

#import <Cocoa/Cocoa.h>
#include <dispatch/dispatch.h>

#include "include/cef_browser.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace shared {
    void PlatformTitleChange(CefRefPtr<CefBrowser> browser, const std::string& title) 
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        std::string titleStr(title);
        NSString* str = [NSString stringWithUTF8String:titleStr.c_str()];
        [window setTitle:str];
    }

    void PlatformIconChange(CefRefPtr<CefBrowser> browser, const std::string& iconPath) 
    {
        int width, height, channels;
        unsigned char* image = stbi_load(iconPath.c_str(), &width, &height, &channels, 4);
        if (!image) {
            return;
        }

        NSImage* nsImage = nil;
        {
            NSData* data = [NSData dataWithBytes:image length:width * height * 4];
            NSBitmapImageRep* imageRep = [[NSBitmapImageRep alloc] initWithData:data];
            [imageRep setSize:NSMakeSize(width, height)];
            nsImage = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
            [nsImage addRepresentation:imageRep];
        }

        if (nsImage) {
            [NSApp setApplicationIconImage:nsImage];
            //TODO: Should nsImage be freed?
        }

        stbi_image_free(image);
    }
    
    bool PlatformGetFullscreen(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return false;

        NSWindow* window = [view window];
        if (!window) return false;

        return ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
    }

    void PlatformSetMinimumWindowSize(CefRefPtr<CefBrowser> browser, int minWidth, int minHeight)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        NSSize minSize;
        minSize.width = minWidth;
        minSize.height = minHeight;
        
        [window setContentMinSize:minSize];
    }

    void PlatformSetFrameless(CefRefPtr<CefBrowser> browser, bool frameless)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
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
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
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
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
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
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        [window zoom:nil];
    }

    void PlatformMinimize(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        [window miniaturize:nil];
    }

    void PlatformRestore(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        [window deminiaturize:nil];
    }

    void PlatformShow(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        [window makeKeyAndOrderFront:nil];
    }

    void PlatformHide(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        [window orderOut:nil];
    }

    void PlatformActivate(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyAndOrderFront:nil];
    }

    void PlatformBringToTop(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        [window orderFrontRegardless];
    }

    void PlatformSetAlwaysOnTop(CefRefPtr<CefBrowser> browser, bool alwaysOnTop)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        if (alwaysOnTop) {
            [window setLevel:NSFloatingWindowLevel];
        } else {
            [window setLevel:NSNormalWindowLevel];
        }
    }

    CefSize PlatformGetWindowSize(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return CefSize(0, 0);

        NSWindow* window = [view window];
        if (!window) return CefSize(0, 0);

        NSRect frame = [window frame];
        return CefSize(frame.size.width, frame.size.height);
    }

    void PlatformCenterWindow(CefRefPtr<CefBrowser> browser, const CefSize& size)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        NSRect screen = [[window screen] frame];
        NSRect frame = NSMakeRect((screen.size.width - size.width) / 2,
                                (screen.size.height - size.height) / 2,
                                size.width, size.height);
        [window setFrame:frame display:YES];
    }

    void PlatformSetWindowSize(CefRefPtr<CefBrowser> browser, const CefSize& size)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        NSRect frame = [window frame];
        frame.size = NSMakeSize(size.width, size.height);
        [window setFrame:frame display:YES];
    }

    CefPoint PlatformGetWindowPosition(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return CefPoint(0, 0);

        NSWindow* window = [view window];
        if (!window) return CefPoint(0, 0);

        NSRect frame = [window frame];
        return CefPoint(frame.origin.x, frame.origin.y);
    }

    void PlatformSetWindowPosition(CefRefPtr<CefBrowser> browser, const CefPoint& position)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        NSRect frame = [window frame];
        frame.origin.x = position.x;
        frame.origin.y = position.y;
        [window setFrameOrigin:frame.origin];
    }

    void PlatformWindowRequestFocus(CefRefPtr<CefBrowser> browser)
    {
        NSView* view = (NSView*)browser->GetHost()->GetWindowHandle();
        if (!view) return;

        NSWindow* window = [view window];
        if (!window) return;

        [window makeKeyWindow];
    }

    std::future<std::vector<std::string>> PlatformPickFiles(bool multiple, const std::vector<std::pair<std::string, std::string>>& filters)
    {
        auto promise = std::make_shared<std::promise<std::vector<std::string>>>();

        dispatch_async(dispatch_get_main_queue(), ^{
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            [panel setAllowsMultipleSelection:multiple];
            [panel setCanChooseDirectories:NO];
            [panel setCanChooseFiles:YES];

            NSMutableArray* fileTypes = [[NSMutableArray alloc] init];
            for (const auto& filter : filters) {
                [fileTypes addObject:[NSString stringWithUTF8String:filter.second.c_str()]];
            }
            [panel setAllowedFileTypes:fileTypes];

            [panel beginWithCompletionHandler:^(NSInteger result) {
                std::vector<std::string> files;
                if (result == NSModalResponseOK) {
                    for (NSURL* url in [panel URLs]) {
                        files.push_back([[url path] UTF8String]);
                    }
                }
                promise->set_value(files);
                [fileTypes release];
            }];
        });

        return promise->get_future();
    }

    std::future<std::string> PlatformPickDirectory()
    {
        auto promise = std::make_shared<std::promise<std::string>>();

        dispatch_async(dispatch_get_main_queue(), ^{
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            [panel setCanChooseDirectories:YES];
            [panel setCanChooseFiles:NO];
            [panel setAllowsMultipleSelection:NO];

            [panel beginWithCompletionHandler:^(NSInteger result) {
                if (result == NSModalResponseOK) {
                    NSURL* url = [[panel URLs] objectAtIndex:0];
                    promise->set_value([[url path] UTF8String]);
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
            [savePanel setNameFieldStringValue:[NSString stringWithUTF8String:default_name.c_str()]];

            NSMutableArray* fileTypes = [[NSMutableArray alloc] init];
            for (const auto& filter : filters) {
                [fileTypes addObject:[NSString stringWithUTF8String:filter.second.c_str()]];
            }
            [savePanel setAllowedFileTypes:fileTypes];

            [savePanel beginWithCompletionHandler:^(NSInteger result) {
                if (result == NSModalResponseOK) {
                    NSURL* url = [[savePanel URL] retain];
                    promise->set_value([[url path] UTF8String]);
                    [url release];
                } else {
                    promise->set_value("");
                }
                [fileTypes release];
            }];
        });

        return promise->get_future();
    }
}
