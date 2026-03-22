#include "client_util.h"

#include <cctype>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "client_manager.h"

namespace {
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
    explicit FileDialogPathsCallback(std::shared_ptr<std::promise<std::vector<std::string>>> promise)
        : promise_(std::move(promise)) {}

    void OnFileDialogDismissed(int /*selected_accept_filter*/, const std::vector<CefString>& file_paths) override {
      std::vector<std::string> paths;
      paths.reserve(file_paths.size());
      for (const CefString& path : file_paths)
        paths.push_back(path.ToString());

      promise_->set_value(std::move(paths));
    }

   private:
    std::shared_ptr<std::promise<std::vector<std::string>>> promise_;

    IMPLEMENT_REFCOUNTING(FileDialogPathsCallback);
    DISALLOW_COPY_AND_ASSIGN(FileDialogPathsCallback);
  };

  class FileDialogPathCallback : public CefRunFileDialogCallback {
   public:
    explicit FileDialogPathCallback(std::shared_ptr<std::promise<std::string>> promise)
        : promise_(std::move(promise)) {}

    void OnFileDialogDismissed(int /*selected_accept_filter*/, const std::vector<CefString>& file_paths) override {
      if (!file_paths.empty())
        promise_->set_value(file_paths.front().ToString());
      else
        promise_->set_value(std::string());
    }

   private:
    std::shared_ptr<std::promise<std::string>> promise_;

    IMPLEMENT_REFCOUNTING(FileDialogPathCallback);
    DISALLOW_COPY_AND_ASSIGN(FileDialogPathCallback);
  };

  void RunFileDialogForPathsOnUi(std::shared_ptr<std::promise<std::vector<std::string>>> promise, CefBrowserHost::FileDialogMode mode, const std::string& title, const std::string& default_file_path, std::vector<CefString> accept_filters) {
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquireFirstPointer();
    if (!browser || !browser->GetHost()) {
      promise->set_value({});
      return;
    }

    browser->GetHost()->RunFileDialog(mode, title, default_file_path, accept_filters, 0, new FileDialogPathsCallback(std::move(promise)));
  }

  void RunFileDialogForPathOnUi(std::shared_ptr<std::promise<std::string>> promise, CefBrowserHost::FileDialogMode mode, const std::string& title, const std::string& default_file_path, std::vector<CefString> accept_filters) {
    CefRefPtr<CefBrowser> browser = ClientManager::GetInstance()->AcquireFirstPointer();
    if (!browser || !browser->GetHost()) {
      promise->set_value(std::string());
      return;
    }

    browser->GetHost()->RunFileDialog(mode, title, default_file_path, accept_filters, 0, new FileDialogPathCallback(std::move(promise)));
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

  std::future<std::vector<std::string>> PlatformPickFiles(bool multiple, const std::vector<std::pair<std::string, std::string>>& filters) {
    auto promise = std::make_shared<std::promise<std::vector<std::string>>>();
    std::future<std::vector<std::string>> future = promise->get_future();
    std::vector<CefString> accept_filters = BuildAcceptFilters(filters);
    CefBrowserHost::FileDialogMode mode = multiple ? FILE_DIALOG_OPEN_MULTIPLE : FILE_DIALOG_OPEN;
    std::string title = multiple ? "Select Files" : "Open File";

    if (CefCurrentlyOn(TID_UI)) {
      RunFileDialogForPathsOnUi(promise, mode, title, std::string(), std::move(accept_filters));
    } else if (!CefPostTask(TID_UI, base::BindOnce(&RunFileDialogForPathsOnUi, promise, mode, title,std::string(), std::move(accept_filters)))) {
      promise->set_value({});
    }

    return future;
  }

  std::future<std::string> PlatformPickDirectory() {
    auto promise = std::make_shared<std::promise<std::string>>();
    std::future<std::string> future = promise->get_future();
    std::vector<CefString> filters;

    if (CefCurrentlyOn(TID_UI)) {
      RunFileDialogForPathOnUi(promise, FILE_DIALOG_OPEN_FOLDER, "Select Directory", std::string(), std::move(filters));
    } else if (!CefPostTask(TID_UI, base::BindOnce(&RunFileDialogForPathOnUi, promise, FILE_DIALOG_OPEN_FOLDER, std::string("Select Directory"), std::string(), std::move(filters)))) {
      promise->set_value(std::string());
    }

    return future;
  }

  std::future<std::string> PlatformSaveFile(const std::string& default_name, const std::vector<std::pair<std::string, std::string>>& filters) {
    auto promise = std::make_shared<std::promise<std::string>>();
    std::future<std::string> future = promise->get_future();
    std::vector<CefString> accept_filters = BuildAcceptFilters(filters);

    if (CefCurrentlyOn(TID_UI)) {
      RunFileDialogForPathOnUi(promise, FILE_DIALOG_SAVE, "Save File", default_name, std::move(accept_filters));
    } else if (!CefPostTask(TID_UI, base::BindOnce(&RunFileDialogForPathOnUi, promise, FILE_DIALOG_SAVE, std::string("Save File"), default_name, std::move(accept_filters)))) {
      promise->set_value(std::string());
    }

    return future;
  }
}  // namespace shared
