#include "client_util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/base/cef_callback.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "client_manager.h"

namespace {
  class PendingFileDialogRequest {
   public:
    explicit PendingFileDialogRequest(int browser_identifier)
        : browser_identifier_(browser_identifier) {}
    virtual ~PendingFileDialogRequest() = default;

    int browser_identifier() const { return browser_identifier_; }
    bool IsCompleted() const { return completed_.load(); }

    void Cancel() {
      if (!TryBeginCompletion())
        return;

      OnCancel();
    }

   protected:
    bool TryBeginCompletion() { return !completed_.exchange(true); }

   private:
    virtual void OnCancel() = 0;

    const int browser_identifier_;
    std::atomic<bool> completed_{false};
  };

  class PendingPathsDialogRequest : public PendingFileDialogRequest {
   public:
    PendingPathsDialogRequest(int browser_identifier, std::shared_ptr<std::promise<std::vector<std::string>>> promise)
        : PendingFileDialogRequest(browser_identifier),
          promise_(std::move(promise)) {}

    void Complete(const std::vector<CefString>& file_paths) {
      if (!TryBeginCompletion())
        return;

      std::vector<std::string> paths;
      paths.reserve(file_paths.size());
      for (const CefString& path : file_paths)
        paths.push_back(path.ToString());

      promise_->set_value(std::move(paths));
    }

   private:
    void OnCancel() override { promise_->set_value({}); }

    std::shared_ptr<std::promise<std::vector<std::string>>> promise_;
  };

  class PendingPathDialogRequest : public PendingFileDialogRequest {
   public:
    PendingPathDialogRequest(int browser_identifier, std::shared_ptr<std::promise<std::string>> promise)
        : PendingFileDialogRequest(browser_identifier),
          promise_(std::move(promise)) {}

    void Complete(const std::vector<CefString>& file_paths) {
      if (!TryBeginCompletion())
        return;

      if (!file_paths.empty())
        promise_->set_value(file_paths.front().ToString());
      else
        promise_->set_value(std::string());
    }

   private:
    void OnCancel() override { promise_->set_value(std::string()); }

    std::shared_ptr<std::promise<std::string>> promise_;
  };

  std::mutex g_pending_file_dialogs_mutex;
  std::unordered_map<int, std::vector<std::shared_ptr<PendingFileDialogRequest>>> g_pending_file_dialogs;

  void RegisterPendingFileDialogRequest(const std::shared_ptr<PendingFileDialogRequest>& request) {
    std::lock_guard<std::mutex> lock(g_pending_file_dialogs_mutex);
    g_pending_file_dialogs[request->browser_identifier()].push_back(request);
  }

  void RemovePendingFileDialogRequest(const std::shared_ptr<PendingFileDialogRequest>& request) {
    std::lock_guard<std::mutex> lock(g_pending_file_dialogs_mutex);
    auto it = g_pending_file_dialogs.find(request->browser_identifier());
    if (it == g_pending_file_dialogs.end())
      return;

    auto& requests = it->second;
    requests.erase(std::remove_if(requests.begin(), requests.end(), [&](const std::shared_ptr<PendingFileDialogRequest>& candidate) {
      return candidate.get() == request.get();
    }), requests.end());

    if (requests.empty())
      g_pending_file_dialogs.erase(it);
  }

  std::vector<std::shared_ptr<PendingFileDialogRequest>> TakePendingFileDialogRequests(int browser_identifier) {
    std::lock_guard<std::mutex> lock(g_pending_file_dialogs_mutex);
    auto it = g_pending_file_dialogs.find(browser_identifier);
    if (it == g_pending_file_dialogs.end())
      return {};

    auto requests = std::move(it->second);
    g_pending_file_dialogs.erase(it);
    return requests;
  }

  std::string TrimWhitespace(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
      ++start;

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
      --end;

    return value.substr(start, end - start);
  }

