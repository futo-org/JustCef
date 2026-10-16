#!/usr/bin/env python3
"""Launch the interactive CEF feature check; close its windows to stop.

Defaults to the patched ARM64 runtime in build/cef-e2e-patched.
Override JUSTCEF_NATIVE to test a different native runtime.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
os.environ.setdefault('JUSTCEF_NATIVE', str(ROOT / 'build/cef-e2e-patched/Release/justcefnative.app/Contents/MacOS/justcefnative'))
from jcipc import Ctl, C, CN, R, N, W, Rd, OK, ERROR, proxy_response, read_request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--both', action='store_true', help='Open Chrome and Alloy test windows.')
    parser.add_argument('--check-close', action='store_true', help='Check both page close buttons, then exit.')
    args = parser.parse_args()
    if not Path(os.environ['JUSTCEF_NATIVE']).is_file():
        raise SystemExit('Set JUSTCEF_NATIVE to an existing justcefnative executable.')
    run_dir = ROOT / 'build/cef-manual' / time.strftime('%Y%m%d-%H%M%S')
    source = run_dir / 'sample-files'
    target = run_dir / 'drop-here'
    source.mkdir(parents=True)
    target.mkdir()
    files = [source / 'Sample notes.txt', source / 'café 文件.txt']
    for i, file in enumerate(files, 1):
        file.write_text(f'JustCef native drag sample {i}.\nUnicode: café 文件\n', encoding='utf-8')
    page = (ROOT / 'tests/pages/manual-features.html').read_bytes()
    version = (ROOT / 'cef/cef.version').read_text().strip()
    windows = {}
    lock = threading.RLock()

    def request(ctl, opcode, body=b''):
        status, payload = ctl.result(ctl.call(opcode, body))
        if status != OK:
            raise RuntimeError(f'Native command {opcode}: status {status}, {payload!r}')
        return payload

    def evaluate(ctl, wid, expression):
        params = json.dumps(dict(expression=expression, awaitPromise=True, returnByValue=True)).encode()
        reader = Rd(request(ctl, C['ExecuteDevTools'], W().i32(wid).s('Runtime.evaluate').b1(True).by(params).b))
        if not reader.b1():
            raise RuntimeError('DevTools evaluation failed')
        result = json.loads(reader.by())
        if 'exceptionDetails' in result:
            raise RuntimeError(str(result))
        return result.get('result', {}).get('value')

    def save_manifest():
        (run_dir / 'session.json').write_text(json.dumps(dict(
            controller_pid=os.getpid(), native=os.environ['JUSTCEF_NATIVE'], target=str(target),
            sample_files=[str(p) for p in files],
            windows={style: dict(pid=ctl.proc.pid, id=wid, log=ctl.log_path) for style, (ctl, wid) in windows.items()}
        ), indent=2))

    def launch(style):
        with lock:
            if style in windows and not windows[style][0].closed.is_set():
                ctl, wid = windows[style]
                request(ctl, 28, W().i32(wid).b)
                return
            ctl = Ctl(['--use-alloy-style'] if style == 'Alloy' else [])
            try:
                ctl.wait_note('Ready')

                def serve(reader):
                    _, _, url, _, _ = read_request(reader)
                    if url.endswith('/manual.html'):
                        return proxy_response(200, {'Content-Type': 'text/html; charset=utf-8'}, page)
                    return proxy_response(404, {'Content-Type': 'text/plain'}, b'Not found')

                def rpc(reader):
                    wid = reader.i32()
                    method = reader.s()
                    reader.by()
                    result = None
                    if method == 'info':
                        runtime = request(ctl, C['GetRuntimeInfo'])
                        result = dict(style=style, version=version, runtime=list(runtime))
                    elif method == 'arm':
                        body = W().i32(wid).u32(len(files))
                        for file in files:
                            body.s(str(file))
                        request(ctl, C['SetNextDragFiles'], body.b)
                        result = dict(count=len(files))
                    elif method == 'open_target':
                        subprocess.run(['open', str(target)], check=True)
                    elif method == 'check_copies':
                        count = sum((target / file.name).is_file() and (target / file.name).read_bytes() == file.read_bytes() for file in files)
                        result = dict(ok=count == len(files), count=count)
                    elif method == 'drag_off':
                        request(ctl, C['SetDraggableRegions'], W().i32(wid).b1(True).u32(0).b)
                    elif method == 'drag_css':
                        request(ctl, C['SetDraggableRegions'], W().i32(wid).b1(False).b)
                    elif method in ('fullscreen', 'exit_fullscreen'):
                        current = Rd(request(ctl, C['Debug'], W().u8(4).u32(wid).b)).b1()
                        request(ctl, 31, W().i32(wid).b1(not current if method == 'fullscreen' else False).b)
                    elif method == 'minimize':
                        request(ctl, 18, W().i32(wid).b)
                    elif method == 'close':
                        request(ctl, C['Close'], W().i32(wid).b1(False).b)
                    elif method == 'other_style':
                        launch('Alloy' if style == 'Chrome' else 'Chrome')
                    else:
                        return ERROR, 'Unknown manual-test action'
                    print(f'{style}: {method}: {result}', flush=True)
                    return OK, W().by(json.dumps(result).encode()).b

                ctl.handlers[R['Proxy']] = serve
                ctl.handlers[R['BridgeRpc']] = rpc
                wid = ctl.create('http://manual.test/manual.html', proxy=True, frameless=True)
                windows[style] = (ctl, wid)
                request(ctl, 44, W().i32(wid).s(f'JustCef feature test — {style}').b)
                ctl.wait_note('LoadingState', lambda r: r.i32() == wid and not r.b1())
                save_manifest()
                deadline = time.time() + 15
                while not evaluate(ctl, wid, 'globalThis.manualReady === true'):
                    if time.time() > deadline:
                        raise RuntimeError('Manual test page did not initialize')
                    time.sleep(.1)
                request(ctl, 32, W().i32(wid).b)
                request(ctl, 28, W().i32(wid).b)
                print(f'READY {style}: native pid {ctl.proc.pid}, window {wid}', flush=True)
            except Exception:
                ctl.close()
                raise

    def poll_windows():
        with lock:
            live = [(style, ctl, wid) for style, (ctl, wid) in windows.items() if not ctl.closed.is_set()]
        for style, ctl, wid in live:
            try:
                op, body, _ = ctl.notes.get(timeout=.05)
                if N.get(op) == 'WindowClosed' and Rd(body).i32() == wid:
                    try:
                        ctl.notify(CN['Exit'])
                    except OSError:
                        pass
            except queue.Empty:
                pass
        return bool(live)

    try:
        launch('Chrome')
        if args.check_close:
            for order in (('Alloy', 'Chrome'), ('Chrome', 'Alloy')):
                for style in order:
                    launch(style)
                for index, style in enumerate(order):
                    ctl, wid = windows[style]
                    params = json.dumps(dict(expression='document.querySelector("[data-command=close]").click(); true')).encode()
                    ctl.call(C['ExecuteDevTools'], W().i32(wid).s('Runtime.evaluate').b1(True).by(params).b)
                    deadline = time.time() + 15
                    while not ctl.closed.is_set():
                        if time.time() > deadline:
                            raise RuntimeError(f'{style} close button did not close the window')
                        poll_windows()
                    if ctl.proc.wait(15) != 0:
                        raise RuntimeError(f'{style} native process did not exit cleanly')
                    ctl.close()
                    if index == 0:
                        other, other_wid = windows[order[1]]
                        if other.proc.poll() is not None or not evaluate(other, other_wid, 'globalThis.manualReady === true'):
                            raise RuntimeError('Closing one demo affected the other window')
                    print(f'PASS {style} page close button; order {order}', flush=True)
            return
        if args.both:
            launch('Alloy')
        print(f'Target folder: {target}\nSession: {run_dir / "session.json"}', flush=True)
        while poll_windows():
            pass
    except KeyboardInterrupt:
        pass
    finally:
        for ctl, _ in windows.values():
            ctl.close()


if __name__ == '__main__':
    main()
