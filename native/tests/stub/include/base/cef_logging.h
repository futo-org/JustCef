#ifndef CEF_INCLUDE_BASE_CEF_LOGGING_H_
#define CEF_INCLUDE_BASE_CEF_LOGGING_H_

#include <ostream>

namespace cef_logging_stub
{

class NullStream : public std::ostream
{
public:
    NullStream() : std::ostream(nullptr) {}
};

} // namespace cef_logging_stub

#define LOG(severity) cef_logging_stub::NullStream()

#endif // CEF_INCLUDE_BASE_CEF_LOGGING_H_