  std::vector<std::string> SplitPatternList(const std::string& pattern_list) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= pattern_list.size()) {
      size_t end = pattern_list.find(';', start);
      if (end == std::string::npos)
        end = pattern_list.size();

      std::string token = TrimWhitespace(pattern_list.substr(start, end - start));
      if (!token.empty())
        parts.push_back(std::move(token));

      if (end == pattern_list.size())
        break;

      start = end + 1;
    }

    return parts;
  }

  std::string NormalizeAcceptFilterToken(const std::string& raw_token) {
    std::string token = TrimWhitespace(raw_token);
    if (token.empty())
      return std::string();

    if (token == "*" || token == "*.*")
      return "*";

    if (token.find('/') != std::string::npos)
      return token;

    if (token.rfind("*.", 0) == 0)
      return "." + token.substr(2);

    if (token[0] == '.')
      return token;

    if (token[0] == '*') {
      size_t dot = token.find('.');
      if (dot != std::string::npos)
        return token.substr(dot);
    }

    if (token.find('*') == std::string::npos && token.find('?') == std::string::npos)
      return "." + token;

    return std::string();
  }

  std::vector<CefString> BuildAcceptFilters(const std::vector<std::pair<std::string, std::string>>& filters) {
    std::vector<CefString> accept_filters;
    accept_filters.reserve(filters.size());

    for (const auto& filter : filters) {
      std::vector<std::string> normalized_patterns;
      for (const std::string& token : SplitPatternList(filter.second)) {
        std::string normalized = NormalizeAcceptFilterToken(token);
        if (!normalized.empty())
          normalized_patterns.push_back(std::move(normalized));
      }

      if (normalized_patterns.empty())
        continue;

      std::string accept_filter;
      if (!filter.first.empty()) {
        accept_filter.append(filter.first);
        accept_filter.push_back('|');
      }

      for (size_t i = 0; i < normalized_patterns.size(); ++i) {
        if (i > 0)
          accept_filter.push_back(';');

        accept_filter.append(normalized_patterns[i]);
      }

      accept_filters.emplace_back(accept_filter);
    }

    return accept_filters;
  }

  class FileDialogPathsCallback : public CefRunFileDialogCallback {
   public:
    explicit FileDialogPathsCallback(std::shared_ptr<PendingPathsDialogRequest> request)
        : request_(std::move(request)) {}

    void OnFileDialogDismissed(const std::vector<CefString>& file_paths) override {
      request_->Complete(file_paths);
      RemovePendingFileDialogRequest(request_);
    }

   private:
    std::shared_ptr<PendingPathsDialogRequest> request_;

    IMPLEMENT_REFCOUNTING(FileDialogPathsCallback);
    DISALLOW_COPY_AND_ASSIGN(FileDialogPathsCallback);
  };

  class FileDialogPathCallback : public CefRunFileDialogCallback {
   public:
    explicit FileDialogPathCallback(std::shared_ptr<PendingPathDialogRequest> request)
        : request_(std::move(request)) {}

    void OnFileDialogDismissed(const std::vector<CefString>& file_paths) override {
      request_->Complete(file_paths);
      RemovePendingFileDialogRequest(request_);
    }

   private:
    std::shared_ptr<PendingPathDialogRequest> request_;

    IMPLEMENT_REFCOUNTING(FileDialogPathCallback);
    DISALLOW_COPY_AND_ASSIGN(FileDialogPathCallback);
  };

  void RunFileDialogForPathsOnUi(std::shared_ptr<PendingPathsDialogRequest> request, int browser_identifier, CefBrowserHost::FileDialogMode mode, const std::string& title, const std::string& default_file_path, std::vector<CefString> accept_filters) {
    CEF_REQUIRE_UI_THREAD();

    if (request->IsCompleted())
      return;

    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(browser_identifier);
    if (!browser || !browser->GetHost()) {
      RemovePendingFileDialogRequest(request);
      request->Cancel();
      return;
    }

    browser->GetHost()->RunFileDialog(mode, title, default_file_path, accept_filters, new FileDialogPathsCallback(std::move(request)));
  }

  void RunFileDialogForPathOnUi(std::shared_ptr<PendingPathDialogRequest> request, int browser_identifier, CefBrowserHost::FileDialogMode mode, const std::string& title, const std::string& default_file_path, std::vector<CefString> accept_filters) {
    CEF_REQUIRE_UI_THREAD();

    if (request->IsCompleted())
      return;

    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquirePointer(browser_identifier);
    if (!browser || !browser->GetHost()) {
      RemovePendingFileDialogRequest(request);
      request->Cancel();
      return;
    }

    browser->GetHost()->RunFileDialog(mode, title, default_file_path, accept_filters, new FileDialogPathCallback(std::move(request)));
  }
}  // namespace

