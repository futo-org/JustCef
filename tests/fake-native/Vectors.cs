namespace FakeNative;

public sealed record Vector(string Name, string Direction, Func<ReadOnlyMemory<byte>> Build);

public static class Vectors
{
    public const string ControllerToNative = "controllerToNative";
    public const string NativeToController = "nativeToController";

    private const string C2N = ControllerToNative;
    private const string N2C = NativeToController;

    private static ReadOnlyMemory<byte> Req(ControllerOp op, Action<PacketBuilder>? body = null)
    {
        var b = new PacketBuilder(PacketType.Request, (byte)op, 7);
        body?.Invoke(b);
        return b.Finish();
    }

    private static ReadOnlyMemory<byte> ControllerResponse(ClientOp op, Status status, Action<PacketBuilder>? payload = null)
    {
        var b = new PacketBuilder(PacketType.Response, (byte)op, 7);
        b.U8((byte)status);
        payload?.Invoke(b);
        return b.Finish();
    }

    private static ReadOnlyMemory<byte> ControllerNotify(ControllerNotification op, Action<PacketBuilder>? body = null)
    {
        var b = new PacketBuilder(PacketType.Notification, (byte)op, 0);
        body?.Invoke(b);
        return b.Finish();
    }

    private static HttpRequestData ProxyRequestData()
    {
        var req = new HttpRequestData { Method = "POST", Url = "https://example.com/api" };
        req.Headers.Add(new HttpHeader("Content-Type", "application/json"));
        req.Headers.Add(new HttpHeader("Accept", "*/*"));
        req.Elements.Add(new BodyElement { Type = 1, Data = "{\"a\":1}"u8.ToArray() });
        req.Elements.Add(new BodyElement { Type = 2, Path = "/tmp/upload.bin" });
        return req;
    }

    private static HttpRequestData ModifyRequestData()
    {
        var req = new HttpRequestData { Method = "GET", Url = "https://example.com/" };
        req.Headers.Add(new HttpHeader("User-Agent", "JustCef"));
        return req;
    }

