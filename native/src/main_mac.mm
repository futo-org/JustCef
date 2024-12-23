#include "main.h"

#import <Cocoa/Cocoa.h>

#include "include/cef_application_mac.h"
#include "include/wrapper/cef_helpers.h"
#import "include/wrapper/cef_library_loader.h"

#include "app_factory.h"
#include "client_manager.h"
#include "main_util.h"
#include "ipc.h"

// Receives notifications from the application.
@interface SharedAppDelegate : NSObject <NSApplicationDelegate>
- (void)createApplication:(id)object;
- (void)tryToTerminateApplication:(NSApplication*)app;
@end

// Provide the CefAppProtocol implementation required by CEF.
@interface SharedApplication : NSApplication <CefAppProtocol> {
 @private
    BOOL handlingSendEvent_;
}
@end

@implementation SharedApplication
- (BOOL)isHandlingSendEvent {
    return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
    handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
    CefScopedSendingEvent sendingEventScoper;
    [super sendEvent:event];
}

// |-terminate:| is the entry point for orderly "quit" operations in Cocoa. This
// includes the application menu's quit menu item and keyboard equivalent, the
// application's dock icon menu's quit menu item, "quit" (not "force quit") in
// the Activity Monitor, and quits triggered by user logout and system restart
// and shutdown.
//
// The default |-terminate:| implementation ends the process by calling exit(),
// and thus never leaves the main run loop. This is unsuitable for Chromium
// since Chromium depends on leaving the main run loop to perform an orderly
// shutdown. We support the normal |-terminate:| interface by overriding the
// default implementation. Our implementation, which is very specific to the
// needs of Chromium, works by asking the application delegate to terminate
// using its |-tryToTerminateApplication:| method.
//
// |-tryToTerminateApplication:| differs from the standard
// |-applicationShouldTerminate:| in that no special event loop is run in the
// case that immediate termination is not possible (e.g., if dialog boxes
// allowing the user to cancel have to be shown). Instead, this method tries to
// close all browsers by calling CloseBrowser(false) via
// ClientHandler::CloseAllBrowsers. Calling CloseBrowser will result in a call
// to ClientHandler::DoClose and execution of |-performClose:| on the NSWindow.
// DoClose sets a flag that is used to differentiate between new close events
// (e.g., user clicked the window close button) and in-progress close events
// (e.g., user approved the close window dialog). The NSWindowDelegate
// |-windowShouldClose:| method checks this flag and either calls
// CloseBrowser(false) in the case of a new close event or destructs the
// NSWindow in the case of an in-progress close event.
// ClientHandler::OnBeforeClose will be called after the CEF NSView hosted in
// the NSWindow is dealloc'ed.
//
// After the final browser window has closed ClientHandler::OnBeforeClose will
// begin actual tear-down of the application by calling CefQuitMessageLoop.
// This ends the NSApplication event loop and execution then returns to the
// main() function for cleanup before application termination.
//
// The standard |-applicationShouldTerminate:| is not supported, and code paths
// leading to it must be redirected.
- (void)terminate:(id)sender {
    SharedAppDelegate* delegate =
            static_cast<SharedAppDelegate*>([NSApp delegate]);
    [delegate tryToTerminateApplication:self];
    // Return, don't exit. The application is responsible for exiting on its own.
}
@end

@implementation SharedAppDelegate

// Create the application on the UI thread.
- (void)createApplication:(id)object {
    [NSApplication sharedApplication];
    [[NSBundle mainBundle] loadNibNamed:@"MainMenu"
                                                                owner:NSApp
                                            topLevelObjects:nil];

    // Set the delegate for application events.
    [[NSApplication sharedApplication] setDelegate:self];
}

- (void)tryToTerminateApplication:(NSApplication*)app {
    printf("tryToTerminateApplication called.\r\n");
    shared::ClientManager* manager = shared::ClientManager::GetInstance();
    if (manager && !manager->IsClosing())
    {
        printf("Calling manager->CloseAllBrowsers(false) started.\r\n");
        manager->CloseAllBrowsers(false);
        printf("Calling manager->CloseAllBrowsers(false) finished.\r\n");
    }
    printf("tryToTerminateApplication finished.\r\n");
}

- (NSApplicationTerminateReply)applicationShouldTerminate:
        (NSApplication*)sender {
    return NSTerminateNow;
}
@end

namespace shared {

// Entry point function for the browser process.
int main(int argc, char* argv[]) {
    // Load the CEF framework library at runtime instead of linking directly
    // as required by the macOS sandbox implementation.
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInMain())
        return 1;

    // Initialize the AutoRelease pool.
    NSAutoreleasePool* autopool = [[NSAutoreleasePool alloc] init];

    // Provide CEF with command-line arguments.
    CefMainArgs main_args(argc, argv);

    // Create a temporary CommandLine object.
    CefRefPtr<CefCommandLine> command_line = CreateCommandLine(main_args);

    int readFd = -1;
    int writeFd = -1;

    // Parse command-line arguments for IPC file descriptors.
    for (int i = 1; i < argc; i++) {
        NSString* arg = [NSString stringWithUTF8String:argv[i]];
        if ([arg isEqualToString:@"--parent-to-child"] && i + 1 < argc) {
            readFd = atoi(argv[++i]);
        } else if ([arg isEqualToString:@"--child-to-parent"] && i + 1 < argc) {
            writeFd = atoi(argv[++i]);
        }
    }

    if (readFd == -1 || writeFd == -1) {
        printf("Missing handles.\r\n");
        return 1;
    }

    // Set the IPC handles.
    // Assuming IPC::Singleton is a singleton accessible in this context.
    // This might need adjustments based on how your IPC is set up in Objective-C++.
    IPC::Singleton.SetHandles(readFd, writeFd);

    // Create a CefApp for the browser process. Other processes are handled by
    // process_helper_mac.cc.
    CefRefPtr<CefApp> app = CreateBrowserProcessApp();

    // Initialize the SharedApplication instance.
    [SharedApplication sharedApplication];

    // Create the singleton manager instance.
    ClientManager manager;

    // Specify CEF global settings here.
    CefSettings settings;
    //settings.log_severity = LOGSEVERITY_WARNING;
    
    NSDate *now = [NSDate date];
    NSTimeInterval s = [now timeIntervalSince1970];
    NSString *uniqueIdentifier = [NSString stringWithFormat:@"%lld", (long long)s];
    NSString *cacheDirectoryName = [@"dotcef_" stringByAppendingString:uniqueIdentifier];
    NSString *cachePath = [NSTemporaryDirectory() stringByAppendingPathComponent:cacheDirectoryName];
    CefString(&settings.cache_path) = CefString([cachePath UTF8String]);
    CefString(&settings.root_cache_path) = CefString([cachePath UTF8String]);

    // Initialize the CEF browser process. The first browser instance will be
    // created in CefBrowserProcessHandler::OnContextInitialized() after CEF has
    // been initialized. May return false if initialization fails or if early exit
    // is desired (for example, due to process singleton relaunch behavior).
    if (!CefInitialize(main_args, settings, app, nullptr)) {
        return 1;
    }

    // Create the application delegate.
    NSObject* delegate = [[SharedAppDelegate alloc] init];
    [delegate performSelectorOnMainThread:@selector(createApplication:)
                                                         withObject:nil
                                                    waitUntilDone:NO];

    // Run the CEF message loop. This will block until CefQuitMessageLoop() is
    // called.
    CefRunMessageLoop();

    // Shut down CEF.
    CefShutdown();

    // Release the delegate.
    [delegate release];

    // Release the AutoRelease pool.
    [autopool release];

    return 0;
}

}
