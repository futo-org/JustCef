#include "bridge.h"

namespace {

constexpr char kBridgeObjectName[] = "bridge";
constexpr char kBridgeFilesObjectName[] = "files";
constexpr char kBridgeFilesGetPathMethodName[] = "getPath";

CefV8Value::PropertyAttribute GetBridgePropertyAttributes() {
  return static_cast<CefV8Value::PropertyAttribute>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
}

void SetBridgeValue(CefRefPtr<CefV8Value> object, const char* key, CefRefPtr<CefV8Value> value) {
  object->SetValue(key, value, GetBridgePropertyAttributes());
}

class BridgeV8Handler final : public CefV8Handler {
 public:
  bool Execute(const CefString& name, CefRefPtr<CefV8Value> object, const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval, CefString& exception) override {
    if (name != kBridgeFilesGetPathMethodName) {
      return false;
    }

    if (arguments.size() != 1) {
      exception = "bridge.files.getPath(file) expects exactly one argument.";
      return true;
    }

    const CefRefPtr<CefV8Value>& file_value = arguments[0];
    if (!file_value) {
      retval = CefV8Value::CreateNull();
      return true;
    }

    const CefString path = file_value->GetPathForFile();
    if (path.empty()) {
      retval = CefV8Value::CreateNull();
      return true;
    }

    retval = CefV8Value::CreateString(path);
    return true;
  }

 private:
  IMPLEMENT_REFCOUNTING(BridgeV8Handler);
  DISALLOW_COPY_AND_ASSIGN(BridgeV8Handler);
};

}  // namespace

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
  CefRefPtr<CefV8Handler> handler = new BridgeV8Handler();

  SetBridgeValue(files, kBridgeFilesGetPathMethodName, CefV8Value::CreateFunction(kBridgeFilesGetPathMethodName, handler));
  SetBridgeValue(bridge, kBridgeFilesObjectName, files);
  SetBridgeValue(window, kBridgeObjectName, bridge);
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
