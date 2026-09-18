const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../../native/src/justcef_view_renderer.cc'), 'utf8');
const bootstrap = [...source.split('constexpr char kViewContentScript')[0].matchAll(/R"JS\(([\s\S]*?)\)JS"/g)]
  .map(match => match[1]).join('');

function harness() {
  let now = 0;
  let nextTimer = 0;
  const frames = new Map();
  const timers = new Map();
  const messages = [];
  const classes = new Map();
  const listeners = new Map();
  class Element {
    constructor() {
      this.attributes = new Map();
      this.isConnected = true;
      this.rect = { left: 20, top: 20, width: 100, height: 100, right: 120, bottom: 120 };
      this.classList = { toggle() {} };
    }
    attachShadow() { return { append() {} }; }
    getContext() { return { drawImage() {} }; }
    addEventListener() {}
    getAttribute(name) { return this.attributes.get(name) ?? null; }
    setAttribute(name, value) {
      const old = this.getAttribute(name);
      this.attributes.set(name, value);
      this.attributeChangedCallback?.(name, old, value);
    }
    removeAttribute(name) {
      const old = this.getAttribute(name);
      this.attributes.delete(name);
      this.attributeChangedCallback?.(name, old, null);
    }
    getBoundingClientRect() { return this.rect; }
    checkVisibility() { return true; }
    dispatchEvent() {}
  }
  const document = {
    baseURI: 'https://app.test/', visibilityState: 'visible',
    documentElement: { clientWidth: 800, clientHeight: 600 },
    createElement: () => new Element(), addEventListener: (name, fn) => listeners.set(name, fn),
    querySelectorAll: () => [], elementsFromPoint: () => [],
  };
  const context = {
    Element, HTMLElement: Element, HTMLSlotElement: class {}, ShadowRoot: class {},
    CSSStyleSheet: class { replaceSync() {} },
    ResizeObserver: class { observe() {} unobserve() {} },
    MutationObserver: class { observe() {} disconnect() {} },
    Node: { DOCUMENT_POSITION_FOLLOWING: 4 },
    CustomEvent: class {}, document, URL, Blob, atob,
    createImageBitmap: async () => ({ width: 100, height: 100, close() {} }),
    customElements: { get: name => classes.get(name), define: (name, cls) => classes.set(name, cls) },
    innerWidth: 800, innerHeight: 600, devicePixelRatio: 1,
    performance: { now: () => now },
    getComputedStyle: () => ({ position: 'relative', backgroundColor: '', pointerEvents: 'auto' }),
    addEventListener: (name, fn) => listeners.set(name, fn),
    matchMedia: () => ({ addEventListener() {}, removeEventListener() {} }),
    requestAnimationFrame: fn => { frames.set(++nextTimer, fn); return nextTimer; },
    cancelAnimationFrame: id => frames.delete(id),
    setInterval: () => ++nextTimer, clearInterval() {},
    setTimeout: (fn, delay) => { timers.set(++nextTimer, { fn, at: now + delay }); return nextTimer; },
    clearTimeout: id => timers.delete(id),
  };
  context.window = context;
  const native = Object.fromEntries(['create', 'update', 'navigate', 'command', 'destroy', 'host']
    .map(name => [name, (...args) => messages.push({ name, args })]));
  const dispatch = vm.runInNewContext(bootstrap, context)(native);
  const view = new (classes.get('justcef-view'))();
  view.attributes.set('src', '/view');
  view.connectedCallback();
  const id = messages.find(m => m.name === 'create').args[0];
  function advance(ms) {
    for (let elapsed = 0; elapsed < ms; elapsed += 16) {
      now += 16;
      for (const [id, timer] of [...timers]) {
        if (timer.at <= now) { timers.delete(id); timer.fn(); }
      }
      const pending = [...frames.values()];
      frames.clear();
      pending.forEach(fn => fn());
    }
  }
  function event(name, detail) { dispatch('event', id, name, JSON.stringify(detail)); }
  return { view, messages, advance, event,
    snapshot: async () => { dispatch('snapshot', id, '', null); await Promise.resolve(); },
    scroll: () => listeners.get('scroll')({ target: document }),
    pageEvent: name => listeners.get(name)({ persisted: true }) };
}

