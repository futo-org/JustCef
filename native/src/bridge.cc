#include "bridge.h"

#include "include/base/cef_logging.h"
#include "include/cef_shared_process_message_builder.h"

#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

constexpr char kBridgeObjectName[] = "bridge";
constexpr char kBridgeFilesObjectName[] = "files";
constexpr char kBridgeRpcObjectName[] = "rpc";
constexpr char kBridgeFilesGetPathMethodName[] = "getPath";
constexpr char kBridgeRpcDispatchMethodName[] = "__dispatchHostCall";
constexpr char kBridgeRpcNativeCallHostMethodName[] = "__nativeCallHost";
constexpr char kBridgeRpcNativeCompleteHostCallMethodName[] = "__nativeCompleteHostCall";
constexpr char kBridgeRpcNativeFailHostCallMethodName[] = "__nativeFailHostCall";
constexpr size_t kBridgeRpcSharedMemoryThreshold = 16 * 1024;

#pragma pack(push, 1)
struct BridgeRpcCallMessageHeader {
    int32_t request_id;
    uint32_t method_size;
    uint32_t payload_size;
};

struct BridgeRpcResultMessageHeader {
    int32_t request_id;
    uint8_t success;
    uint32_t payload_size;
};
#pragma pack(pop)

bool ShouldUseBridgeRpcSharedMemory(size_t payload_size) {
    return payload_size >= kBridgeRpcSharedMemoryThreshold;
}