namespace shared {
  std::string DumpRequestContents(CefRefPtr<CefRequest> request) {
    std::stringstream ss;

    ss << "URL: " << std::string(request->GetURL());
    ss << "\nMethod: " << std::string(request->GetMethod());

    CefRequest::HeaderMap headerMap;
    request->GetHeaderMap(headerMap);
    if (headerMap.size() > 0) {
      ss << "\nHeaders:";
      CefRequest::HeaderMap::const_iterator it = headerMap.begin();
      for (; it != headerMap.end(); ++it) {
        ss << "\n\t" << std::string((*it).first) << ": "
          << std::string((*it).second);
      }
    }

    CefRefPtr<CefPostData> postData = request->GetPostData();
    if (postData.get()) {
      CefPostData::ElementVector elements;
      postData->GetElements(elements);
      if (elements.size() > 0) {
        ss << "\nPost Data:";
        CefRefPtr<CefPostDataElement> element;
        CefPostData::ElementVector::const_iterator it = elements.begin();
        for (; it != elements.end(); ++it) {
          element = (*it);
          if (element->GetType() == PDE_TYPE_BYTES) {
            // the element is composed of bytes
            ss << "\n\tBytes: ";
            if (element->GetBytesCount() == 0) {
              ss << "(empty)";
            } else {
              // retrieve the data.
              size_t size = element->GetBytesCount();
              char* bytes = new char[size];
              element->GetBytes(size, bytes);
              ss << std::string(bytes, size);
              delete[] bytes;
            }
          } else if (element->GetType() == PDE_TYPE_FILE) {
            ss << "\n\tFile: " << std::string(element->GetFile());
          }
        }
      }
    }

    return ss.str();
  }

  std::future<std::vector<std::string>> PlatformPickFiles(int browser_identifier, bool multiple, const std::vector<std::pair<std::string, std::string>>& filters) {
    auto promise = std::make_shared<std::promise<std::vector<std::string>>>();
    auto request = std::make_shared<PendingPathsDialogRequest>(browser_identifier, promise);
    std::future<std::vector<std::string>> future = promise->get_future();
    std::vector<CefString> accept_filters = BuildAcceptFilters(filters);
    CefBrowserHost::FileDialogMode mode = multiple ? FILE_DIALOG_OPEN_MULTIPLE : FILE_DIALOG_OPEN;
    std::string title = multiple ? "Select Files" : "Open File";
    RegisterPendingFileDialogRequest(request);

    if (CefCurrentlyOn(TID_UI)) {
      RunFileDialogForPathsOnUi(request, browser_identifier, mode, title, std::string(), std::move(accept_filters));
    } else if (!CefPostTask(TID_UI, base::BindOnce(&RunFileDialogForPathsOnUi, request, browser_identifier, mode, title, std::string(), std::move(accept_filters)))) {
      RemovePendingFileDialogRequest(request);
      request->Cancel();
    }

    return future;
  }

  std::future<std::string> PlatformPickDirectory(int browser_identifier) {
    auto promise = std::make_shared<std::promise<std::string>>();
    auto request = std::make_shared<PendingPathDialogRequest>(browser_identifier, promise);
    std::future<std::string> future = promise->get_future();
    std::vector<CefString> filters;
    RegisterPendingFileDialogRequest(request);

    if (CefCurrentlyOn(TID_UI)) {
      RunFileDialogForPathOnUi(request, browser_identifier, FILE_DIALOG_OPEN_FOLDER, "Select Directory", std::string(), std::move(filters));
    } else if (!CefPostTask(TID_UI, base::BindOnce(&RunFileDialogForPathOnUi, request, browser_identifier, FILE_DIALOG_OPEN_FOLDER, std::string("Select Directory"), std::string(), std::move(filters)))) {
      RemovePendingFileDialogRequest(request);
      request->Cancel();
    }

    return future;
  }

  std::future<std::string> PlatformSaveFile(int browser_identifier, const std::string& default_name, const std::vector<std::pair<std::string, std::string>>& filters) {
    auto promise = std::make_shared<std::promise<std::string>>();
    auto request = std::make_shared<PendingPathDialogRequest>(browser_identifier, promise);
    std::future<std::string> future = promise->get_future();
    std::vector<CefString> accept_filters = BuildAcceptFilters(filters);
    RegisterPendingFileDialogRequest(request);

    if (CefCurrentlyOn(TID_UI)) {
      RunFileDialogForPathOnUi(request, browser_identifier, FILE_DIALOG_SAVE, "Save File", default_name, std::move(accept_filters));
    } else if (!CefPostTask(TID_UI, base::BindOnce(&RunFileDialogForPathOnUi, request, browser_identifier, FILE_DIALOG_SAVE, std::string("Save File"), default_name, std::move(accept_filters)))) {
      RemovePendingFileDialogRequest(request);
      request->Cancel();
    }

    return future;
  }

  void CancelPendingFileDialogs(int browser_identifier) {
    auto requests = TakePendingFileDialogRequests(browser_identifier);
    for (const auto& request : requests)
      request->Cancel();
  }
}  // namespace shared
