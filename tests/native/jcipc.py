import os
import tempfile
import queue
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import Future

NATIVE = os.environ.get("JUSTCEF_NATIVE", "")
LOG_DIR = os.environ.get("JUSTCEF_TEST_LOGS", tempfile.gettempdir())

REQ, RESP, NOTE, CANCEL = 0, 1, 2, 3
OK, ERROR, CANCELED, NOTFOUND, NOTHANDLED = 0, 1, 2, 3, 4

SCENARIO = "scenario"
LOGS = []
LIVE = []
_reg = threading.Lock()
_seq = [0]


def begin(name):
    global SCENARIO
    with _reg:
        SCENARIO = name
        del LOGS[:]
        _seq[0] = 0


def close_all():
    with _reg:
        live = list(LIVE)
    for ctl in live:
        ctl.close()


def packet(typ, op, rid, body=b""):
    return struct.pack("<IIBB", 10 + len(body) - 4, rid, typ, op) + bytes(body)


C = dict(Ping=0, Print=1, Echo=2, WindowCreate=3, LoadUrl=6, GetSize=50, GetZoom=56, Close=22, ExecuteDevTools=42, BridgeRpc=57,
         SetModify=34, AddUrlToModify=48, Debug=250)
CN = dict(Exit=0, StreamData=1, StreamEnd=2, StreamError=3)
N = {0: "Ready", 1: "Exit", 2: "WindowOpened", 3: "WindowClosed", 5: "WindowFocused", 6: "WindowUnfocused", 12: "Fullscreen", 13: "FrameLoadStart",
     14: "FrameLoadEnd", 15: "FrameLoadError", 16: "DevToolsEvent", 17: "LoadingState", 18: "StreamCredit", 19: "StreamCancel", 250: "Debug"}
R = dict(Ping=0, Print=1, Echo=2, Proxy=3, Modify=4, BridgeRpc=9, ViewCreated=11)


class W:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v): self.b += struct.pack("<B", v); return self
    def b1(self, v): return self.u8(1 if v else 0)
    def i32(self, v): self.b += struct.pack("<i", v); return self
    def u32(self, v): self.b += struct.pack("<I", v); return self
    def i64(self, v): self.b += struct.pack("<q", v); return self
    def u64(self, v): self.b += struct.pack("<Q", v); return self
    def f64(self, v): self.b += struct.pack("<d", v); return self

    def s(self, v):
        if v is None:
            return self.i32(-1)
        e = v.encode()
        self.i32(len(e)); self.b += e; return self

    def by(self, v):
        self.u32(len(v)); self.b += v; return self


class Rd:
    def __init__(self, b):
        self.b = bytes(b); self.p = 0

    def take(self, n):
        v = self.b[self.p:self.p + n]; self.p += n; return v

    def u8(self): return self.take(1)[0]
    def b1(self): return self.u8() != 0
    def i32(self): return struct.unpack("<i", self.take(4))[0]
    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def i64(self): return struct.unpack("<q", self.take(8))[0]
    def u64(self): return struct.unpack("<Q", self.take(8))[0]
    def f64(self): return struct.unpack("<d", self.take(8))[0]

    def s(self):
        n = self.i32()
        return None if n < 0 else self.take(n).decode()

    def by(self): return self.take(self.u32())
    def rest(self): return self.take(len(self.b) - self.p)


