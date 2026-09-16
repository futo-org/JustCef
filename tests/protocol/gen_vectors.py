import json
import os
import struct

C2N = "controllerToNative"
N2C = "nativeToController"

TYPES = {0: "Request", 1: "Response", 2: "Notification", 3: "Cancel"}
STATUS = {0: "Ok", 1: "Error", 2: "Canceled", 3: "NotFound", 4: "NotHandled", 5: "Unsupported", 6: "ShuttingDown", 7: "TooLarge", 8: "InvalidRequest"}

CONTROLLER_OPS = {0: "Ping", 1: "Print", 2: "Echo", 3: "WindowCreate", 6: "WindowLoadUrl", 9: "WindowSetZoom", 15: "WindowSetPosition", 20: "WindowShow", 22: "WindowClose", 34: "WindowSetModifyRequests", 39: "PickFile", 41: "SaveFile", 42: "WindowExecuteDevToolsMethod", 44: "WindowSetTitle", 50: "WindowGetSize", 56: "WindowGetZoom", 57: "WindowBridgeRpc", 59: "GetWidevineStatus", 99: "Unknown"}
CONTROLLER_NOTIFICATIONS = {0: "Exit", 1: "StreamData", 2: "StreamEnd", 3: "StreamError"}
CLIENT_OPS = {2: "Echo", 3: "WindowProxyRequest", 4: "WindowModifyRequest", 9: "WindowBridgeRpc", 11: "WindowViewCreated"}
CLIENT_NOTIFICATIONS = {0: "Ready", 1: "Exit", 2: "WindowOpened", 3: "WindowClosed", 12: "WindowFullscreenChanged", 13: "WindowFrameLoadStart", 14: "WindowFrameLoadEnd", 15: "WindowFrameLoadError", 16: "WindowDevToolsEvent", 17: "WindowLoadingStateChanged", 18: "StreamCredit", 19: "StreamCancel", 250: "Debug"}


def enc(t, v):
    if t == "u8":
        return struct.pack("<B", v)
    if t == "bool":
        return struct.pack("<B", 1 if v else 0)
    if t == "i32":
        return struct.pack("<i", v)
    if t == "u32":
        return struct.pack("<I", v)
    if t == "i64":
        return struct.pack("<q", v)
    if t == "u64":
        return struct.pack("<Q", v)
    if t == "f64":
        return struct.pack("<d", v)
    if t == "str":
        if v is None:
            return struct.pack("<i", -1)
        b = v.encode("utf-8")
        return struct.pack("<i", len(b)) + b
    if t == "bytes":
        return struct.pack("<I", len(v)) + v
    if t == "rest":
        return v
    raise ValueError(t)


def opname(direction, ptype, opcode):
    if ptype == 2:
        table = CONTROLLER_NOTIFICATIONS if direction == C2N else CLIENT_NOTIFICATIONS
    else:
        request_dir = direction if ptype in (0, 3) else (N2C if direction == C2N else C2N)
        table = CONTROLLER_OPS if request_dir == C2N else CLIENT_OPS
    return table[opcode]


def fmt(t, v):
    if t in ("str",):
        if v is None:
            return "null"
        s = json.dumps(v, ensure_ascii=False)
        if any(ord(c) > 127 for c in v):
            cps = " ".join("U+%04X" % ord(c) for c in v)
            s += " (code points: %s; %d UTF-8 bytes)" % (cps, len(v.encode("utf-8")))
        return s
    if t in ("bytes", "rest"):
        try:
            txt = v.decode("ascii")
            if all(32 <= b < 127 for b in v):
                return "%s (%d bytes)" % (json.dumps(txt), len(v))
        except UnicodeDecodeError:
            pass
        return "hex %s (%d bytes)" % (v.hex(), len(v))
    if t == "bool":
        return "true" if v else "false"
    if t == "f64":
        return repr(v)
    return str(v)


vectors = []


def vec(name, direction, ptype, opcode, request_id, fields, status=None, note=None):
    body = b""
    if status is not None:
        body += enc("u8", status)
    for (_, t, v) in fields:
        body += enc(t, v)
    pkt = struct.pack("<IIBB", 6 + len(body), request_id, ptype, opcode) + body
    op = opname(direction, ptype, opcode)
    parts = ["%s %s (opcode %d), requestId %d, %s." % (TYPES[ptype], op, opcode, request_id, direction)]
    if status is not None:
        parts.append("status = %d (%s)." % (status, STATUS[status]))
    if fields:
        parts.append("Fields: " + "; ".join("%s (%s) = %s" % (n, t, fmt(t, v)) for (n, t, v) in fields) + ".")
    else:
        if status is None or status == 0:
            parts.append("Empty body." if status is None else "No payload.")
    if note:
        parts.append(note)
    vectors.append({
        "name": name,
        "direction": direction,
        "description": " ".join(parts),
        "hex": pkt.hex(),
        "_meta": {"type": ptype, "opcode": opcode, "requestId": request_id, "status": status, "fields": fields, "op": op, "note": note},
    })


