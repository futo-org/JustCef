#include "justcef_view_renderer.h"
#include "include/cef_parser.h"

#include "justcef_view_common.h"

#include "include/base/cef_logging.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

constexpr char kViewBootstrapScript[] = R"JS((function (native) {
  'use strict';
  const elements = new Map();
  const internals = new WeakMap();
  let nextId = 0;
  const sheet = new CSSStyleSheet();
  sheet.replaceSync(':host{display:block;position:relative}:host(:fullscreen){width:100%;height:100%}embed{display:block;position:absolute;inset:0;width:100%;height:100%;border:0}');

  function background(el) {
    const match = /rgba?\(([^)]+)\)/.exec(getComputedStyle(el).backgroundColor || '');
    if (!match) return 0;
    const parts = match[1].split(/[ ,\/]+/).filter(Boolean).map(Number);
    if (parts.length > 3 && !(parts[3] > 0)) return 0;
    return ((255 << 24) | ((parts[0] & 255) << 16) | ((parts[1] & 255) << 8) | (parts[2] & 255)) | 0;
  }

  class JustCefViewElement extends HTMLElement {
    static get observedAttributes() { return ['src']; }
    constructor() {
      super();
      const root = this.attachShadow({ mode: 'closed' });
      root.adoptedStyleSheets = [sheet];
      root.append(document.createElement('slot'));
      const el = this;
      const state = {
        id: 0, created: false, viewId: null, frame: null, destroyTimer: 0, suspended: false, fullscreen: false, fullscreenRequest: 0,
        create() {
          if (this.created || !el.isConnected || !el.getAttribute('src')) return;
          let url;
          try { url = new URL(el.getAttribute('src'), document.baseURI).href; }
          catch (_) {
            el.dispatchEvent(new CustomEvent('error', { detail: { reason: 'denied' } }));
            return;
          }
          elements.delete(this.id);
          this.id = ++nextId;
          elements.set(this.id, el);
          this.created = true;
          native.create(this.id, url, background(el));
        },
        destroy() {
          clearTimeout(this.destroyTimer);
          this.destroyTimer = 0;
          if (this.created) native.destroy(this.id);
          this.created = false;
          this.fullscreen = false;
          ++this.fullscreenRequest;
          if (document.fullscreenElement === el) document.exitFullscreen().catch(() => {});
          this.viewId = null;
          if (this.frame) this.frame.remove();
          this.frame = null;
        },
        event(name, encoded) {
          const id = this.id;
          let detail;
          try { detail = encoded ? JSON.parse(encoded) : null; } catch (_) { detail = null; }
          if (name === 'fullscreenchange') {
            const request = ++this.fullscreenRequest;
            this.fullscreen = !!detail?.fullscreen;
            if (this.fullscreen && document.fullscreenElement !== el) {
              el.requestFullscreen().catch(() => {
                if (this.created && this.id === id && this.fullscreen && this.fullscreenRequest === request) native.command(id, 'exitFullscreen');
              });
            } else if (!this.fullscreen && document.fullscreenElement === el) {
              document.exitFullscreen().catch(() => {});
            }
          }
          if (name === 'viewcreated') {
            if (!detail || !detail.guestContentId) { this.destroy(); return; }
            if (this.frame) this.frame.remove();
            const frame = document.createElement('embed');
            frame.type = 'application/x-chromium-surface-embed';
            frame.setAttribute('data-content-id', detail.guestContentId);
            frame.setAttribute('title', el.getAttribute('title') || 'Embedded browser');
            frame.style.visibility = this.suspended ? 'hidden' : '';
            this.frame = frame;
            this.viewId = detail.viewId;
            root.prepend(frame);
            detail = { viewId: this.viewId };
          } else if (name === 'error' && this.viewId === null) {
            this.destroy();
          } else if (name === 'close') {
            if (this.viewId === null || (detail && detail.viewId !== this.viewId)) return;
            this.created = false;
            this.destroy();
            detail = null;
          }
          const decision = ['permissionrequest', 'certificaterequest', 'downloadrequest', 'newwindowrequest'].includes(name);
          const handled = !el.dispatchEvent(new CustomEvent(name, { detail, cancelable: decision }));
          if (decision && !handled && detail && this.created && this.id === id) native.command(id, 'respond', JSON.stringify({ requestId: detail.requestId, allow: false }));
        }
      };
      internals.set(this, state);
      this.addEventListener('focus', () => { if (state.created) native.command(state.id, 'focus'); });
    }
    get viewId() { return internals.get(this).viewId; }
    reload() { this._command('reload'); }
    goBack() { this._command('goBack'); }
    goForward() { this._command('goForward'); }
    focus(options) {
      HTMLElement.prototype.focus.call(this, options);
      this._command('focus');
    }
    suspend() {
      const state = internals.get(this);
      state.suspended = true;
      if (state.frame) state.frame.style.visibility = 'hidden';
    }
    resume() {
      const state = internals.get(this);
      state.suspended = false;
      if (state.frame) state.frame.style.visibility = '';
    }
    respond(requestId, decision) {
      const state = internals.get(this);
      if (state.created) native.command(state.id, 'respond', JSON.stringify({ ...decision, requestId }));
    }
    _command(command) {
      const state = internals.get(this);
      if (state.created) native.command(state.id, command);
    }
    connectedCallback() {
      const state = internals.get(this);
      clearTimeout(state.destroyTimer);
      state.destroyTimer = 0;
      state.create();
    }
    disconnectedCallback() {
      const state = internals.get(this);
      clearTimeout(state.destroyTimer);
      state.destroyTimer = setTimeout(() => {
        if (this.isConnected) return;
        state.destroy();
        elements.delete(state.id);
      }, 1000);
    }
    attributeChangedCallback(name, oldValue, newValue) {
      if (oldValue === newValue) return;
      const state = internals.get(this);
      if (!newValue) { state.destroy(); return; }
      if (!this.isConnected) return;
      if (!state.created) { state.create(); return; }
      try { native.navigate(state.id, new URL(newValue, document.baseURI).href); }
      catch (_) { this.dispatchEvent(new CustomEvent('error', { detail: { reason: 'denied' } })); }
    }
  }

  document.addEventListener('fullscreenchange', () => {
    for (const el of elements.values()) {
      const state = internals.get(el);
      if (state.created && state.fullscreen && document.fullscreenElement !== el) native.command(state.id, 'exitFullscreen');
    }
  });
  addEventListener('pagehide', event => {
    if (event.persisted) for (const el of elements.values()) internals.get(el).destroy();
  });
  addEventListener('pageshow', event => {
    if (event.persisted) for (const el of Array.from(elements.values())) internals.get(el).create();
  });
  if (!customElements.get('justcef-view')) customElements.define('justcef-view', JustCefViewElement);
  return function dispatch(kind, id, name, detail) {
    const el = elements.get(id);
    if (el && kind === 'event' && internals.get(el).created) internals.get(el).event(name, detail);
  };
}))JS";