class Ctl:
    def __init__(self, extra=None):
        c2n_r, c2n_w = os.pipe()
        n2c_r, n2c_w = os.pipe()
        args = [NATIVE, "--change-stack-guard-on-fork=disable", "--parent-to-child", str(c2n_r), "--child-to-parent", str(n2c_w), "--no-sandbox", "--enable-ipc-debug"] + (extra or [])
        os.makedirs(LOG_DIR, exist_ok=True)
        with _reg:
            _seq[0] += 1
            self.log_path = os.path.join(LOG_DIR, "justcef-native-%s-%d-%d.log" % (SCENARIO, os.getpid(), _seq[0]))
            LOGS.append(self.log_path)
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen(args, pass_fds=(c2n_r, n2c_w), stdout=self.log, stderr=self.log)
        os.close(c2n_r); os.close(n2c_w)
        self.wf = os.fdopen(c2n_w, "wb", buffering=0)
        self.rf = os.fdopen(n2c_r, "rb", buffering=0)
        self.wlock = threading.Lock()
        self.next_id = 0
        self.pending = {}
        self.plock = threading.Lock()
        self.notes = queue.Queue()
        self.order = []
        self.raw = {}
        self.handlers = {}
        self.cancels = []
        self.credit = {}
        self.credit_cv = threading.Condition()
        self.canceled_streams = set()
        self.paused = threading.Event(); self.paused.set()
        self.closed = threading.Event()
        self.reader = threading.Thread(target=self.read_loop, daemon=True); self.reader.start()
        with _reg:
            LIVE.append(self)

    def send(self, typ, op, rid, body=b""):
        with self.wlock:
            self.wf.write(packet(typ, op, rid, body))

    def call(self, op, body=b"", timeout=30):
        with self.plock:
            self.next_id += 1; rid = self.next_id
            f = Future(); self.pending[rid] = f
        self.send(REQ, op, rid, body)
        return f

    def result(self, f, timeout=30):
        status, payload = f.result(timeout)
        return status, payload

    def notify(self, op, body=b""):
        self.send(NOTE, op, 0, body)

    def readn(self, n):
        buf = bytearray()
        while len(buf) < n:
            chunk = self.rf.read(n - len(buf))
            if not chunk:
                raise EOFError()
            buf += chunk
        return bytes(buf)

    def read_loop(self):
        try:
            while True:
                self.paused.wait()
                hdr = self.readn(10)
                size, rid, typ, op = struct.unpack("<IIBB", hdr)
                body = self.readn(size + 4 - 10)
                self.order.append((typ, op, rid, len(body)))
                self.raw.setdefault((typ, op), hdr + body)
                if typ == RESP:
                    with self.plock:
                        f = self.pending.pop(rid, None)
                    if f:
                        r = Rd(body); st = r.u8()
                        f.set_result((st, r.rest() if st == OK else r.s()))
                elif typ == NOTE:
                    if op == 18:
                        r = Rd(body); sid = r.u32(); n = r.u32()
                        with self.credit_cv:
                            self.credit[sid] = self.credit.get(sid, 0) + n; self.credit_cv.notify_all()
                    elif op == 19:
                        r = Rd(body); sid = r.u32()
                        with self.credit_cv:
                            self.canceled_streams.add(sid); self.credit_cv.notify_all()
                    self.notes.put((op, body, time.time()))
                elif typ == REQ:
                    h = self.handlers.get(op)
                    threading.Thread(target=self.run_handler, args=(h, op, rid, body), daemon=True).start()
                elif typ == CANCEL:
                    self.cancels.append((op, rid))
        except (EOFError, OSError, ValueError):
            pass
        finally:
            self.closed.set()

    def run_handler(self, h, op, rid, body):
        try:
            if op == R["Echo"]:
                self.send(RESP, op, rid, bytes([OK]) + body); return
            if not h:
                self.send(RESP, op, rid, bytes([5]) + W().s("unsupported").b); return
            st, payload = h(Rd(body))
            if st == OK:
                self.send(RESP, op, rid, bytes([OK]) + payload)
            else:
                self.send(RESP, op, rid, bytes([st]) + W().s(payload).b)
        except Exception as e:
            self.send(RESP, op, rid, bytes([ERROR]) + W().s(repr(e)).b)

    def wait_note(self, name, pred=lambda b: True, timeout=20):
        end = time.time() + timeout
        seen = []
        while time.time() < end:
            try:
                op, body, t = self.notes.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            seen.append(N.get(op, op))
            if N.get(op) == name and pred(Rd(body)):
                return Rd(body)
        raise TimeoutError(f"no {name}; saw {seen[-20:]}")

    def create(self, url, proxy=False, bridge=True, modify=False, modify_body=False, shown=True, views=False, modify_timeout=0, policy=0, proxy_timeout=0):
        w = W()
        for v in [True, False, False, True, shown, True, True, modify, modify_body, proxy, True, bridge]:
            w.b1(v)
        for v in [800, 600, 900, 700]:
            w.i32(v)
        w.s(url).s(None).s(None).s(None).b1(views).u32(modify_timeout).u8(policy).u32(proxy_timeout)
        st, payload = self.result(self.call(C["WindowCreate"], w.b))
        assert st == OK, (st, payload)
        return Rd(payload).i32()

    def stream_body(self, sid, data, chunk=256 * 1024, delay=0.0):
        sent = 0
        with self.credit_cv:
            self.credit.setdefault(sid, 0)
        budget = 1024 * 1024
        while sent < len(data):
            with self.credit_cv:
                while budget <= 0 and sid not in self.canceled_streams:
                    self.credit_cv.wait(5)
                    budget += self.credit.pop(sid, 0)
                if sid in self.canceled_streams:
                    return "canceled"
                n = min(chunk, len(data) - sent, budget)
            self.notify(CN["StreamData"], W().u32(sid).b + data[sent:sent + n])
            sent += n; budget -= n
            with self.credit_cv:
                budget += self.credit.pop(sid, 0)
            if delay:
                time.sleep(delay)
        self.notify(CN["StreamEnd"], W().u32(sid).u64(len(data)).b)
        return "end"

    def close(self):
        with _reg:
            if self in LIVE:
                LIVE.remove(self)
        if self.proc.poll() is None:
            try:
                self.notify(CN["Exit"])
            except Exception:
                pass
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                try:
                    self.proc.wait(10)
                except subprocess.TimeoutExpired:
                    pass
        for f in (self.wf, self.rf, self.log):
            try:
                f.close()
            except Exception:
                pass


def proxy_response(status=200, headers=None, body=b"", stream_id=None, length=-1):
    w = W().u32(status).s("OK")
    headers = headers or {}
    w.u32(len(headers))
    for k, v in headers.items():
        w.s(k).s(v)
    if stream_id is not None:
        w.u8(2).i64(length).u32(stream_id)
    else:
        w.u8(1).by(body)
    return OK, bytes(w.b)


def read_request(r):
    wid = r.i32(); method = r.s(); url = r.s()
    headers = [(r.s(), r.s()) for _ in range(r.i32())]
    elements = []
    for _ in range(r.i32()):
        t = r.u8()
        elements.append((t, r.by() if t == 1 else r.s()))
    return wid, method, url, headers, elements


class PageServer:
    def __init__(self, root, routes=None):
        import functools
        import http.server
        class QuietHandler(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *args):
                pass

        handler = functools.partial(QuietHandler, directory=root)
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.origin = "http://127.0.0.1:%d" % self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
