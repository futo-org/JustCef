import json
import os
import sys
import threading
import time
import traceback
import tempfile
import subprocess
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jcipc import *

PAGES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pages")

with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "protocol", "vectors.json"), encoding="utf-8") as f:
    VECTORS = {v["name"]: bytes.fromhex(v["hex"]) for v in json.load(f)}

PAGE = b"""<!doctype html><title>t</title><script>
bridge.rpc.register('run', async (p) => { return await (0, eval)(p.code); });
</script><body>native tests</body>"""

BIG = bytes((i * 131) & 0xFF for i in range(50 * 1024 * 1024))
SLOW = bytes(8 * 1024 * 1024)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def evaljs(ctl, wid, code, timeout=60):
    st, payload = ctl.result(ctl.call(C["BridgeRpc"], W().i32(wid).s("run").by(json.dumps({"code": code}).encode()).b), timeout=timeout)
    check(st == OK, "bridge eval failed: %r" % (payload,))
    return json.loads(Rd(payload).by())


def devtools_eval(ctl, wid, expression):
    params = json.dumps(dict(expression=expression, awaitPromise=True, returnByValue=True)).encode()
    st, payload = ctl.result(ctl.call(C["ExecuteDevTools"], W().i32(wid).s("Runtime.evaluate").b1(True).by(params).b))
    check(st == OK, "DevTools request failed: %r" % (payload,))
    reader = Rd(payload)
    check(reader.b1(), "Runtime.evaluate failed")
    result = json.loads(reader.by())
    check("exceptionDetails" not in result, "JavaScript failed: %r" % result)
    return result.get("result", {}).get("value")


def open_app(native_args=None, **kw):
    ctl = Ctl(native_args)
    check(ctl.wait_note("Ready").u32() == 2, "protocol version")
    streams = {}
    counter = [100]

    def on_proxy(r):
        wid, method, url, headers, elements = read_request(r)
        if url.endswith("/big"):
            counter[0] += 1
            sid = counter[0]
            threading.Timer(0.01, lambda: streams.setdefault(sid, ctl.stream_body(sid, BIG))).start()
            return proxy_response(200, {"Content-Type": "application/octet-stream"}, stream_id=sid, length=len(BIG))
        if "/slow" in url:
            counter[0] += 1
            sid = counter[0]
            threading.Timer(0.01, lambda: streams.setdefault(sid, ctl.stream_body(sid, SLOW, chunk=64 * 1024, delay=0.05))).start()
            return proxy_response(200, {"Content-Type": "application/octet-stream"}, stream_id=sid, length=len(SLOW))
        if "/post" in url:
            return proxy_response(200, {"Content-Type": "text/plain"}, str(sum(len(e[1]) for e in elements if e[0] == 1)).encode())
        if "/item" in url:
            return proxy_response(200, {"Content-Type": "text/plain"}, b"item")
        return proxy_response(200, {"Content-Type": "text/html"}, PAGE)

    ctl.handlers[R["Proxy"]] = on_proxy
    wid = ctl.create("http://app.test/index.html", proxy=True, **kw)
    ctl.wait_note("LoadingState", lambda b: b.i32() == wid and not b.b1(), timeout=20)
    return ctl, wid, streams


def shutdown(ctl):
    ctl.notify(CN["Exit"])
    check(ctl.closed.wait(15), "native did not close the pipe")
    code = ctl.proc.wait(15)
    ctl.close()
    check(code == 0, "exit code %r" % code)


def test_protocol_vectors():
    built = {
        "ping": packet(REQ, C["Ping"], 7),
        "controller_exit": packet(NOTE, CN["Exit"], 0),
        "window_get_size": packet(REQ, C["GetSize"], 7, W().i32(1).b),
        "load_url": packet(REQ, C["LoadUrl"], 7, W().i32(1).s("https://example.com/next").b),
        "stream_data": packet(NOTE, CN["StreamData"], 0, W().u32(1).b + b"0123456789"),
        "stream_end": packet(NOTE, CN["StreamEnd"], 0, W().u32(1).u64(10).b),
        "proxy_response_inline": packet(RESP, R["Proxy"], 7, bytes([OK]) + proxy_response(200, {"Content-Type": "text/plain"}, b"hello")[1]),
        "proxy_response_stream": packet(RESP, R["Proxy"], 7, bytes([OK]) + proxy_response(200, {"Content-Type": "application/octet-stream"}, stream_id=1, length=5000000)[1]),
    }
    for name, pkt in built.items():
        check(pkt == VECTORS[name], "%s: harness %s, vector %s" % (name, pkt.hex(), VECTORS[name].hex()))
    ctl = Ctl()
    ctl.wait_note("Ready")
    check(ctl.raw.get((NOTE, 0)) == VECTORS["ready"], "ready: native %r, vector %s" % (ctl.raw.get((NOTE, 0)), VECTORS["ready"].hex()))
    st, _ = ctl.result(ctl.call(C["Ping"]))
    check(st == OK, "native accepts the harness Ping encoding")
    shutdown(ctl)


def test_ordering_and_basics():
    ctl, wid, _ = open_app()
    create = next(i for i, e in enumerate(ctl.order) if e[0] == RESP and e[1] == C["WindowCreate"])
    proxy = next(i for i, e in enumerate(ctl.order) if e[0] == REQ and e[1] == R["Proxy"])
    opened = next(i for i, e in enumerate(ctl.order) if e[0] == NOTE and e[1] == 2)
    check(create < opened and create < proxy, "create reply must precede traffic for the new browser")
    states = [e for e in ctl.order if e[0] == NOTE and e[1] == 17]
    check(len(states) >= 2, "loading notifications")
    st, payload = ctl.result(ctl.call(C["GetSize"], W().i32(999).b))
    check(st == NOTFOUND, "unknown browser")
    st, payload = ctl.result(ctl.call(77, b""))
    check(st == 5, "unknown opcode")
    st, payload = ctl.result(ctl.call(C["ExecuteDevTools"], W().i32(wid).s("Runtime.evaluate").b1(True).by(b'{"expression":"\'x\'.repeat(20000000)","returnByValue":true}').b), timeout=60)
    reader = Rd(payload)
    check(st == OK and reader.b1() and len(reader.by()) > 20000000, "20 MB DevTools result")
    shutdown(ctl)
    names = [N.get(e[1]) for e in ctl.order if e[0] == NOTE]
    check(names[-2:] == ["WindowClosed", "Exit"], "WindowClosed then Exit, got %r" % names[-2:])


def test_bridge_calls_that_call_native():
    ctl, wid, _ = open_app()

    def on_bridge(r):
        target = r.i32()
        r.s()
        payload = json.loads(r.by())
        st1, _ = ctl.result(ctl.call(C["GetSize"], W().i32(target).b), timeout=10)
        st2, _ = ctl.result(ctl.call(C["ExecuteDevTools"], W().i32(target).s("Runtime.evaluate").b1(True).by(b'{"expression":"1+1","returnByValue":true}').b), timeout=10)
        return OK, W().by(json.dumps({"i": payload["i"], "st": [st1, st2]}).encode()).b

    ctl.handlers[R["BridgeRpc"]] = on_bridge
    result = evaljs(ctl, wid, "Promise.all(Array.from({length: 64}, (_, i) => bridge.rpc.call('work', {i})))")
    check(len(result) == 64 and all(x["st"] == [0, 0] for x in result), "64 concurrent bridge calls")
    shutdown(ctl)


def test_large_bridge_payloads():
    ctl, wid, _ = open_app()
    ctl.handlers[R["BridgeRpc"]] = lambda r: (r.i32(), r.s(), (OK, W().by(json.dumps({"len": len(r.by())}).encode()).b))[2]
    result = evaljs(ctl, wid, "bridge.rpc.call('big', {s: 'y'.repeat(20000000)})", timeout=90)
    check(result["len"] > 20000000, "20 MB JS to controller")
    check(evaljs(ctl, wid, "'z'.repeat(20000000).length") == 20000000, "20 MB controller to JS")
    shutdown(ctl)


def test_async_modify():
    ctl, wid, _ = open_app(modify=True)

    def on_modify(r):
        target, method, url, headers, elements = read_request(r)
        if "/item/slow" in url and "m=1" not in url:
            time.sleep(5)
        w = W().s(method).s(url if "m=1" in url else url + ("&m=1" if "?" in url else "?m=1")).u32(len(headers))
        for k, v in headers:
            w.s(k).s(v)
        w.u32(0)
        return OK, bytes(w.b)

    ctl.handlers[R["Modify"]] = on_modify
    result = evaljs(ctl, wid, """(async () => {
      const t0 = performance.now();
      const slow = fetch('http://app.test/item/slow').then(r => r.text()).then(() => performance.now() - t0);
      await Promise.all(Array.from({length: 20}, (_, i) => fetch('http://app.test/item/' + i).then(r => r.text())));
      const fast = performance.now() - t0;
      const post = await fetch('http://app.test/post', {method: 'POST', body: new Uint8Array(20 * 1024 * 1024)}).then(r => r.text());
      return {fast, slow: await slow, post};
    })()""")
    check(result["fast"] < 3000, "modify must not block other requests: %r" % result)
    check(result["slow"] >= 5000, "slow modifier applied: %r" % result)
    check(result["post"] == str(20 * 1024 * 1024), "20 MB POST body")
    shutdown(ctl)


def test_streams_and_cancel():
    ctl, wid, streams = open_app()
    result = evaljs(ctl, wid, """(async () => {
      const u = new Uint8Array(await fetch('http://app.test/big').then(r => r.arrayBuffer()));
      let ok = u.length === 50 * 1024 * 1024;
      for (let i = 0; i < u.length; i += 4099) if (u[i] !== ((i * 131) & 255)) { ok = false; break; }
      return ok;
    })()""", timeout=120)
    check(result is True, "50 MB streamed body")
    evaljs(ctl, wid, """(async () => {
      const ctrls = [];
      const ps = [];
      for (let i = 0; i < 100; i++) {
        const c = new AbortController(); ctrls.push(c);
        ps.push(fetch('http://app.test/slow' + i, {signal: c.signal}).then(r => r.body.getReader().read()).catch(e => e.name));
      }
      await new Promise(r => setTimeout(r, 400));
      ctrls.forEach(c => c.abort());
      return (await Promise.allSettled(ps)).length;
    })()""")
    time.sleep(2)
    stopped = sum(1 for v in streams.values() if v == "canceled")
    check(len(ctl.canceled_streams) >= 100 and stopped >= 100, "canceled streams %d, stopped pumps %d" % (len(ctl.canceled_streams), stopped))
    shutdown(ctl)


