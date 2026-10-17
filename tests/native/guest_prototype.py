"""Run Chromium surface embedding; --smoke exercises host-routed input."""
import argparse
import base64
import functools
import json
import os
from pathlib import Path
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import jcipc
from jcipc import Ctl, C, R, W, Rd, OK

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=ROOT/'build/cef-guest-prototype/Release/justcefnative.app/Contents/MacOS/justcefnative')
    parser.add_argument('--port', type=int, default=8768)
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--youtube-smoke',action='store_true',help='also check real YouTube navigation and video playback')
    args = parser.parse_args()
    if args.youtube_smoke: args.smoke=True
    if not args.native.is_file():
        parser.error('build the app first, or supply --native')
    jcipc.NATIVE = str(args.native.resolve())
    output = ROOT/'build/guest-prototype-demo'
    output.mkdir(parents=True, exist_ok=True)
    views = []
    ctl = None
    host = None

    def devtools(browser, method, params):
        status, payload = ctl.result(ctl.call(C['ExecuteDevTools'], W().i32(browser).s(method).b1(True).by(json.dumps(params).encode()).b), timeout=20)
        assert status == OK, (status, payload)
        reader = Rd(payload)
        success, data = reader.b1(), reader.by()
        assert success, data
        result = json.loads(data)
        assert 'exceptionDetails' not in result, result
        return result

    def evaluate(browser, expression):
        result = devtools(browser, 'Runtime.evaluate', dict(expression=expression, returnByValue=True, awaitPromise=True,userGesture=True))
        return result.get('result', {}).get('value')

    def capture():
        image = devtools(host, 'Page.captureScreenshot', dict(format='png', captureBeyondViewport=False))
        (output/'capture.png').write_bytes(base64.b64decode(image['data']))
        return image['data']

    class Handler(SimpleHTTPRequestHandler):
        def do_GET(self):
            if self.path == '/': self.path = '/guest-prototype.html'
            if self.path == '/video-available':
                self.send_response(200); self.send_header('Content-Type','application/json'); self.end_headers(); self.wfile.write(json.dumps(dict(available=(output/'fixture.webm').is_file())).encode()); return
            if self.path == '/fixture.webm':
                self.path='/fixture.webm'
                data=(output/'fixture.webm').read_bytes()
                self.send_response(200); self.send_header('Content-Type','video/webm'); self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data); return
            if self.path.startswith('/capture.png'):
                data = (output/'capture.png').read_bytes()
                self.send_response(200); self.send_header('Content-Type', 'image/png'); self.end_headers(); self.wfile.write(data)
            else: super().do_GET()
        def end_headers(self):
            if self.path.startswith('/guest-prototype-content.html'):
                self.send_header('X-Frame-Options','DENY')
                self.send_header('Content-Security-Policy',"frame-ancestors 'none'")
            super().end_headers()
        def do_POST(self):
            if self.path != '/capture': self.send_error(404); return
            try:
                capture()
                self.send_response(200); self.end_headers(); self.wfile.write(b'ok')
            except Exception as exc:
                self.send_error(500, str(exc))
        def log_message(self, *unused): pass

    server = ThreadingHTTPServer(('127.0.0.1', args.port), functools.partial(Handler, directory=str(ROOT/'tests/pages')))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    ctl = Ctl(['--use-alloy-style', '--cache-path='+str(output/'profile')])
    def on_view(reader):
        parent, browser, url = reader.i32(), reader.i32(), reader.s()
        views.append((browser, url))
        print('view', browser, url, flush=True)
        return OK, W().b1(True).b
    ctl.handlers[R['ViewCreated']] = on_view
    def wait(predicate, message):
        deadline = time.time()+8
        while time.time() < deadline:
            try:
                if predicate(): return
            except AssertionError as exc:
                if not any(text in str(exc) for text in ('Document navigated','Execution context was destroyed','Inspected target navigated or closed')):raise
            time.sleep(.1)
        if host:
            def plugins(node):
                if node.get("nodeName") in ("EMBED","IFRAME"):print("PLUGIN NODE",node.get("nodeName"),node.get("attributes"),flush=True)
                for child in node.get("children",[])+node.get("shadowRoots",[]):plugins(child)
            plugins(devtools(host,"DOM.getDocument",dict(depth=-1,pierce=True))["root"])
            print("HOST DIAGNOSTIC",evaluate(host,"({view:[document.querySelector('#view')?.outerHTML,document.querySelector('#view')?.viewId,Object.getPrototypeOf(document.querySelector('#view')).constructor.name,typeof document.querySelector('#view')?.reload],errors:window.protoErrors,url:location.href,body:document.body?.innerText,status:document.querySelector('#status')?.textContent,viewType:typeof customElements.get('justcef-view')})"),flush=True)
        raise AssertionError(message)
    def mouse(x, y, kind, **extra):
        return devtools(host, 'Input.dispatchMouseEvent', dict(type=kind, x=x, y=y, **extra))
    def click(x, y):
        mouse(x,y,'mouseMoved')
        mouse(x,y,'mousePressed',button='left',clickCount=1)
        mouse(x,y,'mouseReleased',button='left',clickCount=1)
    try:
        host = ctl.create(f'http://127.0.0.1:{args.port}/', views=True, bridge=False)
        wait(lambda: len(views)==1, 'embedded browser did not start')
        child = next(browser for browser,url in views if '?' not in url)
        wait(lambda: evaluate(host, 'attached'), 'guest not attached')
        print(f'Surface demo ready: http://127.0.0.1:{args.port}/ — native log {ctl.log_path}', flush=True)
        if args.smoke:
            child=views[0][0]
            wait(lambda:evaluate(host,'attached'),'guest not attached')
            wait(lambda:evaluate(child,"document.querySelector('#text') !== null"),'framing-denied document did not load')
            identity=evaluate(child,"({topIsSelf:top===self,parentIsSelf:parent===self,frameElement:!!frameElement,referrer:document.referrer,ancestors:Array.from(location.ancestorOrigins),opener:!!opener,hasViewApi:!!customElements.get('justcef-view')})")
            print('GUEST IDENTITY',json.dumps(identity),flush=True)
            assert identity['topIsSelf'] and identity['parentIsSelf'] and not identity['frameElement'] and not identity['opener'] and not identity['hasViewApi'] and not identity['ancestors'] and not identity['referrer'],identity
            def point(selector):
                local=evaluate(child,f'(()=>{{const r=document.querySelector({json.dumps(selector)}).getBoundingClientRect();return {{x:r.x+12,y:r.y+r.height/2}}}})()')
                rect=evaluate(host,"document.querySelector('#view').getBoundingClientRect().toJSON()")
                return rect['x']+local['x'],rect['y']+local['y']
            click(*point('#click'))
            wait(lambda:evaluate(child,'clicks')==1,'native guest click failed')
            click(*point('#text'))
            devtools(host,'Input.insertText',dict(text='Native guest input'))
            wait(lambda:evaluate(child,"document.querySelector('#text').value")=='Native guest input','native guest typing failed')
            devtools(host,'Input.imeSetComposition',dict(text='日本語',selectionStart=3,selectionEnd=3))
            devtools(host,'Input.insertText',dict(text='日本語'))
            wait(lambda:evaluate(child,"document.querySelector('#text').value").endswith('日本語'),'IME commit did not reach guest')
            address=evaluate(host,"(()=>{const r=document.querySelector('#address').getBoundingClientRect();return {x:r.x+20,y:r.y+r.height/2}})()")
            click(address['x'],address['y'])
            before=evaluate(child,"document.querySelector('#text').value")
            devtools(host,'Input.insertText',dict(text='host-focus'))
            assert evaluate(child,"document.querySelector('#text').value")==before,'host typing leaked to guest'
            assert 'host-focus' in evaluate(host,"document.querySelector('#address').value"),'host focus was not restored'
            status,_=ctl.result(ctl.call(25,W().i32(child).b))
            assert status==OK,'controller focus command failed'
            devtools(host,'Input.insertText',dict(text='-focused'))
            wait(lambda:evaluate(child,"document.querySelector('#text').value").endswith('-focused'),'controller focus did not return to guest')
            x,y=point('#scroll')
            mouse(x,y,'mouseWheel',deltaX=0,deltaY=100)
            wait(lambda:evaluate(child,"document.querySelector('#scroll').scrollTop")>30,'inner scroll did not move')
            assert evaluate(child,'scrollY')==0,'inner scroll leaked to document'
            evaluate(child,"document.querySelector('#scroll').scrollTop=10000;true")
            time.sleep(.4)
            mouse(x,y,'mouseWheel',deltaX=0,deltaY=180)
            wait(lambda:evaluate(child,'scrollY')>40,'scroll did not chain into guest document')
            y0=evaluate(child,'scrollY')
            time.sleep(.4)
            assert abs(evaluate(child,'scrollY')-y0)<5,'scroll feedback continued without input'
            evaluate(child,'scrollTo(0,0);true')
            evaluate(host,"navigate('guest-prototype-content.html?navigation=2');true")
            wait(lambda:evaluate(child,"location.search")=='?navigation=2' and evaluate(child,"!!document.querySelector('#text')"),'post-attachment navigation blocked')
            before=evaluate(child,"document.querySelector('#clock').textContent")
            evaluate(host,"document.querySelector('#suggestions').hidden=false;true")
            wait(lambda:evaluate(child,"document.querySelector('#clock').textContent")!=before,'overlay froze guest')
            overlay=evaluate(host,"(()=>{const r=document.querySelector('#overlayButton').getBoundingClientRect();return {x:r.x+15,y:r.y+r.height/2}})()")
            click(overlay['x'],overlay['y'])
            wait(lambda:evaluate(host,'overlayClicks')==1,'overlay pointer did not reach host')
            assert evaluate(child,'clicks')==0,'overlay pointer reached guest'
            capture()
            evaluate(host,"document.querySelector('#suggestions').hidden=true;true")
            evaluate(host,"(()=>{window.nestedReady=false;const v=document.createElement('justcef-view');v.id='nested';v.setAttribute('src','guest-prototype-content.html?nested');v.setAttribute('style','position:absolute;left:530px;top:230px;width:320px;height:200px;overflow:hidden;border-radius:20px');v.addEventListener('guestattached',()=>window.nestedReady=true);document.querySelector('#view').append(v);return true})()")
            wait(lambda:len(views)==2 and evaluate(host,'nestedReady'),'nested guest did not attach')
            nested=views[-1][0]
            evaluate(nested,"document.body.innerHTML='<button id=click>Nested click</button>';document.body.style.background='#7744bb';window.clicks=0;document.querySelector('#click').onclick=()=>clicks++;true")
            evaluate(host,"(()=>{window.deepReady=false;const v=document.createElement('justcef-view');v.id='deep';v.setAttribute('src','guest-prototype-content.html?deep');v.setAttribute('style','position:absolute;left:150px;top:60px;width:140px;height:110px');v.addEventListener('guestattached',()=>window.deepReady=true);document.querySelector('#nested').append(v);return true})()")
            wait(lambda:len(views)==3 and evaluate(host,'deepReady'),'third guest level did not attach')
            rect=evaluate(host,"document.querySelector('#nested').getBoundingClientRect().toJSON()")
            button=evaluate(nested,"(()=>{const r=document.querySelector('#click').getBoundingClientRect();return {x:r.x+15,y:r.y+r.height/2}})()")
            click(rect['x']+button['x'],rect['y']+button['y'])
            wait(lambda:evaluate(nested,'clicks')==1,'nested guest click failed')
            evaluate(host,"document.querySelector('#view').style.transform='scale(.94) rotate(2deg)';true")
            time.sleep(.2)
            button=evaluate(child,"(()=>{const r=document.querySelector('#click').getBoundingClientRect();return {x:r.x+15,y:r.y+r.height/2}})()")
            rotated=evaluate(host,"""(()=>{const e=document.querySelector('#view'),r=e.getBoundingClientRect(),m=new DOMMatrix(getComputedStyle(e).transform),points=[[0,0],[e.clientWidth,0],[0,e.clientHeight],[e.clientWidth,e.clientHeight]].map(([x,y])=>({x:m.a*x+m.c*y,y:m.b*x+m.d*y}));return {x:r.x-Math.min(...points.map(p=>p.x))+m.a*%s+m.c*%s,y:r.y-Math.min(...points.map(p=>p.y))+m.b*%s+m.d*%s}})()"""%(button['x'],button['y'],button['x'],button['y']))
            click(rotated['x'],rotated['y'])
            wait(lambda:evaluate(child,'clicks')==1,'rotated guest click failed')
            evaluate(host,"document.querySelector('#view').style.transform='';document.querySelector('#nested').remove();true")
            time.sleep(.2)
            evaluate(host,'view.suspend();true')
            wait(lambda:evaluate(child,"document.visibilityState")=='hidden','suspension did not hide the surface')
            evaluate(host,'view.resume();true')
            wait(lambda:evaluate(child,"document.visibilityState")=='visible','resume did not restore the surface')
            video_file=output/'fixture.webm'
            if not video_file.is_file():
                encoded=evaluate(host,"""(async()=>{const c=document.createElement('canvas');c.width=1280;c.height=720;const x=c.getContext('2d'),stream=c.captureStream(60),rec=new MediaRecorder(stream,{mimeType:'video/webm;codecs=vp8',videoBitsPerSecond:4000000}),chunks=[];let active=true,n=0;function draw(){if(!active)return;x.fillStyle=n++%2?'#2040b0':'#40a080';x.fillRect(0,0,1280,720);x.fillStyle='white';x.font='70px sans-serif';x.fillText('Frame '+n,100,150);requestAnimationFrame(draw)}draw();const done=new Promise(resolve=>rec.onstop=resolve);rec.ondataavailable=e=>chunks.push(e.data);rec.start();await new Promise(r=>setTimeout(r,5200));rec.stop();await done;active=false;stream.getTracks().forEach(t=>t.stop());const bytes=new Uint8Array(await new Blob(chunks).arrayBuffer());let text='';for(let i=0;i<bytes.length;i+=8192)text+=String.fromCharCode(...bytes.subarray(i,i+8192));return btoa(text)})()""")
                video_file.write_bytes(base64.b64decode(encoded))
            evaluate(child,"(()=>{const v=document.querySelector('#video');v.src='/fixture.webm';v.muted=true;v.scrollIntoView({block:'center'});return v.play()})()")
            wait(lambda:evaluate(child,"document.querySelector('#video').currentTime")>.3,'local video did not play')
            video_stats=evaluate(child,"""(async()=>{const v=document.querySelector('#video'),initial=v.getVideoPlaybackQuality();let frames=0,active=true;function frame(){frames++;if(active)v.requestVideoFrameCallback(frame)}v.requestVideoFrameCallback(frame);const start=performance.now();await new Promise(r=>setTimeout(r,2200));active=false;const q=v.getVideoPlaybackQuality();return {presented:frames,seconds:(performance.now()-start)/1000,total:q.totalVideoFrames-initial.totalVideoFrames,dropped:q.droppedVideoFrames-initial.droppedVideoFrames,width:v.videoWidth,height:v.videoHeight}})()""")
            print('VIDEO',json.dumps(video_stats),flush=True)
            assert video_stats['presented']>45,'video presentation stalled'
            evaluate(host,"document.querySelector('#dialog').showModal();true")
            modal_stats=evaluate(child,"""(async()=>{const v=document.querySelector('#video'),q=v.getVideoPlaybackQuality();await new Promise(r=>setTimeout(r,700));return {frames:v.getVideoPlaybackQuality().totalVideoFrames-q.totalVideoFrames,paused:v.paused}})()""")
            assert modal_stats['frames']>10 and not modal_stats['paused'],modal_stats
            evaluate(host,"document.querySelector('#dialog').close();true")
            evaluate(child,"document.querySelector('#video').pause();const fs=document.createElement('button');fs.id='fullscreenTest';fs.textContent='Fullscreen';fs.style.cssText='position:fixed;top:20px;left:20px;z-index:9999';fs.onclick=()=>document.documentElement.requestFullscreen();document.body.append(fs);true")
            click(*point('#fullscreenTest'))
            wait(lambda:evaluate(host,"!!document.fullscreenElement") and evaluate(child,"!!document.fullscreenElement"),"fullscreen did not include host compositor surface")
            time.sleep(.5)
            print('FULLSCREEN',evaluate(host,"({full:!!document.fullscreenElement,width:innerWidth,view:document.querySelector('#view').getBoundingClientRect().toJSON()})"),evaluate(child,'({full:!!document.fullscreenElement,width:innerWidth,height:innerHeight})'),flush=True)
            evaluate(child,"if(document.fullscreenElement)document.exitFullscreen();document.querySelector('#fullscreenTest').remove();true")
            wait(lambda:not evaluate(host,"!!document.fullscreenElement") and not evaluate(child,"!!document.fullscreenElement"),"fullscreen did not exit")
            time.sleep(.5)
            if args.youtube_smoke:
                evaluate(host,"navigate('https://www.youtube.com/watch?v=jNQXAC9IVRw');true")
                end=time.time()+45
                details={}
                while time.time()<end:
                    try:
                        details=evaluate(child,"({url:location.href,title:document.title,text:document.body?.innerText?.slice(0,1500),video:!!document.querySelector('video'),topIsSelf:top===self})")
                        if details.get('video') or 'consent.youtube' in details.get('url',''):break
                    except AssertionError:pass
                    time.sleep(.3)
                print('YOUTUBE PAGE',json.dumps({k:v for k,v in details.items() if k!='text'}),flush=True)
                assert details.get('video'),'YouTube video element unavailable; inspect page result'
                wait(lambda:evaluate(child,"!!document.querySelector('.ytp-play-button') && !!document.querySelector('#movie_player')?.getPlayerState"),'YouTube controls did not initialize')
                time.sleep(1)
                consent=evaluate(child,"(()=>{const b=Array.from(document.querySelectorAll('button')).find(b=>b.innerText.trim()==='Reject all');if(!b)return null;b.scrollIntoView({block:'center'});return true})()")
                if consent:
                    time.sleep(.2)
                    local=evaluate(child,"(()=>{const b=Array.from(document.querySelectorAll('button')).find(b=>b.innerText.trim()==='Reject all'),r=b.getBoundingClientRect();return {x:r.x+r.width/2,y:r.y+r.height/2}})()")
                    rect=evaluate(host,"document.querySelector('#view').getBoundingClientRect().toJSON()")
                    click(rect['x']+local['x'],rect['y']+local['y'])
                    wait(lambda:not evaluate(child,"Array.from(document.querySelectorAll('button')).some(b=>b.innerText.trim()==='Reject all'&&b.getClientRects().length>0)"),'YouTube consent dialog did not dismiss')
                    print('YOUTUBE optional cookies rejected',flush=True)
                evaluate(child,"document.querySelector('video').muted=true;true")
                evaluate(child,"document.querySelector('.ytp-play-button').scrollIntoView({block:'center'});true")
                time.sleep(.3)
                if evaluate(child,"document.querySelector('#movie_player').getPlayerState()")!=1:
                    click(*point('.ytp-play-button'))
                end=time.time()+25
                playback={}
                while time.time()<end:
                    try:
                        playback=evaluate(child,"(()=>{const v=document.querySelector('video');if(!v)return {time:0,url:location.href};return {time:v.currentTime,ready:v.readyState,paused:v.paused,error:v.error?.message,notice:document.querySelector('.ytp-error')?.innerText,url:location.href,text:document.body?.innerText?.slice(0,700)}})()")
                    except AssertionError as exc:
                        if not any(text in str(exc) for text in ('Document navigated','Execution context was destroyed','Inspected target navigated or closed')):raise
                    if playback.get('time',0)>.5:break
                    time.sleep(.3)
                print('YOUTUBE PLAYBACK',json.dumps({k:v for k,v in playback.items() if k!='text'}),flush=True)
                assert playback['time']>.5,'YouTube playback did not advance'
                evaluate(host,"document.querySelector('#suggestions').hidden=false;true")
                first=playback['time']
                try:
                    wait(lambda:evaluate(child,"document.querySelector('video')?.currentTime")>first+.7,'YouTube stopped beneath HTML overlay')
                finally:
                    print('YOUTUBE OVERLAY STATE',evaluate(child,"(()=>{const v=document.querySelector('video');return {time:v.currentTime,paused:v.paused,ready:v.readyState,visibility:document.visibilityState,network:v.networkState,error:v.error?.message,notice:document.querySelector('.ytp-error')?.innerText,buffered:Array.from({length:v.buffered.length},(_,i)=>[v.buffered.start(i),v.buffered.end(i)]),state:document.querySelector('#movie_player')?.getPlayerState?.()}})()"),flush=True)
                screenshot=capture()
                (output/'youtube-overlay.png').write_bytes(base64.b64decode(screenshot))
                print('YOUTUBE OVERLAY PASS',flush=True)
            evaluate(host,"navigate('guest-prototype-content.html?recovery');window.guestCrashed=false;view.addEventListener('crash',()=>guestCrashed=true,{once:true});true")
            wait(lambda:evaluate(child,"!!document.querySelector('#click')"),'recovery fixture did not load')
            ctl.call(C['ExecuteDevTools'],W().i32(child).s('Page.crash').b1(True).by(b'{}').b)
            wait(lambda:evaluate(host,'guestCrashed'),'guest crash did not reach host')
            evaluate(host,'view.reload();true')
            wait(lambda:evaluate(child,"!!document.querySelector('#click')"),'guest did not reload after crash')
            click(*point('#click'))
            wait(lambda:evaluate(child,'clicks')==1,'input did not recover after renderer crash')
            print('CRASH RECOVERY PASS',flush=True)
            print('SMOKE PASS: restrictive framing headers, top-level identity, native click and typing, live HTML overlay, raw host capture, nested scroll chaining, navigation, 720p video, rotated native hit testing, three DOM view levels',flush=True)
        else:
            while not ctl.closed.wait(.25): pass
    except KeyboardInterrupt: pass
    finally:
        ctl.close()
        print("NATIVE EXIT",ctl.proc.returncode,flush=True)
        server.shutdown(); server.server_close()

if __name__ == '__main__': main()
