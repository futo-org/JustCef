#!/usr/bin/env python3
import json
import queue
import threading
from urllib.parse import unquote, urlparse
import os
from pathlib import Path
import subprocess
import tempfile
import time
import traceback

import run as suite
from jcipc import *

XDOTOOL = os.environ.get('JUSTCEF_XDOTOOL', 'xdotool')


def js_ready(ctl, browser, expression):
    try:
        return suite.devtools_eval(ctl,browser,expression)
    except AssertionError as error:
        if str(error) == 'Runtime.evaluate failed':
            return False  # A frame navigation may invalidate an in-flight CDP evaluation.
        raise


def xdo(*args):
    return subprocess.check_output([XDOTOOL, *map(str, args)], text=True, timeout=20).strip()


def request(ctl, opcode, body):
    status, payload = ctl.result(ctl.call(opcode, body))
    suite.check(status == OK, 'native request %s: %r' % (opcode, (status,payload)))
    return payload


def place(ctl, wid, x, y, title):
    request(ctl, 44, W().i32(wid).s(title).b)
    request(ctl, 15, W().i32(wid).i32(x).i32(y).b)
    window = xdo('search','--sync','--onlyvisible','--pid',ctl.proc.pid,'--name','^'+title+'$').splitlines()[-1]
    return window


def geometry(window):
    values = dict(line.split('=',1) for line in xdo('getwindowgeometry','--shell',window).splitlines())
    return {key:int(values[key]) for key in ('X','Y','WIDTH','HEIGHT')}


def drag(x, y, dx, dy):
    xdo('mousemove','--sync',x,y)
    xdo('mousedown',1)
    try:
        for step in range(1, 13):
            xdo('mousemove','--sync',round(x+dx*step/12),round(y+dy*step/12))
            time.sleep(.04)
    finally:
        xdo('mouseup',1)
    time.sleep(.2)


