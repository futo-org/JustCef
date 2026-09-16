#include "TestSupport.h"

#include "AsyncSignal.h"
#include "JustCefProcess.h"
#include "Packet.h"
#include "Rpc.h"
#include "Transport.h"
#include "WindowInternals.h"
#include "json.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

using namespace justcef;
using namespace justcef::tests;
using namespace std::string_literals;
using detail::PacketReader;
using detail::PacketWriter;
using Bytes = std::vector<std::uint8_t>;

namespace
{

Bytes FromHex(const std::string& hex)
{
    Bytes bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t index = 0; index + 1 < hex.size(); index += 2)
    {
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(index, 2), nullptr, 16)));
    }
    return bytes;
}

std::string ToHex(const Bytes& bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex;
    for (const auto byte : bytes)
    {
        hex.push_back(kDigits[byte >> 4]);
        hex.push_back(kDigits[byte & 0x0F]);
    }
    return hex;
}

Bytes Raw(std::string_view text)
{
    return Bytes(text.begin(), text.end());
}

std::string ReadString(PacketReader& reader)
{
    const auto value = reader.ReadSizePrefixedString();
    REQUIRE(value.has_value());
    return *value;
}

IPCRequest ReadRequest(PacketReader& reader)
{
    IPCRequest request;
    request.method = ReadString(reader);
    request.url = ReadString(reader);
    const auto header_count = *reader.Read<std::int32_t>();
    for (std::int32_t index = 0; index < header_count; ++index)
    {
        auto key = ReadString(reader);
        request.headers[key].push_back(ReadString(reader));
    }
    const auto element_count = *reader.Read<std::int32_t>();
    for (std::int32_t index = 0; index < element_count; ++index)
    {
        const auto type = static_cast<IPCProxyBodyElementType>(*reader.Read<std::uint8_t>());
        if (type == IPCProxyBodyElementType::Bytes)
        {
            request.elements.push_back(IPCProxyBodyElement::Bytes(reader.ReadBytes(*reader.Read<std::uint32_t>())));
        }
        else
        {
            request.elements.push_back(IPCProxyBodyElement::File(ReadString(reader)));
        }
    }
    return request;
}

struct Expected
{
    detail::PacketType type = detail::PacketType::Request;
    std::uint8_t opcode = 0;
    std::uint32_t request_id = 0;
    std::optional<Status> status;
    std::optional<std::string> message;
    std::function<void(PacketReader&)> body;
    const char* unsupported = nullptr;
};

using Fields = std::function<void(PacketReader&)>;

Expected Req(std::uint8_t opcode, Fields body = {})
{
    return Expected{.type = detail::PacketType::Request, .opcode = opcode, .request_id = 7, .body = std::move(body)};
}

Expected Ok(std::uint8_t opcode, Fields body = {})
{
    return Expected{.type = detail::PacketType::Response, .opcode = opcode, .request_id = 7, .status = Status::Ok, .body = std::move(body)};
}

Expected Failure(std::uint8_t opcode, Status status, std::optional<std::string> message)
{
    return Expected{.type = detail::PacketType::Response, .opcode = opcode, .request_id = 7, .status = status, .message = std::move(message)};
}

Expected Notify(std::uint8_t opcode, Fields body = {})
{
    return Expected{.type = detail::PacketType::Notification, .opcode = opcode, .request_id = 0, .body = std::move(body)};
}

Expected Cancel(std::uint8_t opcode)
{
    return Expected{.type = detail::PacketType::Cancel, .opcode = opcode, .request_id = 7};
}

void CheckBlob(PacketReader& reader, std::string_view text)
{
    const auto length = reader.Read<std::uint32_t>();
    REQUIRE(length.has_value());
    CHECK(reader.ReadString(*length) == std::string(text));
}

void CheckRest(PacketReader& reader, const Bytes& bytes)
{
    CHECK(reader.ReadBytes(reader.RemainingSize()) == bytes);
}

void CheckHeaders(PacketReader& reader, const HeaderMap& headers)
{
    std::uint32_t expected = 0;
    for (const auto& [_, values] : headers)
    {
        expected += static_cast<std::uint32_t>(values.size());
    }

    CHECK(reader.Read<std::uint32_t>() == expected);
    HeaderMap decoded;
    for (std::uint32_t index = 0; index < expected; ++index)
    {
        const auto key = ReadString(reader);
        decoded[key].push_back(ReadString(reader));
    }
    CHECK(decoded == headers);
}

void CheckRequest(PacketReader& reader, const IPCRequest& request)
{
    const IPCRequest decoded = ReadRequest(reader);
    CHECK(decoded.method == request.method);
    CHECK(decoded.url == request.url);
    CHECK(decoded.headers == request.headers);
    REQUIRE(decoded.elements.size() == request.elements.size());
    for (std::size_t index = 0; index < request.elements.size(); ++index)
    {
        CHECK(decoded.elements[index].type == request.elements[index].type);
        CHECK(decoded.elements[index].data == request.elements[index].data);
        CHECK(decoded.elements[index].file_name == request.elements[index].file_name);
    }
}

IPCRequest ProxyRequestFields()
{
    IPCRequest request;
    request.method = "POST";
    request.url = "https://example.com/api";
    request.headers["Content-Type"].push_back("application/json");
    request.headers["Accept"].push_back("*/*");
    request.elements.push_back(IPCProxyBodyElement::Bytes(Raw("{\"a\":1}")));
    request.elements.push_back(IPCProxyBodyElement::File("/tmp/upload.bin"));
    return request;
}