bool TryBuildBridgeRpcCallSharedMessage(const char* message_name,
                                        int32_t request_id,
                                        const std::string& method,
                                        const std::string& payload_json,
                                        CefRefPtr<CefProcessMessage>& message_out) {
    if (method.size() > std::numeric_limits<uint32_t>::max() ||
        payload_json.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    const size_t message_size =
        sizeof(BridgeRpcCallMessageHeader) + method.size() + payload_json.size();
    auto builder = CefSharedProcessMessageBuilder::Create(message_name, message_size);
    if (!builder || !builder->IsValid()) {
        return false;
    }

    auto* header = static_cast<BridgeRpcCallMessageHeader*>(builder->Memory());
    header->request_id = request_id;
    header->method_size = static_cast<uint32_t>(method.size());
    header->payload_size = static_cast<uint32_t>(payload_json.size());

    uint8_t* cursor = static_cast<uint8_t*>(builder->Memory()) + sizeof(BridgeRpcCallMessageHeader);
    if (!method.empty()) {
        std::memcpy(cursor, method.data(), method.size());
        cursor += method.size();
    }
    if (!payload_json.empty()) {
        std::memcpy(cursor, payload_json.data(), payload_json.size());
    }

    message_out = builder->Build();
    return message_out != nullptr;
}

bool TryBuildBridgeRpcResultSharedMessage(const char* message_name,
                                          int32_t request_id,
                                          bool success,
                                          const std::string& payload,
                                          CefRefPtr<CefProcessMessage>& message_out) {
    if (payload.size() > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    const size_t message_size =
        sizeof(BridgeRpcResultMessageHeader) + payload.size();
    auto builder = CefSharedProcessMessageBuilder::Create(message_name, message_size);
    if (!builder || !builder->IsValid()) {
        return false;
    }

    auto* header = static_cast<BridgeRpcResultMessageHeader*>(builder->Memory());
    header->request_id = request_id;
    header->success = success ? 1 : 0;
    header->payload_size = static_cast<uint32_t>(payload.size());

    uint8_t* cursor = static_cast<uint8_t*>(builder->Memory()) + sizeof(BridgeRpcResultMessageHeader);
    if (!payload.empty()) {
        std::memcpy(cursor, payload.data(), payload.size());
    }

    message_out = builder->Build();
    return message_out != nullptr;
}

constexpr char kBridgeBootstrapScript[] = R"JS(
(function() {
    const bridge = window.bridge;
    const rpc = bridge.rpc;
    const nativeCallHost = rpc.__nativeCallHost;
    const nativeCompleteHostCall = rpc.__nativeCompleteHostCall;
    const nativeFailHostCall = rpc.__nativeFailHostCall;
    const handlers = new Map();

    const toJson = (value) => {
        if (value === undefined) {
            value = null;
        }
        return JSON.stringify(value);
    };

    const fromJson = (json) => JSON.parse(json);

    const describeError = (error) => {
        if (error instanceof Error) {
            return error.message || String(error);
        }
        if (typeof error === "string") {
            return error;
        }
        try {
            return JSON.stringify(error);
        } catch (_) {
            return String(error);
        }
    };

    Object.defineProperties(rpc, {
        call: {
            value(method, payload) {
                if (typeof method !== "string" || method.length === 0) {
                    return Promise.reject(new TypeError("bridge.rpc.call(method, payload) expects a non-empty string method."));
                }

                let payloadJson;
                try {
                    payloadJson = toJson(payload);
                } catch (error) {
                    return Promise.reject(error);
                }

                return nativeCallHost(method, payloadJson).then((resultJson) => fromJson(resultJson));
            },
            enumerable: true,
            writable: false,
            configurable: false
        },
        register: {
            value(method, handler) {
                if (typeof method !== "string" || method.length === 0) {
                    throw new TypeError("bridge.rpc.register(method, handler) expects a non-empty string method.");
                }
                if (typeof handler !== "function") {
                    throw new TypeError("bridge.rpc.register(method, handler) expects a function handler.");
                }
                handlers.set(method, handler);
            },
            enumerable: true,
            writable: false,
            configurable: false
        },
        unregister: {
            value(method) {
                if (typeof method !== "string" || method.length === 0) {
                    throw new TypeError("bridge.rpc.unregister(method) expects a non-empty string method.");
                }
                return handlers.delete(method);
            },
            enumerable: true,
            writable: false,
            configurable: false
        },
        __dispatchHostCall: {
            value(requestId, method, payloadJson) {
                const handler = handlers.get(method);
                if (!handler) {
                    nativeFailHostCall(requestId, `No bridge RPC handler is registered for method "${method}".`);
                    return;
                }

                let payload;
                try {
                    payload = fromJson(payloadJson);
                } catch (error) {
                    nativeFailHostCall(requestId, `Failed to parse bridge RPC payload for method "${method}": ${describeError(error)}`);
                    return;
                }

                Promise.resolve()
                    .then(() => handler(payload))
                    .then(
                        (result) => {
                            let resultJson;
                            try {
                                resultJson = toJson(result);
                            } catch (error) {
                                nativeFailHostCall(requestId, `Failed to serialize bridge RPC result for method "${method}": ${describeError(error)}`);
                                return;
                            }
                            nativeCompleteHostCall(requestId, resultJson);
                        },
                        (error) => {
                            nativeFailHostCall(requestId, describeError(error));
                        });
            },
            enumerable: false,
            writable: false,
            configurable: false
        }
    });
})();
)JS";

struct PendingBridgePromise {
    CefRefPtr<CefV8Context> context;
    CefRefPtr<CefV8Value> promise;
};

struct BrowserBridgeState {
    int32_t next_request_id = 0;
    std::unordered_map<int32_t, PendingBridgePromise> pending_host_calls;
};

std::unordered_map<int, BrowserBridgeState> g_bridge_states;

CefV8Value::PropertyAttribute GetBridgePropertyAttributes() {
    return static_cast<CefV8Value::PropertyAttribute>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
}

CefV8Value::PropertyAttribute GetBridgeInternalPropertyAttributes() {
    return static_cast<CefV8Value::PropertyAttribute>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE | V8_PROPERTY_ATTRIBUTE_DONTENUM);
}

void SetBridgeValue(CefRefPtr<CefV8Value> object, const char* key, CefRefPtr<CefV8Value> value, bool internal_value = false) {
    object->SetValue(key, value, internal_value ? GetBridgeInternalPropertyAttributes() : GetBridgePropertyAttributes());
}

BrowserBridgeState& GetBridgeState(int browser_identifier) {
    return g_bridge_states[browser_identifier];
}

std::string GetV8ExceptionMessage(CefRefPtr<CefV8Value> value, const std::string& fallback) {
    if (!value || !value->HasException()) {
        return fallback;
    }

    CefRefPtr<CefV8Exception> exception = value->GetException();
    if (!exception) {
        return fallback;
    }

    const std::string message = exception->GetMessage();
    return message.empty() ? fallback : message;
}

