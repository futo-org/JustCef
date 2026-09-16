import json
import os
import sys
import threading
import time
import traceback

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


def open_app(**kw):
    ctl = Ctl()
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


TESTS = [test_protocol_vectors, test_ordering_and_basics, test_bridge_calls_that_call_native, test_large_bridge_payloads, test_async_modify, test_streams_and_cancel,
         test_backpressure_and_threads, test_shutdown_with_pending_work, test_views_handshake, test_views_lifecycle]

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
