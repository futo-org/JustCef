#ifndef JUSTCEF_BRIDGE_H_
#define JUSTCEF_BRIDGE_H_

#include "include/cef_values.h"
#include "include/cef_v8.h"

constexpr char kBridgeEnabledExtraInfoKey[] = "bridgeEnabled";

CefRefPtr<CefDictionaryValue> CreateBridgeExtraInfo(bool bridge_enabled, CefRefPtr<CefDictionaryValue> base_info = nullptr);
bool IsBridgeEnabled(CefRefPtr<CefDictionaryValue> extra_info);
void InstallBridge(CefRefPtr<CefV8Context> context);

#endif  // JUSTCEF_BRIDGE_H_