def test_backpressure_and_threads():
    ctl, wid, _ = open_app()
    ctl.paused.clear()
    flood = ctl.call(C["Debug"], W().u8(0).u32(200000).b)
    time.sleep(3)
    ctl.paused.set()
    st, _ = ctl.result(flood, timeout=60)
    check(st == OK and sum(1 for e in ctl.order if e[0] == NOTE and e[1] == 250) == 200000, "flood delivered")
    st, payload = ctl.result(ctl.call(C["Debug"], W().u8(2).u32(64).b), timeout=30)
    check(st == OK and Rd(payload).u32() == 64, "concurrent calls from the IO thread")
    blocked = ctl.call(C["Debug"], W().u8(1).u32(1500).b)
    started = time.time()
    st, _ = ctl.result(ctl.call(C["Ping"]), timeout=10)
    check(st == OK and time.time() - started < 1.0, "loop requests answered while the UI thread is busy")
    ctl.result(blocked, timeout=10)
    shutdown(ctl)


def test_shutdown_with_pending_work():
    ctl, wid, _ = open_app()
    ctl.handlers[R["BridgeRpc"]] = lambda r: (time.sleep(30), (OK, W().by(b"1").b))[1]
    threading.Thread(target=lambda: ctl.call(C["BridgeRpc"], W().i32(wid).s("run").by(json.dumps({"code": "Promise.all([1,2,3,4].map(i => bridge.rpc.call('slow', {i})))"}).encode()).b), daemon=True).start()
    ctl.call(C["ExecuteDevTools"], W().i32(wid).s("Runtime.evaluate").b1(True).by(b'{"expression":"new Promise(()=>{})","awaitPromise":true}').b)
    time.sleep(1.5)
    started = time.time()
    shutdown(ctl)
    check(time.time() - started < 5, "shutdown is prompt")


def drag_regions(ctl, wid):
    st, payload = ctl.result(ctl.call(C["Debug"], W().u8(3).u32(wid).b))
    check(st == OK, "drag region inspection failed")
    r = Rd(payload)
    return [(r.i32(), r.i32(), r.i32(), r.i32(), r.b1()) for _ in range(r.u32())]


def wait_drag_regions(ctl, wid, predicate):
    deadline = time.time() + 10
    while time.time() < deadline:
        regions = drag_regions(ctl, wid)
        if predicate(regions):
            return regions
        time.sleep(0.02)
    raise AssertionError("unexpected drag regions: %r" % (regions,))


def window_state(ctl, wid):
    status, payload = ctl.result(ctl.call(C["Debug"], W().u8(4).u32(wid).b))
    check(status == OK, "window state inspection")
    r = Rd(payload)
    return dict(fullscreen=r.b1(), native_fullscreen=r.b1(), transitioning=r.b1(), transitions=r.u32(), drag_files=r.u32())


def wait_fullscreen(ctl, wid, target, previous):
    deadline = time.time() + 15
    while time.time() < deadline:
        state = window_state(ctl, wid)
        if state["fullscreen"] == target and not state["transitioning"] and state["transitions"] > previous:
            return
        time.sleep(0.05)
    raise AssertionError("fullscreen transition did not complete: %r" % state)


def test_runtime_info():
    ctl = Ctl()
    ctl.wait_note("Ready")
    status, payload = ctl.result(ctl.call(C["GetRuntimeInfo"], b""))
    check(status == OK and len(payload) == 2, "runtime information response")
    check(payload[0] == (3 if sys.platform.startswith("linux") else 2), "native operating system")
    check(payload[1] in (0, 1, 2), "known Linux backend")
    if payload[0] != 3:
        check(payload[1] == 0, "Linux backend not applicable")
    expected = os.environ.get("JUSTCEF_TEST_EXPECT_LINUX_BACKEND")
    if expected:
        check(payload[1] == {"unknown": 0, "x11": 1, "wayland": 2}[expected], "active Linux backend")
    check(ctl.result(ctl.call(C["GetRuntimeInfo"], b"\0"))[0] == 8, "runtime info rejects request payload")
    shutdown(ctl)


def test_file_paths():

    ctl, wid, _ = open_app()
    try:
        check(evaljs(ctl, wid, "[new File(['x'], 'synthetic.txt'), new Blob(['x']), {}, [], null, undefined, 'x', 1, true].map(v => bridge.files.getPath(v))") == [None] * 9,
              "values without backing files return null")
        check(evaljs(ctl, wid, "[[], [1, 2]].every(args => { try { bridge.files.getPath(...args); return false; } catch (e) { return e.message.includes('exactly one argument'); } })"),
              "file path argument validation")
        with tempfile.TemporaryDirectory(prefix="justcef-file-path-") as directory:
            path = os.path.join(directory, "spaced café 文件.txt")
            with open(path, "w", encoding="utf-8") as output:
                output.write("backed file contents")
            evaljs(ctl, wid, "document.body.innerHTML = '<input id=files type=file multiple>'")

            def cdp(method, params):
                status, payload = ctl.result(ctl.call(C["ExecuteDevTools"], W().i32(wid).s(method).b1(True).by(json.dumps(params).encode()).b))
                reader = Rd(payload)
                check(status == OK and reader.b1(), "CDP " + method)
                return json.loads(reader.by())

            root = cdp("DOM.getDocument", {})["root"]["nodeId"]
            node = cdp("DOM.querySelector", {"nodeId": root, "selector": "#files"})["nodeId"]
            cdp("DOM.setFileInputFiles", {"nodeId": node, "files": [path]})
            check(evaljs(ctl, wid, "bridge.files.getPath(document.querySelector('#files').files[0])") == path,
                  "backing file path survives browser/renderer IPC, including Unicode and spaces")
            check(evaljs(ctl, wid, "document.querySelector('#files').files[0].text()") == "backed file contents",
                  "selected file is readable")
        shutdown(ctl)
    finally:
        ctl.close()


def test_native_file_drag():
    with tempfile.TemporaryDirectory(prefix="justcef-native-drag-") as directory:
        paths = [os.path.join(directory, name) for name in ("spaced café 文件.txt", "second.txt")]
        for path in paths:
            with open(path, "w") as output:
                output.write("native file drag")
        pasteboard = None
        if sys.platform == "darwin":
            pasteboard = os.path.join(directory, "read-drag-pasteboard")
            subprocess.run(["xcrun", "swiftc", os.path.join(os.path.dirname(__file__), "macos-drag-pasteboard.swift"), "-o", pasteboard], check=True, capture_output=True)
        for style, embedded in (([], False), (["--use-alloy-style"], False), ([], True), (["--use-alloy-style"], True)):
            ctl, wid, _ = open_app(native_args=style, views=embedded)
            try:
                def arm(files):
                    body = W().i32(wid).u32(len(files))
                    for path in files:
                        body.s(path)
                    return ctl.result(ctl.call(C["SetNextDragFiles"], body.b))[0]

                check(arm(paths) == OK, "arm next native file drag")
                invalid = [W().i32(wid).u32(129).b, W().i32(wid).u32(1).b,
                           W().i32(wid).u32(0).u8(0).b, W().i32(wid).u32(1).u32(1).u8(255).b]
                for path in ("", "relative.txt", paths[0] + "missing", directory, paths[0] + "\0", os.path.join(directory, "..", "file.txt")):
                    invalid.append(W().i32(wid).u32(1).s(path).b)
                for body in invalid:
                    check(ctl.result(ctl.call(C["SetNextDragFiles"], body))[0] == 8, "invalid drag file request")
                    check(window_state(ctl, wid)["drag_files"] == 2, "invalid requests preserve pending files")
                check(ctl.result(ctl.call(C["SetNextDragFiles"], W().i32(2147483647).u32(0).b))[0] == NOTFOUND, "missing drag browser")
                check(arm([]) == OK and window_state(ctl, wid)["drag_files"] == 0, "explicitly clear pending files")
                check(arm(paths) == OK, "rearm")
                ctl.result(ctl.call(C["LoadUrl"], W().i32(wid).s("http://app.test/next").b))
                ctl.wait_note("LoadingState", lambda r: r.i32() == wid and not r.b1())
                check(window_state(ctl, wid)["drag_files"] == 0, "navigation clears file authority")
                input_host = wid
                input_x = input_y = 0
                if embedded:
                    ctl.handlers[R["ViewCreated"]] = lambda r: (OK, W().b1(True).b)
                    wid = devtools_eval(ctl, wid, """new Promise((resolve, reject) => {
                      const v=document.createElement('justcef-view');
                      v.style.cssText='display:block;width:400px;height:200px';
                      v.addEventListener('load',()=>resolve(v.viewId),{once:true});
                      setTimeout(()=>reject('embedded drag browser not created'),10000);
                      v.setAttribute('src','about:blank'); document.body.append(v);
                    })""")
                    rect = devtools_eval(ctl,input_host,"document.querySelector('justcef-view').getBoundingClientRect().toJSON()")
                    input_x, input_y = round(rect['x']), round(rect['y'])
                devtools_eval(ctl, wid, "document.body.innerHTML = '<div id=drag draggable=true style=\"width:300px;height:100px;background:red\">drag</div>'; document.querySelector('#drag').ondragstart = e => e.dataTransfer.setData('text/plain','fallback'); globalThis.dragEnds=0; document.querySelector('#drag').ondragend=()=>dragEnds++; true")
                devtools_eval(ctl, wid, "new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))")
                check(arm(paths) == OK, "arm drag after page layout")

                def mouse(kind, x, y):
                    check(ctl.result(ctl.call(C["Debug"], W().u8(5).u32(input_host).u8(kind).i32(x+input_x).i32(y+input_y).b))[0] == OK, "CEF mouse event")

                mouse(0, 60, 40)
                mouse(1, 60, 40)
                mouse(2, 100, 60)
                deadline = time.time() + 10
                while window_state(ctl, wid)["drag_files"] and time.time() < deadline:
                    time.sleep(0.05)
                check(window_state(ctl, wid)["drag_files"] == 0, "CEF invoked and consumed the one-shot drag override")
                if pasteboard:
                    actual = json.loads(subprocess.check_output([pasteboard], text=True))
                    normalize = lambda values: sorted(unicodedata.normalize("NFC", path) for path in values)
                    check(normalize(actual) == normalize(paths), "native pasteboard file URLs: expected %r, got %r" % (paths, actual))
                mouse(3, 100, 60)
                devtools_eval(ctl, wid, "new Promise((resolve,reject) => { const until=Date.now()+10000; const t=setInterval(()=>{if(dragEnds){clearInterval(t);resolve(true)}else if(Date.now()>until){clearInterval(t);reject('native drag did not end')}},20) })")
                mouse(0, 60, 40)
                mouse(1, 60, 40)
                mouse(2, 100, 60)
                time.sleep(0.3)
                if pasteboard:
                    check(json.loads(subprocess.check_output([pasteboard], text=True)) == [], "one-shot override cannot leak into a later drag")
                mouse(3, 100, 60)
                devtools_eval(ctl, wid, "new Promise((resolve,reject) => { const until=Date.now()+10000; const t=setInterval(()=>{if(dragEnds>=2){clearInterval(t);resolve(true)}else if(Date.now()>until){clearInterval(t);reject('second native drag did not end')}},20) })")
                shutdown(ctl)
            finally:
                ctl.close()