std::map<std::string, Expected> Expectations()
{
    const std::string utf8_title = "Gr\xc3\xbc\xc3\x9f"
                                   "e \xe6\x97\xa5\xe6\x9c\xac \xf0\x90\x8d\x88";
    std::map<std::string, Expected> expectations;

    expectations["ready"] = Notify(0,
                                   [](PacketReader& reader)
                                   {
                                       CHECK(reader.Read<std::uint32_t>() == detail::kProtocolVersion);
                                   });
    expectations["native_exit"] = Notify(1);
    expectations["controller_exit"] = Notify(0);
    expectations["cancel_window_create"] = Cancel(3);
    expectations["cancel_client_bridge_rpc"] = Cancel(9);
    expectations["ping"] = Req(0);
    expectations["print"] = Req(1,
                                [](PacketReader& reader)
                                {
                                    CheckRest(reader, Raw("hello"));
                                });
    expectations["echo_request"] = Req(2,
                                       [](PacketReader& reader)
                                       {
                                           CheckRest(reader, Raw("ping"));
                                       });
    expectations["echo_ok"] = Ok(2,
                                 [](PacketReader& reader)
                                 {
                                     CheckRest(reader, Raw("ping"));
                                 });
    expectations["response_ok_empty"] = Ok(20);
    expectations["response_error"] = Failure(20, Status::Error, "boom");
    expectations["response_canceled"] = Failure(20, Status::Canceled, std::nullopt);
    expectations["response_not_found"] = Failure(20, Status::NotFound, std::nullopt);
    expectations["response_not_handled"] = Failure(3, Status::NotHandled, std::nullopt);
    expectations["response_unsupported"] = Failure(99, Status::Unsupported, "unknown opcode");
    expectations["response_shutting_down"] = Failure(20, Status::ShuttingDown, std::nullopt);
    expectations["response_too_large"] = Failure(2, Status::TooLarge, std::nullopt);
    expectations["response_invalid_request"] = Failure(20, Status::InvalidRequest, "truncated body");
    expectations["set_title_null"] = Req(44,
                                         [](PacketReader& reader)
                                         {
                                             CHECK(reader.Read<std::int32_t>() == 1);
                                             CHECK_FALSE(reader.ReadSizePrefixedString().has_value());
                                         });
    expectations["set_title_empty"] = Req(44,
                                          [](PacketReader& reader)
                                          {
                                              CHECK(reader.Read<std::int32_t>() == 1);
                                              CHECK(ReadString(reader).empty());
                                          });
    expectations["set_title_utf8"] = Req(44,
                                         [utf8_title](PacketReader& reader)
                                         {
                                             CHECK(reader.Read<std::int32_t>() == 1);
                                             CHECK(ReadString(reader) == utf8_title);
                                         });
    expectations["window_create"] = Req(3,
                                        [](PacketReader& reader)
                                        {
                                            const std::array<bool, 12> flags{true, false, false, true, true, false, true, false, false, true, false, true};
                                            for (const bool flag : flags)
                                            {
                                                CHECK(reader.Read<bool>() == flag);
                                            }
                                            CHECK(reader.Read<std::int32_t>() == 320);
                                            CHECK(reader.Read<std::int32_t>() == 240);
                                            CHECK(reader.Read<std::int32_t>() == 1024);
                                            CHECK(reader.Read<std::int32_t>() == 768);
                                            CHECK(ReadString(reader) == "https://example.com/");
                                            CHECK(ReadString(reader) == "JustCef");
                                            CHECK_FALSE(reader.ReadSizePrefixedString().has_value());
                                            CHECK(ReadString(reader) == "com.futo.justcef");
                                            CHECK(reader.Read<bool>() == true);
                                            CHECK(reader.Read<std::uint32_t>() == 5000u);
                                            CHECK(reader.Read<std::uint8_t>() == static_cast<std::uint8_t>(ModifyTimeoutPolicy::Cancel));
                                            CHECK(reader.Read<std::uint32_t>() == 30000u);
                                        });
    expectations["window_create_ok"] = Ok(3,
                                          [](PacketReader& reader)
                                          {
                                              CHECK(reader.Read<std::int32_t>() == 1);
                                          });
    expectations["load_url"] = Req(6,
                                   [](PacketReader& reader)
                                   {
                                       CHECK(reader.Read<std::int32_t>() == 1);
                                       CHECK(ReadString(reader) == "https://example.com/next");
                                   });
    expectations["window_get_size"] = Req(50,
                                          [](PacketReader& reader)
                                          {
                                              CHECK(reader.Read<std::int32_t>() == 1);
                                          });
    expectations["window_get_size_ok"] = Ok(50,
                                            [](PacketReader& reader)
                                            {
                                                CHECK(reader.Read<std::int32_t>() == 800);
                                                CHECK(reader.Read<std::int32_t>() == 600);
                                            });
    expectations["window_set_position"] = Req(15,
                                              [](PacketReader& reader)
                                              {
                                                  CHECK(reader.Read<std::int32_t>() == 1);
                                                  CHECK(reader.Read<std::int32_t>() == -100);
                                                  CHECK(reader.Read<std::int32_t>() == 200);
                                              });
    expectations["window_set_zoom"] = Req(9,
                                          [](PacketReader& reader)
                                          {
                                              CHECK(reader.Read<std::int32_t>() == 1);
                                              CHECK(reader.Read<double>() == -1.5);
                                          });
    expectations["window_get_zoom_ok"] = Ok(56,
                                            [](PacketReader& reader)
                                            {
                                                CHECK(reader.Read<double>() == 1.25);
                                            });
    expectations["window_close"] = Req(22,
                                       [](PacketReader& reader)
                                       {
                                           CHECK(reader.Read<std::int32_t>() == 1);
                                           CHECK(reader.Read<bool>() == true);
                                       });
    expectations["set_modify_requests"] = Req(34,
                                              [](PacketReader& reader)
                                              {
                                                  CHECK(reader.Read<std::int32_t>() == 1);
                                                  CHECK(reader.Read<std::uint8_t>() == 3);
                                              });
    expectations["pick_file"] = Req(39,
                                    [](PacketReader& reader)
                                    {
                                        CHECK(reader.Read<std::int32_t>() == 1);
                                        CHECK(reader.Read<bool>() == true);
                                        CHECK(reader.Read<std::uint32_t>() == 2u);
                                        CHECK(ReadString(reader) == "Images");
                                        CHECK(ReadString(reader) == "*.png;*.jpg");
                                        CHECK(ReadString(reader) == "All files");
                                        CHECK(ReadString(reader) == "*");
                                    });
    expectations["pick_file_ok"] = Ok(39,
                                      [](PacketReader& reader)
                                      {
                                          CHECK(reader.Read<std::uint32_t>() == 2u);
                                          CHECK(ReadString(reader) == "/home/user/a.png");
                                          CHECK(ReadString(reader) == "/home/user/b.jpg");
                                      });
    expectations["save_file"] = Req(41,
                                    [](PacketReader& reader)
                                    {
                                        CHECK(reader.Read<std::int32_t>() == 1);
                                        CHECK(ReadString(reader) == "report.pdf");
                                        CHECK(reader.Read<std::uint32_t>() == 1u);
                                        CHECK(ReadString(reader) == "PDF");
                                        CHECK(ReadString(reader) == "*.pdf");
                                    });
    expectations["save_file_ok"] = Ok(41,
                                      [](PacketReader& reader)
                                      {
                                          CHECK(ReadString(reader) == "/home/user/report.pdf");
                                      });
    expectations["execute_devtools_method"] = Req(42,
                                                  [](PacketReader& reader)
                                                  {
                                                      CHECK(reader.Read<std::int32_t>() == 1);
                                                      CHECK(ReadString(reader) == "Browser.getVersion");
                                                      CHECK(reader.Read<bool>() == false);
                                                  });
    expectations["execute_devtools_method_params"] = Req(42,
                                                         [](PacketReader& reader)
                                                         {
                                                             CHECK(reader.Read<std::int32_t>() == 1);
                                                             CHECK(ReadString(reader) == "Page.navigate");
                                                             CHECK(reader.Read<bool>() == true);
                                                             CheckBlob(reader, "{\"url\":\"about:blank\"}");
                                                         });
    expectations["execute_devtools_method_ok"] = Ok(42,
                                                    [](PacketReader& reader)
                                                    {
                                                        CHECK(reader.Read<bool>() == true);
                                                        CheckBlob(reader, "{}");
                                                    });
    expectations["widevine_status_ok"] = Ok(59,
                                            [](PacketReader& reader)
                                            {
                                                CHECK(reader.Read<std::int32_t>() == 2);
                                                CHECK(ReadString(reader) == "4.10.2830.0");
                                                CHECK(reader.Read<bool>() == true);
                                                CHECK(reader.Read<bool>() == true);
                                                CHECK(reader.Read<bool>() == false);
                                            });
    expectations["widevine_status_ok"].unsupported = "the cpp controller never sends GetWidevineStatus, so nothing produces or consumes this payload";
    expectations["bridge_rpc_request"] = Req(57,
                                             [](PacketReader& reader)
                                             {
                                                 CHECK(reader.Read<std::int32_t>() == 1);
                                                 CHECK(ReadString(reader) == "add");
                                                 CheckBlob(reader, "[1,2]");
                                             });
    expectations["bridge_rpc_ok"] = Ok(57,
                                       [](PacketReader& reader)
                                       {
                                           CheckBlob(reader, "3");
                                       });
    expectations["client_bridge_rpc_request"] = Req(9,
                                                    [](PacketReader& reader)
                                                    {
                                                        CHECK(reader.Read<std::int32_t>() == 1);
                                                        CHECK(ReadString(reader) == "getUser");
                                                        CheckBlob(reader, "{\"id\":42}");
                                                    });
    expectations["client_bridge_rpc_ok"] = Ok(9,
                                              [](PacketReader& reader)
                                              {
                                                  CheckBlob(reader, "{\"name\":\"Ada\"}");
                                              });
    expectations["client_bridge_rpc_error"] = Failure(9, Status::Error, "no such user");
    expectations["view_created_request"] = Req(11,
                                               [](PacketReader& reader)
                                               {
                                                   CHECK(reader.Read<std::int32_t>() == 1);
                                                   CHECK(reader.Read<std::int32_t>() == 2);
                                                   CHECK(ReadString(reader) == "https://example.com/view");
                                               });
    expectations["view_created_ok"] = Ok(11,
                                         [](PacketReader& reader)
                                         {
                                             CHECK(reader.Read<bool>() == true);
                                         });
    expectations["native_echo_request"] = Req(2,
                                              [](PacketReader& reader)
                                              {
                                                  CheckRest(reader, Bytes{0x00, 0x01, 0x02, 0xff});
                                              });
    expectations["native_echo_ok"] = Ok(2,
                                        [](PacketReader& reader)
                                        {
                                            CheckRest(reader, Bytes{0x00, 0x01, 0x02, 0xff});
                                        });
    expectations["proxy_request"] = Req(3,
                                        [](PacketReader& reader)
                                        {
                                            CHECK(reader.Read<std::int32_t>() == 1);
                                            CheckRequest(reader, ProxyRequestFields());
                                        });
    expectations["proxy_response_no_body"] = Ok(3,
                                                [](PacketReader& reader)
                                                {
                                                    CHECK(reader.Read<std::uint32_t>() == 204u);
                                                    CHECK(ReadString(reader) == "No Content");
                                                    CheckHeaders(reader, {});
                                                    CHECK(reader.Read<std::uint8_t>() == 0);
                                                });
    expectations["proxy_response_inline"] = Ok(3,
                                               [](PacketReader& reader)
                                               {
                                                   CHECK(reader.Read<std::uint32_t>() == 200u);
                                                   CHECK(ReadString(reader) == "OK");
                                                   CheckHeaders(reader, HeaderMap{{"Content-Type", {"text/plain"}}});
                                                   CHECK(reader.Read<std::uint8_t>() == 1);
                                                   CheckBlob(reader, "hello");
                                               });
    expectations["proxy_response_stream"] = Ok(3,
                                               [](PacketReader& reader)
                                               {
                                                   CHECK(reader.Read<std::uint32_t>() == 200u);
                                                   CHECK(ReadString(reader) == "OK");
                                                   CheckHeaders(reader, HeaderMap{{"Content-Type", {"application/octet-stream"}}});
                                                   CHECK(reader.Read<std::uint8_t>() == 2);
                                                   CHECK(reader.Read<std::int64_t>() == 5000000);
                                                   CHECK(reader.Read<std::uint32_t>() == 1u);
                                               });
    expectations["proxy_response_stream_unknown_length"] = Ok(3,
                                                              [](PacketReader& reader)
                                                              {
                                                                  CHECK(reader.Read<std::uint32_t>() == 200u);
                                                                  CHECK(ReadString(reader) == "OK");
                                                                  CheckHeaders(reader, {});
                                                                  CHECK(reader.Read<std::uint8_t>() == 2);
                                                                  CHECK(reader.Read<std::int64_t>() == -1);
                                                                  CHECK(reader.Read<std::uint32_t>() == 2u);
                                                              });
    expectations["modify_request"] = Req(4,
                                         [](PacketReader& reader)
                                         {
                                             IPCRequest request;
                                             request.method = "GET";
                                             request.url = "https://example.com/";
                                             request.headers["User-Agent"].push_back("JustCef");
                                             CHECK(reader.Read<std::int32_t>() == 1);
                                             CheckRequest(reader, request);
                                         });
    expectations["modify_response"] = Ok(4,
                                         [](PacketReader& reader)
                                         {
                                             IPCRequest request;
                                             request.method = "POST";
                                             request.url = "https://example.com/changed";
                                             request.headers["User-Agent"].push_back("Modified");
                                             request.headers["X-Added"].push_back("1");
                                             request.elements.push_back(IPCProxyBodyElement::Bytes(Raw("abc")));
                                             request.elements.push_back(IPCProxyBodyElement::File("/tmp/body.txt"));
                                             CheckRequest(reader, request);
                                         });
    expectations["stream_data"] = Notify(1,
                                         [](PacketReader& reader)
                                         {
                                             CHECK(reader.Read<std::uint32_t>() == 1u);
                                             CheckRest(reader, Raw("0123456789"));
                                         });
    expectations["stream_end"] = Notify(2,
                                        [](PacketReader& reader)
                                        {
                                            CHECK(reader.Read<std::uint32_t>() == 1u);
                                            CHECK(reader.Read<std::uint64_t>() == 10u);
                                        });
    expectations["stream_error"] = Notify(3,
                                          [](PacketReader& reader)
                                          {
                                              CHECK(reader.Read<std::uint32_t>() == 1u);
                                              CHECK(ReadString(reader) == "source failed");
                                          });
    expectations["stream_credit"] = Notify(18,
                                           [](PacketReader& reader)
                                           {
                                               CHECK(reader.Read<std::uint32_t>() == 1u);
                                               CHECK(reader.Read<std::uint32_t>() == 65536u);
                                           });
    expectations["stream_cancel"] = Notify(19,
                                           [](PacketReader& reader)
                                           {
                                               CHECK(reader.Read<std::uint32_t>() == 1u);
                                           });
    expectations["window_opened"] = Notify(2,
                                           [](PacketReader& reader)
                                           {
                                               CHECK(reader.Read<std::int32_t>() == 1);
                                           });
    expectations["window_closed"] = Notify(3,
                                           [](PacketReader& reader)
                                           {
                                               CHECK(reader.Read<std::int32_t>() == 1);
                                           });
    expectations["fullscreen_changed"] = Notify(12,
                                                [](PacketReader& reader)
                                                {
                                                    CHECK(reader.Read<std::int32_t>() == 1);
                                                    CHECK(reader.Read<bool>() == true);
                                                });
    expectations["loading_state_changed"] = Notify(17,
                                                   [](PacketReader& reader)
                                                   {
                                                       CHECK(reader.Read<std::int32_t>() == 1);
                                                       CHECK(reader.Read<bool>() == true);
                                                       CHECK(reader.Read<bool>() == false);
                                                       CHECK(reader.Read<bool>() == true);
                                                   });
    expectations["frame_load_start"] = Notify(13,
                                              [](PacketReader& reader)
                                              {
                                                  CHECK(reader.Read<std::int32_t>() == 1);
                                                  CHECK(ReadString(reader) == "main");
                                                  CHECK(reader.Read<bool>() == true);
                                                  CHECK(ReadString(reader) == "https://example.com/");
                                              });
    expectations["frame_load_end"] = Notify(14,
                                            [](PacketReader& reader)
                                            {
                                                CHECK(reader.Read<std::int32_t>() == 1);
                                                CHECK(ReadString(reader) == "main");
                                                CHECK(reader.Read<bool>() == true);
                                                CHECK(ReadString(reader) == "https://example.com/");
                                                CHECK(reader.Read<std::int32_t>() == 200);
                                            });
    expectations["frame_load_error"] = Notify(15,
                                              [](PacketReader& reader)
                                              {
                                                  CHECK(reader.Read<std::int32_t>() == 1);
                                                  CHECK(ReadString(reader) == "main");
                                                  CHECK(reader.Read<bool>() == true);
                                                  CHECK(reader.Read<std::int32_t>() == -105);
                                                  CHECK(ReadString(reader) == "net::ERR_NAME_NOT_RESOLVED");
                                                  CHECK(ReadString(reader) == "https://nonexistent.invalid/");
                                              });
    expectations["devtools_event"] = Notify(16,
                                            [](PacketReader& reader)
                                            {
                                                CHECK(reader.Read<std::int32_t>() == 1);
                                                CHECK(ReadString(reader) == "Network.requestWillBeSent");
                                                CheckBlob(reader, "{\"requestId\":\"1\"}");
                                            });
    expectations["debug_notification"] = Notify(250,
                                                [](PacketReader& reader)
                                                {
                                                    CHECK(reader.Read<std::uint32_t>() == 42u);
                                                });
    expectations["debug_notification"].unsupported = "the cpp controller logs notification opcode 250 as unhandled and never decodes its body";
    return expectations;
}

