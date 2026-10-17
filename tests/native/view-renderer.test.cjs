const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../../native/src/justcef_view_renderer.cc'), 'utf8');
const bootstrap = source.match(/kViewBootstrapScript\[\] = R"JS\(([\s\S]*?)\)JS"/)[1];

function harness() {
  const messages = [], classes = new Map(), timers = new Map(), listeners = new Map();
  let timer = 0;
  class Element {
    constructor() { this.attributes = new Map(); this.isConnected = true; this.style = {}; this.events = []; }
    attachShadow() { return this.root = { children: [], append(el) { this.children.push(el); }, prepend(el) { this.children.unshift(el); } }; }
    getAttribute(name) { return this.attributes.get(name) ?? null; }
    setAttribute(name, value) { const old = this.getAttribute(name); this.attributes.set(name, value); if (classes.get('justcef-view')?.observedAttributes.includes(name)) this.attributeChangedCallback?.(name, old, value); }
    removeAttribute(name) { const old = this.getAttribute(name); this.attributes.delete(name); this.attributeChangedCallback?.(name, old, null); }
    addEventListener() {}
    dispatchEvent(event) { this.events.push(event); this.onDispatch?.(event); return !this.cancel; }
    remove() { this.removed = true; }
    focus() {}
  }
  const context = {
    HTMLElement: Element, CSSStyleSheet: class { replaceSync() {} }, URL,
    document: { addEventListener() {}, baseURI: 'https://app.test/', createElement: tag => { const el = new Element(); el.tag = tag; return el; } },
    getComputedStyle: () => ({ backgroundColor: 'rgb(1,2,3)' }),
    CustomEvent: class { constructor(type, options) { this.type = type; Object.assign(this, options); } },
    customElements: { get: name => classes.get(name), define: (name, cls) => classes.set(name, cls) },
    addEventListener: (name, fn) => listeners.set(name, fn),
    setTimeout: fn => { timers.set(++timer, fn); return timer; }, clearTimeout: id => timers.delete(id),
  };
  const native = Object.fromEntries(['create', 'navigate', 'command', 'destroy'].map(name => [name, (...args) => messages.push({ name, args })]));
  const dispatch = vm.runInNewContext(bootstrap, context)(native);
  const view = new (classes.get('justcef-view'))();
  view.attributes.set('src', '/view'); view.connectedCallback();
  const id = () => messages.filter(m => m.name === 'create').at(-1).args[0];
  const event = (name, detail, target = id()) => dispatch('event', target, name, JSON.stringify(detail));
  const attach = () => event('viewcreated', { viewId: 42, guestContentId: 'token' });
  return { view, messages, event, attach, id, listeners, timers, flush() { for (const fn of [...timers.values()]) fn(); timers.clear(); } };
}