def test_concurrent_startup():
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=4) as workers:
        controllers = list(workers.map(lambda _: Ctl(), range(4)))
    try:
        for ctl in controllers:
            ctl.wait_note("Ready", timeout=60)
        for ctl in controllers:
            shutdown(ctl)
    finally:
        for ctl in controllers:
            ctl.close()


def test_caption_button_regions():
    ctl = Ctl()
    ctl.wait_note("Ready")
    wid = ctl.create("about:blank", frameless=True)
    valid = W().i32(wid).u32(1).i32(700).i32(4).i32(46).i32(36).u8(2).b
    check(ctl.result(ctl.call(C["SetCaptionButtonRegions"], valid))[0] == 5, "caption targets are Windows-only")
    malformed = [W().i32(wid).u32(4).b, valid[:-1], valid + b"\0"]
    for role in (0, 4, 255):
        malformed.append(W().i32(wid).u32(1).i32(0).i32(4).i32(46).i32(36).u8(role).b)
    for x, y, w, h in [(0, 0, 0, 10), (0, 0, 10, -1), (2147483647, 0, 1, 10), (0, 2147483647, 10, 1)]:
        malformed.append(W().i32(wid).u32(1).i32(x).i32(y).i32(w).i32(h).u8(2).b)
    for second_role, second_x in [(2, 100), (3, 0)]:
        body = W().i32(wid).u32(2).i32(0).i32(4).i32(46).i32(36).u8(2)
        malformed.append(body.i32(second_x).i32(4).i32(46).i32(36).u8(second_role).b)
    for body in malformed:
        check(ctl.result(ctl.call(C["SetCaptionButtonRegions"], body))[0] == 8, "invalid caption target packet rejected")
    check(ctl.result(ctl.call(C["SetCaptionButtonRegions"], W().i32(2147483647).u32(0).b))[0] == 3, "missing browser")
    shutdown(ctl)


def test_frameless_drag_regions():
    server = PageServer(PAGES)
    try:
        for style in ([], ["--use-alloy-style"]):
            ctl = Ctl(style)
            ctl.wait_note("Ready")
            ctl.handlers[R["ViewCreated"]] = lambda r: (OK, W().b1(True).b)
            wid = ctl.create(server.origin + "/frameless.html", frameless=True, bridge=False, views=True)
            ctl.wait_note("LoadingState", lambda r: r.i32() == wid and not r.b1())
            regions = wait_drag_regions(ctl, wid, lambda rs: any(r[4] for r in rs) and any(not r[4] for r in rs))
            check(any(r[3] == 48 and r[4] for r in regions), "CSS title bar geometry")

            devtools_eval(ctl, wid, "document.querySelector('header').style.height = '64px'")
            wait_drag_regions(ctl, wid, lambda rs: any(r[3] == 64 and r[4] for r in rs))
            st, _ = ctl.result(ctl.call(9, W().i32(wid).f64(1).b))
            check(st == OK, "set zoom")
            wait_drag_regions(ctl, wid, lambda rs: any(r[3] > 64 and r[4] for r in rs))
            ctl.result(ctl.call(9, W().i32(wid).f64(0).b))
            wait_drag_regions(ctl, wid, lambda rs: any(r[3] == 64 and r[4] for r in rs))

            override = W().i32(wid).b1(True).u32(2).i32(0).i32(0).i32(800).i32(40).b1(True).i32(700).i32(0).i32(100).i32(40).b1(False)
            check(ctl.result(ctl.call(C["SetDraggableRegions"], override.b))[0] == OK, "set override")
            expected = [(0, 0, 800, 40, True), (700, 0, 100, 40, False)]
            check(drag_regions(ctl, wid) == expected, "host rectangles and exclusions")

            if os.environ.get("JUSTCEF_TEST_WINDOW_MANAGER") == "1":
                previous = window_state(ctl, wid)["transitions"]
                check(ctl.result(ctl.call(31, W().i32(wid).b1(True).b))[0] == OK, "fullscreen")
                wait_fullscreen(ctl, wid, True, previous)
                wait_drag_regions(ctl, wid, lambda rs: not rs)
                previous = window_state(ctl, wid)["transitions"]
                check(ctl.result(ctl.call(31, W().i32(wid).b1(False).b))[0] == OK, "exit fullscreen")
                wait_fullscreen(ctl, wid, False, previous)
                wait_drag_regions(ctl, wid, lambda rs: rs == expected)

            malformed = [W().i32(wid).u8(2).b, W().i32(wid).b1(False).u8(0).b,
                         W().i32(wid).b1(True).u32(4097).b,
                         W().i32(wid).b1(True).u32(1).i32(0).i32(0).i32(0).i32(40).b1(True).b,
                         W().i32(wid).b1(True).u32(1).i32(2147483647).i32(0).i32(1).i32(40).b1(True).b,
                         W().i32(wid).b1(True).u32(1).i32(0).i32(0).i32(1).i32(40).u8(2).b]
            for body in malformed:
                check(ctl.result(ctl.call(C["SetDraggableRegions"], body))[0] == 8, "reject malformed regions")
                check(drag_regions(ctl, wid) == expected, "invalid packet cannot mutate regions")

            ctl.result(ctl.call(C["SetDraggableRegions"], W().i32(wid).b1(True).u32(0).b))
            check(not drag_regions(ctl, wid), "empty override disables dragging")
            ctl.result(ctl.call(C["SetDraggableRegions"], W().i32(wid).b1(False).b))
            wait_drag_regions(ctl, wid, lambda rs: any(r[3] == 64 and r[4] for r in rs) and any(not r[4] for r in rs))

            ctl.result(ctl.call(C["SetDraggableRegions"], override.b))
            ctl.result(ctl.call(C["LoadUrl"], W().i32(wid).s(server.origin + "/view.html").b))
            ctl.wait_note("LoadingState", lambda r: r.i32() == wid and not r.b1())
            check(drag_regions(ctl, wid) == expected, "override survives navigation")
            ctl.result(ctl.call(C["SetDraggableRegions"], W().i32(wid).b1(False).b))
            check(not drag_regions(ctl, wid), "navigation clears stale CSS drag regions")

            child = devtools_eval(ctl, wid, """new Promise((resolve, reject) => {
                const view = document.createElement('justcef-view');
                view.style.cssText = 'display:block;width:400px;height:300px';
                setTimeout(() => reject('embedded view was not created'), 10000);
                view.addEventListener('load', () => resolve(view.viewId), {once:true});
                view.setAttribute('src', 'frameless.html');
                document.body.append(view);
            })""")
            check(not drag_regions(ctl, child) and not drag_regions(ctl, wid), "embedded CSS cannot replace the host drag regions")
            check(ctl.result(ctl.call(C["SetDraggableRegions"], W().i32(child).b1(False).b))[0] == 5, "embedded overrides unsupported")
            check(ctl.result(ctl.call(C["SetNextDragFiles"], W().i32(child).u32(0).b))[0] == OK, "embedded file drag override")

            framed = ctl.create("about:blank", bridge=False)
            check(ctl.result(ctl.call(C["SetDraggableRegions"], W().i32(framed).b1(False).b))[0] == 5, "framed windows unsupported")
            shutdown(ctl)
    finally:
        server.close()


def test_views_handshake():
    server = PageServer(PAGES)
    try:
        for allow in (True, False):
            ctl = Ctl()
            ctl.wait_note("Ready")
            requests = []
            ctl.handlers[R["ViewCreated"]] = lambda r: (requests.append((r.i32(), r.i32(), r.s())), (OK, W().b1(allow).b))[1]
            wid = ctl.create(server.origin + "/host.html", views=True, bridge=False)
            deadline = time.time() + 15
            while len(requests) < 3 and time.time() < deadline:
                time.sleep(0.1)
            check(len(requests) == 3 and all(p == wid for p, v, s in requests), "view requests %r" % requests)
            if not allow:
                closed = set()
                deadline = time.time() + 10
                while len(closed) < 3 and time.time() < deadline:
                    try:
                        op, body, t = ctl.notes.get(timeout=0.5)
                        if op == 3:
                            closed.add(Rd(body).i32())
                    except queue.Empty:
                        pass
                check(closed >= {v for p, v, s in requests}, "denied views close")
            shutdown(ctl)
    finally:
        server.close()