std::optional<PendingBridgePromise> TakePendingHostCall(int browser_identifier, int32_t request_id) {
    auto browser_it = g_bridge_states.find(browser_identifier);
    if (browser_it == g_bridge_states.end()) {
        return std::nullopt;
    }

    auto request_it = browser_it->second.pending_host_calls.find(request_id);
    if (request_it == browser_it->second.pending_host_calls.end()) {
        return std::nullopt;
    }

    PendingBridgePromise pending = request_it->second;
    browser_it->second.pending_host_calls.erase(request_it);
    return pending;
}

void SendBridgeRpcResult(CefRefPtr<CefFrame> frame, CefProcessId target_process, const char* message_name, int32_t request_id, bool success, const std::string& payload) {
    SendBridgeRpcResultMessage(frame, target_process, message_name, request_id, success, payload);
}

void CompletePendingHostCall(int browser_identifier, int32_t request_id, bool success, const std::string& payload) {
    std::optional<PendingBridgePromise> pending = TakePendingHostCall(browser_identifier, request_id);
    if (!pending || !pending->context || !pending->promise) {
        return;
    }

    if (!pending->context->Enter()) {
        return;
    }

    if (success) {
        pending->promise->ResolvePromise(CefV8Value::CreateString(payload));
    } else {
        pending->promise->RejectPromise(payload);
    }

    pending->context->Exit();
}

void DropPendingHostCallsForContext(int browser_identifier, CefRefPtr<CefV8Context> context) {
    auto browser_it = g_bridge_states.find(browser_identifier);
    if (browser_it == g_bridge_states.end()) {
        return;
    }

    auto& pending_host_calls = browser_it->second.pending_host_calls;
    for (auto it = pending_host_calls.begin(); it != pending_host_calls.end();) {
        if (it->second.context && context && it->second.context->IsSame(context)) {
            it = pending_host_calls.erase(it);
        } else {
            ++it;
        }
    }
}

bool DispatchHostCallToJavascript(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int32_t request_id, const std::string& method, const std::string& payload_json) {
    if (!browser || !frame || !frame->IsMain()) {
        SendBridgeRpcResult(frame, PID_BROWSER, kBridgeRpcCallJsResultMessageName, request_id, false, "Bridge RPC requires the main frame context.");
        return true;
    }

    CefRefPtr<CefV8Context> context = frame->GetV8Context();
    if (!context) {
        SendBridgeRpcResult(frame, PID_BROWSER, kBridgeRpcCallJsResultMessageName, request_id, false, "Bridge RPC main frame context is not available.");
        return true;
    }

    if (!context->Enter()) {
        SendBridgeRpcResult(frame, PID_BROWSER, kBridgeRpcCallJsResultMessageName, request_id, false, "Failed to enter the bridge RPC V8 context.");
        return true;
    }

    std::string error;
    CefRefPtr<CefV8Value> global = context->GetGlobal();
    CefRefPtr<CefV8Value> bridge = global ? global->GetValue(kBridgeObjectName) : nullptr;
    CefRefPtr<CefV8Value> rpc = bridge ? bridge->GetValue(kBridgeRpcObjectName) : nullptr;
    CefRefPtr<CefV8Value> dispatch = rpc ? rpc->GetValue(kBridgeRpcDispatchMethodName) : nullptr;
    if (!dispatch || !dispatch->IsFunction()) {
        error = "Bridge RPC dispatcher is not available in the main frame.";
    } else {
        CefV8ValueList arguments;
        arguments.push_back(CefV8Value::CreateInt(request_id));
        arguments.push_back(CefV8Value::CreateString(method));
        arguments.push_back(CefV8Value::CreateString(payload_json));

        CefRefPtr<CefV8Value> result = dispatch->ExecuteFunctionWithContext(context, rpc, arguments);
        if (!result && dispatch->HasException()) {
            error = GetV8ExceptionMessage(dispatch, "JavaScript bridge RPC dispatch failed.");
        }
    }

    context->Exit();

    if (!error.empty()) {
        SendBridgeRpcResult(frame, PID_BROWSER, kBridgeRpcCallJsResultMessageName, request_id, false, error);
    }

    return true;
}

class BridgeV8Handler final : public CefV8Handler {
 public:
    BridgeV8Handler() = default;

