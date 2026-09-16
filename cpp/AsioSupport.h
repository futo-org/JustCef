#pragma once

#include "IpcTypes.h"
#include <asio.hpp>

#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace justcef
{
namespace asio_support_detail
{

template <typename T> struct IsAwaitable : std::false_type
{
};

template <typename T, typename Executor> struct IsAwaitable<asio::awaitable<T, Executor>> : std::true_type
{
};

template <typename T> inline constexpr bool kIsAwaitableV = IsAwaitable<T>::value;

template <typename Output, typename HandlerType, typename Target>
asio::awaitable<std::optional<Output>> InvokeBound(std::shared_ptr<HandlerType> handler, Target* target, const IPCRequest* request)
{
    using Result = std::invoke_result_t<HandlerType&, Target&, const IPCRequest&>;
    if constexpr (kIsAwaitableV<Result>)
    {
        co_return co_await std::invoke(*handler, *target, *request);
    }
    else
    {
        co_return std::invoke(*handler, *target, *request);
    }
}

} // namespace asio_support_detail

template <typename Handler> auto BindHandler(asio::any_io_executor executor, Handler&& handler)
{
    using HandlerType = std::decay_t<Handler>;

    auto shared_handler = std::make_shared<HandlerType>(std::forward<Handler>(handler));
    return [executor = std::move(executor), shared_handler](auto&&... args) mutable
    {
        auto args_tuple = std::make_tuple(std::forward<decltype(args)>(args)...);
        asio::post(executor,
                   [shared_handler, args = std::move(args_tuple)]() mutable
                   {
                       std::apply(
                           [shared_handler](auto&&... unpacked)
                           {
                               std::invoke(*shared_handler, std::forward<decltype(unpacked)>(unpacked)...);
                           },
                           std::move(args));
                   });
    };
}

template <typename Handler> RequestProxy BindRequestProxy(asio::any_io_executor executor, Handler&& handler)
{
    using HandlerType = std::decay_t<Handler>;

    auto shared_handler = std::make_shared<HandlerType>(std::forward<Handler>(handler));
    return [executor = std::move(executor), shared_handler](JustCefWindow& window, const IPCRequest& request) mutable -> asio::awaitable<std::optional<IPCResponse>>
    {
        return asio::co_spawn(executor, asio_support_detail::InvokeBound<IPCResponse>(shared_handler, &window, &request), asio::use_awaitable);
    };
}

template <typename Handler> RequestModifier BindRequestModifier(asio::any_io_executor executor, Handler&& handler)
{
    using HandlerType = std::decay_t<Handler>;

    auto shared_handler = std::make_shared<HandlerType>(std::forward<Handler>(handler));
    return [executor = std::move(executor), shared_handler](JustCefWindow& window, const IPCRequest& request) mutable -> asio::awaitable<std::optional<IPCRequest>>
    {
        return asio::co_spawn(executor, asio_support_detail::InvokeBound<IPCRequest>(shared_handler, &window, &request), asio::use_awaitable);
    };
}

} // namespace justcef
