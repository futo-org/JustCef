#ifndef JUSTCEF_VIEW_RENDERER_H_
#define JUSTCEF_VIEW_RENDERER_H_

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"

void InstallViewElement(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context);
void InstallViewContentScript(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context);
void ReleaseViewContentContext(CefRefPtr<CefBrowser> browser, CefRefPtr<CefV8Context> context);
bool IsViewContent(CefRefPtr<CefDictionaryValue> extra_info);
void ReleaseViewContext(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context);
bool HandleViewProcessMessage(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message);
void ClearViewState(CefRefPtr<CefBrowser> browser);

#endif // JUSTCEF_VIEW_RENDERER_H_