def test_cross_process_native_file_drop():
    with tempfile.TemporaryDirectory(prefix='justcef-os-drop-') as directory:
        files = [Path(directory)/name for name in ('spaced café 文件.txt','second.txt')]
        for i,path in enumerate(files): path.write_text('payload-'+str(i),encoding='utf-8')
        receiver_binary=Path(directory)/'drop-receiver'
        subprocess.run(['cc','-Wall','-Wextra','-Werror',str(Path(__file__).with_name('x11_drop_receiver.c')),'-lX11','-o',str(receiver_binary)],check=True)
        for style in ([],['--use-alloy-style']):
            for embedded in (False,True):
                source= Ctl(style+['--custom-schemes=app'])
                receiver=subprocess.Popen([str(receiver_binary)],stdout=subprocess.PIPE,text=True)
                messages=queue.Queue()
                threading.Thread(target=lambda:[messages.put(line.strip()) for line in receiver.stdout],daemon=True).start()
                try:
                    source.wait_note('Ready')
                    ready=messages.get(timeout=10).split()
                    suite.check(ready[0]=='READY','native receiver started')
                    target_window=ready[1]
                    source.handlers[R['Proxy']]=lambda r:proxy_response(200,{'Content-Type':'text/html'},b'''<!doctype html><style>body{margin:0}#drag{width:300px;height:180px;background:red}</style><div id=drag draggable=true>Drag files</div><script>drag.ondragstart=e=>e.dataTransfer.setData('text/plain','unarmed fallback')</script>''')
                    host=source.create('http://source.test/',proxy=True,frameless=True,views=embedded)
                    source.wait_note('LoadingState',lambda r:r.i32()==host and not r.b1())
                    source_window=place(source,host,20,30,'JustCef drag source')
                    xdo('windowmove','--sync',target_window,1000,30)
                    browser=host
                    if embedded:
                        source.handlers[R['ViewCreated']]=lambda r:(OK,W().b1(True).b)
                        suite.devtools_eval(source,host,"document.body.innerHTML='';window.view=document.createElement('justcef-view');view.style.cssText='display:block;width:400px;height:250px';window.attached=false;view.addEventListener('guestattached',()=>attached=true);view.setAttribute('src','app://guest/drag');document.body.append(view);true")
                        suite.wait_until(lambda:suite.devtools_eval(source,host,'view.viewId') is not None,'source guest created')
                        browser=suite.devtools_eval(source,host,'view.viewId')
                        source.wait_note('LoadingState',lambda r:r.i32()==browser and not r.b1())
                        suite.wait_until(lambda:suite.devtools_eval(source,host,'attached'),'source surface attached')
                        suite.devtools_eval(source,browser,"document.body.innerHTML='<div id=drag draggable=true style=\"width:300px;height:180px;background:red\">Drag files</div>';drag.ondragstart=e=>e.dataTransfer.setData('text/plain','unarmed fallback');true")
                    body=W().i32(browser).u32(len(files))
                    for path in files:body.s(str(path))
                    request(source,C['SetNextDragFiles'],body.b)
                    sg,tg=geometry(source_window),geometry(target_window)
                    sx,sy,tx,ty=sg['X']+80,sg['Y']+80,tg['X']+100,tg['Y']+100
                    iframe_html = '<div draggable=true style="width:280px;height:90px" ondragstart="event.dataTransfer.setData(\'text/plain\',\'subframe fallback\')">Subframe drag</div>'
                    suite.devtools_eval(source,browser,"document.querySelector('#drag').style.height='80px';window.subframe=document.createElement('iframe');subframe.style.cssText='position:absolute;left:0;top:110px;width:300px;height:100px;border:0';subframe.srcdoc=%s;document.body.append(subframe);true" % json.dumps(iframe_html))
                    suite.wait_until(lambda:js_ready(source,browser,"subframe.contentDocument?.readyState==='complete'"),'drag subframe loaded')
                    suite.devtools_eval(source,browser,'new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))')
                    drag(sg['X']+80,sg['Y']+150,tx-(sg['X']+80),ty-(sg['Y']+150))
                    guard=messages.get(timeout=15).split(' ',2)
                    suite.check(guard[1]!='text/uri-list' and bytes.fromhex(guard[2]).decode()=='subframe fallback','subframe drag retains normal payload')
                    suite.check(suite.window_state(source,browser)['drag_files']==len(files),'subframe cannot consume main-frame file authority')
                    sy=sg['Y']+40
                    drag(sx,sy,tx-sx,ty-sy)
                    print('drag consumed',embedded,style,suite.window_state(source,browser),flush=True)
                    message=messages.get(timeout=15).split(' ',2)
                    suite.check(message[:2]==['DROP','text/uri-list'],'OS file URI selection offered: '+str(message))
                    uris=bytes.fromhex(message[2]).decode('utf-8').splitlines()
                    paths=[unquote(urlparse(uri).path) for uri in uris if uri and not uri.startswith('#')]
                    suite.check(sorted(paths)==sorted(str(path) for path in files),'OS delivered exact Unicode file paths: '+str(paths))
                    suite.check([Path(path).read_text(encoding='utf-8') for path in paths]==['payload-'+str(files.index(Path(path))) for path in paths],'OS recipient reads source file contents')
                    suite.check(suite.window_state(source,browser)['drag_files']==0,'OS drop consumed one-shot override')
                    drag(sx,sy,tx-sx,ty-sy)
                    message=messages.get(timeout=15).split(' ',2)
                    suite.check(message[0]=='DROP' and message[1]!='text/uri-list','files do not leak into subsequent OS drag')
                    suite.check(bytes.fromhex(message[2]).decode()=='unarmed fallback','normal text drag preserved')
                    print('PASS OS file delivery', 'Alloy' if style else 'Chrome', 'embedded' if embedded else 'top-level',flush=True)
                    suite.shutdown(source)
                finally:
                    source.close();receiver.terminate();receiver.wait(timeout=10)


def test_frameless_move_exclusion_and_minimum_size():
    server=PageServer(suite.PAGES)
    try:
        for style in ([],['--use-alloy-style']):
            ctl=Ctl(style)
            try:
                ctl.wait_note('Ready')
                wid=ctl.create(server.origin+'/frameless.html',bridge=False,frameless=True)
                ctl.wait_note('LoadingState',lambda r:r.i32()==wid and not r.b1())
                suite.wait_drag_regions(ctl,wid,lambda regions:any(r[4] for r in regions))
                window=place(ctl,wid,40,40,'JustCef frameless movement')
                before=geometry(window)
                drag(before['X']+60,before['Y']+25,120,60)
                moved=geometry(window)
                suite.check(abs(moved['X']-before['X']-120)<=15 and abs(moved['Y']-before['Y']-60)<=10,'CSS title bar moves OS window: %r -> %r'%(before,moved))
                request(ctl,C['SetDraggableRegions'],W().i32(wid).b1(True).u32(0).b)
                drag(moved['X']+60,moved['Y']+25,100,50)
                suite.check(geometry(window)==moved,'empty host override disables OS window movement')
                request(ctl,C['SetDraggableRegions'],W().i32(wid).b1(False).b)
                request(ctl,C['SetDraggableRegions'],W().i32(wid).b1(True).u32(2).i32(0).i32(0).i32(900).i32(48).b1(True).i32(0).i32(0).i32(100).i32(48).b1(False).b)
                drag(moved['X']+60,moved['Y']+25,100,50)
                suite.check(geometry(window)==moved,'no-drag exclusion prevents OS movement')
                xdo('windowsize','--sync',window,100,100)
                size=geometry(window)
                suite.check(size['WIDTH']>=800 and size['HEIGHT']>=600,'native minimum window dimensions enforced: %r'%size)
                print('PASS OS frameless movement/exclusion/minimum size', 'Alloy' if style else 'Chrome',flush=True)
                suite.shutdown(ctl)
            finally:ctl.close()
    finally:server.close()