nlohmann::json LoadVectors()
{
    const std::filesystem::path path = std::filesystem::path(JUSTCEF_TESTS_REPO_ROOT) / "tests" / "protocol" / "vectors.json";
    std::ifstream stream(path);
    REQUIRE_MESSAGE(stream.good(), "Missing " << path.string());
    return nlohmann::json::parse(stream);
}

Bytes Reencode(const detail::PacketHeader& header, const Bytes& body, const Expected& expected)
{
    switch (header.packet_type)
    {
    case detail::PacketType::Notification:
        return detail::MakeNotification(header.opcode, body).Flatten();
    case detail::PacketType::Cancel:
        return detail::MakeCancel(header.opcode, header.request_id).Flatten();
    case detail::PacketType::Response:
        if (expected.status != Status::Ok)
        {
            return detail::MakeStatusResponse(header.opcode, header.request_id, *expected.status, expected.message).Flatten();
        }
        return detail::MakeOkResponse(header.opcode, header.request_id, Bytes(body.begin() + 1, body.end())).Flatten();
    default:
        return detail::MakePacket(detail::PacketType::Request, header.opcode, header.request_id, body).Flatten();
    }
}

} // namespace

TEST_CASE("codec decodes and re-encodes every golden vector")
{
    const auto vectors = LoadVectors();
    const auto expectations = Expectations();

    std::set<std::string> seen;
    for (const auto& vector : vectors)
    {
        const std::string name = vector.at("name").get<std::string>();
        const std::string hex = vector.at("hex").get<std::string>();
        seen.insert(name);

        INFO("vector " << name);
        const auto entry = expectations.find(name);
        REQUIRE_MESSAGE(entry != expectations.end(), "No expectation for vector " << name);
        const Expected& expected = entry->second;

        const Bytes packet = FromHex(hex);
        REQUIRE(packet.size() >= detail::kPacketHeaderSize);
        const detail::PacketHeader header = detail::DecodeHeader(packet.data());
        CHECK(header.BodySize() + detail::kPacketHeaderSize == packet.size());
        CHECK(header.packet_type == expected.type);
        CHECK(header.opcode == expected.opcode);
        CHECK(header.request_id == expected.request_id);

        const Bytes body(packet.begin() + detail::kPacketHeaderSize, packet.end());
        PacketReader reader(body);
        if (expected.status)
        {
            CHECK(reader.Read<std::uint8_t>() == static_cast<std::uint8_t>(*expected.status));
        }
        if (expected.status && *expected.status != Status::Ok)
        {
            CHECK(reader.ReadSizePrefixedString() == expected.message);
        }
        else if (expected.body)
        {
            expected.body(reader);
        }
        CHECK(reader.RemainingSize() == 0);
        CHECK(ToHex(Reencode(header, body, expected)) == hex);
    }

    for (const auto& [name, _] : expectations)
    {
        CHECK_MESSAGE(seen.contains(name), "Expectation without vector: " << name);
    }
}

