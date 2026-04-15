#pragma once

#include "asio.h"
#include "IpcTypes.h"

#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace justcef {
namespace asio_support_detail {

template <typename T>
struct IsAwaitable : std::false_type {};

template <typename T, typename Executor>
struct IsAwaitable<asio::awaitable<T, Executor>> : std::true_type {};

template <typename T>
inline constexpr bool kIsAwaitableV = IsAwaitable<T>::value;

}  // namespace asio_support_detail

template <typename Handler>
auto BindHandler(asio::any_io_executor executor, Handler&& handler) {
    using HandlerType = std::decay_t<Handler>;

    auto shared_handler = std::make_shared<HandlerType>(std::forward<Handler>(handler));
    return [executor = std::move(executor), shared_handler](auto&&... args) mutable {
        auto args_tuple = std::make_tuple(std::forward<decltype(args)>(args)...);
        asio::dispatch(executor, [shared_handler, args = std::move(args_tuple)]() mutable {
            std::apply(
                [shared_handler](auto&&... unpacked) {
                    std::invoke(*shared_handler, std::forward<decltype(unpacked)>(unpacked)...);
                },
                std::move(args));
        });
    };
}

template <typename Handler>
RequestProxy BindRequestProxy(asio::any_io_executor executor, Handler&& handler) {
    using HandlerType = std::decay_t<Handler>;
    using Result = std::invoke_result_t<HandlerType&, JustCefWindow&, const IPCRequest&>;

    auto shared_handler = std::make_shared<HandlerType>(std::forward<Handler>(handler));
    return [executor = std::move(executor), shared_handler](JustCefWindow& window, const IPCRequest& request) mutable
               -> asio::awaitable<std::optional<IPCResponse>> {
        co_await asio::post(executor, asio::use_awaitable);
        if constexpr (asio_support_detail::kIsAwaitableV<Result>) {
            co_return co_await std::invoke(*shared_handler, window, request);
        } else {
            co_return std::invoke(*shared_handler, window, request);
        }
    };
}

template <typename Handler>
RequestModifier BindRequestModifier(asio::any_io_executor executor, Handler&& handler) {
    using HandlerType = std::decay_t<Handler>;
    using Result = std::invoke_result_t<HandlerType&, JustCefWindow&, const IPCRequest&>;

    auto shared_handler = std::make_shared<HandlerType>(std::forward<Handler>(handler));
    return [executor = std::move(executor), shared_handler](JustCefWindow& window, const IPCRequest& request) mutable
               -> asio::awaitable<std::optional<IPCRequest>> {
        co_await asio::post(executor, asio::use_awaitable);
        if constexpr (asio_support_detail::kIsAwaitableV<Result>) {
            co_return co_await std::invoke(*shared_handler, window, request);
        } else {
            co_return std::invoke(*shared_handler, window, request);
        }
    };
}

}  // namespace justcef