    bool Execute(const CefString& name, CefRefPtr<CefV8Value> object, const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval, CefString& exception) override {
        if (name == kBridgeFilesGetPathMethodName) {
            return ExecuteGetPath(arguments, retval, exception);
        }
        if (name == kBridgeRpcNativeCallHostMethodName) {
            return ExecuteCallHost(arguments, retval, exception);
        }
        if (name == kBridgeRpcNativeCompleteHostCallMethodName) {
            return ExecuteHostCallCompletion(arguments, retval, exception, true);
        }
        if (name == kBridgeRpcNativeFailHostCallMethodName) {
            return ExecuteHostCallCompletion(arguments, retval, exception, false);
        }

        return false;
    }

 private:
    bool ExecuteGetPath(const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval, CefString& exception) {
        if (arguments.size() != 1) {
            exception = "bridge.files.getPath(file) expects exactly one argument.";
            return true;
        }

        const CefRefPtr<CefV8Value>& file_value = arguments[0];
        if (!file_value) {
            retval = CefV8Value::CreateNull();
            return true;
        }

        CefString path = file_value->GetPathForFile();
        if (path.empty()) {
            retval = CefV8Value::CreateNull();
            return true;
        }

        retval = CefV8Value::CreateString(path);
        return true;
    }

    bool ExecuteCallHost(const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval, CefString& exception) {
        if (arguments.size() != 2 || !arguments[0] || !arguments[0]->IsString()) {
            exception = "bridge.rpc.call(method, payload) expects a string method and JSON payload.";
            return true;
        }

        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        CefRefPtr<CefBrowser> browser = context ? context->GetBrowser() : nullptr;
        CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
        if (!context || !browser || !frame || !frame->IsMain()) {
            exception = "bridge.rpc.call(method, payload) is only available in the main frame.";
            return true;
        }

        CefRefPtr<CefV8Value> promise = CefV8Value::CreatePromise();
        if (!promise) {
            exception = "Failed to create bridge RPC promise.";
            return true;
        }

        const std::string payload_json = arguments[1] && arguments[1]->IsString() ? arguments[1]->GetStringValue()  : "null";
        BrowserBridgeState& bridge_state = GetBridgeState(browser->GetIdentifier());
        const int32_t request_id = ++bridge_state.next_request_id;
        bridge_state.pending_host_calls[request_id] = {context, promise};

        SendBridgeRpcCallMessage(
            frame,
            PID_BROWSER,
            kBridgeRpcCallHostMessageName,
            request_id,
            arguments[0]->GetStringValue(),
            payload_json);

        retval = promise;
        return true;
    }

    bool ExecuteHostCallCompletion(const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval, CefString& exception, bool success) {
        if (arguments.size() != 2 || !arguments[0] || !arguments[0]->IsInt()) {
            exception = success
                ? "__nativeCompleteHostCall(requestId, json) expects an integer request id."
                : "__nativeFailHostCall(requestId, error) expects an integer request id.";
            return true;
        }

        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
        if (!frame || !frame->IsMain()) {
            exception = "Bridge RPC host call completion is only available in the main frame.";
            return true;
        }

        const std::string payload = arguments[1] && arguments[1]->IsString() ? arguments[1]->GetStringValue() : (success ? "null" : "Bridge RPC failed.");
        SendBridgeRpcResult(frame, PID_BROWSER, kBridgeRpcCallJsResultMessageName, arguments[0]->GetIntValue(), success, payload);
        retval = CefV8Value::CreateUndefined();
        return true;
    }

    IMPLEMENT_REFCOUNTING(BridgeV8Handler);
    DISALLOW_COPY_AND_ASSIGN(BridgeV8Handler);
};

}    // namespace

bool SendBridgeRpcCallMessage(CefRefPtr<CefFrame> frame, CefProcessId target_process, const char* message_name, int32_t request_id, const std::string& method, const std::string& payload_json) {
    if (!frame) {
        return false;
    }

    const size_t message_size =
        sizeof(BridgeRpcCallMessageHeader) + method.size() + payload_json.size();
    CefRefPtr<CefProcessMessage> message;
    if (ShouldUseBridgeRpcSharedMemory(message_size) &&
        TryBuildBridgeRpcCallSharedMessage(message_name, request_id, method, payload_json, message)) {
        frame->SendProcessMessage(target_process, message);
        return true;
    }

    message = CefProcessMessage::Create(message_name);
    CefRefPtr<CefListValue> arguments = message->GetArgumentList();
    arguments->SetInt(0, request_id);
    arguments->SetString(1, method);
    arguments->SetString(2, payload_json);
    frame->SendProcessMessage(target_process, message);
    return true;
}

