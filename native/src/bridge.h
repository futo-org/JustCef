#ifndef JUSTCEF_BRIDGE_H_
#define JUSTCEF_BRIDGE_H_

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_process_message.h"
#include "include/cef_values.h"
#include "include/cef_v8.h"

constexpr char kBridgeEnabledExtraInfoKey[] = "bridgeEnabled";
constexpr char kBridgeRpcCallHostMessageName[] = "JustCef.BridgeRpc.CallHost";
constexpr char kBridgeRpcCallHostResultMessageName[] = "JustCef.BridgeRpc.CallHostResult";
constexpr char kBridgeRpcCallJsMessageName[] = "JustCef.BridgeRpc.CallJs";
constexpr char kBridgeRpcCallJsResultMessageName[] = "JustCef.BridgeRpc.CallJsResult";
constexpr char kBridgeRpcContextReleasedMessageName[] = "JustCef.BridgeRpc.ContextReleased";

CefRefPtr<CefDictionaryValue> CreateBridgeExtraInfo(bool bridge_enabled, CefRefPtr<CefDictionaryValue> base_info = nullptr);
bool IsBridgeEnabled(CefRefPtr<CefDictionaryValue> extra_info);
void InstallBridge(CefRefPtr<CefV8Context> context);
bool HandleBridgeProcessMessage(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message);
void ReleaseBridgeContext(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context);
void ClearBridgeState(CefRefPtr<CefBrowser> browser);

#endif  // JUSTCEF_BRIDGE_H_
