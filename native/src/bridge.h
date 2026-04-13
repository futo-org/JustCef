#ifndef JUSTCEF_BRIDGE_H_
#define JUSTCEF_BRIDGE_H_

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_process_message.h"
#include "include/cef_values.h"
#include "include/cef_v8.h"

#include <string>

constexpr char kBridgeEnabledExtraInfoKey[] = "bridgeEnabled";
constexpr char kBridgeRpcCallHostMessageName[] = "JustCef.BridgeRpc.CallHost";
constexpr char kBridgeRpcCallHostResultMessageName[] = "JustCef.BridgeRpc.CallHostResult";
constexpr char kBridgeRpcCallJsMessageName[] = "JustCef.BridgeRpc.CallJs";
constexpr char kBridgeRpcCallJsResultMessageName[] = "JustCef.BridgeRpc.CallJsResult";
constexpr char kBridgeRpcContextReleasedMessageName[] = "JustCef.BridgeRpc.ContextReleased";

CefRefPtr<CefDictionaryValue> CreateBridgeExtraInfo(bool bridge_enabled, CefRefPtr<CefDictionaryValue> base_info = nullptr);
bool IsBridgeEnabled(CefRefPtr<CefDictionaryValue> extra_info);
void InstallBridge(CefRefPtr<CefV8Context> context);
bool SendBridgeRpcCallMessage(CefRefPtr<CefFrame> frame, CefProcessId target_process, const char* message_name, int32_t request_id, const std::string& method, const std::string& payload_json);
bool SendBridgeRpcResultMessage(CefRefPtr<CefFrame> frame, CefProcessId target_process, const char* message_name, int32_t request_id, bool success, const std::string& payload);
bool ParseBridgeRpcCallMessage(CefRefPtr<CefProcessMessage> message, int32_t& request_id, std::string& method, std::string& payload_json);
bool ParseBridgeRpcResultMessage(CefRefPtr<CefProcessMessage> message, int32_t& request_id, bool& success, std::string& payload);
bool HandleBridgeProcessMessage(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message);
void ReleaseBridgeContext(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context);
void ClearBridgeState(CefRefPtr<CefBrowser> browser);

#endif  // JUSTCEF_BRIDGE_H_