test('motion freeze resumes after geometry settles', async () => {
  const h = harness();
  h.event('viewcreated', { viewId: 10 });
  await h.snapshot();
  h.view.setAttribute('transition', 'freeze');
  h.view.rect = { left: 40, top: 20, width: 100, height: 100, right: 140, bottom: 120 };
  h.advance(32);
  assert.ok(h.messages.some(m => m.name === 'update' && m.args[17] === true));
  h.advance(400);
  const last = h.messages.filter(m => m.name === 'update').at(-1);
  assert.equal(last.args[17], false, 'must leave frozen mode');
  assert.equal(last.args[13], true, 'must restore the native view');
});

test('default parent scrolling updates immediately without hiding or requesting snapshots', () => {
  const h = harness();
  h.event('viewcreated', { viewId: 10 });
  for (let i = 1; i <= 20; ++i) {
    h.view.rect = { left: 20, top: 20 - i, width: 100, height: 100, right: 120, bottom: 120 - i };
    h.scroll();
    const update = h.messages.filter(m => m.name === 'update').at(-1);
    assert.equal(update.args[3], 20 - i, 'position is sent during the scroll callback');
    assert.equal(update.args[13], true, 'native view stays visible');
    assert.equal(update.args[17], false, 'scroll does not freeze');
    h.advance(16);
  }
  assert.equal(h.messages.filter(m => m.name === 'command' && m.args[1] === 'snapshot').length, 0);
  const count = h.messages.filter(m => m.name === 'update').length;
  h.advance(600);
  assert.equal(h.messages.filter(m => m.name === 'update').length, count, 'settled geometry sends no extra updates');
});

test('rejected navigation preserves a live view and its commands', () => {
  const h = harness();
  h.event('viewcreated', { viewId: 10 });
  h.view.setAttribute('src', 'file:///denied');
  h.event('error', { reason: 'denied' });
  assert.equal(h.view.viewId, 10);
  h.view.reload();
  assert.equal(h.messages.at(-1).name, 'command');
  assert.equal(h.messages.at(-1).args[1], 'reload');
  h.view.setAttribute('src', '/valid');
  assert.equal(h.messages.at(-1).name, 'navigate');
});

test('removing src destroys the browser and permits recreation', () => {
  const h = harness();
  h.event('viewcreated', { viewId: 10 });
  h.view.removeAttribute('src');
  assert.equal(h.view.viewId, null);
  assert.equal(h.messages.at(-1).name, 'destroy');
  h.view.setAttribute('src', '/new');
  assert.equal(h.messages.filter(m => m.name === 'create').length, 2);
});

test('creation errors release pending native state and can be retried', () => {
  const h = harness();
  h.event('error', { reason: 'limit' });
  assert.equal(h.messages.at(-1).name, 'destroy');
  h.view.setAttribute('src', '/retry');
  assert.equal(h.messages.filter(m => m.name === 'create').length, 2);
});

test('late creation replies cannot resurrect a removed browser', () => {
  const h = harness();
  const oldId = h.messages.find(m => m.name === 'create').args[0];
  h.view.removeAttribute('src');
  h.view.setAttribute('src', '/replacement');
  const newId = h.messages.filter(m => m.name === 'create').at(-1).args[0];
  assert.notEqual(oldId, newId);
  h.event('viewcreated', { viewId: 99 });
  assert.equal(h.view.viewId, null, 'old creation reply must be ignored');
  h.event('error', { reason: 'create-failed' });
  h.view.reload();
  assert.deepEqual(h.messages.at(-1), { name: 'command', args: [newId, 'reload'] });
});

test('back-forward cache releases native surfaces and recreates on restore', () => {
  const h = harness();
  h.event('viewcreated', { viewId: 10 });
  h.pageEvent('pagehide');
  assert.equal(h.view.viewId, null);
  assert.equal(h.messages.at(-1).name, 'destroy');
  h.pageEvent('pageshow');
  assert.equal(h.messages.filter(m => m.name === 'create').length, 2);
});
