#include "resource_util.h"

#include "include/cef_parser.h"
#include "include/wrapper/cef_stream_resource_handler.h"

namespace shared {

  const char kTestOrigin[] = "https://example.com/";

  // Returns |url| without the query or fragment components, if any.
  std::string GetUrlWithoutQueryOrFragment(const std::string& url) {
    // Find the first instance of '?' or '#'.
    const size_t pos = std::min(url.find('?'), url.find('#'));
    if (pos != std::string::npos)
      return url.substr(0, pos);

    return url;
  }

  std::string GetResourcePath(const std::string& url) {
    if (url.find(kTestOrigin) != 0U)
      return std::string();

    const std::string& url_no_query = GetUrlWithoutQueryOrFragment(url);
    return url_no_query.substr(sizeof(kTestOrigin) - 1);
  }

  // Determine the mime type based on the |file_path| file extension.
  std::string GetMimeType(const std::string& resource_path) {
    std::string mime_type;
    size_t sep = resource_path.find_last_of(".");
    if (sep != std::string::npos) {
      mime_type = CefGetMimeType(resource_path.substr(sep + 1));
      if (!mime_type.empty())
        return mime_type;
    }
    return "text/html";
  }

  CefRefPtr<CefResourceHandler> GetResourceHandler(
      const std::string& resource_path) {
    CefRefPtr<CefStreamReader> reader = GetResourceReader(resource_path);
    if (!reader)
      return nullptr;

    return new CefStreamResourceHandler(GetMimeType(resource_path), reader);
  }

}