TEST_CASE("golden vectors without a cpp code path are decode only")
{
    const auto expectations = Expectations();
    std::set<std::string> unsupported;
    for (const auto& [name, expected] : expectations)
    {
        if (expected.unsupported == nullptr)
        {
            continue;
        }

        unsupported.insert(name);
        MESSAGE("vector " << name << ": " << std::string(expected.unsupported));
    }

    CHECK(unsupported == std::set<std::string>{"debug_notification", "widevine_status_ok"});
}

TEST_CASE("codec rejects oversize and truncated packets")
{
    std::array<std::uint8_t, detail::kPacketHeaderSize> header{};
    std::uint32_t size = static_cast<std::uint32_t>(detail::kMaxIpcSize + 7);
    std::memcpy(header.data(), &size, sizeof(size));
    CHECK_THROWS_AS(detail::DecodeHeader(header.data()), detail::ProtocolError);

    size = 5;
    std::memcpy(header.data(), &size, sizeof(size));
    CHECK_THROWS_AS(detail::DecodeHeader(header.data()), detail::ProtocolError);

    PacketReader reader(Bytes{0x05, 0x00, 0x00, 0x00, 'a'});
    CHECK_FALSE(reader.ReadSizePrefixedString().has_value());
    CHECK_THROWS_AS(reader.ReadBytes(5), detail::ProtocolError);

    CHECK_THROWS_AS(detail::MakePacket(detail::PacketType::Request, 2, 1, Bytes(detail::kMaxIpcSize + 1)), detail::ProtocolError);
}