struct ViewContextState
{
    CefRefPtr<CefV8Context> context;
    CefRefPtr<CefV8Value> dispatch;
    std::string token;
};

std::unordered_map<int, std::vector<ViewContextState>> g_view_contexts;

uint64_t SplitMix64(uint64_t value)
{
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

std::string CreateDocumentToken()
{
    static const uint64_t seed = SplitMix64(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                                            static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()));
    static uint64_t counter = 0;
    static const char* digits = "0123456789abcdef";
    uint64_t value = SplitMix64(seed + ++counter);
    std::string token(16, '0');
    for (int i = 15; i >= 0; --i)
    {
        token[i] = digits[value & 0xF];
        value >>= 4;
    }
    return token;
}

ViewContextState* FindStateForContext(CefRefPtr<CefBrowser> browser, CefRefPtr<CefV8Context> context)
{
    if (!browser || !context)
        return nullptr;

    auto it = g_view_contexts.find(browser->GetIdentifier());
    if (it == g_view_contexts.end())
        return nullptr;

    for (auto& state : it->second)
    {
        if (state.context && state.context->IsSame(context))
            return &state;
    }
    return nullptr;
}

ViewContextState* FindStateForToken(CefRefPtr<CefBrowser> browser, const std::string& token)
{
    if (!browser)
        return nullptr;

    auto it = g_view_contexts.find(browser->GetIdentifier());
    if (it == g_view_contexts.end())
        return nullptr;

    for (auto& state : it->second)
    {
        if (state.token == token)
            return &state;
    }
    return nullptr;
}