def test_views_rounded_clipping():
    server = PageServer(PAGES)
    ctl = None
    try:
        ctl = Ctl()
        ctl.wait_note("Ready")
        ctl.handlers[R["ViewCreated"]] = lambda r: (OK, W().b1(True).b)
        wid = ctl.create(server.origin + "/rounded-view.html", views=True, bridge=False)
        ctl.wait_note("LoadingState", lambda r: r.i32() == wid and not r.b1())
        child = devtools_eval(ctl, wid, """new Promise((resolve, reject) => {
          const deadline = Date.now() + 10000;
          const timer = setInterval(() => {
            if (chat.viewId !== null) { clearInterval(timer); resolve(chat.viewId); }
            else if (Date.now() > deadline) { clearInterval(timer); reject('view not created'); }
          }, 20);
        })""")
        deadline = time.time() + 10
        viewport = None
        while time.time() < deadline:
            try:
                viewport = devtools_eval(ctl, child, "location.href === %s && document.readyState === 'complete' ? [innerWidth, innerHeight] : null" % json.dumps(server.origin + '/view.html'))
            except AssertionError as error:
                if str(error) != 'Runtime.evaluate failed': raise
                viewport = None
            if viewport and all(abs(actual - expected) <= 1 for actual, expected in zip(viewport, [400, 360])):
                break
            time.sleep(0.05)
        check(viewport and all(abs(actual - expected) <= 1 for actual, expected in zip(viewport, [400, 360])),
              "rounded clipping must not leave a zero-size native view: %r" % viewport)
        shutdown(ctl)
    finally:
        if ctl is not None:
            ctl.close()
        server.close()


def test_views_lifecycle():
    server = PageServer(PAGES)
    try:
        ctl = Ctl()
        ctl.wait_note("Ready")
        requests = []
        ctl.handlers[R["ViewCreated"]] = lambda r: (requests.append((r.i32(), r.i32(), r.s())), (OK, W().b1(True).b))[1]
        wid = ctl.create(server.origin + "/host.html", views=True, bridge=False)
        ctl.wait_note("LoadingState", lambda r: r.i32() == wid and not r.b1())
        ids = devtools_eval(ctl, wid, """new Promise((resolve, reject) => {
          const deadline = Date.now() + 10000;
          const timer = setInterval(() => {
            const views = [...document.querySelectorAll('justcef-view')];
            if (views.length === 3 && views.every(v => v.viewId !== null)) {
              clearInterval(timer); resolve(views.map(v => v.viewId));
            } else if (Date.now() > deadline) { clearInterval(timer); reject('views not created'); }
          }, 20);
        })""")
        check(len(ids) == 3, "three views created")
        blank = devtools_eval(ctl, wid, """new Promise(resolve => {
          const v = document.createElement('justcef-view');
          v.addEventListener('viewcreated', () => resolve(v.viewId), {once: true});
          v.setAttribute('src', 'about:blank');
          document.body.append(v);
        })""")
        check(isinstance(blank, int), "about:blank view created")
        check(devtools_eval(ctl, wid, """new Promise(resolve => {
          const v = [...document.querySelectorAll('justcef-view')].find(x => x.viewId === %d);
          v.addEventListener('load', () => resolve(v.viewId === %d), {once: true});
          v.setAttribute('src', 'view.html?after-blank');
        })""" % (blank, blank)), "about:blank view navigates to HTTP")
        devtools_eval(ctl, wid, "[...document.querySelectorAll('justcef-view')].find(x => x.viewId === %d).remove()" % blank)
        ctl.wait_note("WindowClosed", lambda r: r.i32() == blank)
        kept = devtools_eval(ctl, wid, """new Promise(resolve => {
          const v = document.getElementById('chat');
          const id = v.viewId;
          v.addEventListener('error', () => resolve(v.viewId === id), {once: true});
          v.setAttribute('src', 'file:///denied');
        })""")
        check(kept, "denied navigation must preserve the live view")
        check(devtools_eval(ctl, wid, """new Promise(resolve => {
          const v = document.getElementById('chat');
          v.addEventListener('load', () => resolve(v.viewId !== null), {once: true});
          v.setAttribute('src', 'view.html?recovered');
        })"""), "view can navigate after rejection")
        removed = devtools_eval(ctl, wid, """(() => {
          const v = document.getElementById('chat');
          const id = v.viewId;
          v.removeAttribute('src');
          return {id, cleared: v.viewId === null};
        })()""")
        check(removed["cleared"], "removing src clears viewId")
        ctl.wait_note("WindowClosed", lambda r: r.i32() == removed["id"])
        recreated = devtools_eval(ctl, wid, """new Promise(resolve => {
          const v = document.getElementById('chat');
          v.addEventListener('viewcreated', () => resolve(v.viewId), {once: true});
          v.setAttribute('src', 'view.html?recreated');
        })""")
        check(recreated != removed["id"], "src recreates a new native browser")
        st, _ = ctl.result(ctl.call(C["LoadUrl"], W().i32(wid).s(server.origin + "/view.html").b))
        check(st == OK, "host navigation succeeds")
        remaining = (set(ids) - {removed["id"]}) | {recreated}
        deadline = time.time() + 10
        while remaining and time.time() < deadline:
            try:
                op, body, _ = ctl.notes.get(timeout=0.5)
                if op == 3:
                    remaining.discard(Rd(body).i32())
            except queue.Empty:
                pass
        check(not remaining, "host navigation closes all old views: %r" % remaining)
        shutdown(ctl)
    finally:
        server.close()



def browser_command(ctl, wid, command, options=None):
    status, payload = ctl.result(ctl.call(C["BrowserCommand"], W().i32(wid).s(command).s(json.dumps(options or {})).b))
    check(status == OK, "browser command failed: %r" % ((status, payload),))
    return json.loads(Rd(payload).s())


def cdp(ctl, wid, method, params=None):
    status, payload = ctl.result(ctl.call(C["ExecuteDevTools"], W().i32(wid).s(method).b1(True).by(json.dumps(params or {}).encode()).b), timeout=65)
    check(status == OK, "CDP transport failed: %r" % ((status, payload),))
    result = Rd(payload)
    success, data = result.b1(), result.by()
    check(success, "%s failed: %r" % (method, data))
    return json.loads(data)


def test_capture_pdf_and_permission_records():
    import base64
    ctl, wid, _ = open_app()
    try:
        screenshot = base64.b64decode(cdp(ctl, wid, "Page.captureScreenshot", {"format": "png"})["data"])
        check(screenshot.startswith(b"\x89PNG\r\n\x1a\n"), "PNG screenshot")
        pdf = base64.b64decode(cdp(ctl, wid, "Page.printToPDF", {"printBackground": True})["data"])
        check(pdf.startswith(b"%PDF-"), "PDF export")
        browser_command(ctl, wid, "permission", {"origin": "https://permissions.test", "permissions": 3, "value": 1})
        settings = browser_command(ctl, wid, "permission", {"origin": "https://permissions.test"})
        check(settings == {"microphone": 1, "camera": 1}, "remembered camera and microphone")
        browser_command(ctl, wid, "permission", {"origin": "https://permissions.test", "permissions": 3, "value": 0})
        settings = browser_command(ctl, wid, "permission", {"origin": "https://permissions.test"})
        check(settings["microphone"] != 1 and settings["camera"] != 1, "permission reset")
        status, _ = ctl.result(ctl.call(C["BrowserCommand"], W().i32(wid).s("permission").s('{"origin":"https://a.test","permissions":3,"value":999}').b))
        check(status == 8, "invalid content setting rejected")
        status, _ = ctl.result(ctl.call(C["BrowserCommand"], W().i32(wid).s("permission").s('{"origin":"https://a.test","topLevelOrigin":"app://host","permissions":3,"value":1}').b))
        check(status == 5, "custom embedding origin persistence is explicitly unsupported")
        shutdown(ctl)
    finally:
        ctl.close()


def test_custom_schemes_and_view_boundary():
    ctl = Ctl(["--custom-schemes=app"])
    try:
        ctl.wait_note("Ready")
        requests = []
        def proxy(r):
            browser, method, url, headers, elements = read_request(r)
            requests.append((browser, url))
            if url.endswith('/value'): return proxy_response(200, {"Content-Type": "text/plain"}, b"custom-response")
            return proxy_response(200, {"Content-Type": "text/html"}, b'<!doctype html><title>custom</title><style>justcef-view{display:block;width:400px;height:300px}</style><body><justcef-view src="app://content/page"></justcef-view></body>')
        views = []
        ctl.handlers[R["Proxy"]] = proxy
        ctl.handlers[R["ViewCreated"]] = lambda r: (views.append((r.i32(), r.i32(), r.s())), (OK, W().b1(True).b))[1]
        wid = ctl.create("app://host/index", proxy=False, bridge=True, views=True)
        ctl.wait_note("LoadingState", lambda r: r.i32() == wid and not r.b1())
        check(devtools_eval(ctl, wid, "fetch('/value').then(r => r.text())") == "custom-response", "relative custom scheme fetch")
        deadline = time.time() + 15
        while not views and time.time() < deadline: time.sleep(.05)
        check(len(views) == 1, "custom scheme view created")
        vid = views[0][1]
        deadline = time.time() + 15
        while not any(browser == vid for browser, _ in requests) and time.time() < deadline: time.sleep(.05)
        check(any(browser == vid for browser, _ in requests), "custom scheme view resource handled")
        ctl.wait_note("LoadingState", lambda r: r.i32() == vid and not r.b1())
        check(devtools_eval(ctl, vid, "[typeof bridge, typeof customElements.get('justcef-view')]") == ['undefined', 'undefined'], "privileged APIs absent inside website")
        import base64
        check(base64.b64decode(cdp(ctl, vid, "Page.captureScreenshot")["data"]).startswith(b"\x89PNG"), "view screenshot")
        check(base64.b64decode(cdp(ctl, vid, "Page.printToPDF")["data"]).startswith(b"%PDF-"), "view PDF")
        shutdown(ctl)
    finally:
        ctl.close()