TEST_CASE("AsyncSignal never resumes a waiter inline")
{
    asio::io_context io;
    detail::AsyncSignal signal;
    bool resumed = false;
    bool signalled = false;

    auto waiter = Spawn(io,
                        [&]() -> asio::awaitable<void>
                        {
                            co_await signal.AsyncWait(io.get_executor());
                            resumed = true;
                        });
    auto signaller = Spawn(io,
                           [&]() -> asio::awaitable<void>
                           {
                               co_await asio::post(io, asio::use_awaitable);
                               signal.SignalSuccess();
                               CHECK_FALSE(resumed);
                               signalled = true;
                           });
    io.run();
    CHECK(signalled);
    CHECK(resumed);
    CHECK(waiter->done);
    CHECK(signaller->done);
}

TEST_CASE("AsyncSignal completes an already signalled wait through the executor")
{
    asio::io_context io;
    detail::AsyncSignal signal;
    signal.SignalSuccess();
    bool marker = false;
    bool checked = false;

    Spawn(io,
          [&]() -> asio::awaitable<void>
          {
              asio::post(io,
                         [&]()
                         {
                             marker = true;
                         });
              co_await signal.AsyncWait(io.get_executor());
              CHECK(marker);
              checked = true;
          });
    io.run();
    CHECK(checked);
}

TEST_CASE("AsyncSignal propagates failure and supports cancellation")
{
    asio::io_context io;
    detail::AsyncSignal failing;
    detail::AsyncSignal never;
    asio::cancellation_signal cancel;
    bool failed = false;
    bool aborted = false;

    Spawn(io,
          [&]() -> asio::awaitable<void>
          {
              try
              {
                  co_await failing.AsyncWait(io.get_executor());
              }
              catch (const std::runtime_error&)
              {
                  failed = true;
              }
          });
    asio::co_spawn(io, never.AsyncWait(io.get_executor()),
                   asio::bind_cancellation_slot(cancel.slot(),
                                                [&](std::exception_ptr exception)
                                                {
                                                    try
                                                    {
                                                        std::rethrow_exception(exception);
                                                    }
                                                    catch (const asio::system_error& error)
                                                    {
                                                        aborted = error.code() == asio::error::operation_aborted;
                                                    }
                                                    catch (...)
                                                    {
                                                    }
                                                }));
    io.poll();
    failing.SignalFailure(std::make_exception_ptr(std::runtime_error("failed")));
    cancel.emit(asio::cancellation_type::terminal);
    io.run();
    CHECK(failed);
    CHECK(aborted);
}

TEST_CASE("AsyncSignal cancellation releases only the canceled waiter")
{
    asio::io_context io;
    detail::AsyncSignal signal;
    asio::cancellation_signal cancel;
    int completed = 0;
    int aborted = 0;

    const auto wait = [&]() -> asio::awaitable<void>
    {
        co_await signal.AsyncWait(io.get_executor());
        ++completed;
    };

    for (int index = 0; index < 4; ++index)
    {
        Spawn(io, wait());
    }
    asio::co_spawn(io, signal.AsyncWait(io.get_executor()),
                   asio::bind_cancellation_slot(cancel.slot(),
                                                [&](std::exception_ptr exception)
                                                {
                                                    try
                                                    {
                                                        std::rethrow_exception(exception);
                                                    }
                                                    catch (const asio::system_error& error)
                                                    {
                                                        aborted += error.code() == asio::error::operation_aborted;
                                                    }
                                                    catch (...)
                                                    {
                                                    }
                                                }));
    for (int index = 0; index < 4; ++index)
    {
        Spawn(io, wait());
    }

    io.poll();
    CHECK(completed == 0);
    cancel.emit(asio::cancellation_type::terminal);
    io.poll();
    CHECK(aborted == 1);
    CHECK(completed == 0);

    signal.SignalSuccess();
    io.run();
    CHECK(completed == 8);
    CHECK(aborted == 1);
}

TEST_CASE("LoadingState completes a navigation only after loading was seen")
{
    asio::io_context io;
    LoadingState state;
    auto navigation = state.ArmNavigation();
    bool completed = false;

    Spawn(io,
          [&]() -> asio::awaitable<void>
          {
              co_await navigation->AsyncWait(io.get_executor());
              completed = true;
          });

    io.poll();
    state.Apply(false, false, false);
    io.poll();
    CHECK_FALSE(completed);
    CHECK_FALSE(navigation->IsSignaled());

    state.Apply(true, false, false);
    io.poll();
    CHECK_FALSE(completed);

    state.Apply(false, true, false);
    CHECK(navigation->IsSignaled());
    CHECK_FALSE(completed);
    io.run();
    CHECK(completed);
    CHECK(state.CanGoBack());
    CHECK_FALSE(state.IsLoading());
}