class ViewV8Handler final : public CefV8Handler
{
public:
    ViewV8Handler() = default;

    bool Execute(const CefString& name, CefRefPtr<CefV8Value> object, const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval, CefString& exception) override
    {
        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        CefRefPtr<CefBrowser> browser = context ? context->GetBrowser() : nullptr;
        CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
        ViewContextState* state = FindStateForContext(browser, context);
        if (!state || !frame || !frame->IsMain())
        {
            retval = CefV8Value::CreateUndefined();
            return true;
        }

        if (arguments.empty() || !arguments[0]->IsInt())
        {
            exception = "Invalid view element identifier.";
            return true;
        }

        const int32_t elementId = arguments[0]->GetIntValue();
        CefRefPtr<CefProcessMessage> message;

        if (name == "create")
        {
            if (arguments.size() < 3 || !arguments[1]->IsString())
            {
                exception = "create(id, src, background) expects a string source.";
                return true;
            }

            message = CefProcessMessage::Create(kViewCreateMessageName);
            CefRefPtr<CefListValue> args = message->GetArgumentList();
            args->SetString(0, state->token);
            args->SetInt(1, elementId);
            args->SetString(2, arguments[1]->GetStringValue());
            args->SetInt(3, arguments[2]->IsInt() ? arguments[2]->GetIntValue() : 0);
        }
        else if (name == "navigate" || name == "command")
        {
            if (arguments.size() < 2 || !arguments[1]->IsString())
            {
                exception = "Expected a string argument.";
                return true;
            }

            message = CefProcessMessage::Create(name == "navigate" ? kViewNavigateMessageName : kViewCommandMessageName);
            CefRefPtr<CefListValue> args = message->GetArgumentList();
            args->SetString(0, state->token);
            args->SetInt(1, elementId);
            args->SetString(2, arguments[1]->GetStringValue());
            if (name == "command" && arguments.size() > 2 && arguments[2]->IsString())
            {
                auto value = CefParseJSON(arguments[2]->GetStringValue(), JSON_PARSER_RFC);
                if (value && value->GetType() == VTYPE_DICTIONARY)
                    args->SetDictionary(3, value->GetDictionary());
            }
        }
        else if (name == "destroy")
        {
            message = CefProcessMessage::Create(kViewDestroyMessageName);
            CefRefPtr<CefListValue> args = message->GetArgumentList();
            args->SetString(0, state->token);
            args->SetInt(1, elementId);
        }
        else
        {
            return false;
        }

        frame->SendProcessMessage(PID_BROWSER, message);
        retval = CefV8Value::CreateUndefined();
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(ViewV8Handler);
    DISALLOW_COPY_AND_ASSIGN(ViewV8Handler);
};

void Dispatch(CefRefPtr<CefBrowser> browser, const std::string& token, const std::string& kind, int32_t elementId, CefRefPtr<CefV8Value> a, CefRefPtr<CefV8Value> b)
{
    ViewContextState* state = FindStateForToken(browser, token);
    if (!state || !state->context || !state->dispatch)
    {
        VLOG(1) << "Dropped view message " << kind << " for element " << elementId << " (token = " << token << ").";
        return;
    }

    CefRefPtr<CefV8Context> context = state->context;
    CefRefPtr<CefV8Value> dispatch = state->dispatch;
    if (!context->Enter())
        return;

    CefV8ValueList arguments;
    arguments.push_back(CefV8Value::CreateString(kind));
    arguments.push_back(CefV8Value::CreateInt(elementId));
    arguments.push_back(a ? a : CefV8Value::CreateUndefined());
    arguments.push_back(b ? b : CefV8Value::CreateUndefined());
    dispatch->ExecuteFunction(nullptr, arguments);
    if (dispatch->HasException())
    {
        CefRefPtr<CefV8Exception> error = dispatch->GetException();
        LOG(ERROR) << "Failed to dispatch justcef-view event: " << (error ? error->GetMessage().ToString() : "unknown error");
        dispatch->ClearException();
    }

    context->Exit();
}

}