REQ, RESP, NOTIF, CANCEL = 0, 1, 2, 3

vec("ready", N2C, NOTIF, 0, 0, [("protocolVersion", "u32", 2)])
vec("native_exit", N2C, NOTIF, 1, 0, [])
vec("controller_exit", C2N, NOTIF, 0, 0, [])
vec("cancel_window_create", C2N, CANCEL, 3, 7, [], note="Names the controller's WindowCreate request 7.")
vec("cancel_client_bridge_rpc", N2C, CANCEL, 9, 7, [], note="Names native's WindowBridgeRpc request 7.")

vec("ping", C2N, REQ, 0, 7, [])
vec("print", C2N, REQ, 1, 7, [("text", "rest", b"hello")])
vec("echo_request", C2N, REQ, 2, 7, [("data", "rest", b"ping")])
vec("echo_ok", N2C, RESP, 2, 7, [("data", "rest", b"ping")], status=0)

vec("response_ok_empty", N2C, RESP, 20, 7, [], status=0)
vec("response_error", N2C, RESP, 20, 7, [("message", "str", "boom")], status=1)
vec("response_canceled", N2C, RESP, 20, 7, [("message", "str", None)], status=2)
vec("response_not_found", N2C, RESP, 20, 7, [("message", "str", None)], status=3)
vec("response_not_handled", C2N, RESP, 3, 7, [("message", "str", None)], status=4, note="Controller declines a WindowProxyRequest.")
vec("response_unsupported", N2C, RESP, 99, 7, [("message", "str", "unknown opcode")], status=5)
vec("response_shutting_down", N2C, RESP, 20, 7, [("message", "str", None)], status=6)
vec("response_too_large", C2N, RESP, 2, 7, [("message", "str", None)], status=7, note="Controller answers native's Echo request.")
vec("response_invalid_request", N2C, RESP, 20, 7, [("message", "str", "truncated body")], status=8)

vec("set_title_null", C2N, REQ, 44, 7, [("browserId", "i32", 1), ("title", "str", None)])
vec("set_title_empty", C2N, REQ, 44, 7, [("browserId", "i32", 1), ("title", "str", "")])
vec("set_title_utf8", C2N, REQ, 44, 7, [("browserId", "i32", 1), ("title", "str", "Grüße 日本 \U00010348")])

vec("window_create", C2N, REQ, 3, 7, [
    ("resizable", "bool", True),
    ("frameless", "bool", False),
    ("fullscreen", "bool", False),
    ("centered", "bool", True),
    ("shown", "bool", True),
    ("contextMenuEnable", "bool", False),
    ("developerToolsEnabled", "bool", True),
    ("modifyRequests", "bool", False),
    ("modifyRequestBody", "bool", False),
    ("proxyRequests", "bool", True),
    ("logConsole", "bool", False),
    ("bridgeEnabled", "bool", True),
    ("minimumWidth", "i32", 320),
    ("minimumHeight", "i32", 240),
    ("preferredWidth", "i32", 1024),
    ("preferredHeight", "i32", 768),
    ("url", "str", "https://example.com/"),
    ("title", "str", "JustCef"),
    ("iconPath", "str", None),
    ("appId", "str", "com.futo.justcef"),
    ("viewsEnabled", "bool", True),
    ("modifyTimeoutMs", "u32", 5000),
    ("modifyTimeoutPolicy", "u8", 1),
    ("proxyOpenTimeoutMs", "u32", 30000),
])
vec("window_create_ok", N2C, RESP, 3, 7, [("browserId", "i32", 1)], status=0)
vec("load_url", C2N, REQ, 6, 7, [("browserId", "i32", 1), ("url", "str", "https://example.com/next")])
vec("window_get_size", C2N, REQ, 50, 7, [("browserId", "i32", 1)])
vec("window_get_size_ok", N2C, RESP, 50, 7, [("width", "i32", 800), ("height", "i32", 600)], status=0)
vec("window_set_position", C2N, REQ, 15, 7, [("browserId", "i32", 1), ("x", "i32", -100), ("y", "i32", 200)])
vec("window_set_zoom", C2N, REQ, 9, 7, [("browserId", "i32", 1), ("level", "f64", -1.5)])
vec("window_get_zoom_ok", N2C, RESP, 56, 7, [("level", "f64", 1.25)], status=0)
vec("window_close", C2N, REQ, 22, 7, [("browserId", "i32", 1), ("force", "bool", True)])
vec("set_modify_requests", C2N, REQ, 34, 7, [("browserId", "i32", 1), ("flags", "u8", 3)])

