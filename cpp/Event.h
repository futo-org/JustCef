#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace justcef {

template <typename... Args>
class Event {
public:
    using Handler = std::function<void(Args...)>;

    std::size_t Connect(Handler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t token = next_token_++;
        handlers_.push_back({token, std::move(handler)});
        return token;
    }

    void Disconnect(std::size_t token) {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_.erase(
            std::remove_if(
                handlers_.begin(),
                handlers_.end(),
                [token](const HandlerEntry& entry) { return entry.token == token; }),
            handlers_.end());
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        handlers_.clear();
    }

    void Emit(Args... args) const {
        std::vector<Handler> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot.reserve(handlers_.size());
            for (const auto& entry : handlers_) {
                snapshot.push_back(entry.handler);
            }
        }

        for (const auto& handler : snapshot) {
            if (handler) {
                handler(args...);
            }
        }
    }

private:
    struct HandlerEntry {
        std::size_t token;
        Handler handler;
    };

    mutable std::mutex mutex_;
    std::vector<HandlerEntry> handlers_;
    std::size_t next_token_ = 1;
};

}  // namespace justcef
