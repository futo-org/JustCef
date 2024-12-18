#include "client_util.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <string>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <fstream>

#include "include/base/cef_logging.h"
#include "include/cef_browser.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace shared {
    void PlatformTitleChange(CefRefPtr<CefBrowser> browser, const std::string& title) 
    {
        std::string titleStr(title);
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        Atom _NET_WM_NAME = XInternAtom(display, "_NET_WM_NAME", False);
        Atom UTF8_STRING = XInternAtom(display, "UTF8_STRING", False);

        XChangeProperty(display, window, _NET_WM_NAME, UTF8_STRING, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(titleStr.c_str()), titleStr.size());

        XTextProperty textProperty;
        char* cTitle = const_cast<char*>(titleStr.c_str());
        Xutf8TextListToTextProperty(display, &cTitle, 1, XUTF8StringStyle, &textProperty);
        XSetWMName(display, window, &textProperty);
        XFree(textProperty.value);

        XFlush(display);
    }

    void PlatformIconChange(CefRefPtr<CefBrowser> browser, const std::string& iconPath) 
    {
        int width, height, channels;
        unsigned char* image = stbi_load(iconPath.c_str(), &width, &height, &channels, 4);
        if (!image) {
            LOG(ERROR) << "Failed to load image from path: " << iconPath;
            return;
        }

        Display* display = XOpenDisplay(NULL);
        if (!display) {
            LOG(ERROR) << "Failed to open X display.";
            stbi_image_free(image);
            return;
        }

        Window window = (Window)browser->GetHost()->GetWindowHandle();
        if (!window) {
            LOG(ERROR) << "Invalid window handle.";
            stbi_image_free(image);
            XCloseDisplay(display);
            return;
        }

        Atom _NET_WM_ICON = XInternAtom(display, "_NET_WM_ICON", False);
        if (_NET_WM_ICON == None) {
            LOG(ERROR) << "Failed to get _NET_WM_ICON atom.";
            stbi_image_free(image);
            XCloseDisplay(display);
            return;
        }

        Atom cardinal = XInternAtom(display, "CARDINAL", False);
        if (cardinal == None) {
            LOG(ERROR) << "Failed to get CARDINAL atom.";
            stbi_image_free(image);
            XCloseDisplay(display);
            return;
        }

        size_t sz = width * height;
        size_t asz = sz + 2;
        unsigned long* iconData = (unsigned long*)malloc(asz * sizeof(unsigned long));
        if (!iconData) {
            LOG(ERROR) << "Failed to allocate memory for icon data.";
            stbi_image_free(image);
            XCloseDisplay(display);
            return;
        }

        iconData[0] = width;
        iconData[1] = height;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                size_t srcIndex = (y * width + x) * 4;
                size_t dstIndex = (x + y * width) + 2;
                uint32_t pixel = *((uint32_t*)(image + srcIndex));
                uint32_t bgra = ((pixel & 0x000000FF) << 16) | (pixel & 0x0000FF00) | ((pixel & 0x00FF0000) >> 16) | (pixel & 0xFF000000);
                iconData[dstIndex] = bgra;
            }
        }

        XChangeProperty(display, window, _NET_WM_ICON, cardinal, 32, PropModeReplace, (const unsigned char*)iconData, asz);
        XFlush(display);
        XCloseDisplay(display);

        free(iconData);
        stbi_image_free(image);
    }

    bool PlatformGetFullscreen(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
        Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
        
        Atom actualType;
        int actualFormat;
        unsigned long numItems, bytesAfter;
        unsigned char* prop = NULL;

        int status = XGetWindowProperty(display, window, wmState, 0, LONG_MAX, False, AnyPropertyType,
                                        &actualType, &actualFormat, &numItems, &bytesAfter, &prop);
        if (status != Success)
        {
            XFree(prop);
            return false;
        }

        bool isFullscreen = false;
        Atom* p = (Atom*)prop;
        for (unsigned long i = 0; i < numItems; i++)
        {
            if (p[i] == fullscreen)
            {
                isFullscreen = true;
                break;
            }
        }

        XFree(prop);
        return isFullscreen;
    }

    void PlatformSetMinimumWindowSize(CefRefPtr<CefBrowser> browser, int minWidth, int minHeight)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XSizeHints* sizeHints = XAllocSizeHints();
        sizeHints->flags = PMinSize;
        sizeHints->min_width = minWidth;
        sizeHints->min_height = minHeight;

        XSetWMNormalHints(display, window, sizeHints);
        XFree(sizeHints);
        XFlush(display);
    }

    void PlatformSetFrameless(CefRefPtr<CefBrowser> browser, bool frameless)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        Atom wmHints = XInternAtom(display, "_MOTIF_WM_HINTS", True);
        if (wmHints != None) {
            struct MotifWmHints {
                unsigned long flags;
                unsigned long functions;
                unsigned long decorations;
                long input_mode;
                unsigned long status;
            };

            MotifWmHints hints;
            hints.flags = 2; // MWM_HINTS_DECORATIONS
            hints.decorations = frameless ? 0 : 1; // No decorations if frameless, otherwise default decorations

            XChangeProperty(display, window, wmHints, wmHints, 32, PropModeReplace, (unsigned char*)&hints, 5);
        }

        XFlush(display);
    }

    void PlatformSetResizable(CefRefPtr<CefBrowser> browser, bool resizable)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XSizeHints* sizeHints = XAllocSizeHints();
        long supplied_return;

        XGetWMNormalHints(display, window, sizeHints, &supplied_return);
        if (resizable) {
            sizeHints->flags &= ~PMaxSize;
        } else {
            sizeHints->flags |= PMaxSize;
        }

        XSetWMNormalHints(display, window, sizeHints);
        XFree(sizeHints);
        XFlush(display);
    }

    void PlatformSetFullscreen(CefRefPtr<CefBrowser> browser, bool fullscreen)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
        Atom fullscreenAtom = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);

        XEvent xev;
        memset(&xev, 0, sizeof(xev));
        xev.type = ClientMessage;
        xev.xclient.window = window;
        xev.xclient.message_type = wmState;
        xev.xclient.format = 32;
        xev.xclient.data.l[0] = fullscreen ? 1 : 0; // 1 for _NET_WM_STATE_ADD, 0 for _NET_WM_STATE_REMOVE
        xev.xclient.data.l[1] = fullscreenAtom;
        xev.xclient.data.l[2] = 0; // No second property to toggle
        xev.xclient.data.l[3] = 0; // No source indication

        XSendEvent(display, DefaultRootWindow(display), False,
                SubstructureRedirectMask | SubstructureNotifyMask, &xev);

        XFlush(display);
    }

    void PlatformMaximize(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        const char* kAtoms[] = {"_NET_WM_STATE", "UTF8_STRING", "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"};
        Atom atoms[4];
        int result = XInternAtoms(display, const_cast<char**>(kAtoms), 4, false, atoms);
        DCHECK(result);

        XEvent xev;
        memset(&xev, 0, sizeof(xev));
        xev.type = ClientMessage;
        xev.xclient.window = window;
        xev.xclient.message_type = atoms[0]; // _NET_WM_STATE
        xev.xclient.format = 32;
        xev.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
        xev.xclient.data.l[1] = atoms[2]; // _NET_WM_STATE_MAXIMIZED_VERT
        xev.xclient.data.l[2] = atoms[3]; // _NET_WM_STATE_MAXIMIZED_HORZ
        xev.xclient.data.l[3] = 0; // no source indication

        XSendEvent(display, DefaultRootWindow(display), False,
                SubstructureRedirectMask | SubstructureNotifyMask, &xev);

        XFlush(display);
    }

    void PlatformMinimize(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XIconifyWindow(display, window, DefaultScreen(display));
        XFlush(display);
    }

    void PlatformRestore(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XMapWindow(display, window);
        XFlush(display);
    }

    void PlatformShow(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XMapRaised(display, window);
        XFlush(display);
    }

    void PlatformHide(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XUnmapWindow(display, window);
        XFlush(display);
    }

    void PlatformActivate(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        Atom wmActivate = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);

        XEvent xev;
        memset(&xev, 0, sizeof(xev));
        xev.type = ClientMessage;
        xev.xclient.window = window;
        xev.xclient.message_type = wmActivate;
        xev.xclient.format = 32;
        xev.xclient.data.l[0] = 2; // 2 means the request comes from a window or application
        xev.xclient.data.l[1] = CurrentTime; // We use CurrentTime for simplicity
        xev.xclient.data.l[2] = 0;
        xev.xclient.data.l[3] = 0;
        xev.xclient.data.l[4] = 0;

        XSendEvent(display, DefaultRootWindow(display), False,
                SubstructureRedirectMask | SubstructureNotifyMask, &xev);

        XFlush(display);
    }

    void PlatformBringToTop(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XRaiseWindow(display, window);
        XFlush(display);
    }

    void PlatformSetAlwaysOnTop(CefRefPtr<CefBrowser> browser, bool alwaysOnTop)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
        Atom wmStateAbove = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);

        XEvent xev;
        memset(&xev, 0, sizeof(xev));
        xev.type = ClientMessage;
        xev.xclient.window = window;
        xev.xclient.message_type = wmState;
        xev.xclient.format = 32;
        xev.xclient.data.l[0] = alwaysOnTop ? 1 : 0; // 1 to add, 0 to remove the state
        xev.xclient.data.l[1] = wmStateAbove;
        xev.xclient.data.l[2] = 0; // No second property to toggle
        xev.xclient.data.l[3] = 0; // No source indication
        xev.xclient.data.l[4] = 0; // Unused

        XSendEvent(display, DefaultRootWindow(display), False,
                SubstructureRedirectMask | SubstructureNotifyMask, &xev);

        XFlush(display);
    }

    CefSize PlatformGetWindowSize(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XWindowAttributes attrs;
        XGetWindowAttributes(display, window, &attrs);

        return CefSize(attrs.width, attrs.height);
    }

    void PlatformCenterWindow(CefRefPtr<CefBrowser> browser, const CefSize& size)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        Screen* screen = DefaultScreenOfDisplay(display);
        int screenWidth = screen->width;
        int screenHeight = screen->height;

        int x = (screenWidth - size.width) / 2;
        int y = (screenHeight - size.height) / 2;

        XMoveResizeWindow(display, window, x, y, size.width, size.height);
        XFlush(display);
    }

    void PlatformSetWindowSize(CefRefPtr<CefBrowser> browser, const CefSize& size)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XResizeWindow(display, window, size.width, size.height);
        XFlush(display);
    }

    CefPoint PlatformGetWindowPosition(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        ::Window root;
        int x, y;
        unsigned int width, height, border_width, depth;
        
        if (XGetGeometry(display, window, &root, &x, &y, &width, &height, &border_width, &depth))
        {
            int root_x, root_y;
            ::Window child;
            XTranslateCoordinates(display, window, root, 0, 0, &root_x, &root_y, &child);
            return CefPoint(root_x, root_y);
        }

        return CefPoint(0, 0);
    }

    void PlatformSetWindowPosition(CefRefPtr<CefBrowser> browser, const CefPoint& position)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XMoveWindow(display, window, position.x, position.y);
        XFlush(display);
    }

    void PlatformWindowRequestFocus(CefRefPtr<CefBrowser> browser)
    {
        ::Display* display = cef_get_xdisplay();
        DCHECK(display);

        ::Window window = browser->GetHost()->GetWindowHandle();
        DCHECK(window != kNullWindowHandle);

        XSetInputFocus(display, window, RevertToParent, CurrentTime);
        XFlush(display);
    }

    std::future<std::vector<std::string>> PlatformPickFiles(bool multiple, const std::vector<std::pair<std::string /* name [Text Files (*.txt)] */, std::string /* *.txt */>>& filters)
    {
        typedef struct _Parameters {
            std::promise<std::vector<std::string>> promise;
            bool multiple;
             std::vector<std::pair<std::string, std::string>> filters;
        } Parameters;

        Parameters* parameters = new Parameters();
        parameters->multiple = multiple;
        parameters->filters = filters;

        //TODO: Remove when crash TODO below is solved
        sleep(1);

        g_main_context_invoke(nullptr, [](gpointer user_data) -> gboolean {
            Parameters* parameters = (Parameters*)user_data;

            std::vector<std::string> files;
            GtkWidget* dialog = gtk_file_chooser_dialog_new(parameters->multiple ? "Select Files" : "Open File", nullptr, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
            if (parameters->multiple) 
                gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);

            for (const auto& filter : parameters->filters) 
            {
                GtkFileFilter* gtkFilter = gtk_file_filter_new();
                gtk_file_filter_set_name(gtkFilter, filter.first.c_str());
                gtk_file_filter_add_pattern(gtkFilter, filter.second.c_str());
                gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), gtkFilter);
            }

            //TODO: Why does it crash here when executed too quickly after startup?
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
            {
                GtkFileChooser* chooser = GTK_FILE_CHOOSER(dialog);
                if (parameters->multiple)
                {
                    GSList* filenames = gtk_file_chooser_get_filenames(chooser);
                    for (GSList* iter = filenames; iter != nullptr; iter = iter->next)
                    {
                        char* filename = static_cast<char*>(iter->data);
                        if (filename) 
                        {
                            std::string result(filename);
                            g_free(filename);
                            files.push_back(result);
                        }
                    }
                    g_slist_free(filenames);
                }
                else
                {
                    char* filename = gtk_file_chooser_get_filename(chooser);
                    if (filename) 
                    {
                        std::string result(filename);
                        g_free(filename);
                        files.push_back(result);
                    }
                }
            }

            gtk_widget_destroy(dialog);
            parameters->promise.set_value(files);
            delete parameters;
            return FALSE;
        }, parameters);
        return parameters->promise.get_future();
    }

    std::future<std::string> PlatformPickDirectory()
    {
        std::promise<std::string>* promise = new std::promise<std::string>();

        //TODO: Remove when crash TODO below is solved
        sleep(1);

        g_main_context_invoke(nullptr, [](gpointer user_data) -> gboolean {
            std::promise<std::string>* promise = (std::promise<std::string>*)user_data;

            GtkWidget* dialog = gtk_file_chooser_dialog_new("Select Directory", nullptr, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, nullptr);
            std::string directoryPath;
            
            //TODO: Why does it crash here when executed too quickly after startup?
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
            {
                GtkFileChooser* chooser = GTK_FILE_CHOOSER(dialog);
                char* directory = gtk_file_chooser_get_filename(chooser);
                directoryPath = std::string(directory);
                g_free(directory);
            }

            gtk_widget_destroy(dialog);
            promise->set_value(directoryPath);
            delete promise;
            return FALSE;
        }, promise);

        return promise->get_future();
    }

    std::future<std::string> PlatformSaveFile(const std::string& default_name, const std::vector<std::pair<std::string /* name [Text Files (*.txt)] */, std::string /* pattern */>>& filters)
    {
        typedef struct _Parameters {
            std::promise<std::string> promise;
            std::string defaultName;
             std::vector<std::pair<std::string, std::string>> filters;
        } Parameters;

        Parameters* parameters = new Parameters();
        parameters->defaultName = default_name;
        parameters->filters = filters;

        //TODO: Remove when crash TODO below is solved
        sleep(1);

        g_main_context_invoke(nullptr, [](gpointer user_data) -> gboolean {
            Parameters* parameters = (Parameters*)user_data;

            std::string file_name;
            GtkWidget* dialog = gtk_file_chooser_dialog_new("Save File", nullptr, GTK_FILE_CHOOSER_ACTION_SAVE, "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, nullptr);
            gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), parameters->defaultName.c_str());
            gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
            for (const auto& filter : parameters->filters)
            {
                GtkFileFilter* gtkFilter = gtk_file_filter_new();
                gtk_file_filter_set_name(gtkFilter, filter.first.c_str());
                gtk_file_filter_add_pattern(gtkFilter, filter.second.c_str());
                gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), gtkFilter);
            }

            //TODO: Why does it crash here when executed too quickly after startup?
            if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
            {
                char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
                if (filename)
                {
                    file_name = std::string(filename);
                    g_free(filename);
                }
            }

            gtk_widget_destroy(dialog);
            parameters->promise.set_value(file_name);
            delete parameters;
            return FALSE;
        }, parameters);
        return parameters->promise.get_future();
    }
}