def check_link_context_menu(styles):
    for style in styles:
        ctl=Ctl(style+['--custom-schemes=app'])
        try:
            ctl.wait_note('Ready')
            ctl.handlers[R['Proxy']]=lambda r:proxy_response(200,{'Content-Type':'text/html'},b'<!doctype html><style>body{margin:0}a{display:block;width:300px;height:100px}</style><a href="app://popup/context">Open new window</a>')
            ctl.handlers[R['ViewCreated']]=lambda r:(OK,W().b1(True).b)
            host=ctl.create('app://host/index',frameless=True,views=True,bridge=False)
            ctl.wait_note('LoadingState',lambda r:r.i32()==host and not r.b1())
            window=place(ctl,host,40,40,'JustCef context menu')
            for embedded in (False,True):
                browser=host
                if embedded:
                    suite.devtools_eval(ctl,host,"document.body.innerHTML='';window.attached=false;window.view=document.createElement('justcef-view');view.style.cssText='display:block;width:400px;height:250px';view.addEventListener('guestattached',()=>attached=true);view.setAttribute('src','app://guest/index');document.body.append(view);true")
                    suite.wait_until(lambda:suite.devtools_eval(ctl,host,'view.viewId') is not None,'context guest created')
                    browser=suite.devtools_eval(ctl,host,'view.viewId')
                    ctl.wait_note('LoadingState',lambda r:r.i32()==browser and not r.b1())
                    suite.wait_until(lambda:suite.devtools_eval(ctl,host,'attached'),'context guest attached')
                before=suite.devtools_eval(ctl,browser,'location.href')
                suite.devtools_eval(ctl,browser,"window.contextHits=[];document.addEventListener('contextmenu',e=>contextHits.push(e.target.tagName));new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))")
                bounds=geometry(window)
                xdo('windowactivate','--sync',window)
                xdo('mousemove','--sync',bounds['X']+70,bounds['Y']+40)
                xdo('click',3)
                time.sleep(.3)
                print('context target',style,embedded,suite.devtools_eval(ctl,browser,'contextHits'),flush=True)
                xdo('key','--clearmodifiers','Home','Return')
                popup=ctl.wait_note('WindowOpened',lambda r:r.i32() not in (host,browser)).i32()
                ctl.wait_note('LoadingState',lambda r:r.i32()==popup and not r.b1())
                suite.check(suite.devtools_eval(ctl,popup,'location.href')=='app://popup/context','native link context menu opens intended URL')
                suite.check(suite.devtools_eval(ctl,browser,'location.href')==before,'context menu does not navigate source browser')
                suite.check(suite.devtools_eval(ctl,popup,'[typeof bridge,typeof customElements.get("justcef-view")]')==['undefined','undefined'],'context popup has no privileged APIs')
                request(ctl,C['Close'],W().i32(popup).b1(True).b)
                ctl.wait_note('WindowClosed',lambda r:r.i32()==popup)
                print('PASS native link context menu','Alloy' if style else 'Chrome','embedded' if embedded else 'top-level',flush=True)
            suite.shutdown(ctl)
        finally:ctl.close()


def test_chrome_link_context_menu_routes_to_new_window():
    check_link_context_menu([[]])

def test_alloy_link_context_menu_routes_to_new_window():
    check_link_context_menu([['--use-alloy-style']])


if __name__=='__main__':
    tests=[test_cross_process_native_file_drop,test_frameless_move_exclusion_and_minimum_size,test_chrome_link_context_menu_routes_to_new_window,test_alloy_link_context_menu_routes_to_new_window]
    import sys
    tests=[test for test in tests if len(sys.argv)<2 or test.__name__ in sys.argv[1:]]
    failures=0
    for test in tests:
        begin(test.__name__)
        try:
            test()
            print('PASS',test.__name__,flush=True)
        except Exception:
            failures+=1;traceback.print_exc()
            print('FAIL',test.__name__,flush=True)
        finally:close_all()
    raise SystemExit(bool(failures))
