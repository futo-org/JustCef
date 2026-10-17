"""Interactive browser-feature demo. Run with --help for native/profile options."""
import argparse
import base64
import json
import os
from pathlib import Path
import secrets
import shutil
import ssl
import struct
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

import jcipc
from jcipc import Ctl, C, CN, R, W, Rd, OK, proxy_response

PAGES = Path(__file__).resolve().parents[1] / 'pages'
ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', default=os.environ.get('JUSTCEF_NATIVE'), help='path to justcefnative')
    parser.add_argument('--port', type=int, default=8765, help='fixed localhost port preserves the permission origin across restarts')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/browser-features-demo', help='captures, downloads, certificate, and persistent Chromium profile')
    parser.add_argument('--fake-media', action='store_true', help='use Chromium test camera/microphone instead of physical devices')
    parser.add_argument('--smoke', action='store_true', help='check startup, controls and capture, then exit')
    args = parser.parse_args()
    native = args.native
    if not native:
        for candidate in [ROOT / 'build/cef-e2e-arm64/Release/justcefnative.app/Contents/MacOS/justcefnative', ROOT / 'native/build/Release/justcefnative.app/Contents/MacOS/justcefnative', ROOT / 'native/build/Release/justcefnative', ROOT / 'native/build/Release/justcefnative.exe']:
            if candidate.is_file():
                native = str(candidate)
                break
    if not native or not Path(native).is_file():
        parser.error('specify --native /absolute/path/to/justcefnative or JUSTCEF_NATIVE')
    jcipc.NATIVE = str(Path(native).resolve())
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    token = secrets.token_urlsafe(24)
    lock = threading.RLock()
    state = dict(events=[], pending=[], views=[], host=None, mode='delegate', downloads={})
    waiting = {}
    sequence = [0]
    ctl = None
    tls = None

    def log(kind, **detail):
        with lock:
            sequence[0] += 1
            state['events'].append(dict(seq=sequence[0], time=time.strftime('%H:%M:%S'), kind=kind, **detail))
            state['events'] = state['events'][-300:]
        print(kind, json.dumps(detail), flush=True)

    def native_call(op, body):
        status, payload = ctl.result(ctl.call(op, body), timeout=65)
        if status != OK:
            raise RuntimeError(f'native status {status}: {payload}')
        return Rd(payload)

    def command(browser, name, options=None):
        return json.loads(native_call(C['BrowserCommand'], W().i32(browser).s(name).s(json.dumps(options or {})).b).s())

    def devtools(browser, method, params):
        reader = native_call(C['ExecuteDevTools'], W().i32(browser).s(method).b1(True).by(json.dumps(params).encode()).b)
        success, data = reader.b1(), reader.by()
        if not success:
            raise RuntimeError(data.decode(errors='replace'))
        result = json.loads(data)
        if 'exceptionDetails' in result:
            raise RuntimeError(json.dumps(result['exceptionDetails']))
        return result

    def action(data):
        name = data['action']
        browser = int(data.get('browser') or state['host'])
        with lock:
            if browser not in [state['host'], *state['views']]:
                raise ValueError('select the host or a live demo view')
        if name == 'policy':
            result = command(browser, 'windowOpenPolicy', dict(allow=data['allow'], intercept=data['intercept']))
        elif name == 'mode':
            if data['mode'] not in ('delegate', 'allow', 'deny'):
                raise ValueError('invalid decision mode')
            with lock:
                state['mode'] = data['mode']
            result = data['mode']
        elif name == 'answer':
            with lock:
                entry = waiting.get(int(data['id']))
                if not entry:
                    raise ValueError('request expired or was canceled')
                entry['answer'] = data['answer']
                entry['event'].set()
            result = 'answered'
        elif name in ('permissions', 'resetPermissions', 'allowPermissions', 'blockPermissions'):
            options = dict(origin=origin, topLevelOrigin=origin)
            if name != 'permissions':
                options.update(permissions=3, value={'resetPermissions':0, 'allowPermissions':1, 'blockPermissions':2}[name])
            result = command(browser, 'permission', options)
        elif name == 'loadFrame':
            result = devtools(browser, 'Runtime.evaluate', dict(expression="(()=>{const frame=document.createElement('iframe');frame.src='about:blank';document.body.append(frame);return true;})()", returnByValue=True))
        elif name == 'navigateAway':
            native_call(C['LoadUrl'], W().i32(browser).s(origin + '/content?canceled=1').b)
            result = 'navigated; pending source-document requests should be canceled'
        elif name == 'print':
            result = command(browser, 'print')
        elif name == 'download':
            result = command(browser, 'download', dict(url=origin + '/download.bin'))
        elif name == 'downloadControl':
            result = command(browser, 'downloadControl', dict(id=int(data['id']), action=data['control']))
        elif name in ('screenshot', 'pdf'):
            image_format = data.get('format', 'png')
            if image_format not in ('png', 'jpeg', 'webp'):
                raise ValueError('unsupported image format')
            params = dict(format=image_format, captureBeyondViewport=bool(data.get('fullPage')))
            if image_format != 'png':
                params['quality'] = int(data.get('quality', 90))
            if name == 'pdf':
                method, extension = 'Page.printToPDF', 'pdf'
                params = dict(printBackground=True, preferCSSPageSize=True)
            else:
                method, extension = 'Page.captureScreenshot', image_format
                if data.get('fullPage'):
                    params['clip'] = dict(devtools(browser, 'Page.getLayoutMetrics', {})['cssContentSize'], scale=1)
            result = command(browser, 'captureScreenshot', dict(format=image_format, fullPage=bool(data.get('fullPage')), quality=int(data.get('quality', 90)))) if name == 'screenshot' else devtools(browser, method, params)
            target = 'host' if browser == state['host'] else 'view'
            scope = 'full-page' if data.get('fullPage') else 'viewport'
            path = output / f'{name}-{target}-{scope}-{browser}-{time.time_ns()}.{extension}'
            path.write_bytes(base64.b64decode(result['data']))
            result = dict(path=str(path), url='/files/' + path.name, bytes=path.stat().st_size, target=target, scope=scope)
        else:
            raise ValueError('unknown action')
        log('control', browser=browser, action=name, result=result)
        return result

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send(self, body, content_type='application/json', status=200):
            self.send_response(status)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            if urlsplit(self.path).path == '/':
                self.send_header('Content-Security-Policy', "default-src 'self'; style-src 'self' 'unsafe-inline'; object-src 'self'")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == '/api/state':
                with lock:
                    body = json.dumps(dict(state, origin=origin, custom='app://demo/content', certificate=certificate_url,
                                           output=str(output), downloadPath=str(output / 'download.bin'), token=token)).encode()
                return self.send(body)
            if path == '/download.bin':
                self.send_response(200)
                self.send_header('Content-Type', 'application/octet-stream')
                self.send_header('Content-Disposition', 'attachment; filename="download.bin"')
                self.send_header('Content-Length', str(8 * 1024 * 1024))
                self.end_headers()
                try:
                    for _ in range(128):
                        self.wfile.write(b'JustCef demo download\n'.ljust(65536, b'.'))
                        self.wfile.flush()
                        time.sleep(.08)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            if path.startswith('/files/'):
                name = path.removeprefix('/files/')
                if Path(name).name == name and name.endswith(('.png', '.jpeg', '.webp', '.pdf')) and (output / name).is_file():
                    return self.send((output / name).read_bytes(), 'application/pdf' if name.endswith('.pdf') else 'image/' + name.rsplit('.', 1)[-1])
            routes = {'/':'browser-features.html', '/browser-features.js':'browser-features.js', '/content':'browser-features-content.html', '/popup':'browser-features-content.html'}
            if path in routes:
                return self.send((PAGES / routes[path]).read_bytes(), 'text/javascript' if path.endswith('.js') else 'text/html; charset=utf-8')
            self.send(b'Not found', 'text/plain', 404)

        def do_POST(self):
            if self.path != '/api/action' or self.headers.get('X-Demo-Token') != token:
                return self.send(b'Forbidden', 'text/plain', 403)
            try:
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size <= 65536:
                    raise ValueError('invalid request size')
                result = action(json.loads(self.rfile.read(size)))
                self.send(json.dumps(dict(result=result)).encode())
            except Exception as error:
                log('error', message=str(error))
                self.send(json.dumps(dict(error=str(error))).encode(), status=400)

    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    origin = f'http://127.0.0.1:{server.server_port}'
    certificate_url = None
    key, cert = output / 'localhost.key', output / 'localhost.crt'
    if shutil.which('openssl'):
        if not key.exists() or not cert.exists():
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(key), '-out', str(cert), '-days', '30', '-subj', '/CN=localhost'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            key.chmod(0o600)
        tls = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        tls.socket = context.wrap_socket(tls.socket, server_side=True)
        certificate_url = f'https://localhost:{tls.server_port}/content'
        threading.Thread(target=tls.serve_forever, daemon=True).start()
    threading.Thread(target=server.serve_forever, daemon=True).start()

    extra = ['--custom-schemes=app', '--cache-path=' + str(output / 'profile'), '--use-alloy-style']
    if args.fake_media:
        extra.append('--use-fake-device-for-media-stream')
    ctl = Ctl(extra)

    def on_view(reader):
        parent, browser, url = reader.i32(), reader.i32(), reader.s()
        with lock:
            state['views'].append(browser)
        log('view-created', browser=browser, parent=parent, url=url)
        return OK, W().b1(True).b

    def proxy(reader):
        browser, method, url = reader.i32(), reader.s(), reader.s()
        if url == 'app://demo/fixture.json':
            return proxy_response(200, {'Content-Type':'application/json', 'Access-Control-Allow-Origin':'*'}, b'{"scheme":"app","servedBy":"application proxy"}')
        if urlsplit(url).netloc == 'demo' and urlsplit(url).path in ('/content', '/popup'):
            return proxy_response(200, {'Content-Type':'text/html'}, (PAGES / 'browser-features-content.html').read_bytes())
        return proxy_response(404, {'Content-Type':'text/plain'}, b'Unknown demo resource')

    def decide(reader):
        browser, kind, detail = reader.i32(), reader.s(), json.loads(reader.s())
        log('application-request', browser=browser, requestKind=kind, detail=detail)
        with lock:
            mode = state['mode']
            is_view = browser in state['views']
        if mode == 'delegate' and is_view:
            return OK, W().s('{}').b
        if mode != 'delegate':
            answer = dict(allow=mode == 'allow', remember=False, path=str(output / 'download.bin'))
        else:
            event = threading.Event()
            with lock:
                request_id = secrets.randbits(48)
                entry = dict(event=event, answer={})
                waiting[request_id] = entry
                state['pending'].append(dict(id=request_id, browser=browser, kind=kind, detail=detail))
            event.wait(55)
            with lock:
                answer = entry['answer']
                waiting.pop(request_id, None)
                state['pending'] = [item for item in state['pending'] if item['id'] != request_id]
        log('application-answer', browser=browser, requestKind=kind, answer=answer)
        return OK, W().s(json.dumps(answer)).b

    def download_update(reader):
        browser, detail = reader.i32(), json.loads(reader.s())
        with lock:
            state['downloads'][str(detail['id'])] = dict(detail, browser=browser)

    ctl.handlers.update({R['ViewCreated']:on_view, R['Proxy']:proxy, R['BrowserDecision']:decide})
    ctl.note_handlers[22] = download_update
    popups = set()

    def popup_created(reader):
        parent, browser = reader.i32(), reader.i32()
        popups.add(browser)
        log('popup-created', parent=parent, browser=browser)

    def frame_loaded(reader):
        browser, frame, main, url = reader.i32(), reader.s(), reader.b1(), reader.s()
        if main and browser in popups:
            log('popup-loaded', browser=browser, url=url)

    ctl.note_handlers[21] = popup_created
    ctl.note_handlers[14] = frame_loaded
    try:
        ctl.wait_note('Ready')
        state['host'] = ctl.create(origin + '/', views=True, bridge=False)
        ctl.wait_note('LoadingState', lambda r:r.i32() == state['host'] and not r.b1())
        print(f'\nDemo: {origin}/\nOutput/profile: {output}\nNative log: {ctl.log_path}\nClose the window or press Ctrl+C to exit.\n', flush=True)
        if args.smoke:
            deadline = time.time() + 15
            while not state['views'] and time.time() < deadline:
                time.sleep(.05)
            assert state['views'], 'embedded view not created'
            browser = state['views'][0]
            def evaluate(target, expression):
                result = devtools(target, 'Runtime.evaluate', dict(expression=expression, awaitPromise=True, returnByValue=True, userGesture=True))
                assert 'exceptionDetails' not in result, result
                return result.get('result', {}).get('value')
            deadline = time.time() + 15
            while time.time() < deadline:
                try:
                    if evaluate(browser, 'location.href') == origin + '/content':
                        break
                except RuntimeError:
                    pass
                time.sleep(.1)
            assert evaluate(browser, 'location.href') == origin + '/content'
            assert evaluate(state['host'], "action('permissions').then(x=>!!x)")
            evaluate(state['host'], "suspend(); true")
            evaluate(state['host'], "suspend(); true")
            evaluate(state['host'], "document.querySelector('#overlay').click(); true")
            time.sleep(.5)
            evaluate(state['host'], "document.querySelector('#closeOverlay').click(); true")
            custom = evaluate(browser, "fetch('app://demo/fixture.json').then(r=>r.json())")
            assert custom['scheme'] == 'app', custom
            if args.fake_media:
                command(browser, 'permission', dict(origin=origin, permissions=3, value=0))
                evaluate(browser, "document.querySelector('#microphone').click();true")
                deadline = time.time() + 10
                while not evaluate(state['host'], '!!current') and time.time() < deadline:
                    time.sleep(.1)
                assert evaluate(state['host'], "current?.browser") == browser
                action(dict(action='loadFrame', browser=browser))
                assert evaluate(browser, "document.querySelectorAll('iframe').length") == 1
                evaluate(state['host'], 'answer(true)')
                deadline = time.time() + 10
                while not evaluate(browser, "document.querySelector('#log').textContent.includes('media granted')") and time.time() < deadline:
                    time.sleep(.1)
                assert evaluate(browser, "document.querySelector('#log').textContent.includes('media granted')")
                evaluate(browser, 'stop();true')
            evaluate(browser, "document.querySelector('#open').click();true")
            deadline = time.time() + 10
            while not any(item['kind'] == 'popup-created' for item in state['events']) and time.time() < deadline:
                time.sleep(.1)
            assert any(item['kind'] == 'popup-created' for item in state['events'])
            for popup in [item['browser'] for item in state['events'] if item['kind'] == 'popup-created']:
                native_call(C['Close'], W().i32(popup).b1(False).b)
            pdf = action(dict(action='pdf', browser=browser))
            assert (output / Path(pdf['path']).name).read_bytes().startswith(b'%PDF-')
            command(browser, 'permission', dict(origin=origin))
            result = action(dict(action='screenshot', browser=state['host']))
            assert result['bytes'] > 100
            assert evaluate(state['host'], '1+1') == 2
            result = action(dict(action='screenshot', browser=browser))
            assert result['bytes'] > 100
            assert evaluate(browser, '1+1') == 2
            assert evaluate(state['host'], '1+1') == 2
            evaluate(state['host'], 'scrollTo(0,0);true')
            deadline = time.time() + 5
            while evaluate(browser, 'document.visibilityState') != 'hidden' and time.time() < deadline:
                time.sleep(.05)
            assert evaluate(browser, 'document.visibilityState') == 'hidden'
            assert evaluate(state['host'], "action('screenshot').then(x=>!!x)")
            assert evaluate(state['host'], "action('screenshot',{fullPage:true}).then(x=>!!x)")
            assert evaluate(state['host'], 'scrollY') == 0
            assert evaluate(browser, 'document.visibilityState') == 'hidden'
            captures = []
            for target, full_page in [('view', False), ('view', True), ('host', False), ('host', True)]:
                evaluate(state['host'], "document.querySelector('#target').value=%s;true" % json.dumps(target))
                captures.append(evaluate(state['host'], "action('screenshot',{fullPage:%s})" % json.dumps(full_page)))
            for captured in [*captures, captures[0], captures[-1], captures[1]]:
                selector = '#files a[href="%s"]' % captured['url']
                point = evaluate(state['host'], """(()=>{const a=document.querySelector(%s);a.scrollIntoView({block:'center'});const r=a.getClientRects()[0];return {x:Math.round(r.x+Math.min(100,r.width/2)),y:Math.round(r.y+r.height/2),url:a.href}})()""" % json.dumps(selector))
                assert point['url'] == origin + captured['url'], point
                time.sleep(.15)
                for kind in (0, 1, 3):
                    native_call(C['Debug'], W().u8(5).u32(state['host']).u8(kind).i32(point['x']).i32(point['y']).b)
                deadline = time.time() + 5
                image = None
                while time.time() < deadline:
                    image = evaluate(state['host'], "(()=>{const i=document.querySelector('#captureImage');return {open:document.querySelector('#capturePreview').open,url:i.src,loaded:i.complete&&!i.hidden,width:i.naturalWidth,height:i.naturalHeight,download:document.querySelector('#captureFile').href}})()")
                    if image['open'] and image['loaded']:
                        break
                    time.sleep(.05)
                assert image['open'] and image['loaded'] and image['width'] > 0, image
                assert image['url'] == origin + captured['url'] == image['download'], image
                width, height = struct.unpack('>II', Path(captured['path']).read_bytes()[16:24])
                assert (image['width'], image['height']) == (width, height), image
                evaluate(state['host'], "document.querySelector('#closeCapture').click();true")
            evaluate(state['host'], "document.querySelector('#view').scrollIntoView({block:'center'});true")
            deadline = time.time() + 5
            while evaluate(browser, 'document.visibilityState') != 'visible' and time.time() < deadline:
                time.sleep(.05)
            assert evaluate(browser, 'document.visibilityState') == 'visible', evaluate(state['host'], "(()=>{const v=document.querySelector('#view'),r=v.getBoundingClientRect();return {hostVisibility:document.visibilityState,rect:r.toJSON(),size:[innerWidth,innerHeight],visible:v.checkVisibility({visibilityProperty:true}),dialogs:Array.from(document.querySelectorAll('dialog[open]'),d=>d.id),hits:document.elementsFromPoint(r.x+r.width/2,Math.max(1,r.y)+20).map(e=>e.tagName+'#'+e.id)}})()")
            frames = evaluate(browser, "document.querySelectorAll('iframe').length")
            point = evaluate(browser, "(()=>{const b=document.querySelector('#frame');b.scrollIntoView({block:'center'});const r=b.getBoundingClientRect();return {x:Math.round(r.x+30),y:Math.round(r.y+r.height/2)}})()")
            for kind in (0, 1, 3):
                native_call(C['Debug'], W().u8(5).u32(browser).u8(kind).i32(point['x']).i32(point['y']).b)
            deadline = time.time() + 5
            while evaluate(browser, "document.querySelectorAll('iframe').length") == frames and time.time() < deadline:
                time.sleep(.05)
            assert evaluate(browser, "document.querySelectorAll('iframe').length") == frames + 1, 'view input did not recover after capture previews'
            print('SMOKE PASS', flush=True)
        else:
            while not ctl.closed.wait(.25):
                pass
    except KeyboardInterrupt:
        pass
    finally:
        with lock:
            for entry in waiting.values():
                entry['event'].set()
        ctl.close()
        server.shutdown()
        server.server_close()
        if tls:
            tls.shutdown()
            tls.server_close()


if __name__ == '__main__':
    main()
