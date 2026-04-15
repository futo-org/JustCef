#pragma once

#include <exception>
#include <functional>
#include <string>
#include <string_view>

namespace justcef {

enum class LogLevel : int {
    None,
    Error,
    Warning,
    Info,
    Verbose,
    Debug,
};

class Logger {
public:
    using LogCallback =
        std::function<void(LogLevel level, std::string_view tag, std::string_view message, std::exception_ptr exception)>;
    using WillLogCallback = std::function<bool(LogLevel level)>;

    static void SetLogCallback(LogCallback callback);
    static void SetWillLogCallback(WillLogCallback callback);

    static bool WillLog(LogLevel level);

    static void Debug(std::string_view tag, std::string_view message, std::exception_ptr exception = nullptr);
    static void Verbose(std::string_view tag, std::string_view message, std::exception_ptr exception = nullptr);
    static void Info(std::string_view tag, std::string_view message, std::exception_ptr exception = nullptr);
    static void Warning(std::string_view tag, std::string_view message, std::exception_ptr exception = nullptr);
    static void Error(std::string_view tag, std::string_view message, std::exception_ptr exception = nullptr);

private:
    static void Write(LogLevel level, std::string_view tag, std::string_view message, std::exception_ptr exception);
};

}  // namespace justcef