bool SendBridgeRpcResultMessage(CefRefPtr<CefFrame> frame, CefProcessId target_process, const char* message_name, int32_t request_id, bool success, const std::string& payload) {
    if (!frame) {
        return false;
    }

    const size_t message_size =
        sizeof(BridgeRpcResultMessageHeader) + payload.size();
    CefRefPtr<CefProcessMessage> message;
    if (ShouldUseBridgeRpcSharedMemory(message_size) &&
        TryBuildBridgeRpcResultSharedMessage(message_name, request_id, success, payload, message)) {
        frame->SendProcessMessage(target_process, message);
        return true;
    }

    message = CefProcessMessage::Create(message_name);
    CefRefPtr<CefListValue> arguments = message->GetArgumentList();
    arguments->SetInt(0, request_id);
    arguments->SetBool(1, success);
    arguments->SetString(2, payload);
    frame->SendProcessMessage(target_process, message);
    return true;
}

bool ParseBridgeRpcCallMessage(CefRefPtr<CefProcessMessage> message, int32_t& request_id, std::string& method, std::string& payload_json) {
    if (!message) {
        return false;
    }

    if (auto arguments = message->GetArgumentList()) {
        if (arguments->GetSize() < 3 ||
            arguments->GetType(0) != VTYPE_INT ||
            arguments->GetType(1) != VTYPE_STRING ||
            arguments->GetType(2) != VTYPE_STRING) {
            return false;
        }

        request_id = arguments->GetInt(0);
        method = arguments->GetString(1);
        payload_json = arguments->GetString(2);
        return true;
    }

    auto region = message->GetSharedMemoryRegion();
    if (!region || !region->IsValid() || region->Size() < sizeof(BridgeRpcCallMessageHeader)) {
        return false;
    }

    const auto* header = static_cast<const BridgeRpcCallMessageHeader*>(region->Memory());
    const size_t expected_size =
        sizeof(BridgeRpcCallMessageHeader) +
        static_cast<size_t>(header->method_size) +
        static_cast<size_t>(header->payload_size);
    if (region->Size() < expected_size) {
        return false;
    }

    const char* cursor = static_cast<const char*>(region->Memory()) + sizeof(BridgeRpcCallMessageHeader);
    request_id = header->request_id;
    method.assign(cursor, header->method_size);
    cursor += header->method_size;
    payload_json.assign(cursor, header->payload_size);
    return true;
}

bool ParseBridgeRpcResultMessage(CefRefPtr<CefProcessMessage> message, int32_t& request_id, bool& success, std::string& payload) {
    if (!message) {
        return false;
    }

    if (auto arguments = message->GetArgumentList()) {
        if (arguments->GetSize() < 3 ||
            arguments->GetType(0) != VTYPE_INT ||
            arguments->GetType(1) != VTYPE_BOOL ||
            arguments->GetType(2) != VTYPE_STRING) {
            return false;
        }

        request_id = arguments->GetInt(0);
        success = arguments->GetBool(1);
        payload = arguments->GetString(2);
        return true;
    }

    auto region = message->GetSharedMemoryRegion();
    if (!region || !region->IsValid() || region->Size() < sizeof(BridgeRpcResultMessageHeader)) {
        return false;
    }

    const auto* header = static_cast<const BridgeRpcResultMessageHeader*>(region->Memory());
    const size_t expected_size =
        sizeof(BridgeRpcResultMessageHeader) +
        static_cast<size_t>(header->payload_size);
    if (region->Size() < expected_size) {
        return false;
    }

    const char* cursor = static_cast<const char*>(region->Memory()) + sizeof(BridgeRpcResultMessageHeader);
    request_id = header->request_id;
    success = header->success != 0;
    payload.assign(cursor, header->payload_size);
    return true;
}