TEST_CASE("LoadingState faults a navigation on a main frame error except aborts")
{
    asio::io_context io;
    LoadingState state;
    auto navigation = state.ArmNavigation();
    std::string error;

    Spawn(io,
          [&]() -> asio::awaitable<void>
          {
              try
              {
                  co_await navigation->AsyncWait(io.get_executor());
              }
              catch (const std::exception& exception)
              {
                  error = exception.what();
              }
          });

    state.Apply(true, false, false);
    state.OnMainFrameLoadError(-3, "net::ERR_ABORTED");
    io.poll();
    CHECK(error.empty());
    CHECK_FALSE(navigation->IsSignaled());

    state.OnMainFrameLoadError(-105, "net::ERR_NAME_NOT_RESOLVED");
    io.run();
    CHECK(error == "net::ERR_NAME_NOT_RESOLVED");
}

TEST_CASE("LoadingState idle waiters follow the armed navigation and fail on close")
{
    asio::io_context io;
    LoadingState state;
    CHECK(state.IdleSignal() == nullptr);

    auto first = state.ArmNavigation();
    auto idle = state.IdleSignal();
    REQUIRE(idle != nullptr);

    auto second = state.ArmNavigation();
    CHECK(first->IsSignaled());
    CHECK_FALSE(idle->IsSignaled());

    state.Apply(true, false, false);
    state.Apply(false, false, false);
    CHECK(second->IsSignaled());
    CHECK(idle->IsSignaled());
    CHECK(state.IdleSignal() == nullptr);

    state.Apply(true, false, false);
    auto closing = state.IdleSignal();
    REQUIRE(closing != nullptr);
    auto pending = state.ArmNavigation();
    bool closed_failed = false;
    Spawn(io,
          [&]() -> asio::awaitable<void>
          {
              try
              {
                  co_await closing->AsyncWait(io.get_executor());
              }
              catch (const std::exception&)
              {
                  closed_failed = true;
              }
          });
    state.Close();
    CHECK(pending->IsSignaled());
    io.run();
    CHECK(closed_failed);
    CHECK(state.IdleSignal() == nullptr);
}

namespace
{

struct RecordingSender
{
    std::mutex mutex;
    std::vector<detail::OutgoingPacket> packets;
    bool accept = true;

    detail::Rpc::Sender Make()
    {
        return [this](detail::OutgoingPacket packet)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!accept)
            {
                return false;
            }
            packets.push_back(std::move(packet));
            return true;
        };
    }

    std::vector<detail::PacketHeader> Headers()
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<detail::PacketHeader> headers;
        for (const auto& packet : packets)
        {
            headers.push_back(detail::DecodeHeader(packet.head.data()));
        }
        return headers;
    }
};

Bytes OkBody(Bytes payload = {})
{
    Bytes body{0};
    body.insert(body.end(), payload.begin(), payload.end());
    return body;
}

} // namespace

TEST_CASE("Rpc completes through the executor and runs reply hooks first")
{
    asio::io_context io;
    RecordingSender sender;
    auto rpc = std::make_shared<detail::Rpc>(io.get_executor(), sender.Make());
    bool hook_ran = false;
    bool completed = false;
    Bytes result;

    Spawn(io,
          [&]() -> asio::awaitable<void>
          {
              auto reader = co_await rpc->CallAsync(2, Raw("abc"), std::chrono::seconds(5),
                                                    [&](PacketReader&)
                                                    {
                                                        CHECK_FALSE(completed);
                                                        hook_ran = true;
                                                    });
              CHECK(hook_ran);
              result = reader.ReadBytes(reader.RemainingSize());
              completed = true;
          });
    io.poll();
    auto headers = sender.Headers();
    REQUIRE(headers.size() == 1);
    CHECK(headers[0].packet_type == detail::PacketType::Request);
    CHECK(headers[0].request_id != 0);

    rpc->OnResponse(headers[0].request_id, 2, OkBody(Raw("abc")));
    CHECK(hook_ran);
    CHECK_FALSE(completed);
    io.run();
    CHECK(completed);
    CHECK(result == Raw("abc"));
    CHECK(rpc->PendingCount() == 0);
}

TEST_CASE("Rpc maps failure statuses to RemoteError")
{
    asio::io_context io;
    RecordingSender sender;
    auto rpc = std::make_shared<detail::Rpc>(io.get_executor(), sender.Make());
    std::optional<Status> status;
    std::string message;

    Spawn(io,
          [&]() -> asio::awaitable<void>
          {
              try
              {
                  (void)co_await rpc->CallAsync(20, {}, std::nullopt);
              }
              catch (const RemoteError& error)
              {
                  status = error.GetStatus();
                  message = error.what();
              }
          });
    io.poll();
    auto headers = sender.Headers();
    REQUIRE(headers.size() == 1);
    PacketWriter body;
    body.Write<std::uint8_t>(static_cast<std::uint8_t>(Status::NotFound)).WriteSizePrefixedString("gone"s);
    rpc->OnResponse(headers[0].request_id, 20, body.Release());
    io.run();
    REQUIRE(status.has_value());
    CHECK(*status == Status::NotFound);
    CHECK(message == "gone");
}

TEST_CASE("Rpc timeout sends Cancel and drops the late response")
{
    asio::io_context io;
    RecordingSender sender;
    auto rpc = std::make_shared<detail::Rpc>(io.get_executor(), sender.Make());
    bool timed_out = false;
    int completions = 0;
    int hooks = 0;

    Spawn(io,
          [&]() -> asio::awaitable<void>
          {
              try
              {
                  (void)co_await rpc->CallAsync(20, {}, std::chrono::milliseconds(50),
                                                [&](PacketReader&)
                                                {
                                                    ++hooks;
                                                });
              }
              catch (const asio::system_error& error)
              {
                  timed_out = error.code() == asio::error::timed_out;
              }
              ++completions;
          });
    io.run();
    CHECK(timed_out);
    CHECK(completions == 1);
    auto headers = sender.Headers();
    REQUIRE(headers.size() == 2);
    CHECK(headers[1].packet_type == detail::PacketType::Cancel);
    CHECK(headers[1].request_id == headers[0].request_id);
    CHECK(headers[1].opcode == 20);
    CHECK(rpc->PendingCount() == 0);

    rpc->OnResponse(headers[0].request_id, 20, OkBody(Raw("late")));
    io.restart();
    io.run();
    CHECK(hooks == 0);
    CHECK(completions == 1);
    CHECK(sender.Headers().size() == 2);
    CHECK(rpc->PendingCount() == 0);
}

