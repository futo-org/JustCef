"""JUSTCEF_NATIVE=/absolute/path/justcefnative python3 scroll_demo.py [--interactive] [--live]."""
import json
import os
import sys
import time
import re
import statistics
import math
import subprocess
import tempfile
from jcipc import Ctl, PageServer, C, CN, R, W, Rd, OK


def devtools(ctl, browser, method, params):
    status, payload = ctl.result(ctl.call(C['ExecuteDevTools'], W().i32(browser).s(method).b1(True).by(json.dumps(params).encode()).b))
    assert status == OK
    reader = Rd(payload)
    assert reader.b1()
    result = json.loads(reader.by())
    assert 'exceptionDetails' not in result, result
    return result


def evaluate(ctl, browser, expression):
    result = devtools(ctl, browser, 'Runtime.evaluate', dict(expression=expression, awaitPromise=True, returnByValue=True))
    return result.get('result', {}).get('value')


def main():
    server = PageServer(os.path.join(os.path.dirname(__file__), '..', 'pages'))
    platform = 'wayland' if '--wayland' in sys.argv else 'x11'
    ctl = Ctl(extra=['--ozone-platform=' + platform, '--enable-logging=stderr', '--vmodule=justcef_view_host=1'])
    try:
        ctl.wait_note('Ready')
        ctl.handlers[R['ViewCreated']] = lambda r: (OK, W().b1(True).b)
        host = ctl.create(server.origin + '/scroll-demo.html', views=True, bridge=False)
        ctl.wait_note('LoadingState', lambda r: r.i32() == host and not r.b1())
        child = evaluate(ctl, host, """new Promise((resolve,reject) => {
          const end = Date.now() + 10000;
          const timer = setInterval(() => {
            if (chat.viewId) { clearInterval(timer); resolve(chat.viewId); }
            else if (Date.now() > end) {clearInterval(timer); reject('view missing');}
          }, 20);
        })""")
        ctl.wait_note('FrameLoadEnd', lambda r: r.i32() == child)
        if '--live' in sys.argv:
            evaluate(ctl, host, "chat.setAttribute('transition', 'live')")
        zoom = 1.25 if '--zoom' in sys.argv else 1
        if zoom != 1:
            status, _ = ctl.result(ctl.call(9, W().i32(host).f64(math.log(zoom) / math.log(1.2)).b))
            assert status == OK
        time.sleep(1)
        initial = evaluate(ctl, child, '[innerWidth, innerHeight]')
        evaluate(ctl, child, 'measurements.resizes = []; measurements.visibility = []')
        if '--wheel' in sys.argv:
            evaluate(ctl, host, """window.scrollSamples = []; window.sampling = true;
              requestAnimationFrame(function sample() {
                scrollSamples.push({time: performance.timeOrigin + performance.now(), y: chat.getBoundingClientRect().top});
                if (sampling) requestAnimationFrame(sample);
              });""")
            for distance in (-240, 240):
                devtools(ctl, host, 'Input.synthesizeScrollGesture', dict(x=650, y=250, yDistance=distance, speed=250, gestureSourceType='mouse'))
            samples = evaluate(ctl, host, 'sampling = false; scrollSamples')
            assert max(s['y'] for s in samples) - min(s['y'] for s in samples) > 100, 'wheel did not scroll the parent'
        else:
            samples = evaluate(ctl, host, 'sweep()')
        time.sleep(.5)
        result = evaluate(ctl, child, 'measurements')
        result['initialViewport'] = initial
        assert all(size == initial for size in result['resizes']), result
        if '--assert-live' in sys.argv:
            assert not result['visibility'], result
        if '--capture' in sys.argv:
            # Capture only the demo window, never the user's entire desktop.
            capture = os.path.join(tempfile.mkdtemp(prefix='justcef-scroll-'), 'demo.png')
            evaluate(ctl, host, 'scroller.scrollTop = 230')
            time.sleep(.3)
            subprocess.run(['import', '-window', 'JustCef scroll laboratory', capture], check=True, timeout=10)
            result['capture'] = capture
        result['hostFrames'] = len(samples)
        result['log'] = ctl.log_path
        # Compare the sampled DOM position with the browser-process apply log.
        # This measures geometry delivery, not compositor presentation latency.
        ages = []
        with open(ctl.log_path) as log:
            for line in log:
                match = re.search(r':(\d{4}/\d{6}\.\d+):.*View %d frame full = \([^,]+, (-?\d+),' % child, line)
                if not match:
                    continue
                stamp, y = match.groups()
                timestamp = time.mktime(time.strptime(str(time.localtime().tm_year) + stamp.split('.')[0], '%Y%m%d/%H%M%S')) * 1000 + float('0.' + stamp.split('.')[1]) * 1000
                candidates = [s for s in samples if abs(s['y'] * zoom - int(y)) < 1 and 0 <= timestamp - s['time'] < 200]
                if candidates:
                    ages.append(timestamp - candidates[-1]['time'])
        if ages:
            result['geometryDeliveryMs'] = {'samples': len(ages), 'median': round(statistics.median(ages), 2), 'p95': round(sorted(ages)[int((len(ages)-1)*.95)], 2)}
        print(json.dumps(result, indent=2), flush=True)
        if '--interactive' in sys.argv:
            input('Demo remains open. Enter to exit. ')
    finally:
        if not ctl.closed.is_set():
            ctl.notify(CN['Exit'])
            ctl.closed.wait(10)
        ctl.close()
        server.close()


if __name__ == '__main__':
    main()
