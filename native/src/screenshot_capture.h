#ifndef JUSTCEF_SCREENSHOT_CAPTURE_H_
#define JUSTCEF_SCREENSHOT_CAPTURE_H_
#include "include/cef_browser.h"
#include "include/cef_process_message.h"
#include "include/cef_values.h"
#include <functional>
#include <string>

std::function<void()> CaptureBrowserScreenshot(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDictionaryValue> options, std::function<void(bool, std::string)> callback);
#endif