vec("pick_file", C2N, REQ, 39, 7, [
    ("browserId", "i32", 1),
    ("multiple", "bool", True),
    ("filterCount", "u32", 2),
    ("filters[0].name", "str", "Images"),
    ("filters[0].pattern", "str", "*.png;*.jpg"),
    ("filters[1].name", "str", "All files"),
    ("filters[1].pattern", "str", "*"),
])
vec("pick_file_ok", N2C, RESP, 39, 7, [("pathCount", "u32", 2), ("paths[0]", "str", "/home/user/a.png"), ("paths[1]", "str", "/home/user/b.jpg")], status=0)
vec("save_file", C2N, REQ, 41, 7, [("browserId", "i32", 1), ("defaultName", "str", "report.pdf"), ("filterCount", "u32", 1), ("filters[0].name", "str", "PDF"), ("filters[0].pattern", "str", "*.pdf")])
vec("save_file_ok", N2C, RESP, 41, 7, [("path", "str", "/home/user/report.pdf")], status=0)

vec("execute_devtools_method", C2N, REQ, 42, 7, [("browserId", "i32", 1), ("method", "str", "Browser.getVersion"), ("hasParams", "bool", False)])
vec("execute_devtools_method_params", C2N, REQ, 42, 7, [("browserId", "i32", 1), ("method", "str", "Page.navigate"), ("hasParams", "bool", True), ("paramsJson", "bytes", b'{"url":"about:blank"}')])
vec("execute_devtools_method_ok", N2C, RESP, 42, 7, [("success", "bool", True), ("result", "bytes", b"{}")], status=0)
vec("widevine_status_ok", N2C, RESP, 59, 7, [("state", "i32", 2), ("version", "str", "4.10.2830.0"), ("registered", "bool", True), ("installed", "bool", True), ("requiresRestart", "bool", False)], status=0)

vec("bridge_rpc_request", C2N, REQ, 57, 7, [("browserId", "i32", 1), ("method", "str", "add"), ("json", "bytes", b"[1,2]")])
vec("bridge_rpc_ok", N2C, RESP, 57, 7, [("json", "bytes", b"3")], status=0)
vec("client_bridge_rpc_request", N2C, REQ, 9, 7, [("browserId", "i32", 1), ("method", "str", "getUser"), ("json", "bytes", b'{"id":42}')])
vec("client_bridge_rpc_ok", C2N, RESP, 9, 7, [("json", "bytes", b'{"name":"Ada"}')], status=0)
vec("client_bridge_rpc_error", C2N, RESP, 9, 7, [("message", "str", "no such user")], status=1)

vec("view_created_request", N2C, REQ, 11, 7, [("parentId", "i32", 1), ("viewId", "i32", 2), ("src", "str", "https://example.com/view")])
vec("view_created_ok", C2N, RESP, 11, 7, [("allow", "bool", True)], status=0)

vec("native_echo_request", N2C, REQ, 2, 7, [("data", "rest", bytes([0x00, 0x01, 0x02, 0xFF]))])
vec("native_echo_ok", C2N, RESP, 2, 7, [("data", "rest", bytes([0x00, 0x01, 0x02, 0xFF]))], status=0)

vec("proxy_request", N2C, REQ, 3, 7, [
    ("browserId", "i32", 1),
    ("method", "str", "POST"),
    ("url", "str", "https://example.com/api"),
    ("headerCount", "i32", 2),
    ("headers[0].name", "str", "Content-Type"),
    ("headers[0].value", "str", "application/json"),
    ("headers[1].name", "str", "Accept"),
    ("headers[1].value", "str", "*/*"),
    ("elementCount", "i32", 2),
    ("elements[0].type", "u8", 1),
    ("elements[0].data", "bytes", b'{"a":1}'),
    ("elements[1].type", "u8", 2),
    ("elements[1].path", "str", "/tmp/upload.bin"),
])
vec("proxy_response_no_body", C2N, RESP, 3, 7, [
    ("statusCode", "u32", 204),
    ("statusText", "str", "No Content"),
    ("headerCount", "u32", 0),
    ("bodyType", "u8", 0),
], status=0)
vec("proxy_response_inline", C2N, RESP, 3, 7, [
    ("statusCode", "u32", 200),
    ("statusText", "str", "OK"),
    ("headerCount", "u32", 1),
    ("headers[0].name", "str", "Content-Type"),
    ("headers[0].value", "str", "text/plain"),
    ("bodyType", "u8", 1),
    ("data", "bytes", b"hello"),
], status=0)
vec("proxy_response_stream", C2N, RESP, 3, 7, [
    ("statusCode", "u32", 200),
    ("statusText", "str", "OK"),
    ("headerCount", "u32", 1),
    ("headers[0].name", "str", "Content-Type"),
    ("headers[0].value", "str", "application/octet-stream"),
    ("bodyType", "u8", 2),
    ("length", "i64", 5000000),
    ("streamId", "u32", 1),
], status=0)
vec("proxy_response_stream_unknown_length", C2N, RESP, 3, 7, [
    ("statusCode", "u32", 200),
    ("statusText", "str", "OK"),
    ("headerCount", "u32", 0),
    ("bodyType", "u8", 2),
    ("length", "i64", -1),
    ("streamId", "u32", 2),
], status=0)