TEST_CASE("Rpc makes a call pending before it arms the timeout")
{
    Watchdog watchdog(std::chrono::seconds(60), "rpc timeout race");
    constexpr int kCalls = 8000;
    asio::io_context io;
    auto guard = asio::make_work_guard(io);
    std::thread timer_thread(
        [&]()
        {
            io.run();
        });
    asio::thread_pool pool(8);
    RecordingSender sender;
    auto rpc = std::make_shared<detail::Rpc>(io.get_executor(), sender.Make());
    std::atomic<int> timed_out = 0;

    std::vector<std::future<void>> parked;
    for (int index = 0; index < kCalls; ++index)
    {
        parked.push_back(asio::co_spawn(
            pool.get_executor(),
            [rpc]() -> asio::awaitable<void>
            {
                try
                {
                    (void)co_await rpc->CallAsync(21, {}, std::nullopt);
                }
                catch (const std::exception&)
                {
                }
            },
            asio::use_future));
    }

    std::atomic<bool> stop = false;
    std::vector<std::thread> hammers;
    for (int index = 0; index < 12; ++index)
    {
        hammers.emplace_back(
            [&]()
            {
                while (!stop.load())
                {
                    (void)rpc->PendingCount();
                }
            });
    }

    std::vector<std::future<void>> calls;
    for (int index = 0; index < 8; ++index)
    {
        calls.push_back(asio::co_spawn(
            pool.get_executor(),
            [rpc, &timed_out]() -> asio::awaitable<void>
            {
                for (int attempt = 0; attempt < kCalls / 8; ++attempt)
                {
                    try
                    {
                        (void)co_await rpc->CallAsync(20, {}, std::chrono::milliseconds(0));
                    }
                    catch (const asio::system_error& error)
                    {
                        timed_out += error.code() == asio::error::timed_out;
                    }
                }
            },
            asio::use_future));
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    int finished = 0;
    for (auto& call : calls)
    {
        finished += call.wait_until(deadline) == std::future_status::ready;
    }
    stop = true;
    for (auto& hammer : hammers)
    {
        hammer.join();
    }
    CHECK(finished == 8);
    CHECK(timed_out.load() == kCalls);

    rpc->Close(std::make_exception_ptr(std::runtime_error("closed")));
    for (auto& call : parked)
    {
        call.wait_until(deadline + std::chrono::seconds(10));
    }
    CHECK(rpc->PendingCount() == 0);

    guard.reset();
    io.stop();
    timer_thread.join();
}

TEST_CASE("Rpc cancellation slot aborts the call and sends Cancel")
{
    asio::io_context io;
    RecordingSender sender;
    auto rpc = std::make_shared<detail::Rpc>(io.get_executor(), sender.Make());
    asio::cancellation_signal cancel;
    bool aborted = false;

    asio::co_spawn(io, rpc->CallAsync(21, {}, std::nullopt),
                   asio::bind_cancellation_slot(cancel.slot(),
                                                [&](std::exception_ptr exception, PacketReader)
                                                {
                                                    try
                                                    {
                                                        std::rethrow_exception(exception);
                                                    }
                                                    catch (const asio::system_error& error)
                                                    {
                                                        aborted = error.code() == asio::error::operation_aborted;
                                                    }
                                                    catch (...)
                                                    {
                                                    }
                                                }));
    io.poll();
    cancel.emit(asio::cancellation_type::terminal);
    io.run();
    CHECK(aborted);
    auto headers = sender.Headers();
    REQUIRE(headers.size() == 2);
    CHECK(headers[1].packet_type == detail::PacketType::Cancel);
}

TEST_CASE("Rpc close fails pending and future calls with the close error")
{
    asio::io_context io;
    RecordingSender sender;
    auto rpc = std::make_shared<detail::Rpc>(io.get_executor(), sender.Make());
    int disconnected = 0;

    const auto call = [&]() -> asio::awaitable<void>
    {
        try
        {
            (void)co_await rpc->CallAsync(0, {}, std::nullopt);
        }
        catch (const std::runtime_error& error)
        {
            disconnected += std::string(error.what()) == "disconnected";
        }
    };

    Spawn(io, call());
    io.poll();
    rpc->Close(std::make_exception_ptr(std::runtime_error("disconnected")));
    Spawn(io, call());
    io.run();
    CHECK(disconnected == 2);
}

#ifndef _WIN32

namespace
{

struct PipePair
{
    int read = -1;
    int write = -1;

    PipePair()
    {
        int fds[2];
        REQUIRE(::pipe(fds) == 0);
        read = fds[0];
        write = fds[1];
    }

    ~PipePair()
    {
        if (read >= 0)
        {
            ::close(read);
        }
        if (write >= 0)
        {
            ::close(write);
        }
    }
};

std::size_t Buffered(int fd)
{
    int available = 0;
    REQUIRE(::ioctl(fd, FIONREAD, &available) == 0);
    return static_cast<std::size_t>(available);
}

std::size_t WaitUntilWriterBlocks(int fd)
{
    std::size_t buffered = 0;
    for (int attempt = 0; attempt < 400; ++attempt)
    {
        const std::size_t current = Buffered(fd);
        if (current > 0 && current == buffered)
        {
            return current;
        }
        buffered = current;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return buffered;
}

std::size_t OpenDescriptorCount()
{
    std::error_code error;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd", error))
    {
        (void)entry;
        ++count;
    }
    return count;
}

void WriteAllBlocking(int fd, const Bytes& bytes)
{
    std::size_t written = 0;
    while (written < bytes.size())
    {
        const auto result = ::write(fd, bytes.data() + written, bytes.size() - written);
        REQUIRE(result > 0);
        written += static_cast<std::size_t>(result);
    }
}

} // namespace

TEST_CASE("Transport frames packets, writes in order and closes on EOF")
{
    Watchdog watchdog(std::chrono::seconds(20), "transport framing");
    PipePair inbound;
    PipePair outbound;

    std::mutex mutex;
    std::vector<detail::IncomingPacket> received;
    std::atomic<bool> closed = false;
    std::atomic<bool> on_transport_thread = false;

    auto transport = std::make_unique<detail::Transport>(inbound.read, outbound.write);
    inbound.read = -1;
    outbound.write = -1;
    transport->Start(
        [&](detail::IncomingPacket&& packet)
        {
            on_transport_thread = detail::Transport::IsTransportThread();
            std::lock_guard<std::mutex> lock(mutex);
            received.push_back(std::move(packet));
        },
        [&]()
        {
            closed = true;
        });

    Bytes stream;
    for (int index = 0; index < 100; ++index)
    {
        PacketWriter writer;
        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(index));
        const auto packet = detail::MakeNotification(250, writer.Release()).Flatten();
        stream.insert(stream.end(), packet.begin(), packet.end());
    }
    const auto big = detail::MakePacket(detail::PacketType::Request, 2, 9, PatternBytes(3 * 1024 * 1024)).Flatten();
    stream.insert(stream.end(), big.begin(), big.end());
    for (std::size_t offset = 0; offset < stream.size(); offset += 7777)
    {
        WriteAllBlocking(inbound.write, Bytes(stream.begin() + static_cast<std::ptrdiff_t>(offset),
                                              stream.begin() + static_cast<std::ptrdiff_t>(std::min(stream.size(), offset + 7777))));
    }

    for (int index = 0; index < 50; ++index)
    {
        PacketWriter writer;
        writer.Write<std::uint32_t>(static_cast<std::uint32_t>(index));
        CHECK(transport->Enqueue(detail::MakeNotification(1, writer.Release())));
    }

    Bytes output;
    const std::size_t expected = 50 * 14;
    while (output.size() < expected)
    {
        std::uint8_t buffer[4096];
        const auto result = ::read(outbound.read, buffer, sizeof(buffer));
        REQUIRE(result > 0);
        output.insert(output.end(), buffer, buffer + result);
    }
    for (int index = 0; index < 50; ++index)
    {
        PacketReader reader(Bytes(output.begin() + index * 14 + 10, output.begin() + index * 14 + 14));
        CHECK(reader.Read<std::uint32_t>() == static_cast<std::uint32_t>(index));
    }

    ::close(inbound.write);
    inbound.write = -1;
    while (!closed.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    transport->Join();
    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE(received.size() == 101);
    for (int index = 0; index < 100; ++index)
    {
        PacketReader reader(received[static_cast<std::size_t>(index)].body);
        CHECK(reader.Read<std::uint32_t>() == static_cast<std::uint32_t>(index));
    }
    CHECK(received[100].body == PatternBytes(3 * 1024 * 1024));
    CHECK(on_transport_thread.load());
    CHECK_FALSE(transport->Enqueue(detail::MakeNotification(1, {})));
}

TEST_CASE("Transport close interrupts a blocked reader and a blocked writer")
{
    Watchdog watchdog(std::chrono::seconds(20), "transport close");
    PipePair inbound;
    PipePair outbound;
    std::atomic<bool> closed = false;

    auto transport = std::make_unique<detail::Transport>(inbound.read, outbound.write);
    inbound.read = -1;
    outbound.write = -1;
    transport->Start([](detail::IncomingPacket&&) {},
                     [&]()
                     {
                         closed = true;
                     });

    const std::size_t body = 8 * 1024 * 1024;
    CHECK(transport->Enqueue(detail::MakePacket(detail::PacketType::Request, 2, 1, PatternBytes(body))));
    const std::size_t buffered = WaitUntilWriterBlocks(outbound.read);
    CHECK(buffered > 0);
    CHECK(buffered < body);
    CHECK_FALSE(closed.load());

    const auto start = std::chrono::steady_clock::now();
    transport->Close(detail::CloseMode::Immediate);
    transport->Join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(closed.load());
    CHECK(elapsed < std::chrono::seconds(1));
    CHECK(Buffered(outbound.read) == buffered);
}

TEST_CASE("Transport finishes a packet a drain close interrupts")
{
    Watchdog watchdog(std::chrono::seconds(30), "transport drain");
    PipePair inbound;
    PipePair outbound;

    auto transport = std::make_unique<detail::Transport>(inbound.read, outbound.write);
    inbound.read = -1;
    outbound.write = -1;
    transport->Start([](detail::IncomingPacket&&) {}, []() {});

    const std::size_t body = 1024 * 1024;
    const auto payload = PatternBytes(body, 11);
    CHECK(transport->Enqueue(detail::MakeNotification(250, payload)));
    const std::size_t buffered = WaitUntilWriterBlocks(outbound.read);
    CHECK(buffered > 0);
    CHECK(buffered < body);

    transport->Close(detail::CloseMode::Drain);

    Bytes output;
    const std::size_t expected = detail::kPacketHeaderSize + body;
    while (output.size() < expected)
    {
        pollfd fds{outbound.read, POLLIN, 0};
        if (::poll(&fds, 1, 2000) <= 0)
        {
            break;
        }

        std::uint8_t buffer[64 * 1024];
        const auto result = ::read(outbound.read, buffer, sizeof(buffer));
        if (result <= 0)
        {
            break;
        }
        output.insert(output.end(), buffer, buffer + result);
    }

    transport->Join();
    REQUIRE(output.size() == expected);
    CHECK(Bytes(output.begin() + detail::kPacketHeaderSize, output.end()) == payload);
}

TEST_CASE("Transport closes its handles when a transport thread joins it")
{
    Watchdog watchdog(std::chrono::seconds(30), "transport self join");
    const std::size_t before = OpenDescriptorCount();
    REQUIRE(before > 0);

    {
        PipePair inbound;
        PipePair outbound;
        auto transport = std::make_shared<detail::Transport>(inbound.read, outbound.write);
        inbound.read = -1;
        outbound.write = -1;

        std::atomic<bool> joined = false;
        std::atomic<bool> finished = false;
        transport->Start(
            [&](detail::IncomingPacket&&)
            {
                CHECK(detail::Transport::IsTransportThread());
                transport->Close(detail::CloseMode::Immediate);
                transport->Join();
                joined = true;
            },
            [&]()
            {
                finished = true;
            });

        WriteAllBlocking(inbound.write, detail::MakeNotification(250, {}).Flatten());
        while (!joined.load() || !finished.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    CHECK(OpenDescriptorCount() == before);
}

TEST_CASE("Transport closes the connection on an oversize packet")
{
    Watchdog watchdog(std::chrono::seconds(20), "transport oversize");
    PipePair inbound;
    PipePair outbound;
    std::atomic<bool> closed = false;
    std::atomic<int> packets = 0;

    auto transport = std::make_unique<detail::Transport>(inbound.read, outbound.write);
    inbound.read = -1;
    outbound.write = -1;
    transport->Start(
        [&](detail::IncomingPacket&&)
        {
            ++packets;
        },
        [&]()
        {
            closed = true;
        });

    Bytes header(10);
    const auto size = static_cast<std::uint32_t>(detail::kMaxIpcSize + 100);
    std::memcpy(header.data(), &size, sizeof(size));
    header[8] = 2;
    WriteAllBlocking(inbound.write, header);
    while (!closed.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    transport->Join();
    CHECK(packets.load() == 0);
}

#endif
