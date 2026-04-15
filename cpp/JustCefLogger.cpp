#include "JustCefLogger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

namespace justcef {
namespace {

std::mutex g_logger_mutex;
Logger::LogCallback g_log_callback = [](LogLevel level,
                                        std::string_view tag,
                                        std::string_view message,
                                        std::exception_ptr exception) {
    std::ostringstream stream;

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &now_time);
#else
    localtime_r(&now_time, &local_time);
#endif

    stream << '[' << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << milliseconds.count() << "] [";

    switch (level) {
        case LogLevel::Error:
            stream << "ERROR";
            break;
        case LogLevel::Warning:
            stream << "WARNING";
            break;
        case LogLevel::Info:
            stream << "INFO";
            break;
        case LogLevel::Verbose:
            stream << "VERBOSE";
            break;
        case LogLevel::Debug:
            stream << "DEBUG";
            break;
        case LogLevel::None:
        default:
            stream << "NONE";
            break;
    }

    stream << "] [" << tag << "] " << message;

    if (exception) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& ex) {
            stream << "\nException: " << ex.what();
        } catch (...) {
            stream << "\nException: unknown";
        }
    }

    std::cerr << stream.str() << std::endl;
};

Logger::WillLogCallback g_will_log = [](LogLevel) { return true; };

}  // namespace

void Logger::SetLogCallback(LogCallback callback) {
    std::lock_guard<std::mutex> lock(g_logger_mutex);
    g_log_callback = std::move(callback);
}

void Logger::SetWillLogCallback(WillLogCallback callback) {
    std::lock_guard<std::mutex> lock(g_logger_mutex);
    g_will_log = std::move(callback);
}

bool Logger::WillLog(LogLevel level) {
    std::lock_guard<std::mutex> lock(g_logger_mutex);
    return g_will_log ? g_will_log(level) : false;
}

void Logger::Debug(std::string_view tag, std::string_view message, std::exception_ptr exception) {
    Write(LogLevel::Debug, tag, message, exception);
}

void Logger::Verbose(std::string_view tag, std::string_view message, std::exception_ptr exception) {
    Write(LogLevel::Verbose, tag, message, exception);
}

void Logger::Info(std::string_view tag, std::string_view message, std::exception_ptr exception) {
    Write(LogLevel::Info, tag, message, exception);
}

void Logger::Warning(std::string_view tag, std::string_view message, std::exception_ptr exception) {
    Write(LogLevel::Warning, tag, message, exception);
}

void Logger::Error(std::string_view tag, std::string_view message, std::exception_ptr exception) {
    Write(LogLevel::Error, tag, message, exception);
}

void Logger::Write(LogLevel level, std::string_view tag, std::string_view message, std::exception_ptr exception) {
    LogCallback callback;
    WillLogCallback will_log;
    {
        std::lock_guard<std::mutex> lock(g_logger_mutex);
        callback = g_log_callback;
        will_log = g_will_log;
    }

    if (will_log && !will_log(level)) {
        return;
    }

    if (callback) {
        callback(level, tag, message, exception);
    }
}

}  // namespace justcef
