#include "client_util.h"

#include <sstream>
#include <string>

#include "include/cef_command_line.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"

#include "client_manager.h"

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
}