def test_view_media_permission_decisions():
    ctl = Ctl(["--custom-schemes=app", "--use-fake-device-for-media-stream"])
    try:
        ctl.wait_note("Ready")
        views, decisions = [], []
        def proxy(r):
            browser, method, url, headers, elements = read_request(r)
            body = b'<justcef-view src="https://content.test/page"></justcef-view>' if 'host' in url else b'<!doctype html><title>content</title>'
            return proxy_response(200, {"Content-Type": "text/html"}, body)
        def decide(r):
            browser, kind, detail = r.i32(), r.s(), json.loads(r.s())
            decisions.append((browser, kind, detail))
            return OK, W().s(json.dumps({"allow": True, "remember": True})).b
        ctl.handlers[R["Proxy"]] = proxy
        ctl.handlers[R["BrowserDecision"]] = decide
        def configure_view(r):
            parent, view, url = r.i32(), r.i32(), r.s()
            views.append((parent, view, url))
            check(ctl.result(ctl.call(33, W().i32(view).b1(True).b))[0] == OK, "view resource proxy")
            return OK, W().b1(True).b
        ctl.handlers[R["ViewCreated"]] = configure_view
        wid = ctl.create("app://host/index", views=True, bridge=False)
        deadline = time.time() + 15
        while not views and time.time() < deadline: time.sleep(.05)
        check(bool(views), "view created")
        vid = views[0][1]
        ctl.wait_note("LoadingState", lambda r: r.i32() == vid and not r.b1())
        check(devtools_eval(ctl, vid, "navigator.mediaDevices.getUserMedia({audio:true}).then(s => {s.getTracks().forEach(t=>t.stop()); return true;}).catch(e=>e.name)") is True, "microphone grant")
        check(any(browser == vid and kind == "permission" and detail["origin"].startswith('https://content.test') for browser, kind, detail in decisions), "permission routed to requesting view")
        check(not any(browser == wid for browser, _, _ in decisions), "host did not own view permission")
        check(browser_command(ctl, vid, "permission", {"origin":"https://content.test"})["microphone"] == 1, "grant recorded in Chromium: %r, %r" % (decisions, browser_command(ctl, vid, "permission", {"origin":"https://content.test"})))
        browser_command(ctl, vid, "permission", {"origin":"https://content.test", "permissions":1, "value":0})
        del ctl.handlers[R["BrowserDecision"]]
        devtools_eval(ctl, wid, "document.querySelector('justcef-view').addEventListener('permissionrequest', e => { e.preventDefault(); document.querySelector('justcef-view').respond(e.detail.requestId, {allow:false}); }); true")
        check(devtools_eval(ctl, vid, "navigator.mediaDevices.getUserMedia({audio:true}).then(s=>{s.getTracks().forEach(t=>t.stop());return 'unexpected';}).catch(e=>e.name)") == 'NotAllowedError', "host renderer can deny view permission")
        shutdown(ctl)
    finally:
        ctl.close()



def test_view_popup_routing_and_policy():
    ctl = Ctl(["--custom-schemes=app", "--disable-popup-blocking"])
    try:
        ctl.wait_note("Ready")
        views = []
        ctl.handlers[R["Proxy"]] = lambda r: proxy_response(200, {"Content-Type":"text/html"}, b'<justcef-view src="app://content/page"></justcef-view>' if 'host' in read_request(r)[2] else b'<!doctype html><title>content</title>')
        ctl.handlers[R["ViewCreated"]] = lambda r: (views.append((r.i32(), r.i32(), r.s())), (OK, W().b1(True).b))[1]
        wid = ctl.create("app://host/index", views=True, bridge=False)
        deadline = time.time() + 15
        while not views and time.time() < deadline: time.sleep(.05)
        check(bool(views), "view created")
        vid = views[0][1]
        ctl.wait_note("LoadingState", lambda r: r.i32() == vid and not r.b1())
        while not ctl.notes.empty(): ctl.notes.get_nowait()
        devtools_eval(ctl, vid, "window.open('app://popup/page'); true")
        popup = ctl.wait_note("WindowOpened", lambda r: r.i32() not in (wid, vid)).i32()
        ctl.wait_note("LoadingState", lambda r: r.i32() == popup and not r.b1())
        check(devtools_eval(ctl, wid, "location.href") == "app://host/index", "host did not navigate")
        check(devtools_eval(ctl, vid, "location.href") == "app://content/page", "view did not navigate")
        check(devtools_eval(ctl, popup, "[typeof bridge, typeof customElements.get('justcef-view')]") == ['undefined','undefined'], "popup privilege boundary")
        browser_command(ctl, vid, "windowOpenPolicy", {"allow":False})
        check(devtools_eval(ctl, vid, "window.open('app://blocked/page') === null"), "window policy denies popup")
        shutdown(ctl)
    finally:
        ctl.close()


def test_download_lifecycle():
    import http.server
    from socketserver import ThreadingMixIn
    body = b'JustCef download payload' * 1000
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_GET(self):
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Disposition', 'attachment; filename="sample.bin"')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    ctl = Ctl()
    ctl.wait_note("Ready")
    wid = ctl.create("about:blank", bridge=False)
    try:
        with tempfile.TemporaryDirectory(prefix='justcef-download-test-') as directory:
            path = os.path.join(directory, 'sample.bin')
            updated = []
            def decide(r):
                browser, kind, detail = r.i32(), r.s(), json.loads(r.s())
                return OK, W().s(json.dumps({'allow':kind == 'download', 'path':path})).b
            ctl.handlers[R['BrowserDecision']] = decide
            ctl.note_handlers[22] = lambda r: (r.i32(), updated.append(json.loads(r.s())))
            browser_command(ctl, wid, 'download', {'url':'http://127.0.0.1:%d/file' % server.server_port})
            deadline = time.time() + 20
            while not any(item.get('complete') for item in updated) and time.time() < deadline: time.sleep(.05)
            check(any(item.get('complete') for item in updated), 'download completion event: %r' % updated)
            with open(path, 'rb') as file: check(file.read() == body, 'download contents')
            os.remove(path)
            updated.clear()
            ctl.handlers[R['BrowserDecision']] = lambda r: (OK, W().s('{"allow":false}').b)
            browser_command(ctl, wid, 'download', {'url':'http://127.0.0.1:%d/file' % server.server_port})
            deadline = time.time() + 10
            while not any(item.get('canceled') for item in updated) and time.time() < deadline: time.sleep(.05)
            check(any(item.get('canceled') for item in updated), 'denied download is canceled: %r' % updated)
            check(not os.path.exists(path), 'denied download must not write a file')
        shutdown(ctl)
    finally:
        ctl.close()
        server.shutdown()
        server.server_close()



def test_permission_records_survive_restart():
    with tempfile.TemporaryDirectory(prefix='justcef-permission-profile-') as directory:
        for write in (True, False):
            ctl = Ctl(['--cache-path=' + directory])
            try:
                ctl.wait_note('Ready')
                wid = ctl.create('about:blank', bridge=False)
                if write: browser_command(ctl, wid, 'permission', {'origin':'https://persistent.test', 'permissions':3, 'value':1})
                check(browser_command(ctl, wid, 'permission', {'origin':'https://persistent.test'}) == {'camera':1,'microphone':1}, 'persistent content settings')
                shutdown(ctl)
            finally:
                ctl.close()