void InstallViewElement(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
    if (!browser || !frame || !frame->IsMain() || !context)
        return;

    CefRefPtr<CefV8Value> factory;
    CefRefPtr<CefV8Exception> exception;
    if (!context->Eval(kViewBootstrapScript, "justcef://view/bootstrap.js", 1, factory, exception) || !factory || !factory->IsFunction())
    {
        LOG(ERROR) << "Failed to bootstrap justcef-view: " << (exception ? exception->GetMessage().ToString() : "unknown error");
        return;
    }

    CefRefPtr<CefV8Handler> handler = new ViewV8Handler();
    CefRefPtr<CefV8Value> native = CefV8Value::CreateObject(nullptr, nullptr);
    const auto attributes = static_cast<CefV8Value::PropertyAttribute>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
    for (const char* name : {"create", "navigate", "command", "destroy"})
        native->SetValue(name, CefV8Value::CreateFunction(name, handler), attributes);

    ViewContextState state;
    state.context = context;
    state.token = CreateDocumentToken();
    g_view_contexts[browser->GetIdentifier()].push_back(state);

    if (!context->Enter())
        return;

    CefV8ValueList arguments;
    arguments.push_back(native);
    CefRefPtr<CefV8Value> dispatch = factory->ExecuteFunction(nullptr, arguments);
    if (!dispatch || !dispatch->IsFunction())
    {
        CefRefPtr<CefV8Exception> error = factory->HasException() ? factory->GetException() : nullptr;
        LOG(ERROR) << "Failed to initialize justcef-view: " << (error ? error->GetMessage().ToString() : "unknown error");
        if (factory->HasException())
            factory->ClearException();
    }
    else if (ViewContextState* stored = FindStateForContext(browser, context))
    {
        stored->dispatch = dispatch;
    }

    context->Exit();
}

void ReleaseViewContext(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
    if (!browser || !frame || !frame->IsMain())
        return;

    auto it = g_view_contexts.find(browser->GetIdentifier());
    if (it == g_view_contexts.end())
        return;

    auto& states = it->second;
    for (auto state = states.begin(); state != states.end(); ++state)
    {
        if (state->context && context && state->context->IsSame(context))
        {
            CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kViewContextReleasedMessageName);
            message->GetArgumentList()->SetString(0, state->token);
            frame->SendProcessMessage(PID_BROWSER, message);
            states.erase(state);
            break;
        }
    }

    if (states.empty())
        g_view_contexts.erase(it);
}

bool HandleViewProcessMessage(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message)
{
    const std::string name = message->GetName();
    if (name.rfind(kViewMessagePrefix, 0) != 0)
        return false;

    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (!args || args->GetSize() < 3 || args->GetType(0) != VTYPE_STRING || args->GetType(1) != VTYPE_INT)
        return true;

    const std::string token = args->GetString(0);
    const int32_t elementId = args->GetInt(1);

    if (name == kViewEventMessageName && args->GetSize() >= 4)
    {
        Dispatch(browser, token, "event", elementId, CefV8Value::CreateString(args->GetString(2)), CefV8Value::CreateString(args->GetString(3)));
        return true;
    }

    return true;
}

void ClearViewState(CefRefPtr<CefBrowser> browser)
{
    if (!browser)
        return;

    g_view_contexts.erase(browser->GetIdentifier());
}
