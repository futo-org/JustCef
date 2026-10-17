'use strict';
const $ = selector => document.querySelector(selector);
const view = $('#view');
let config, current, lastSequence = 0, clearedThrough = 0, suspended = false;
const queue = [], seenApplication = new Set();
const log = (kind, detail) => {
  $('#log').textContent += `${new Date().toLocaleTimeString()} ${kind} ${typeof detail === 'string' ? detail : JSON.stringify(detail)}\n`;
  $('#log').scrollTop = $('#log').scrollHeight;
};
async function action(name, options = {}) {
  try {
    const browser = $('#target').value === 'host' ? config.host : view.viewId;
    const response = await fetch('/api/action', {method:'POST', headers:{'Content-Type':'application/json', 'X-Demo-Token':config.token}, body:JSON.stringify({action:name, browser, ...options})});
    const data = await response.json();
    if (!response.ok) throw new Error(data.error);
    log(name, data.result);
    if (data.result?.url) {
      const link = document.createElement('a');
      link.href = data.result.url; link.target = '_blank'; link.rel = 'noopener';
      if (name === 'screenshot') link.addEventListener('click', event => {
        if (event.button !== 0 || event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
        event.preventDefault();
        const image = $('#captureImage');
        image.hidden = true;
        $('#captureTitle').textContent = link.textContent;
        image.alt = link.textContent;
        image.src = link.href;
        $('#captureFile').href = link.href;
        $('#capturePreview').showModal();
      });
      link.textContent = `${data.result.target === 'host' ? 'Host page' : 'Embedded view'} · ${data.result.scope} · ${data.result.path.split('/').pop()} (${data.result.bytes} bytes)`;
      const row = document.createElement('p'); row.append(link);
      const files = $('#files'); files.append(row); files.scrollTop = files.scrollHeight;
    }
    $('#notice').textContent = `${name}: completed`;
    return data.result;
  } catch (error) {
    log('ERROR', error.message); $('#notice').textContent = error.message;
  }
}
function navigate(url) { $('#url').value = url; view.setAttribute('src', url); }
async function policy() {
  const selected = $('#policy').value;
  await action('policy', {browser:view.viewId, allow:selected !== 'block', intercept:selected === 'intercept'});
}
function showNext() {
  if (current || !queue.length || $('#overlayDialog').open || $('#capturePreview').open) return;
  current = queue.shift();
  $('#requestTitle').textContent = `${current.application ? 'Application' : 'Host element'} decision: ${current.kind} — browser ${current.browser}`;
  $('#detail').textContent = JSON.stringify(current.detail, null, 2);
  $('#pendingFrame').disabled = $('#pendingNavigate').disabled = !!current.application;
  $('#remember').checked = false;
  $('#remember').disabled = !current.detail.canRemember;
  $('#decision').showModal();
  current.expiry = setTimeout(() => answer(false), 55000);
}
function enqueue(request) { queue.push(request); showNext(); }
async function answer(allow) {
  if (!current) return;
  const request = current; current = null;
  clearTimeout(request.expiry);
  const answer = {allow, remember:$('#remember').checked && !$('#remember').disabled, path:config.downloadPath};
  $('#decision').close();
  if (request.application) await action('answer', {id:request.id, answer});
  else view.respond(request.detail.requestId, answer);
  log('decision', {kind:request.kind, browser:request.browser, ...answer});
  showNext();
}
for (const kind of ['permission', 'certificate', 'download', 'newwindow']) {
  view.addEventListener(kind + 'request', event => {
    event.preventDefault();
    enqueue({kind, detail:event.detail, browser:view.viewId});
  });
}
view.addEventListener('viewcreated', () => { if(config) policy(); });
for (const name of ['load','loaderror','crash','error','close']) view.addEventListener(name, event => log(name, event.detail));
$('#navigation').onsubmit = event => { event.preventDefault(); navigate($('#url').value); };
$('#local').onclick = () => navigate(config.origin + '/content');
$('#custom').onclick = () => navigate(config.custom);
$('#certificate').onclick = () => navigate(config.certificate);
$('#mode').onchange = () => action('mode', {mode:$('#mode').value});
$('#policy').onchange = policy;
for (const button of document.querySelectorAll('[data-action]')) button.onclick = () => action(button.dataset.action, {fullPage:button.dataset.full === 'true', format:$('#format').value, quality:Number($('#quality').value)});
for (const button of document.querySelectorAll('[data-control]')) button.onclick = () => action('downloadControl', {id:Number($('#downloadId').value), control:button.dataset.control});
$('#overlay').onclick = () => { view.scrollIntoView({block:'center'}); $('#overlayDialog').showModal(); };
$('#closeOverlay').onclick = () => { $('#overlayDialog').close(); showNext(); };
$('#overlayDialog').addEventListener('close', showNext);
$('#closeCapture').onclick = () => $('#capturePreview').close();
$('#captureImage').onload = () => { $('#captureImage').hidden = false; };
$('#captureImage').onerror = () => { $('#captureTitle').textContent = 'Could not load ' + $('#captureImage').src; };
$('#capturePreview').addEventListener('close', () => { $('#captureImage').removeAttribute('src'); showNext(); });
function suspend() { suspended = !suspended; suspended ? view.suspend() : view.resume(); $('#suspend').textContent = suspended ? 'Resume view' : 'Suspend view'; }
function hide() { view.style.visibility = view.style.visibility === 'hidden' ? '' : 'hidden'; $('#hide').textContent = view.style.visibility ? 'Show view' : 'Hide view'; }
$('#suspend').onclick = $('#suspendCovered').onclick = suspend;
$('#hide').onclick = $('#hideCovered').onclick = hide;
$('#resizeCovered').onclick = () => { const h = view.getBoundingClientRect().height > 400 ? 300 : 520; view.style.height = h + 'px'; $('#height').value = h; };
$('#height').oninput = event => { view.style.height = event.target.value + 'px'; };
$('#allow').onclick = () => answer(true); $('#deny').onclick = () => answer(false);
$('#decision').oncancel = event => { event.preventDefault(); answer(false); };
$('#clear').onclick = () => { clearedThrough = lastSequence; $('#log').textContent = ''; };
async function poll() {
  try {
    const next = await (await fetch('/api/state')).json();
    if (!config) {
      config = next;
      $('#output').textContent = config.output;
      $('#certificate').disabled = !config.certificate;
      $('#printStatus').textContent = 'Print opens the print UI. Close it to return to the page.';
      $('#notice').textContent = 'Ready — requests are answered by this host UI. Scroll down to the embedded view.';
      navigate(config.origin + '/content');
      if (view.viewId) policy();
    } else config = next;
    for (const event of next.events) if (event.seq > lastSequence) {
      if (event.seq > clearedThrough) log(event.kind, event);
      lastSequence = event.seq;
    }
    for (const request of next.pending) if (!seenApplication.has(request.id)) {
      seenApplication.add(request.id); enqueue({...request, application:true});
    }
    $('#downloads').textContent = Object.keys(next.downloads).length ? JSON.stringify(next.downloads, null, 2) : 'No downloads yet.';
    if (!$('#downloadId').value && Object.keys(next.downloads).length) $('#downloadId').value = Object.keys(next.downloads).at(-1);
  } catch (error) { $('#notice').textContent = `Launcher unavailable: ${error.message}`; }
  setTimeout(poll, 400);
}
poll();

$('#pendingFrame').onclick = () => action('loadFrame', {browser:current.browser});
$('#pendingNavigate').onclick = async () => {
  const request = current; current = null;
  clearTimeout(request.expiry); $('#decision').close();
  await action('navigateAway', {browser:request.browser});
  log('cancellation test', 'Navigated without answering the old request; try a new request after the page loads.');
  showNext();
};
