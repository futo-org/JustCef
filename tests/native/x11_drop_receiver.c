#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Display* display;
static Atom atom(const char* name) { return XInternAtom(display, name, False); }
static void send_message(Window window, Atom type, long a, long b, long c) {
    XEvent event = {0};
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = window;
    event.xclient.message_type = type;
    event.xclient.format = 32;
    event.xclient.data.l[0] = a;
    event.xclient.data.l[1] = b;
    event.xclient.data.l[2] = c;
    XSendEvent(display, window, False, NoEventMask, &event);
    XFlush(display);
}
int main(void) {
    display = XOpenDisplay(NULL);
    if (!display) return 2;
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 1000, 30, 850, 700, 0, 0, 0x208040);
    unsigned long version = 5, pid = getpid();
    XChangeProperty(display, window, atom("XdndAware"), XA_ATOM, 32, PropModeReplace, (unsigned char*)&version, 1);
    XChangeProperty(display, window, atom("_NET_WM_PID"), XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&pid, 1);
    XStoreName(display, window, "JustCef native drop receiver");
    XSelectInput(display, window, StructureNotifyMask | PropertyChangeMask);
    XMapWindow(display, window);
    XFlush(display);
    printf("READY %lu\n", window); fflush(stdout);
    Window source = None;
    Atom selected = None;
    for (;;) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == ClientMessage) {
            XClientMessageEvent* m = &event.xclient;
            if (m->message_type == atom("XdndEnter")) {
                source = m->data.l[0]; selected = None;
                Atom inline_types[3] = {m->data.l[2], m->data.l[3], m->data.l[4]};
                Atom* types = inline_types;
                unsigned char* property = NULL;
                unsigned long count = 3, remaining;
                Atom actual; int format;
                if (m->data.l[1] & 1) {
                    XGetWindowProperty(display, source, atom("XdndTypeList"), 0, 4096, False, XA_ATOM, &actual, &format, &count, &remaining, &property);
                    types = (Atom*)property;
                }
                for (unsigned long i = 0; types && i < count; ++i) {
                    if (types[i] == atom("text/uri-list")) { selected = types[i]; break; }
                    if (types[i] == atom("text/plain") || types[i] == atom("UTF8_STRING")) selected = types[i];
                }
                if (property) XFree(property);
            } else if (m->message_type == atom("XdndPosition")) {
                XEvent status = {0};
                status.xclient.type = ClientMessage; status.xclient.display = display;
                status.xclient.window = source; status.xclient.message_type = atom("XdndStatus"); status.xclient.format = 32;
                status.xclient.data.l[0] = window; status.xclient.data.l[1] = selected ? 3 : 0;
                status.xclient.data.l[4] = atom("XdndActionCopy");
                XSendEvent(display, source, False, NoEventMask, &status); XFlush(display);
            } else if (m->message_type == atom("XdndDrop")) {
                XConvertSelection(display, atom("XdndSelection"), selected, atom("JustCefDropData"), window, m->data.l[2]);
                XFlush(display);
            }
        } else if (event.type == SelectionNotify && event.xselection.property != None) {
            Atom actual; int format; unsigned long count, remaining; unsigned char* data = NULL;
            XGetWindowProperty(display, window, event.xselection.property, 0, 1024*1024, True, AnyPropertyType, &actual, &format, &count, &remaining, &data);
            if (format != 8 || remaining) return 3;
            char* name = XGetAtomName(display, selected);
            printf("DROP %s ", name);
            for (unsigned long i = 0; i < count; ++i) printf("%02x", data[i]);
            printf("\n"); fflush(stdout);
            XFree(name); if (data) XFree(data);
            send_message(source, atom("XdndFinished"), window, 1, atom("XdndActionCopy"));
        }
    }
}