test('creates an independent browser and attaches a compositor plugin', () => {
  const h = harness(); h.attach();
  assert.equal(h.view.viewId, 42);
  assert.equal(h.view.root.children[0].type, 'application/x-chromium-surface-embed');
  assert.equal(h.view.root.children[0].getAttribute('data-content-id'), 'token');
  assert.equal(h.view.root.children.some(el => ['canvas', 'img', 'textarea'].includes(el.tag)), false);
});
test('ordinary navigation retains the browser and plugin', () => {
  const h = harness(); h.attach(); const plugin = h.view.root.children[0];
  h.view.setAttribute('src', '/next');
  assert.equal(h.messages.at(-1).name, 'navigate'); assert.equal(h.messages.at(-1).args[1], 'https://app.test/next');
  assert.equal(h.view.root.children[0], plugin); assert.equal(h.view.viewId, 42);
});
test('DOM moves preserve browser lifetime', () => {
  const h = harness(); h.attach(); h.view.isConnected = false; h.view.disconnectedCallback();
  h.view.isConnected = true; h.view.connectedCallback(); h.flush();
  assert.equal(h.messages.filter(m => m.name === 'create').length, 1);
  assert.equal(h.messages.filter(m => m.name === 'destroy').length, 0);
});
test('removed views release browser and ignore messages for the old element ID', () => {
  const h = harness(); h.attach(); const old = h.id(); const plugin = h.view.root.children[0];
  h.view.isConnected = false; h.view.disconnectedCallback(); h.flush();
  assert.equal(plugin.removed, true); assert.equal(h.view.viewId, null);
  h.event('viewcreated', { viewId: 99, guestContentId: 'stale' }, old);
  assert.equal(h.view.viewId, null);
  h.view.isConnected = true; h.view.connectedCallback(); assert.notEqual(h.id(), old);
});
test('unhandled permission decisions deny; handled decisions are left to the app', () => {
  const h = harness(); h.attach(); h.event('permissionrequest', { requestId: 8 });
  assert.deepEqual(JSON.parse(h.messages.at(-1).args[2]), { requestId: 8, allow: false });
  const count = h.messages.length; h.view.cancel = true; h.event('permissionrequest', { requestId: 9 });
  assert.equal(h.messages.length, count);
});
test('suspension hides presentation without replacing the live browser', () => {
  const h = harness(); h.view.suspend(); h.attach(); const plugin = h.view.root.children[0];
  assert.equal(plugin.style.visibility, 'hidden'); h.view.resume(); assert.equal(plugin.style.visibility, '');
  assert.equal(h.messages.filter(m => m.name === 'create').length, 1);
});
test('controller rejection allows retry with a new source and ID', () => {
  const h = harness(), old = h.id(); h.event('error', { reason: 'create-failed' });
  h.view.setAttribute('src', '/retry'); assert.notEqual(h.id(), old); h.attach(); assert.equal(h.view.viewId, 42);
});
test('back-forward cache restores fresh browser IDs', () => {
  const h = harness(); h.attach(); const old = h.id();
  h.listeners.get('pagehide')({ persisted: true }); assert.equal(h.view.viewId, null);
  h.listeners.get('pageshow')({ persisted: true }); assert.notEqual(h.id(), old);
});
test('removing src releases its surface', () => {
  const h = harness(); h.attach(); const plugin = h.view.root.children[0]; h.view.removeAttribute('src');
  assert.equal(h.view.viewId, null); assert.equal(plugin.removed, true);
});

test('a decision handler recreating the view cannot send a denial to the replacement', () => {
  const h = harness(); h.attach(); const old = h.id();
  h.view.onDispatch = event => {
    if (event.type !== 'permissionrequest') return;
    h.view.removeAttribute('src'); h.view.setAttribute('src', '/replacement'); h.attach();
  };
  h.event('permissionrequest', { requestId: 8 }, old);
  assert.notEqual(h.id(), old);
  assert.equal(h.messages.some(m => m.name === 'command' && m.args[1] === 'respond'), false);
});
test('a delayed fullscreen rejection cannot exit a replacement browser', async () => {
  const h = harness(); h.attach(); let reject;
  h.view.requestFullscreen = () => new Promise((_, fail) => { reject = fail; });
  h.event('fullscreenchange', { fullscreen: true });
  h.view.removeAttribute('src'); h.view.setAttribute('src', '/replacement'); h.attach();
  reject(new Error('old request')); await Promise.resolve();
  assert.equal(h.messages.some(m => m.name === 'command' && m.args[1] === 'exitFullscreen'), false);
});
test('a superseded fullscreen request cannot exit the current fullscreen session', async () => {
  const h = harness(); h.attach(); const rejects = [];
  h.view.requestFullscreen = () => new Promise((_, fail) => rejects.push(fail));
  h.event('fullscreenchange', { fullscreen: true });
  h.event('fullscreenchange', { fullscreen: false });
  h.event('fullscreenchange', { fullscreen: true });
  rejects[0](new Error('superseded')); await Promise.resolve();
  assert.equal(h.messages.some(m => m.name === 'command' && m.args[1] === 'exitFullscreen'), false);
  rejects[1](new Error('current')); await Promise.resolve();
  assert.equal(h.messages.at(-1).args[1], 'exitFullscreen');
});