    public static readonly IReadOnlyList<Vector> All = new List<Vector>
    {
        new("ready", N2C, () => Encode.Ready(2)),
        new("native_exit", N2C, () => Encode.Exit()),
        new("controller_exit", C2N, () => ControllerNotify(ControllerNotification.Exit)),
        new("cancel_window_create", C2N, () => new PacketBuilder(PacketType.Cancel, (byte)ControllerOp.WindowCreate, 7, 0).Finish()),
        new("cancel_client_bridge_rpc", N2C, () => Encode.Cancel(7, (byte)ClientOp.WindowBridgeRpc)),

        new("ping", C2N, () => Req(ControllerOp.Ping)),
        new("print", C2N, () => Req(ControllerOp.Print, b => b.Raw("hello"u8))),
        new("echo_request", C2N, () => Req(ControllerOp.Echo, b => b.Raw("ping"u8))),
        new("echo_ok", N2C, () => Encode.ResponseOk(7, (byte)ControllerOp.Echo, b => b.Raw("ping"u8))),

        new("response_ok_empty", N2C, () => Encode.ResponseOk(7, (byte)ControllerOp.WindowShow)),
        new("response_error", N2C, () => Encode.Response(7, (byte)ControllerOp.WindowShow, Status.Error, "boom")),
        new("response_canceled", N2C, () => Encode.Response(7, (byte)ControllerOp.WindowShow, Status.Canceled, null)),
        new("response_not_found", N2C, () => Encode.Response(7, (byte)ControllerOp.WindowShow, Status.NotFound, null)),
        new("response_not_handled", C2N, () => ControllerResponse(ClientOp.WindowProxyRequest, Status.NotHandled, b => b.Str(null))),
        new("response_unsupported", N2C, () => Encode.Response(7, 99, Status.Unsupported, "unknown opcode")),
        new("response_shutting_down", N2C, () => Encode.Response(7, (byte)ControllerOp.WindowShow, Status.ShuttingDown, null)),
        new("response_too_large", C2N, () => ControllerResponse(ClientOp.Echo, Status.TooLarge, b => b.Str(null))),
        new("response_invalid_request", N2C, () => Encode.Response(7, (byte)ControllerOp.WindowShow, Status.InvalidRequest, "truncated body")),

        new("set_title_null", C2N, () => Req(ControllerOp.WindowSetTitle, b => b.I32(1).Str(null))),
        new("set_title_empty", C2N, () => Req(ControllerOp.WindowSetTitle, b => b.I32(1).Str(""))),
        new("set_title_utf8", C2N, () => Req(ControllerOp.WindowSetTitle, b => b.I32(1).Str("Grüße 日本 \U00010348"))),

        new("window_create", C2N, () => Req(ControllerOp.WindowCreate, b => b
            .Bool(true).Bool(false).Bool(false).Bool(true).Bool(true).Bool(false)
            .Bool(true).Bool(false).Bool(false).Bool(true).Bool(false).Bool(true)
            .I32(320).I32(240).I32(1024).I32(768)
            .Str("https://example.com/").Str("JustCef").Str(null).Str("com.futo.justcef")
            .Bool(true).U32(5000).U8(1).U32(30000))),
        new("window_create_ok", N2C, () => Encode.ResponseOk(7, (byte)ControllerOp.WindowCreate, b => b.I32(1))),
        new("load_url", C2N, () => Req(ControllerOp.WindowLoadUrl, b => b.I32(1).Str("https://example.com/next"))),
        new("window_get_size", C2N, () => Req(ControllerOp.WindowGetSize, b => b.I32(1))),
        new("window_get_size_ok", N2C, () => Encode.ResponseOk(7, (byte)ControllerOp.WindowGetSize, b => b.I32(800).I32(600))),
        new("window_set_position", C2N, () => Req(ControllerOp.WindowSetPosition, b => b.I32(1).I32(-100).I32(200))),
        new("window_set_zoom", C2N, () => Req(ControllerOp.WindowSetZoom, b => b.I32(1).F64(-1.5))),
        new("window_get_zoom_ok", N2C, () => Encode.ResponseOk(7, (byte)ControllerOp.WindowGetZoom, b => b.F64(1.25))),
        new("window_close", C2N, () => Req(ControllerOp.WindowClose, b => b.I32(1).Bool(true))),
        new("set_modify_requests", C2N, () => Req(ControllerOp.WindowSetModifyRequests, b => b.I32(1).U8(3))),

        new("pick_file", C2N, () => Req(ControllerOp.PickFile, b => b.I32(1).Bool(true).U32(2).Str("Images").Str("*.png;*.jpg").Str("All files").Str("*"))),
        new("pick_file_ok", N2C, () => Encode.ResponseOk(7, (byte)ControllerOp.PickFile, b => b.U32(2).Str("/home/user/a.png").Str("/home/user/b.jpg"))),
        new("save_file", C2N, () => Req(ControllerOp.SaveFile, b => b.I32(1).Str("report.pdf").U32(1).Str("PDF").Str("*.pdf"))),
        new("save_file_ok", N2C, () => Encode.ResponseOk(7, (byte)ControllerOp.SaveFile, b => b.Str("/home/user/report.pdf"))),

        new("execute_devtools_method", C2N, () => Req(ControllerOp.WindowExecuteDevToolsMethod, b => b.I32(1).Str("Browser.getVersion").Bool(false))),
        new("execute_devtools_method_params", C2N, () => Req(ControllerOp.WindowExecuteDevToolsMethod, b => b.I32(1).Str("Page.navigate").Bool(true).Bytes("{\"url\":\"about:blank\"}"u8))),
        new("execute_devtools_method_ok", N2C, () => Encode.ResponseOk(7, (byte)ControllerOp.WindowExecuteDevToolsMethod, b => b.Bool(true).Bytes("{}"u8))),
        new("widevine_status_ok", N2C, () => Encode.ResponseOk(7, (byte)ControllerOp.GetWidevineStatus, b => b.I32(2).Str("4.10.2830.0").Bool(true).Bool(true).Bool(false))),

        new("bridge_rpc_request", C2N, () => Req(ControllerOp.WindowBridgeRpc, b => b.I32(1).Str("add").Bytes("[1,2]"u8))),
        new("bridge_rpc_ok", N2C, () => Encode.ResponseOk(7, (byte)ControllerOp.WindowBridgeRpc, b => b.Bytes("3"u8))),
        new("client_bridge_rpc_request", N2C, () => Encode.BridgeRpc(7, 1, "getUser", "{\"id\":42}"u8.ToArray())),
        new("client_bridge_rpc_ok", C2N, () => ControllerResponse(ClientOp.WindowBridgeRpc, Status.Ok, b => b.Bytes("{\"name\":\"Ada\"}"u8))),
        new("client_bridge_rpc_error", C2N, () => ControllerResponse(ClientOp.WindowBridgeRpc, Status.Error, b => b.Str("no such user"))),

        new("view_created_request", N2C, () => Encode.ViewCreated(7, 1, 2, "https://example.com/view")),
        new("view_created_ok", C2N, () => ControllerResponse(ClientOp.WindowViewCreated, Status.Ok, b => b.Bool(true))),

        new("native_echo_request", N2C, () => Encode.EchoRequest(7, new byte[] { 0x00, 0x01, 0x02, 0xFF })),
        new("native_echo_ok", C2N, () => ControllerResponse(ClientOp.Echo, Status.Ok, b => b.Raw(new byte[] { 0x00, 0x01, 0x02, 0xFF }))),

        new("proxy_request", N2C, () => Encode.ProxyRequest(7, 1, ProxyRequestData())),
        new("proxy_response_no_body", C2N, () => ControllerResponse(ClientOp.WindowProxyRequest, Status.Ok, b => b
            .U32(204).Str("No Content").U32(0).U8(0))),
        new("proxy_response_inline", C2N, () => ControllerResponse(ClientOp.WindowProxyRequest, Status.Ok, b => b
            .U32(200).Str("OK").U32(1).Str("Content-Type").Str("text/plain").U8(1).Bytes("hello"u8))),
        new("proxy_response_stream", C2N, () => ControllerResponse(ClientOp.WindowProxyRequest, Status.Ok, b => b
            .U32(200).Str("OK").U32(1).Str("Content-Type").Str("application/octet-stream").U8(2).I64(5000000).U32(1))),
        new("proxy_response_stream_unknown_length", C2N, () => ControllerResponse(ClientOp.WindowProxyRequest, Status.Ok, b => b
            .U32(200).Str("OK").U32(0).U8(2).I64(-1).U32(2))),

        new("modify_request", N2C, () => Encode.ModifyRequest(7, 1, ModifyRequestData())),
        new("modify_response", C2N, () => ControllerResponse(ClientOp.WindowModifyRequest, Status.Ok, b => b
            .Str("POST").Str("https://example.com/changed")
            .U32(2).Str("User-Agent").Str("Modified").Str("X-Added").Str("1")
            .U32(2).U8(1).Bytes("abc"u8).U8(2).Str("/tmp/body.txt"))),

        new("stream_data", C2N, () => ControllerNotify(ControllerNotification.StreamData, b => b.U32(1).Raw("0123456789"u8))),
        new("stream_end", C2N, () => ControllerNotify(ControllerNotification.StreamEnd, b => b.U32(1).U64(10))),
        new("stream_error", C2N, () => ControllerNotify(ControllerNotification.StreamError, b => b.U32(1).Str("source failed"))),
        new("stream_credit", N2C, () => Encode.StreamCredit(1, 65536)),
        new("stream_cancel", N2C, () => Encode.StreamCancel(1)),

        new("window_opened", N2C, () => Encode.WindowOpened(1)),
        new("window_closed", N2C, () => Encode.WindowClosed(1)),
        new("fullscreen_changed", N2C, () => Encode.FullscreenChanged(1, true)),
        new("loading_state_changed", N2C, () => Encode.LoadingStateChanged(1, true, false, true)),
        new("frame_load_start", N2C, () => Encode.FrameLoadStart(1, "main", true, "https://example.com/")),
        new("frame_load_end", N2C, () => Encode.FrameLoadEnd(1, "main", true, "https://example.com/", 200)),
        new("frame_load_error", N2C, () => Encode.FrameLoadError(1, "main", true, -105, "net::ERR_NAME_NOT_RESOLVED", "https://nonexistent.invalid/")),
        new("devtools_event", N2C, () => Encode.DevToolsEvent(1, "Network.requestWillBeSent", "{\"requestId\":\"1\"}"u8.ToArray())),
        new("debug_notification", N2C, () => Encode.Debug(42))
    };
}