vec("modify_request", N2C, REQ, 4, 7, [
    ("browserId", "i32", 1),
    ("method", "str", "GET"),
    ("url", "str", "https://example.com/"),
    ("headerCount", "i32", 1),
    ("headers[0].name", "str", "User-Agent"),
    ("headers[0].value", "str", "JustCef"),
    ("elementCount", "i32", 0),
])
vec("modify_response", C2N, RESP, 4, 7, [
    ("method", "str", "POST"),
    ("url", "str", "https://example.com/changed"),
    ("headerCount", "u32", 2),
    ("headers[0].name", "str", "User-Agent"),
    ("headers[0].value", "str", "Modified"),
    ("headers[1].name", "str", "X-Added"),
    ("headers[1].value", "str", "1"),
    ("elementCount", "u32", 2),
    ("elements[0].type", "u8", 1),
    ("elements[0].data", "bytes", b"abc"),
    ("elements[1].type", "u8", 2),
    ("elements[1].path", "str", "/tmp/body.txt"),
], status=0)

vec("stream_data", C2N, NOTIF, 1, 0, [("streamId", "u32", 1), ("data", "rest", b"0123456789")])
vec("stream_end", C2N, NOTIF, 2, 0, [("streamId", "u32", 1), ("totalBytes", "u64", 10)])
vec("stream_error", C2N, NOTIF, 3, 0, [("streamId", "u32", 1), ("message", "str", "source failed")])
vec("stream_credit", N2C, NOTIF, 18, 0, [("streamId", "u32", 1), ("bytes", "u32", 65536)])
vec("stream_cancel", N2C, NOTIF, 19, 0, [("streamId", "u32", 1)])

vec("window_opened", N2C, NOTIF, 2, 0, [("browserId", "i32", 1)])
vec("window_closed", N2C, NOTIF, 3, 0, [("browserId", "i32", 1)])
vec("fullscreen_changed", N2C, NOTIF, 12, 0, [("browserId", "i32", 1), ("fullscreen", "bool", True)])
vec("loading_state_changed", N2C, NOTIF, 17, 0, [("browserId", "i32", 1), ("isLoading", "bool", True), ("canGoBack", "bool", False), ("canGoForward", "bool", True)])
vec("frame_load_start", N2C, NOTIF, 13, 0, [("browserId", "i32", 1), ("frameId", "str", "main"), ("isMain", "bool", True), ("url", "str", "https://example.com/")])
vec("frame_load_end", N2C, NOTIF, 14, 0, [("browserId", "i32", 1), ("frameId", "str", "main"), ("isMain", "bool", True), ("url", "str", "https://example.com/"), ("httpStatusCode", "i32", 200)])
vec("frame_load_error", N2C, NOTIF, 15, 0, [("browserId", "i32", 1), ("frameId", "str", "main"), ("isMain", "bool", True), ("errorCode", "i32", -105), ("errorText", "str", "net::ERR_NAME_NOT_RESOLVED"), ("url", "str", "https://nonexistent.invalid/")])
vec("devtools_event", N2C, NOTIF, 16, 0, [("browserId", "i32", 1), ("method", "str", "Network.requestWillBeSent"), ("params", "bytes", b'{"requestId":"1"}')])
vec("debug_notification", N2C, NOTIF, 250, 0, [("sequence", "u32", 42)])

here = os.path.dirname(os.path.abspath(__file__))
out_json = os.path.join(here, "vectors.json")
with open(out_json, "w", encoding="utf-8", newline="\n") as f:
    f.write("[\n")
    for i, v in enumerate(vectors):
        obj = {"name": v["name"], "direction": v["direction"], "description": v["description"], "hex": v["hex"]}
        f.write("  " + json.dumps(obj, ensure_ascii=False))
        f.write(",\n" if i < len(vectors) - 1 else "\n")
    f.write("]\n")

print("%d vectors" % len(vectors))