void InstallBridge(CefRefPtr<CefV8Context> context) {
    if (!context) {
        return;
    }

    CefRefPtr<CefV8Value> window = context->GetGlobal();
    if (!window) {
        return;
    }

    CefRefPtr<CefV8Value> bridge = CefV8Value::CreateObject(nullptr, nullptr);
    CefRefPtr<CefV8Value> files = CefV8Value::CreateObject(nullptr, nullptr);
    CefRefPtr<CefV8Value> rpc = CefV8Value::CreateObject(nullptr, nullptr);
    CefRefPtr<CefV8Handler> handler = new BridgeV8Handler();

    SetBridgeValue(files, kBridgeFilesGetPathMethodName, CefV8Value::CreateFunction(kBridgeFilesGetPathMethodName, handler));
    SetBridgeValue(rpc, kBridgeRpcNativeCallHostMethodName, CefV8Value::CreateFunction(kBridgeRpcNativeCallHostMethodName, handler), true);
    SetBridgeValue(rpc, kBridgeRpcNativeCompleteHostCallMethodName, CefV8Value::CreateFunction(kBridgeRpcNativeCompleteHostCallMethodName, handler), true);
    SetBridgeValue(rpc, kBridgeRpcNativeFailHostCallMethodName, CefV8Value::CreateFunction(kBridgeRpcNativeFailHostCallMethodName, handler), true);
    SetBridgeValue(bridge, kBridgeFilesObjectName, files);
    SetBridgeValue(bridge, kBridgeRpcObjectName, rpc);
    SetBridgeValue(window, kBridgeObjectName, bridge);

    CefRefPtr<CefV8Value> eval_result;
    CefRefPtr<CefV8Exception> eval_exception;
    if (!context->Eval(kBridgeBootstrapScript, "justcef://bridge/bootstrap.js", 1, eval_result, eval_exception)) {
        const std::string message = eval_exception ? eval_exception->GetMessage() : "Unknown bridge bootstrap error.";
        LOG(ERROR) << "Failed to bootstrap bridge RPC runtime: " << message;
    }
}

CefRefPtr<CefDictionaryValue> CreateBridgeExtraInfo(bool bridge_enabled, CefRefPtr<CefDictionaryValue> base_info) {
    if (!bridge_enabled && !base_info) {
        return nullptr;
    }

    CefRefPtr<CefDictionaryValue> extra_info = base_info ? base_info->Copy(false) : CefDictionaryValue::Create();
    if (!extra_info) {
        return nullptr;
    }

    if (bridge_enabled) {
        extra_info->SetBool(kBridgeEnabledExtraInfoKey, true);
        return extra_info;
    }

    extra_info->Remove(kBridgeEnabledExtraInfoKey);
    if (extra_info->GetSize() == 0) {
        return nullptr;
    }

    return extra_info;
}

bool IsBridgeEnabled(CefRefPtr<CefDictionaryValue> extra_info) {
    return extra_info && extra_info->HasKey(kBridgeEnabledExtraInfoKey) && extra_info->GetBool(kBridgeEnabledExtraInfoKey);
}

bool HandleBridgeProcessMessage(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message) {
    if (!browser || !message) {
        return false;
    }

    const std::string message_name = message->GetName();
    if (message_name == kBridgeRpcCallHostResultMessageName) {
        int32_t request_id = 0;
        bool success = false;
        std::string payload;
        if (!ParseBridgeRpcResultMessage(message, request_id, success, payload)) {
            return true;
        }

        CompletePendingHostCall(browser->GetIdentifier(), request_id, success, payload);
        return true;
    }

    if (message_name == kBridgeRpcCallJsMessageName) {
        int32_t request_id = 0;
        std::string method;
        std::string payload_json;
        if (!ParseBridgeRpcCallMessage(message, request_id, method, payload_json)) {
            return true;
        }

        return DispatchHostCallToJavascript(browser, frame, request_id, method, payload_json);
    }

    return false;
}

void ReleaseBridgeContext(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context) {
    if (!browser || !frame || !frame->IsMain()) {
        return;
    }

    DropPendingHostCallsForContext(browser->GetIdentifier(), context);

    CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kBridgeRpcContextReleasedMessageName);
    frame->SendProcessMessage(PID_BROWSER, message);
}

void ClearBridgeState(CefRefPtr<CefBrowser> browser) {
    if (!browser) {
        return;
    }

    g_bridge_states.erase(browser->GetIdentifier());
}
