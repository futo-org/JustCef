// Copyright (c) 2013 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "simple_handler.h"

#include <sstream>
#include <string>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_parser.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

namespace {

SimpleHandler* g_instance = nullptr;

// Returns a data: URI with the specified contents.
std::string GetDataURI(const std::string& data, const std::string& mime_type) {
  return "data:" + mime_type + ";base64," +
         CefURIEncode(CefBase64Encode(data.data(), data.size()), false)
             .ToString();
}

}  // namespace

SimpleHandler::SimpleHandler(bool is_alloy_style)
    : is_alloy_style_(is_alloy_style) {
  DCHECK(!g_instance);
  g_instance = this;
}

SimpleHandler::~SimpleHandler() {
  g_instance = nullptr;
}

// static
SimpleHandler* SimpleHandler::GetInstance() {
  return g_instance;
}

void SimpleHandler::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                  const CefString& title) {
  CEF_REQUIRE_UI_THREAD();

  if (auto browser_view = CefBrowserView::GetForBrowser(browser)) {
    // Set the title of the window using the Views framework.
    CefRefPtr<CefWindow> window = browser_view->GetWindow();
    if (window) {
      window->SetTitle(title);
    }
  }
}

void SimpleHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();

  CHECK_EQ(is_alloy_style_ ? CEF_RUNTIME_STYLE_ALLOY : CEF_RUNTIME_STYLE_CHROME, browser->GetHost()->GetRuntimeStyle());
  browser_list_.push_back(browser);
}

bool SimpleHandler::DoClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();

  if (browser_list_.size() == 1) {
    is_closing_ = true;
  }

  return false;
}

void SimpleHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();

  BrowserList::iterator bit = browser_list_.begin();
  for (; bit != browser_list_.end(); ++bit) {
    if ((*bit)->IsSame(browser)) {
      browser_list_.erase(bit);
      break;
    }
  }

  if (browser_list_.empty()) {
    CefQuitMessageLoop();
  }
}

void SimpleHandler::OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, ErrorCode errorCode, const CefString& errorText, const CefString& failedUrl) {
  CEF_REQUIRE_UI_THREAD();

  if (!is_alloy_style_) {
    return;
  }

  if (errorCode == ERR_ABORTED) {
    return;
  }

  std::stringstream ss;
  ss << "<html><body bgcolor=\"white\">"
        "<h2>Failed to load URL "
     << std::string(failedUrl) << " with error " << std::string(errorText)
     << " (" << errorCode << ").</h2></body></html>";

  frame->LoadURL(GetDataURI(ss.str(), "text/html"));
}

void SimpleHandler::ShowMainWindow() {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::BindOnce(&SimpleHandler::ShowMainWindow, this));
    return;
  }

  if (browser_list_.empty()) {
    return;
  }

  auto main_browser = browser_list_.front();
  if (auto browser_view = CefBrowserView::GetForBrowser(main_browser)) {
    if (auto window = browser_view->GetWindow()) {
      window->Show();
    }
  }
}

void SimpleHandler::CloseAllBrowsers(bool force_close) {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::BindOnce(&SimpleHandler::CloseAllBrowsers, this, force_close));
    return;
  }

  if (browser_list_.empty()) {
    return;
  }

  BrowserList::const_iterator it = browser_list_.begin();
  for (; it != browser_list_.end(); ++it) {
    (*it)->GetHost()->CloseBrowser(force_close);
  }
}