def test_certificate_decisions():
    import http.server
    import ssl
    with tempfile.TemporaryDirectory(prefix='justcef-certificate-test-') as directory:
        key, cert = os.path.join(directory, 'key.pem'), os.path.join(directory, 'cert.pem')
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',key,'-out',cert,'-days','1','-subj','/CN=localhost'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_GET(self):
                body = b'<!doctype html><title>certificate accepted</title>'
                self.send_response(200)
                self.send_header('Content-Type', 'text/html')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = http.server.ThreadingHTTPServer(('127.0.0.1',0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            for allow in (False, True):
                ctl = Ctl(['--use-alloy-style'])
                try:
                    ctl.wait_note('Ready')
                    requests = []
                    def decide(r):
                        browser, kind, detail = r.i32(), r.s(), json.loads(r.s())
                        requests.append((browser,kind,detail))
                        return OK, W().s(json.dumps({'allow':allow})).b
                    ctl.handlers[R['BrowserDecision']] = decide
                    wid = ctl.create('https://localhost:%d/' % server.server_port, bridge=False)
                    if allow:
                        ctl.wait_note('FrameLoadEnd', lambda r: r.i32() == wid)
                        check(devtools_eval(ctl,wid,'document.title') == 'certificate accepted','explicit certificate acceptance')
                    else:
                        ctl.wait_note('FrameLoadError', lambda r: r.i32() == wid)
                    check(any(browser == wid and kind == 'certificate' and detail.get('error',0) < 0 for browser,kind,detail in requests),'certificate request routed to browser')
                    shutdown(ctl)
                finally:
                    ctl.close()
        finally:
            server.shutdown()
            server.server_close()



def test_http_popups_inherit_proxy_routing():
    for selective, intercept in ((False, False), (True, False), (False, True)):
        ctl, wid, _ = open_app(native_args=['--disable-popup-blocking'])
        try:
            if selective:
                check(ctl.result(ctl.call(54, W().i32(wid).s('app.test').b))[0] == OK, 'domain proxy configured')
                check(ctl.result(ctl.call(33, W().i32(wid).b1(False).b))[0] == OK, 'global proxy disabled')
            if intercept:
                ctl.handlers[R['BrowserDecision']] = lambda r: (OK, W().s('{"allow":true}').b)
                browser_command(ctl, wid, 'windowOpenPolicy', {'allow':True,'intercept':True})
            while not ctl.notes.empty(): ctl.notes.get_nowait()
            cdp(ctl,wid,'Runtime.evaluate',{'expression':"window.open('http://app.test/popup');true",'userGesture':True,'returnByValue':True})
            popup = ctl.wait_note('WindowOpened',lambda r:r.i32()!=wid).i32()
            ctl.wait_note('LoadingState',lambda r:r.i32()==popup and not r.b1())
            check(devtools_eval(ctl,popup,'location.href') == 'http://app.test/popup','HTTP popup used configured request proxy')
            check(devtools_eval(ctl,popup,'typeof bridge') == 'undefined','proxy inheritance did not enable bridge')
            shutdown(ctl)
        finally:
            ctl.close()


def test_permission_navigation_is_frame_scoped():
    ctl, wid, _ = open_app(native_args=['--use-fake-device-for-media-stream','--unsafely-treat-insecure-origin-as-secure=http://app.test'])
    release, started = threading.Event(), threading.Event()
    try:
        def decide(r):
            browser, kind, detail = r.i32(), r.s(), json.loads(r.s())
            started.set()
            release.wait(10)
            return OK, W().s('{"allow":true}').b
        ctl.handlers[R['BrowserDecision']] = decide
        devtools_eval(ctl,wid,"window.mediaResult='pending';navigator.mediaDevices.getUserMedia({audio:true}).then(s=>{s.getTracks().forEach(t=>t.stop());window.mediaResult='allowed';}).catch(e=>window.mediaResult=e.name);true")
        check(started.wait(5),'media decision pending')
        devtools_eval(ctl,wid,"var unrelated=document.createElement('iframe');unrelated.src='http://app.test/unrelated';document.body.append(unrelated);true")
        deadline = time.time() + 10
        while not devtools_eval(ctl,wid,"unrelated.contentDocument?.readyState === 'complete'") and time.time() < deadline: time.sleep(.05)
        check(devtools_eval(ctl,wid,'window.mediaResult') == 'pending','unrelated iframe did not cancel microphone request')
        release.set()
        deadline = time.time() + 10
        while devtools_eval(ctl,wid,'window.mediaResult') == 'pending' and time.time() < deadline: time.sleep(.05)
        check(devtools_eval(ctl,wid,'window.mediaResult') == 'allowed','pending request can still be granted')
        release.clear();started.clear()
        before = len(ctl.cancels)
        devtools_eval(ctl,wid,"unrelated.contentWindow.navigator.mediaDevices.getUserMedia({audio:true}).then(s=>s.getTracks().forEach(t=>t.stop())).catch(()=>{});true")
        check(started.wait(5),'subframe media decision pending')
        devtools_eval(ctl,wid,'unrelated.remove();true')
        deadline = time.time() + 5
        while len(ctl.cancels) == before and time.time() < deadline: time.sleep(.05)
        check(any(op == R['BrowserDecision'] for op, _ in ctl.cancels[before:]),'requesting frame removal canceled its continuation')
        release.set()
        shutdown(ctl)
    finally:
        release.set()
        ctl.close()



def test_live_popup_with_restrictive_csp():
    ctl = Ctl(['--custom-schemes=app'])
    try:
        ctl.wait_note('Ready')
        views = []
        def serve(r):
            browser, method, url, headers, elements = read_request(r)
            if 'host' in url:
                return proxy_response(200, {'Content-Type':'text/html', 'Content-Security-Policy':"default-src 'self'; style-src 'unsafe-inline'"}, b'<style>justcef-view{display:block;width:400px;height:300px}</style><justcef-view src="app://content/page"></justcef-view>')
            return proxy_response(200, {'Content-Type':'text/html'}, b'<!doctype html><style>html,body{margin:0;background:#123456;height:100%}</style>')
        ctl.handlers[R['Proxy']] = serve
        ctl.handlers[R['ViewCreated']] = lambda r: (views.append((r.i32(),r.i32(),r.s())),(OK,W().b1(True).b))[1]
        host = ctl.create('app://host/index',views=True,bridge=False)
        deadline = time.time() + 10
        while not views and time.time() < deadline: time.sleep(.05)
        check(bool(views),'view created')
        ctl.wait_note('LoadingState',lambda r:r.i32()==views[0][1] and not r.b1())
        devtools_eval(ctl,host,"window.violations=[];document.addEventListener('securitypolicyviolation',e=>violations.push(e.violatedDirective));const create=document.createElement.bind(document);document.createElement=(tag,...args)=>{const el=create(tag,...args);if(tag==='canvas'||tag==='img')window.placeholder=el;return el;};const dialog=document.createElement('dialog');dialog.style.cssText='position:fixed;inset:40px auto auto 40px;margin:0;width:100px;height:100px';document.body.append(dialog);dialog.showModal();true")
        time.sleep(.2)
        check(not devtools_eval(ctl,host,'!!window.placeholder'),'overlap must not create a snapshot placeholder')
        check(devtools_eval(ctl,host,'window.violations') == [],'surface embedding respects host CSP')
        check(devtools_eval(ctl,views[0][1],'document.visibilityState') == 'visible','modal overlap keeps the guest live')
        devtools_eval(ctl,views[0][1],"window.ticks=0;setInterval(()=>ticks++,20);true")
        time.sleep(.15)
        check(devtools_eval(ctl,views[0][1],'ticks') > 2,'guest continues updating beneath the modal')
        shutdown(ctl)
    finally:
        ctl.close()


def test_capture_preserves_view_input():
    import base64
    for style in ([], ['--use-alloy-style']):
        ctl = Ctl(style + ['--custom-schemes=app'])
        try:
            ctl.wait_note('Ready')
            views = []
            content = b'''<!doctype html><style>body{margin:0}button{position:absolute;left:20px;top:20px;width:180px;height:60px}main{height:1200px}</style>
                <button onclick="clicks++">Click counter</button><main></main><script>window.clicks=0;window.ticks=0;requestAnimationFrame(function tick(){ticks++;requestAnimationFrame(tick)});</script>'''
            def serve(r):
                url = read_request(r)[2]
                body = b'<style>body{margin:0}justcef-view{display:block;width:500px;height:340px}</style><justcef-view src="app://content/page"></justcef-view>' if 'host' in url else content
                return proxy_response(200, {'Content-Type':'text/html'}, body)
            ctl.handlers[R['Proxy']] = serve
            ctl.handlers[R['ViewCreated']] = lambda r: (views.append((r.i32(),r.i32(),r.s())),(OK,W().b1(True).b))[1]
            host = ctl.create('app://host/index', views=True, bridge=False)
            deadline = time.time() + 10
            while not views and time.time() < deadline: time.sleep(.05)
            check(bool(views), 'view created')
            view = views[0][1]
            ctl.wait_note('LoadingState',lambda r:r.i32()==view and not r.b1())
            initial = devtools_eval(ctl,view,'[innerWidth,innerHeight]')
            for index, image_format in enumerate(('png','jpeg','webp','png'), 1):
                params = {'format':image_format, 'captureBeyondViewport':index==4}
                if index == 4:
                    params['clip'] = dict(cdp(ctl,view,'Page.getLayoutMetrics')['cssContentSize'],scale=1)
                image = base64.b64decode(cdp(ctl,view,'Page.captureScreenshot',params)['data'])
                check(len(image)>100, 'captured image')
                deadline=time.time()+3
                while devtools_eval(ctl,view,'[innerWidth,innerHeight]') != initial and time.time()<deadline:time.sleep(.02)
                check(devtools_eval(ctl,view,'[innerWidth,innerHeight]') == initial,'capture restored the embedded viewport')
                rect = devtools_eval(ctl,host,"document.querySelector('justcef-view').getBoundingClientRect().toJSON()")
                for kind in ('mouseMoved','mousePressed','mouseReleased'):
                    params = dict(type=kind,x=rect['x']+70,y=rect['y']+40)
                    if kind != 'mouseMoved': params.update(button='left',clickCount=1)
                    cdp(ctl,host,'Input.dispatchMouseEvent',params)
                deadline=time.time()+3
                while devtools_eval(ctl,view,'clicks') < index and time.time()<deadline: time.sleep(.05)
                check(devtools_eval(ctl,view,'clicks') == index,'embedded native mouse input still works after capture')
                tick = devtools_eval(ctl,view,'ticks')
                time.sleep(.15)
                check(devtools_eval(ctl,view,'ticks') > tick,'embedded animation remains live after capture')
                check(devtools_eval(ctl,host,'1+1')==2,'host remains responsive after embedded capture')
            import struct
            for target in (host, view):
                dimensions = devtools_eval(ctl,target,'[innerWidth,innerHeight]')
                expected = {}
                for full in (False, True):
                    encoded = browser_command(ctl,target,'captureScreenshot',{'fullPage':full})
                    image = base64.b64decode(encoded['data'])
                    expected[full] = struct.unpack('>II',image[16:24])
                check(expected[False] != expected[True] or target == host, 'fixture has distinct viewport and full-page dimensions')
                requests = [(full,ctl.call(C['BrowserCommand'],W().i32(target).s('captureScreenshot').s(json.dumps({'fullPage':full})).b)) for full in (True,False,True,False)]
                for full, request in requests:
                    status, payload = ctl.result(request,timeout=65)
                    check(status == OK,'overlapping screenshot succeeded')
                    image = base64.b64decode(json.loads(Rd(payload).s())['data'])
                    check(struct.unpack('>II',image[16:24]) == expected[full], 'overlapping screenshot retained its requested viewport')
                bad = ctl.call(C['BrowserCommand'],W().i32(target).s('captureScreenshot').s(json.dumps({'clip':{'x':0,'y':0,'width':0,'height':10,'scale':1}})).b)
                canceled = ctl.call(C['BrowserCommand'],W().i32(target).s('captureScreenshot').s(json.dumps({'fullPage':True})).b)
                with ctl.plock:
                    cancel_id = next(rid for rid,future in ctl.pending.items() if future is canceled)
                ctl.send(CANCEL,C['BrowserCommand'],cancel_id)
                following = ctl.call(C['BrowserCommand'],W().i32(target).s('captureScreenshot').s('{}').b)
                check(ctl.result(bad,timeout=65)[0] == ERROR,'invalid screenshot reports failure')
                check(ctl.result(canceled,timeout=65)[0] == CANCELED,'screenshot cancellation acknowledged')
                status, payload = ctl.result(following,timeout=65)
                check(status == OK,'capture queue continues after failure and cancellation')
                image = base64.b64decode(json.loads(Rd(payload).s())['data'])
                check(struct.unpack('>II',image[16:24]) == expected[False], 'capture after cancellation retains viewport')
                deadline=time.time()+3
                while devtools_eval(ctl,target,'[innerWidth,innerHeight]') != dimensions and time.time()<deadline:time.sleep(.02)
                check(devtools_eval(ctl,target,'[innerWidth,innerHeight]') == dimensions,'overlapping screenshots restored viewport')
            shutdown(ctl)
        finally:
            ctl.close()


def test_macos_print_preview_remains_responsive():
    if sys.platform != 'darwin':
        return
    ctl = Ctl(['--custom-schemes=app', '--use-alloy-style'])
    try:
        ctl.wait_note('Ready')
        views=[]
        ctl.handlers[R['Proxy']] = lambda r: proxy_response(200, {'Content-Type':'text/html'}, b'<style>justcef-view{display:block;width:500px;height:340px}</style><justcef-view src="app://content/page"></justcef-view>' if 'host' in read_request(r)[2] else b'<!doctype html><h1>Print regression</h1>')
        ctl.handlers[R['ViewCreated']] = lambda r: (views.append((r.i32(),r.i32(),r.s())),(OK,W().b1(True).b))[1]
        host=ctl.create('app://host/index',views=True,bridge=False)
        deadline=time.time()+10
        while not views and time.time()<deadline: time.sleep(.05)
        check(bool(views),'print test view created')
        view=views[0][1]
        ctl.wait_note('LoadingState',lambda r:r.i32()==view and not r.b1())
        for browser in (view,host):
            cdp(ctl,browser,'Page.captureScreenshot',{'format':'png'})
            browser_command(ctl,browser,'print')
            deadline=time.time()+10
            previews=[]
            while not previews and time.time()<deadline:
                previews=[t for t in cdp(ctl,browser,'Target.getTargets')['targetInfos'] if t['url'].startswith('chrome://print')]
                if not previews: time.sleep(.1)
            check(bool(previews),'native Print opened Chromium print preview')
            check(devtools_eval(ctl,host,'1+1')==2,'host renderer responds while print preview is open')
            check(devtools_eval(ctl,view,'1+1')==2,'view renderer responds while print preview is open')
            for preview in previews:
                session = cdp(ctl,browser,'Target.attachToTarget',{'targetId':preview['targetId']})['sessionId']
                cancel = """new Promise((resolve,reject)=>{
                  const until=Date.now()+5000;
                  const find=root=>{for(const e of root.querySelectorAll('*')){if(e.matches('.cancel-button'))return e;if(e.shadowRoot){const match=find(e.shadowRoot);if(match)return match;}}};
                  const timer=setInterval(()=>{const button=find(document);if(button){clearInterval(timer);button.click();resolve(true);}else if(Date.now()>until){clearInterval(timer);reject('Cancel button missing');}},50);
                })"""
                cdp(ctl,browser,'Target.sendMessageToTarget',{'sessionId':session,'message':json.dumps({'id':1,'method':'Runtime.evaluate','params':{'expression':cancel,'awaitPromise':True}})})
            deadline=time.time()+8
            while any(t['url'].startswith('chrome://print') for t in cdp(ctl,browser,'Target.getTargets')['targetInfos']) and time.time()<deadline: time.sleep(.05)
            check(not any(t['url'].startswith('chrome://print') for t in cdp(ctl,browser,'Target.getTargets')['targetInfos']),'preview Cancel closed the dialog')
            check(devtools_eval(ctl,host,'1+1')==2,'host responds after closing print preview')
            check(devtools_eval(ctl,view,'1+1')==2,'view responds after closing print preview')
            cdp(ctl,browser,'Page.captureScreenshot',{'format':'png'})
        shutdown(ctl)
    finally:
        ctl.close()





def test_hidden_view_full_page_capture_preserves_state():
    import base64
    for style in ([], ['--use-alloy-style']):
        ctl=Ctl(style+['--custom-schemes=app'])
        try:
            ctl.wait_note('Ready')
            views=[]
            def serve(r):
                url=read_request(r)[2]
                body=b'<style>body{margin:0}justcef-view{display:block;width:500px;height:340px}</style><div style="height:1400px">Capture controls above the view</div><justcef-view src="app://content/page"></justcef-view>' if 'host' in url else b'<!doctype html><style>body{margin:0;height:1800px;background:#123456}</style><button style="position:absolute;left:20px;top:20px;width:160px;height:60px" onclick="clicks++">Click</button><script>window.clicks=0;</script>'
                return proxy_response(200,{'Content-Type':'text/html'},body)
            ctl.handlers[R['Proxy']]=serve
            ctl.handlers[R['ViewCreated']]=lambda r:(views.append((r.i32(),r.i32(),r.s())),(OK,W().b1(True).b))[1]
            host=ctl.create('app://host/index',views=True,bridge=False)
            deadline=time.time()+10
            while not views and time.time()<deadline:time.sleep(.05)
            check(bool(views),'offscreen capture view created')
            view=views[0][1]
            ctl.wait_note('LoadingState',lambda r:r.i32()==view and not r.b1())
            def visibility(expected):
                deadline=time.time()+5
                while devtools_eval(ctl,view,'document.visibilityState')!=expected and time.time()<deadline:time.sleep(.05)
                check(devtools_eval(ctl,view,'document.visibilityState')==expected,'view visibility '+expected)
            def show():
                devtools_eval(ctl,host,"document.querySelector('justcef-view').scrollIntoView({block:'center'});true")
                visibility('visible')
                devtools_eval(ctl,host,"new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))")
                time.sleep(.05)
            baseline_visibility=devtools_eval(ctl,view,'document.visibilityState')
            deadline=time.time()+5
            while devtools_eval(ctl,view,'[innerWidth,innerHeight]') != [500,340] and time.time()<deadline:time.sleep(.05)
            check(devtools_eval(ctl,view,'[innerWidth,innerHeight]')==[500,340],'never-shown view has its CSS viewport dimensions')
            cdp(ctl,view,'Page.captureScreenshot',{'format':'png'})
            initial=devtools_eval(ctl,view,'[innerWidth,innerHeight]')
            devtools_eval(ctl,host,'scrollTo(0,0);true')
            visibility(baseline_visibility)
            devtools_eval(ctl,view,"window.visibilityEvents=[];document.addEventListener('visibilitychange',()=>visibilityEvents.push(document.visibilityState));document.body.style.backgroundColor='#2468ac';true")
            for index in range(2):
                size=cdp(ctl,view,'Page.getLayoutMetrics')['cssContentSize']
                started=time.monotonic()
                png=base64.b64decode(cdp(ctl,view,'Page.captureScreenshot',{'format':'png','captureBeyondViewport':True,'clip':dict(size,scale=1)})['data'])
                check(time.monotonic()-started<5,'hidden full-page capture completed promptly')
                data=base64.b64encode(png).decode()
                pixels=devtools_eval(ctl,host,"""(async()=>{const bytes=Uint8Array.from(atob('%s'),c=>c.charCodeAt(0));const bitmap=await createImageBitmap(new Blob([bytes],{type:'image/png'}));try{const canvas=document.createElement('canvas');canvas.width=bitmap.width;canvas.height=bitmap.height;const ctx=canvas.getContext('2d');ctx.drawImage(bitmap,0,0);return {height:bitmap.height,pixel:Array.from(ctx.getImageData(100,1000,1,1).data).slice(0,3)}}finally{bitmap.close()}})()""" % data)
                check(pixels['height']>=1800,'hidden capture includes the full document')
                check(all(abs(a-b)<=1 for a,b in zip(pixels['pixel'],[36,104,172])),'hidden capture contains fresh pixels: %r' % pixels)
                check(devtools_eval(ctl,host,'scrollY')==0,'capture did not scroll the host page')
                check(devtools_eval(ctl,view,'document.visibilityState')==baseline_visibility,'capture preserved page visibility')
                check(devtools_eval(ctl,view,'visibilityEvents')==[],'capture did not expose a visibility transition')
                check(devtools_eval(ctl,view,'[innerWidth,innerHeight]')==initial,'capture restored the viewport')
            devtools_eval(ctl,host,"const v=document.querySelector('justcef-view');v.style.width='600px';v.style.height='400px';true")
            deadline=time.time()+5
            while devtools_eval(ctl,view,'[innerWidth,innerHeight]') != [600,400] and time.time()<deadline:time.sleep(.05)
            check(devtools_eval(ctl,view,'[innerWidth,innerHeight]')==[600,400],'hidden resize updates the real website viewport')
            size=cdp(ctl,view,'Page.getLayoutMetrics')['cssContentSize']
            cdp(ctl,view,'Page.captureScreenshot',{'format':'png','captureBeyondViewport':True,'clip':dict(size,scale=1)})
            check(devtools_eval(ctl,view,'[innerWidth,innerHeight]')==[600,400],'capture preserved the resized hidden viewport')
            devtools_eval(ctl,host,"v.style.width='500px';v.style.height='340px';true")
            show()
            rect=devtools_eval(ctl,host,"v.getBoundingClientRect().toJSON()")
            for kind in ('mouseMoved','mousePressed','mouseReleased'):
                params=dict(type=kind,x=rect['x']+70,y=rect['y']+40)
                if kind!='mouseMoved':params.update(button='left',clickCount=1)
                cdp(ctl,host,'Input.dispatchMouseEvent',params)
            deadline=time.time()+3
            while devtools_eval(ctl,view,'clicks')<1 and time.time()<deadline:time.sleep(.05)
            check(devtools_eval(ctl,view,'clicks')==1,'native input survives hidden full-page captures: '+str(devtools_eval(ctl,view,"({clicks,scrollY,width:innerWidth,height:innerHeight,button:document.querySelector('button').getBoundingClientRect().toJSON()})")))
            shutdown(ctl)
        finally:
            ctl.close()



def test_composed_capture_alignment_and_nested_content():
    import base64
    import html
    def sample(ctl, host, data, raw, checks, width, height):
        return devtools_eval(ctl,host,"""(async()=>{const bitmap=await createImageBitmap(new Blob([Uint8Array.from(atob('%s'),c=>c.charCodeAt(0))],{type:'image/png'}));try{const original=await createImageBitmap(new Blob([Uint8Array.from(atob('%s'),c=>c.charCodeAt(0))],{type:'image/png'}));const c=new OffscreenCanvas(bitmap.width,bitmap.height),ctx=c.getContext('2d'),o=new OffscreenCanvas(original.width,original.height),raw=o.getContext('2d');ctx.drawImage(bitmap,0,0);raw.drawImage(original,0,0);original.close();return %s.map(([x,y,color])=>{const px=Math.round(x*bitmap.width/%s),py=Math.round(y*bitmap.height/%s);return {x,y,expected:color??Array.from(raw.getImageData(px,py,1,1).data).slice(0,3),actual:Array.from(ctx.getImageData(px,py,1,1).data).slice(0,3)}})}finally{bitmap.close()}})()""" % (data,raw,json.dumps(checks),width,height))
    for style in ([], ['--use-alloy-style']):
        ctl = Ctl(style + ['--custom-schemes=app'])
        try:
            ctl.wait_note('Ready')
            views = []
            nested = '<style>body{margin:0;background:#20c080}</style><iframe style="position:absolute;left:9px;top:7px;width:30px;height:25px;border:0" srcdoc="&lt;style&gt;body{margin:0;background:#dc267f}&lt;/style&gt;&lt;body id=deep&gt;"></iframe>'
            content = '<!doctype html><style>body{margin:0;background:#e04030;height:900px}iframe{position:absolute;left:23px;top:29px;width:80px;height:70px;border:0}</style><iframe srcdoc="' + html.escape(nested, quote=True) + '"></iframe>'
            host_page = b'''<!doctype html><style>body{margin:0;background:#132436}justcef-view{display:block;background:white}.outer{position:relative;margin-left:31.25px;width:340px;height:220px;overflow:hidden}.inner{position:relative;left:-13.5px;top:17.25px;width:400px;height:190px;overflow:hidden}#first{overflow:hidden;position:relative;left:20.25px;top:11.5px;width:350.5px;height:150.5px}</style><div style="height:1100px"></div><div class="outer"><div class="inner"><justcef-view id="first" src="app://content/one"><justcef-view id="nested" style="position:absolute;left:180.25px;top:40.25px;width:90.5px;height:80.5px;overflow:hidden" src="app://content/nested"><justcef-view id="deepest" style="position:absolute;left:50.25px;top:35.25px;width:80.5px;height:60.5px" src="app://content/deepest"></justcef-view></justcef-view></justcef-view></div></div><div style="height:120px"></div><div id="shadow"></div><div style="height:250px"></div><script>const root=document.querySelector('#shadow').attachShadow({mode:'closed'});root.innerHTML='<div style="position:relative;margin-left:91.25px;width:260px;height:130px;overflow:hidden"><justcef-view style="display:block;position:relative;left:-9.5px;top:-7.25px;width:280.75px;height:160.25px;background:white" src="app://content/two"></justcef-view></div>';window.testShadowView=root.querySelector('justcef-view');</script>'''
            def serve(r):
                url = read_request(r)[2]
                body = host_page if 'host' in url else (b'<style>body{margin:0;background:#8030c0}</style>' if 'nested' in url else (b'<style>body{margin:0;background:#e0c030}</style>' if 'deepest' in url else content.encode()))
                return proxy_response(200, {'Content-Type':'text/html'}, body)
            ctl.handlers[R['Proxy']] = serve
            ctl.handlers[R['ViewCreated']] = lambda r: (views.append((r.i32(), r.i32(), r.s())), (OK, W().b1(True).b))[1]
            host = ctl.create('app://host/index', views=True, bridge=False)
            deadline = time.time()+10
            while len(views)<4 and time.time()<deadline: time.sleep(.05)
            check(len(views)==4, 'three view levels and a closed-shadow view created')
            for _, view, url in views:
                if url.endswith(('/nested', '/deepest')):
                    continue
                deadline = time.time()+10
                ready = False
                while not ready and time.time()<deadline:
                    try:
                        ready = devtools_eval(ctl,view,"!!document.querySelector('iframe')?.contentDocument?.querySelector('iframe')?.contentDocument?.querySelector('#deep')")
                    except AssertionError:
                        pass
                    if not ready: time.sleep(.05)
                check(ready,'two iframe levels loaded')
            geometry = """(()=>{const list=[document.querySelector('#first'),window.testShadowView];return list.map(v=>{const r=v.getBoundingClientRect();let n=v.parentNode instanceof ShadowRoot?v.parentNode.host:v.parentElement;const outer=n.parentElement?.className==='outer'?n.parentElement:n;const c=outer.getBoundingClientRect();return {id:v.viewId,x:r.x+scrollX,y:r.y+scrollY,width:r.width,height:r.height,left:Math.max(r.left,c.left)+scrollX,right:Math.min(r.right,c.right)+scrollX,top:Math.max(r.top,c.top)+scrollY,bottom:Math.min(r.bottom,c.bottom)+scrollY}})})()"""
            for zoom in (0, 1):
                status,_ = ctl.result(ctl.call(9, W().i32(host).f64(float(zoom)).b))
                check(status==OK, 'set capture zoom')
                time.sleep(.2)
                for scroll in (0, 1050):
                    devtools_eval(ctl,host,'scrollTo(0,%d);true'%scroll)
                    time.sleep(.2)
                    positions = devtools_eval(ctl,host,geometry)
                    state = devtools_eval(ctl,host,'[scrollX,scrollY,innerWidth,innerHeight]')
                    body_color = [224,64,48] if scroll == 0 else [48,96,224]
                    color = '#e04030' if scroll == 0 else '#3060e0'
                    for _, view, url in views:
                        if url.endswith(('/nested', '/deepest')): continue
                        devtools_eval(ctl,view,"document.body.style.backgroundColor='%s';true"%color)
                    image = browser_command(ctl,host,'captureScreenshot',{'fullPage':True,'format':'png'})
                    check(devtools_eval(ctl,host,'[scrollX,scrollY,innerWidth,innerHeight]')==state,'composition preserves scrolling and layout')
                    data = image['data']
                    checks=[]
                    for position in positions:
                        checks.extend([
                            [position['left']+5,position['top']+5,body_color],
                            [position['right']-3,position['top']+5,body_color],
                            [position['left']-3,position['top']+5,[19,36,54]],
                            [position['right']+3,position['top']+5,[19,36,54]],
                            [position['x']+40,position['y']+43,[220,38,127]],
                            [position['left']+5,position['bottom']+3,[19,36,54]],
                        ])
                    first = positions[0]
                    checks.extend([[first['x']+200,first['y']+50,[128,48,192]],
                                   [first['x']+240,first['y']+90,[224,192,48]],
                                   [first['x']+275,first['y']+90,body_color]])
                    if zoom == 0:
                        checks.extend([[38,1134,body_color],[370,1134,body_color],[371,1134,None],
                                       [43,1128,None],[43,1129,body_color],[43,1277,body_color],[43,1280,None]])
                    metrics=cdp(ctl,host,'Page.getLayoutMetrics')['cssContentSize']
                    raw=cdp(ctl,host,'Page.captureScreenshot',{'format':'png','captureBeyondViewport':True,'clip':dict(x=0,y=0,width=metrics['width']*(1.2**zoom),height=metrics['height']*(1.2**zoom),scale=1)})['data']
                    pixels=sample(ctl,host,data,raw,checks,metrics['width'],metrics['height'])
                    for pixel in pixels:
                        check(all(abs(a-b)<=1 for a,b in zip(pixel['actual'],pixel['expected'])), 'composed pixel alignment, zoom %s scroll %s: %r'%(zoom,scroll,pixel))
                    if scroll == 1050:
                        viewport = browser_command(ctl,host,'captureScreenshot',{'format':'png'})['data']
                        viewport_checks = [[x-state[0],y-state[1],None if color == [19,36,54] else color] for x,y,color in checks if 0<=x-state[0]<state[2] and 0<=y-state[1]<state[3]]
                        original = cdp(ctl,host,'Page.captureScreenshot',{'format':'png'})['data']
                        viewport_pixels=sample(ctl,host,viewport,original,viewport_checks,state[2],state[3])
                        for pixel in viewport_pixels:
                            check(all(abs(a-b)<=1 for a,b in zip(pixel['actual'],pixel['expected'])), 'viewport composition alignment: %r'%pixel)
            for format, signature in [('jpeg',b'\xff\xd8'),('webp',b'RIFF')]:
                encoded = browser_command(ctl,host,'captureScreenshot',{'fullPage':True,'format':format,'quality':100})
                check(base64.b64decode(encoded['data']).startswith(signature),'composed '+format+' encoding')
            shutdown(ctl)
        finally:
            ctl.close()

TESTS = [test_composed_capture_alignment_and_nested_content, test_hidden_view_full_page_capture_preserves_state, test_capture_preserves_view_input, test_macos_print_preview_remains_responsive, test_live_popup_with_restrictive_csp, test_http_popups_inherit_proxy_routing, test_permission_navigation_is_frame_scoped, test_permission_records_survive_restart, test_certificate_decisions, test_view_popup_routing_and_policy, test_download_lifecycle, test_capture_pdf_and_permission_records, test_custom_schemes_and_view_boundary, test_view_media_permission_decisions, test_protocol_vectors, test_ordering_and_basics, test_bridge_calls_that_call_native, test_large_bridge_payloads, test_async_modify, test_streams_and_cancel,
         test_backpressure_and_threads, test_shutdown_with_pending_work, test_runtime_info, test_file_paths, test_native_file_drag, test_concurrent_startup, test_frameless_drag_regions, test_caption_button_regions, test_views_handshake, test_views_rounded_clipping, test_views_lifecycle]

if __name__ == "__main__":
    if not NATIVE or not os.path.exists(NATIVE):
        print("Set JUSTCEF_NATIVE to the justcefnative binary.")
        sys.exit(2)
    selected = [t for t in TESTS if len(sys.argv) < 2 or t.__name__ in sys.argv[1:]]
    failures = 0
    for test in selected:
        started = time.time()
        result = {}
        begin(test.__name__)

        def run():
            try:
                test()
                result["error"] = None
            except Exception:
                result["error"] = traceback.format_exc()

        worker = threading.Thread(target=run, daemon=True)
        worker.start()
        worker.join(240)
        if worker.is_alive():
            result["error"] = "timed out"
        logs = list(LOGS)
        close_all()
        worker.join(5)
        status = "PASS" if result.get("error") is None else "FAIL"
        failures += status == "FAIL"
        print("%s %s (%.1fs)" % (status, test.__name__, time.time() - started))
        if result.get("error"):
            print(result["error"])
            for path in logs:
                print("native log: %s" % path)
    sys.exit(1 if failures else 0)
