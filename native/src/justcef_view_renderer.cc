#include "justcef_view_renderer.h"

#include "justcef_view_common.h"

#include "include/base/cef_logging.h"

#include <algorithm>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

constexpr char kViewBootstrapScript[] = R"JS((function (native) {
  'use strict';

  const ANCHOR_START = 0;
  const ANCHOR_END = 1;
  const ANCHOR_STRETCH = 2;
  const ANCHOR_CENTER = 3;
  const HOT_MS = 500;
  const POLL_MS = 1000;
  const RESIZE_SETTLE_MS = 150;
  const SCROLL_SETTLE_MS = 120;
  const RELEASE_STABLE_MS = 120;
  const DESTROY_GRACE_MS = 1000;
  const SUSPEND_SNAPSHOT_MS = 150;

  const internals = new WeakMap();
  const elements = new Map();
  const tracked = new Set();
  let nextId = 0;
  let seq = 0;
  let hotUntil = 0;
  let rafId = 0;
  let pollId = 0;
  let mutationObserver = null;
  let resolutionQuery = null;
  let lastInnerWidth = innerWidth;
  let lastInnerHeight = innerHeight;
  let resizeTimer = 0;
  let pageHidden = false;

  function nextSequence() {
    seq = (seq + 1) | 0;
    return seq;
  }

  const sheet = new CSSStyleSheet();
  sheet.replaceSync(':host{display:block;position:relative}' +
    '.underlay{position:absolute;left:0;top:0;width:100%;height:100%;pointer-events:none;display:none}' +
    '.underlay.shown{display:block}');

  const resizeObserver = new ResizeObserver(() => markHot());

  function composedParent(node) {
    if (!node) return null;
    if (node.assignedSlot) return node.assignedSlot;
    const parent = node.parentNode;
    if (!parent) return null;
    if (parent instanceof ShadowRoot) return parent.host;
    return parent instanceof Element ? parent : null;
  }

  function composedContains(ancestor, node) {
    for (let current = node; current; current = composedParent(current)) {
      if (current === ancestor) return true;
    }
    return false;
  }

  function hasFixedAncestor(el) {
    for (let current = el; current && current !== document.documentElement; current = composedParent(current)) {
      if (getComputedStyle(current).position === 'fixed') return true;
    }
    return false;
  }

  function overlaps(a, b) {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
  }

  function parseColor(value) {
    const match = /rgba?\(([^)]+)\)/.exec(value || '');
    if (!match) return 0;
    const parts = match[1].split(/[ ,\/]+/).filter(Boolean).map(Number);
    const alpha = parts.length > 3 ? parts[3] : 1;
    if (!(alpha > 0)) return 0;
    return ((255 << 24) | ((parts[0] & 255) << 16) | ((parts[1] & 255) << 8) | (parts[2] & 255)) | 0;
  }

  function parseAnchor(value) {
    const result = { h: null, v: null };
    if (!value || value === 'auto') return result;
    const words = value.toLowerCase().split(/\s+/).filter(Boolean);
    const left = words.includes('left');
    const right = words.includes('right');
    const top = words.includes('top');
    const bottom = words.includes('bottom');
    if (left && right) result.h = ANCHOR_STRETCH;
    else if (right) result.h = ANCHOR_END;
    else if (left) result.h = ANCHOR_START;
    else if (words.includes('hcenter')) result.h = ANCHOR_CENTER;
    if (top && bottom) result.v = ANCHOR_STRETCH;
    else if (bottom) result.v = ANCHOR_END;
    else if (top) result.v = ANCHOR_START;
    else if (words.includes('vcenter')) result.v = ANCHOR_CENTER;
    return result;
  }

  function guessHorizontal(rect, size) {
    const start = rect.left;
    const end = size - rect.right;
    if (Math.abs(start) <= 1 && Math.abs(end) <= 1) return ANCHOR_STRETCH;
    if (Math.abs(start - end) <= 1) return ANCHOR_CENTER;
    return end < start ? ANCHOR_END : ANCHOR_START;
  }

  function guessVertical(rect, size) {
    const start = rect.top;
    const end = size - rect.bottom;
    if (Math.abs(start) <= 1 && Math.abs(end) <= 1) return ANCHOR_STRETCH;
    if (Math.abs(end) <= 1) return ANCHOR_END;
    return ANCHOR_START;
  }

  function learnAxis(s0, e0, s1, e1, size0, size1) {
    if (Math.abs(size1 - size0) < 2) return undefined;
    const startConst = Math.abs(s1 - s0) <= 1;
    const endConst = Math.abs((size1 - e1) - (size0 - e0)) <= 1;
    const sizeConst = Math.abs((e1 - s1) - (e0 - s0)) <= 1;
    if (startConst && endConst) return ANCHOR_STRETCH;
    if (startConst && sizeConst) return ANCHOR_START;
    if (endConst && sizeConst) return ANCHOR_END;
    const c0 = (s0 + e0) / 2 - size0 / 2;
    const c1 = (s1 + e1) / 2 - size1 / 2;
    if (sizeConst && Math.abs(c1 - c0) <= 1) return ANCHOR_CENTER;
    return null;
  }

  function computeClip(el, rect) {
    let left = rect.left;
    let top = rect.top;
    let right = rect.right;
    let bottom = rect.bottom;
    for (let node = composedParent(el); node && node !== document.documentElement; node = composedParent(node)) {
      if (!(node instanceof Element) || node instanceof HTMLSlotElement) continue;
      const style = getComputedStyle(node);
      const clipX = style.overflowX !== 'visible';
      const clipY = style.overflowY !== 'visible';
      if (clipX || clipY) {
        const box = node.getBoundingClientRect();
        const cl = box.left + node.clientLeft;
        const ct = box.top + node.clientTop;
        if (clipX) {
          left = Math.max(left, cl);
          right = Math.min(right, cl + node.clientWidth);
        }
        if (clipY) {
          top = Math.max(top, ct);
          bottom = Math.min(bottom, ct + node.clientHeight);
        }
      }
      if (style.position === 'fixed') break;
    }
    const root = document.documentElement;
    left = Math.max(left, 0);
    top = Math.max(top, 0);
    right = Math.min(right, root.clientWidth || innerWidth);
    bottom = Math.min(bottom, root.clientHeight || innerHeight);
    return { left, top, right: Math.max(left, right), bottom: Math.max(top, bottom) };
  }

  function topLayerOccludes(el, rect) {
    try {
      for (const dialog of document.querySelectorAll('dialog[open]')) {
        if (composedContains(dialog, el)) continue;
        if (dialog.matches(':modal') || overlaps(dialog.getBoundingClientRect(), rect)) return true;
      }
      for (const popover of document.querySelectorAll(':popover-open')) {
        if (!composedContains(popover, el) && overlaps(popover.getBoundingClientRect(), rect)) return true;
      }
    } catch (e) {
    }
    return false;
  }

  function sampleOccluded(el, clip) {
    const width = clip.right - clip.left;
    const height = clip.bottom - clip.top;
    if (width < 4 || height < 4 || getComputedStyle(el).pointerEvents === 'none') return false;
    const xs = [clip.left + 1, clip.left + width / 2, clip.right - 1];
    const ys = [clip.top + 1, clip.top + height / 2, clip.bottom - 1];
    for (const x of xs) {
      for (const y of ys) {
        const hit = document.elementsFromPoint(x, y);
        const top = hit.length ? hit[0] : null;
        if (top && top !== el && !composedContains(el, top)) return true;
      }
    }
    return false;
  }

  function fullscreenBlocks(el) {
    const fullscreen = document.fullscreenElement;
    return !!fullscreen && fullscreen !== el && !composedContains(fullscreen, el);
  }

  function canScrollAxis(node, horizontal, direction) {
    const style = getComputedStyle(node);
    const overflow = horizontal ? style.overflowX : style.overflowY;
    if (!/(auto|scroll|overlay)/.test(overflow)) return false;
    const max = horizontal ? node.scrollWidth - node.clientWidth : node.scrollHeight - node.clientHeight;
    const position = horizontal ? Math.abs(node.scrollLeft) : node.scrollTop;
    return max > 1 && (direction > 0 ? position < max - 1 : position > 1);
  }

  function keyboardScroll(el, kind) {
    const horizontal = kind === 'left' || kind === 'right';
    const direction = kind === 'up' || kind === 'pageup' || kind === 'home' || kind === 'left' ? -1 : 1;
    let target = null;
    let node = getComputedStyle(el).position === 'fixed' ? null : composedParent(el);
    for (; node && node !== document.documentElement && node !== document.body; node = composedParent(node)) {
      if (!(node instanceof Element) || node instanceof HTMLSlotElement) continue;
      if (canScrollAxis(node, horizontal, direction)) {
        target = node;
        break;
      }
      if (getComputedStyle(node).position === 'fixed') break;
    }
    const scroller = target || window;
    const size = target ? (horizontal ? target.clientWidth : target.clientHeight) : (horizontal ? innerWidth : innerHeight);
    if (kind === 'home' || kind === 'end') {
      const extent = target ? target.scrollHeight : document.documentElement.scrollHeight;
      scroller.scrollTo({ top: kind === 'home' ? 0 : extent, behavior: 'smooth' });
      return;
    }
    const amount = kind === 'pageup' || kind === 'pagedown' ? Math.max(size * 0.875, size - 40) : 40;
    scroller.scrollBy(horizontal ? { left: direction * amount, behavior: 'smooth' } : { top: direction * amount, behavior: 'smooth' });
  }

  let lastHostWheelReport = 0;

  function insideView(event) {
    for (let node = event.composedPath()[0]; node; node = composedParent(node)) {
      if (node instanceof Element && node.localName === 'justcef-view') return true;
    }
    return false;
  }

  addEventListener('wheel', (event) => {
    if (!event.isTrusted || !tracked.size || insideView(event)) return;
    const now = performance.now();
    if (now - lastHostWheelReport < 50) return;
    lastHostWheelReport = now;
    native.host('wheel');
  }, { capture: true, passive: true });

  addEventListener('pointerdown', (event) => {
    if (event.isTrusted && tracked.size) native.host('input');
  }, { capture: true, passive: true });

  addEventListener('keydown', (event) => {
    if (event.isTrusted && tracked.size) native.host('input');
  }, { capture: true, passive: true });

  function scrollAffects(target, el) {
    if (target === document || target === document.scrollingElement || target === document.documentElement || target === document.body) {
      return !hasFixedAncestor(el);
    }
    if (!(target instanceof Element) || !composedContains(target, el)) return false;
    for (let node = composedParent(el); node && node !== target; node = composedParent(node)) {
      if (node instanceof Element && getComputedStyle(node).position === 'fixed') return false;
    }
    return true;
  }
)JS"
                                            R"JS(
  function documentOrder() {
    return Array.from(tracked).sort((a, b) => (a.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING) ? -1 : 1);
  }

  function tick() {
    rafId = 0;
    const ordered = documentOrder();
    ordered.forEach((el, index) => {
      const state = internals.get(el);
      if (state) state.measure(index);
    });
    if (!rafId && performance.now() < hotUntil && tracked.size) rafId = requestAnimationFrame(tick);
  }

  function markHot(ms) {
    if (!tracked.size) return;
    hotUntil = Math.max(hotUntil, performance.now() + (ms || HOT_MS));
    if (!rafId) rafId = requestAnimationFrame(tick);
  }

  function watchResolution() {
    if (resolutionQuery) resolutionQuery.removeEventListener('change', onResolutionChange);
    resolutionQuery = matchMedia('(resolution: ' + devicePixelRatio + 'dppx)');
    resolutionQuery.addEventListener('change', onResolutionChange);
  }

  function onResolutionChange() {
    watchResolution();
    markHot();
  }

  function startTracking(el) {
    const wasEmpty = !tracked.size;
    tracked.add(el);
    resizeObserver.observe(el);
    if (wasEmpty) {
      mutationObserver = new MutationObserver(() => markHot());
      mutationObserver.observe(document.documentElement, { subtree: true, childList: true, attributes: true });
      pollId = setInterval(() => {
        if (rafId) {
          cancelAnimationFrame(rafId);
          rafId = 0;
        }
        tick();
      }, POLL_MS);
      watchResolution();
    }
    markHot();
  }

  function stopTracking(el) {
    tracked.delete(el);
    resizeObserver.unobserve(el);
    if (!tracked.size) {
      if (mutationObserver) mutationObserver.disconnect();
      mutationObserver = null;
      clearInterval(pollId);
      pollId = 0;
    }
  }

  function forEachState(callback) {
    for (const el of tracked) {
      const state = internals.get(el);
      if (state) callback(state, el);
    }
  }

  function tabbables() {
    const selector = 'a[href],area[href],button,input,select,textarea,iframe,summary,[tabindex],[contenteditable=""],[contenteditable="true"]';
    return Array.from(document.querySelectorAll(selector)).filter((node) => {
      if (node.tabIndex < 0 || node.disabled) return false;
      if (node.closest('[inert]')) return false;
      return typeof node.checkVisibility !== 'function' || node.checkVisibility({ visibilityProperty: true });
    });
  }

  function moveFocus(el, forward) {
    const all = tabbables().filter((node) => node !== el && !composedContains(el, node));
    let target = null;
    if (forward) {
      target = all.find((node) => el.compareDocumentPosition(node) & Node.DOCUMENT_POSITION_FOLLOWING) || all[0] || null;
    } else {
      for (let i = all.length - 1; i >= 0; i--) {
        if (el.compareDocumentPosition(all[i]) & Node.DOCUMENT_POSITION_PRECEDING) {
          target = all[i];
          break;
        }
      }
      if (!target) target = all.length ? all[all.length - 1] : null;
    }
    if (target) {
      target.focus();
    } else if (document.activeElement === el) {
      el.blur();
    }
  }

  function decodeBase64(data) {
    const binary = atob(data);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    return bytes;
  }

  class JustCefViewElement extends HTMLElement {
    static get observedAttributes() {
      return ['src', 'anchor', 'transition', 'occlusion'];
    }

    constructor() {
      super();
      const root = this.attachShadow({ mode: 'closed' });
      root.adoptedStyleSheets = [sheet];
      const slot = document.createElement('slot');
      const canvas = document.createElement('canvas');
      canvas.className = 'underlay';
      root.append(slot, canvas);

      const el = this;
      const state = {
        id: ++nextId,
        canvas,
        created: false,
        failed: false,
        viewId: null,
        resizeMode: 'anchor',
        destroyTimer: 0,
        suspended: false,
        suspendPending: 0,
        freezes: new Set(),
        releasing: new Map(),
        lastKey: '',
        lastRect: null,
        movingFrames: 0,
        learned: { h: undefined, v: undefined },
        resizeBase: null,
        occluded: false,
        hasSnapshot: false,
        snapshotToken: 0,

        transition() {
          const value = (el.getAttribute('transition') || 'auto').toLowerCase();
          return value === 'live' || value === 'freeze' ? value : 'auto';
        },

        occlusion() {
          const value = (el.getAttribute('occlusion') || 'snapshot').toLowerCase();
          return value === 'hide' || value === 'none' ? value : 'snapshot';
        },

        freeze(reason) {
          if (this.transition() === 'live' && reason !== 'suspend') return;
          this.freezes.add(reason);
          this.releasing.delete(reason);
          markHot();
        },

        release(reason) {
          if (!this.freezes.has(reason) || this.releasing.has(reason)) return;
          this.releasing.set(reason, performance.now());
          markHot();
        },

        create() {
          if (this.created || !el.isConnected) return;
          const src = el.getAttribute('src');
          if (!src) return;
          let url;
          try {
            url = new URL(src, document.baseURI).href;
          } catch (e) {
            el.dispatchEvent(new CustomEvent('error', { detail: { reason: 'denied' } }));
            return;
          }
          elements.delete(this.id);
          this.id = ++nextId;
          elements.set(this.id, el);
          this.created = true;
          this.failed = false;
          this.lastKey = '';
          native.create(this.id, url, parseColor(getComputedStyle(el).backgroundColor));
          startTracking(el);
          this.measure(documentOrder().indexOf(el));
        },

        destroy() {
          clearTimeout(this.destroyTimer);
          this.destroyTimer = 0;
          clearTimeout(this.suspendPending);
          this.suspendPending = 0;
          if (this.created) native.destroy(this.id);
          this.created = false;
          this.viewId = null;
          this.lastKey = '';
          this.snapshotToken++;
          this.hasSnapshot = false;
          this.showUnderlay(false);
          this.freezes.clear();
          this.releasing.clear();
          this.lastRect = null;
          this.movingFrames = 0;
          stopTracking(el);
        },

        showUnderlay(show) {
          this.canvas.classList.toggle('shown', !!show && this.hasSnapshot);
        },
)JS"
                                            R"JS(
        measure(order) {
          if (!this.created || this.failed || !el.isConnected) return;

          const rect = el.getBoundingClientRect();
          const width = innerWidth;
          const height = innerHeight;
          const now = performance.now();

          if (width !== lastInnerWidth || height !== lastInnerHeight) {
            lastInnerWidth = width;
            lastInnerHeight = height;
            onWindowResized();
          }

          if (this.resizeBase && (this.resizeBase.width !== width || this.resizeBase.height !== height)) {
            const base = this.resizeBase;
            const h = learnAxis(base.rect.left, base.rect.right, rect.left, rect.right, base.width, width);
            const v = learnAxis(base.rect.top, base.rect.bottom, rect.top, rect.bottom, base.height, height);
            if (h !== undefined) this.learned.h = h;
            if (v !== undefined) this.learned.v = v;
          }
          if (!this.freezes.has('resize')) this.resizeBase = { rect, width, height };

          const last = this.lastRect;
          const moved = !last || last.left !== rect.left || last.top !== rect.top || last.width !== rect.width || last.height !== rect.height;
          this.lastRect = rect;

          if (moved && last && !this.freezes.has('scroll') && !this.freezes.has('resize')) {
            this.movingFrames++;
            if (this.transition() === 'freeze' || this.movingFrames >= 3) this.freeze('motion');
          } else if (!moved) {
            this.movingFrames = 0;
            if (this.freezes.has('motion')) this.release('motion');
          }

          for (const [reason, since] of this.releasing) {
            if (moved) {
              this.releasing.set(reason, now);
            } else if (now - since >= RELEASE_STABLE_MS) {
              this.releasing.delete(reason);
              this.freezes.delete(reason);
            }
          }

          if (this.freezes.size > 0 && !this.suspended) {
            if (this.lastKey !== 'frozen') {
              this.lastKey = 'frozen';
              native.update(this.id, nextSequence(), rect.left, rect.top, rect.width, rect.height, rect.left, rect.top, rect.width, rect.height,
                width, height, devicePixelRatio, false, 0, 0, order, true);
            }
            if (moved) markHot();
            return;
          }

          const clip = computeClip(el, rect);
          const hasArea = rect.width > 0 && rect.height > 0 && clip.right > clip.left && clip.bottom > clip.top;
          let visible = hasArea && !pageHidden && document.visibilityState === 'visible';
          if (visible && typeof el.checkVisibility === 'function') {
            visible = el.checkVisibility({ visibilityProperty: true, opacityProperty: true, contentVisibilityAuto: true });
          }
          if (visible && window.visualViewport && Math.abs(visualViewport.scale - 1) > 0.001) visible = false;
          if (visible && fullscreenBlocks(el)) visible = false;

          const occlusionMode = this.occlusion();
          let occluded = false;
          if (visible && occlusionMode !== 'none') {
            occluded = topLayerOccludes(el, clip) || sampleOccluded(el, clip);
          }
          if (occluded !== this.occluded) this.occluded = occluded;

          const show = visible && !occluded && !this.suspended;
          this.showUnderlay(!(occluded && occlusionMode === 'hide'));

          const explicit = parseAnchor(el.getAttribute('anchor'));
          const anchorH = explicit.h !== null ? explicit.h : (this.learned.h !== undefined && this.learned.h !== null ? this.learned.h : guessHorizontal(rect, width));
          const anchorV = explicit.v !== null ? explicit.v : (this.learned.v !== undefined && this.learned.v !== null ? this.learned.v : guessVertical(rect, height));

          const values = [rect.left, rect.top, rect.width, rect.height, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top,
            width, height, devicePixelRatio, show, anchorH, anchorV, order];
          const key = values.join(',');
          if (key === this.lastKey) return;
          this.lastKey = key;
          native.update(this.id, nextSequence(), rect.left, rect.top, rect.width, rect.height, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top,
            width, height, devicePixelRatio, show, anchorH, anchorV, order, false);
          if (moved) markHot();
        },

        onWindowResize() {
          const unpredictable = this.learned.h === null || this.learned.v === null;
          if (this.transition() === 'freeze' || this.resizeMode === 'freeze' || unpredictable) this.freeze('resize');
        },

        onNative(kind, a, b) {
          if (kind === 'snapshot') {
            const token = ++this.snapshotToken;
            let bytes;
            try {
              bytes = decodeBase64(a);
            } catch (e) {
              return;
            }
            createImageBitmap(new Blob([bytes], { type: 'image/jpeg' })).then((bitmap) => {
              if (token !== this.snapshotToken) {
                bitmap.close();
                return;
              }
              this.canvas.width = bitmap.width;
              this.canvas.height = bitmap.height;
              const context = this.canvas.getContext('2d');
              if (context) context.drawImage(bitmap, 0, 0);
              bitmap.close();
              this.hasSnapshot = true;
              this.showUnderlay(!(this.occluded && this.occlusion() === 'hide'));
              if (this.suspendPending) {
                clearTimeout(this.suspendPending);
                this.suspendPending = 0;
                this.freeze('suspend');
              }
            }, () => {});
            return;
          }

          if (kind === 'keyscroll') {
            keyboardScroll(el, a);
            return;
          }

          if (kind === 'takefocus') {
            moveFocus(el, !!a);
            return;
          }

          let detail = null;
          try {
            detail = b ? JSON.parse(b) : null;
          } catch (e) {
            detail = null;
          }

          if (a === 'viewfocus' && document.activeElement === el) {
            el.blur();
          }

          if (a === 'viewcreated') {
            this.viewId = detail ? detail.viewId : null;
            this.resizeMode = detail && detail.resizeMode === 'freeze' ? 'freeze' : 'anchor';
            detail = { viewId: this.viewId };
          } else if (a === 'error') {
            if (this.viewId === null) {
              this.destroy();
              this.failed = true;
            }
          } else if (a === 'close') {
            const closedId = detail && typeof detail.viewId === 'number' ? detail.viewId : null;
            if (this.viewId === null || (closedId !== null && closedId !== this.viewId)) return;
            this.created = false;
            this.destroy();
            detail = null;
          }

          const bubbles = a === 'viewkeydown';
          el.dispatchEvent(new CustomEvent(a, { detail, bubbles, composed: bubbles }));
        }
      };

      internals.set(this, state);
      this.addEventListener('focus', () => {
        if (state.created) native.command(state.id, 'focus');
      });
    }

    get viewId() {
      const state = internals.get(this);
      return state ? state.viewId : null;
    }

    reload() {
      this._command('reload');
    }

    goBack() {
      this._command('goBack');
    }

    goForward() {
      this._command('goForward');
    }

    focus(options) {
      HTMLElement.prototype.focus.call(this, options);
      const state = internals.get(this);
      if (state && state.created && (document.activeElement !== this || !document.hasFocus())) native.command(state.id, 'focus');
    }

    suspend() {
      const state = internals.get(this);
      if (!state || state.suspended || state.suspendPending) return;
      if (!state.created) {
        state.suspended = true;
        return;
      }
      native.command(state.id, 'snapshot');
      state.suspendPending = setTimeout(() => {
        state.suspendPending = 0;
        state.freeze('suspend');
      }, SUSPEND_SNAPSHOT_MS);
      state.suspended = true;
      state.freezes.delete('suspend');
    }

    resume() {
      const state = internals.get(this);
      if (!state) return;
      clearTimeout(state.suspendPending);
      state.suspendPending = 0;
      state.suspended = false;
      state.release('suspend');
      markHot();
    }

    _command(command) {
      const state = internals.get(this);
      if (state && state.created) native.command(state.id, command);
    }

    connectedCallback() {
      const state = internals.get(this);
      clearTimeout(state.destroyTimer);
      state.destroyTimer = 0;
      elements.set(state.id, this);
      if (state.created) {
        startTracking(this);
      } else {
        state.create();
      }
    }

    disconnectedCallback() {
      const state = internals.get(this);
      if (state.created) {
        native.update(state.id, nextSequence(), 0, 0, 0, 0, 0, 0, 0, 0, innerWidth, innerHeight, devicePixelRatio, false, 0, 0, 0, false);
        state.lastKey = '';
      }
      stopTracking(this);
      clearTimeout(state.destroyTimer);
      state.destroyTimer = setTimeout(() => {
        state.destroyTimer = 0;
        if (!this.isConnected) {
          state.destroy();
          elements.delete(state.id);
        }
      }, DESTROY_GRACE_MS);
    }

    attributeChangedCallback(name, oldValue, newValue) {
      const state = internals.get(this);
      if (!state) return;
      if (name === 'src') {
        if (!newValue) {
          state.destroy();
          return;
        }
        if (!this.isConnected) return;
        if (!state.created) {
          state.create();
          return;
        }
        try {
          native.navigate(state.id, new URL(newValue, document.baseURI).href);
        } catch (e) {
          this.dispatchEvent(new CustomEvent('error', { detail: { reason: 'denied' } }));
        }
        return;
      }
      state.lastKey = '';
      markHot();
    }
  }
)JS"
                                            R"JS(
  function onWindowResized() {
    forEachState((state) => state.onWindowResize());
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => {
      resizeTimer = 0;
      forEachState((state) => state.release('resize'));
    }, RESIZE_SETTLE_MS);
  }

  const scrollTimers = new Map();

  function onScroll(event) {
    if (!tracked.size) return;
    const target = event.target;
    forEachState((state, el) => {
      if (state.transition() === 'live' || !scrollAffects(target, el)) return;
      state.freeze('scroll');
      clearTimeout(scrollTimers.get(el));
      scrollTimers.set(el, setTimeout(() => {
        scrollTimers.delete(el);
        state.release('scroll');
      }, SCROLL_SETTLE_MS));
    });
    markHot();
  }


  function onAnimationStart(event) {
    const target = event.target;
    forEachState((state, el) => {
      if (state.transition() !== 'live' && target instanceof Element && composedContains(target, el)) state.freeze('animation');
    });
  }

  function onAnimationEnd(event) {
    const target = event.target;
    forEachState((state, el) => {
      if (target instanceof Element && composedContains(target, el)) state.release('animation');
    });
    markHot();
  }

  addEventListener('resize', () => {
    markHot();
  });
  document.addEventListener('scroll', onScroll, { capture: true, passive: true });
  document.addEventListener('transitionrun', onAnimationStart, true);
  document.addEventListener('animationstart', onAnimationStart, true);
  document.addEventListener('transitionend', onAnimationEnd, true);
  document.addEventListener('transitioncancel', onAnimationEnd, true);
  document.addEventListener('animationend', onAnimationEnd, true);
  document.addEventListener('animationcancel', onAnimationEnd, true);
  document.addEventListener('visibilitychange', () => markHot(), true);
  document.addEventListener('fullscreenchange', () => markHot(), true);
  document.addEventListener('toggle', () => markHot(), true);
  document.addEventListener('close', () => markHot(), true);
  document.addEventListener('cancel', () => markHot(), true);
  if (window.visualViewport) {
    visualViewport.addEventListener('resize', () => markHot());
    visualViewport.addEventListener('scroll', () => markHot());
  }
  if (document.fonts) document.fonts.addEventListener('loadingdone', () => markHot());

  addEventListener('pagehide', (event) => {
    if (!event.persisted) return;
    pageHidden = true;
    forEachState((state) => {
      state.destroy();
    });
  });

  addEventListener('pageshow', (event) => {
    if (!event.persisted) return;
    pageHidden = false;
    for (const el of Array.from(elements.values())) {
      const state = internals.get(el);
      if (!state || !el.isConnected) continue;
      state.created = false;
      state.create();
    }
    markHot();
  });

  if (!customElements.get('justcef-view')) {
    customElements.define('justcef-view', JustCefViewElement);
  }

  return function dispatch(kind, id, a, b) {
    const el = elements.get(id);
    if (!el) return;
    const state = internals.get(el);
    if (state) state.onNative(kind, a, b);
  };
}))JS";

constexpr char kViewContentScript[] = R"JS((function (native) {
  'use strict';

  const GESTURE_GAP_MS = 250;
  const TOUCH_SLOP = 8;
  const PHASE_WHEEL = 0;
  const PHASE_PRECISE_WHEEL = 1;
  const PHASE_TOUCH_MOVE = 2;
  const PHASE_TOUCH_END = 3;
  const PHASE_CANCEL = 4;

  let forwarding = null;
  let lastWheel = 0;
  let latchUntil = 0;
  let blocking = false;
  let latchTimer = 0;
  let touch = null;
  const handled = new WeakSet();

  function parentOf(node) {
    if (node.assignedSlot) return node.assignedSlot;
    const parent = node.parentNode;
    if (parent instanceof ShadowRoot) return parent.host;
    return parent instanceof Element ? parent : null;
  }

  function canScroll(el, dx, dy) {
    const root = el === document.scrollingElement;
    const style = getComputedStyle(el);
    if (dy !== 0) {
      const scrollable = root ? style.overflowY !== 'hidden' && style.overflowY !== 'clip' : /(auto|scroll|overlay)/.test(style.overflowY);
      const max = el.scrollHeight - el.clientHeight;
      if (scrollable && max > 1 && (dy > 0 ? el.scrollTop < max - 1 : el.scrollTop > 1)) return true;
    }
    if (dx !== 0) {
      const scrollable = root ? style.overflowX !== 'hidden' && style.overflowX !== 'clip' : /(auto|scroll|overlay)/.test(style.overflowX);
      const max = el.scrollWidth - el.clientWidth;
      const position = Math.abs(el.scrollLeft);
      if (scrollable && max > 1 && (dx > 0 ? position < max - 1 : position > 1)) return true;
    }
    return false;
  }

  function chainCanScroll(target, dx, dy) {
    for (let node = target instanceof Element ? target : null; node; node = parentOf(node)) {
      if (node === document.documentElement || node === document.body) break;
      if (canScroll(node, dx, dy)) return true;
    }
    const root = document.scrollingElement;
    return !!root && canScroll(root, dx, dy);
  }

  function isEditable(node) {
    for (let current = node instanceof Element ? node : null; current; current = parentOf(current)) {
      if (current.isContentEditable) return true;
      const tag = current.localName;
      if (tag === 'input' || tag === 'textarea' || tag === 'select') return true;
    }
    return false;
  }

  function wheelDeltas(event) {
    let dx = event.deltaX;
    let dy = event.deltaY;
    if (event.deltaMode === 1) {
      dx *= 40;
      dy *= 40;
    } else if (event.deltaMode === 2) {
      dx *= innerWidth;
      dy *= innerHeight;
    }
    return { dx, dy };
  }

  function wheelPhase(event) {
    const precise = event.deltaMode === 0 && (event.wheelDeltaX % 120 !== 0 || event.wheelDeltaY % 120 !== 0);
    return precise ? PHASE_PRECISE_WHEEL : PHASE_WHEEL;
  }

  function onLatchedWheel(event) {
    const now = performance.now();
    if (!event.isTrusted || event.ctrlKey || now >= latchUntil) return;
    event.preventDefault();
    handled.add(event);
    forwarding = true;
    lastWheel = now;
    extendLatch(GESTURE_GAP_MS);
    const delta = wheelDeltas(event);
    native.forward(delta.dx, delta.dy, event.clientX, event.clientY, wheelPhase(event));
  }

  function extendLatch(ms) {
    latchUntil = Math.max(latchUntil, performance.now() + ms);
    if (!blocking) {
      addEventListener('wheel', onLatchedWheel, { capture: true, passive: false });
      blocking = true;
    }
    clearTimeout(latchTimer);
    latchTimer = setTimeout(() => {
      removeEventListener('wheel', onLatchedWheel, { capture: true });
      blocking = false;
    }, latchUntil - performance.now() + 50);
  }

  addEventListener('wheel', (event) => {
    if (!event.isTrusted || event.ctrlKey || handled.has(event)) return;
    const delta = wheelDeltas(event);
    const phase = wheelPhase(event);
    const now = performance.now();
    if (now - lastWheel > GESTURE_GAP_MS) forwarding = null;
    lastWheel = now;
    const target = event.composedPath()[0];
    const x = event.clientX;
    const y = event.clientY;
    setTimeout(() => {
      if (forwarding === null) forwarding = !event.defaultPrevented && !chainCanScroll(target, delta.dx, delta.dy);
      if (forwarding && !event.defaultPrevented) native.forward(delta.dx, delta.dy, x, y, phase);
    }, 0);
  }, { passive: true });

  addEventListener('keydown', (event) => {
    if (!event.isTrusted || event.ctrlKey || event.altKey || event.metaKey) return;
    let kind = null;
    let dx = 0;
    let dy = 0;
    switch (event.key) {
      case 'ArrowDown': kind = 'down'; dy = 1; break;
      case 'ArrowUp': kind = 'up'; dy = -1; break;
      case 'ArrowRight': kind = 'right'; dx = 1; break;
      case 'ArrowLeft': kind = 'left'; dx = -1; break;
      case 'PageDown': kind = 'pagedown'; dy = 1; break;
      case 'PageUp': kind = 'pageup'; dy = -1; break;
      case ' ': kind = event.shiftKey ? 'pageup' : 'pagedown'; dy = event.shiftKey ? -1 : 1; break;
      case 'End': kind = 'end'; dy = 1; break;
      case 'Home': kind = 'home'; dy = -1; break;
      default: return;
    }
    if (event.shiftKey && event.key !== ' ') return;
    const target = event.composedPath()[0];
    if (isEditable(target)) return;
    const active = document.activeElement;
    const start = active && active !== document.body ? active : target;
    const couldScroll = chainCanScroll(start, dx, dy);
    setTimeout(() => {
      if (!event.defaultPrevented && !couldScroll) native.key(kind);
    }, 0);
  }, { passive: true });

  function cancelMomentum(x, y) {
    native.forward(0, 0, x, y, PHASE_CANCEL);
  }

  addEventListener('pointerdown', (event) => {
    if (event.isTrusted && event.pointerType !== 'touch') cancelMomentum(event.clientX, event.clientY);
  }, { capture: true, passive: true });

  addEventListener('touchstart', (event) => {
    if (!event.isTrusted) return;
    const first = event.touches[0];
    if (first) cancelMomentum(first.clientX, first.clientY);
    if (event.touches.length !== 1) {
      touch = null;
      return;
    }
    touch = { id: first.identifier, target: event.composedPath()[0], startX: first.clientX, startY: first.clientY, lastX: first.clientX, lastY: first.clientY, forwarding: null };
  }, { capture: true, passive: true });

  addEventListener('touchmove', (event) => {
    const current = touch;
    if (!current || !event.isTrusted) return;
    if (event.touches.length !== 1) {
      touch = null;
      return;
    }
    const point = Array.from(event.changedTouches).find((item) => item.identifier === current.id);
    if (!point) return;
    const x = point.clientX;
    const y = point.clientY;
    setTimeout(() => {
      if (touch !== current) return;
      if (current.forwarding === null) {
        const mx = current.startX - x;
        const my = current.startY - y;
        if (Math.hypot(mx, my) < TOUCH_SLOP) return;
        const horizontal = Math.abs(mx) > Math.abs(my);
        current.forwarding = !event.defaultPrevented && !chainCanScroll(current.target, horizontal ? mx : 0, horizontal ? 0 : my);
      }
      if (!current.forwarding || event.defaultPrevented) return;
      const dx = current.lastX - x;
      const dy = current.lastY - y;
      current.lastX = x;
      current.lastY = y;
      if (dx || dy) native.forward(dx, dy, x, y, PHASE_TOUCH_MOVE);
    }, 0);
  }, { capture: true, passive: true });

  function endTouch(event) {
    const current = touch;
    if (!current || !event.isTrusted) return;
    touch = null;
    setTimeout(() => {
      if (current.forwarding) native.forward(0, 0, current.lastX, current.lastY, event.type === 'touchend' ? PHASE_TOUCH_END : PHASE_CANCEL);
    }, 0);
  }

  addEventListener('touchend', endTouch, { capture: true, passive: true });
  addEventListener('touchcancel', endTouch, { capture: true, passive: true });

  return function dispatch(kind, value) {
    if (kind === 'latch') extendLatch(value);
  };
}))JS";

class ViewContentV8Handler final : public CefV8Handler
{
public:
    ViewContentV8Handler() = default;

    bool Execute(const CefString& name, CefRefPtr<CefV8Value> object, const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval, CefString& exception) override
    {
        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
        retval = CefV8Value::CreateUndefined();

        if (name == "key")
        {
            if (arguments.size() < 1 || !arguments[0]->IsString())
                return false;
            if (frame && frame->IsMain())
            {
                CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kViewKeyScrollMessageName);
                message->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
                frame->SendProcessMessage(PID_BROWSER, message);
            }
            return true;
        }

        if (name != "forward" || arguments.size() < 5)
            return false;

        for (size_t i = 0; i < 5; ++i)
        {
            if (!arguments[i]->IsDouble() && !arguments[i]->IsInt())
                return false;
        }

        if (!frame || !frame->IsMain())
            return true;

        CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kViewWheelMessageName);
        CefRefPtr<CefListValue> args = message->GetArgumentList();
        for (size_t i = 0; i < 5; ++i)
            args->SetDouble(i, arguments[i]->GetDoubleValue());
        frame->SendProcessMessage(PID_BROWSER, message);
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(ViewContentV8Handler);
    DISALLOW_COPY_AND_ASSIGN(ViewContentV8Handler);
};

struct ViewContextState
{
    CefRefPtr<CefV8Context> context;
    CefRefPtr<CefV8Value> dispatch;
    std::string token;
};

struct ViewContentState
{
    CefRefPtr<CefV8Context> context;
    CefRefPtr<CefV8Value> dispatch;
};

std::unordered_map<int, std::vector<ViewContentState>> g_view_content_contexts;

std::unordered_map<int, std::vector<ViewContextState>> g_view_contexts;

std::string CreateDocumentToken()
{
    static std::random_device device;
    static std::mt19937_64 generator(device());
    static const char* digits = "0123456789abcdef";
    uint64_t value = generator();
    std::string token(16, '0');
    for (int i = 15; i >= 0; --i)
    {
        token[i] = digits[value & 0xF];
        value >>= 4;
    }
    return token;
}

ViewContextState* FindStateForContext(CefRefPtr<CefBrowser> browser, CefRefPtr<CefV8Context> context)
{
    if (!browser || !context)
        return nullptr;

    auto it = g_view_contexts.find(browser->GetIdentifier());
    if (it == g_view_contexts.end())
        return nullptr;

    for (auto& state : it->second)
    {
        if (state.context && state.context->IsSame(context))
            return &state;
    }
    return nullptr;
}

ViewContextState* FindStateForToken(CefRefPtr<CefBrowser> browser, const std::string& token)
{
    if (!browser)
        return nullptr;

    auto it = g_view_contexts.find(browser->GetIdentifier());
    if (it == g_view_contexts.end())
        return nullptr;

    for (auto& state : it->second)
    {
        if (state.token == token)
            return &state;
    }
    return nullptr;
}

class ViewV8Handler final : public CefV8Handler
{
public:
    ViewV8Handler() = default;

    bool Execute(const CefString& name, CefRefPtr<CefV8Value> object, const CefV8ValueList& arguments, CefRefPtr<CefV8Value>& retval, CefString& exception) override
    {
        CefRefPtr<CefV8Context> context = CefV8Context::GetCurrentContext();
        CefRefPtr<CefBrowser> browser = context ? context->GetBrowser() : nullptr;
        CefRefPtr<CefFrame> frame = context ? context->GetFrame() : nullptr;
        ViewContextState* state = FindStateForContext(browser, context);
        if (!state || !frame || !frame->IsMain())
        {
            retval = CefV8Value::CreateUndefined();
            return true;
        }

        if (name == "host")
        {
            if (arguments.empty() || !arguments[0]->IsString())
                return false;

            CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kViewHostActivityMessageName);
            CefRefPtr<CefListValue> args = message->GetArgumentList();
            args->SetString(0, state->token);
            args->SetString(1, arguments[0]->GetStringValue());
            frame->SendProcessMessage(PID_BROWSER, message);
            retval = CefV8Value::CreateUndefined();
            return true;
        }

        if (arguments.empty() || !arguments[0]->IsInt())
        {
            exception = "Invalid view element identifier.";
            return true;
        }

        const int32_t elementId = arguments[0]->GetIntValue();
        CefRefPtr<CefProcessMessage> message;

        if (name == "create")
        {
            if (arguments.size() < 3 || !arguments[1]->IsString())
            {
                exception = "create(id, src, background) expects a string source.";
                return true;
            }

            message = CefProcessMessage::Create(kViewCreateMessageName);
            CefRefPtr<CefListValue> args = message->GetArgumentList();
            args->SetString(0, state->token);
            args->SetInt(1, elementId);
            args->SetString(2, arguments[1]->GetStringValue());
            args->SetInt(3, arguments[2]->IsInt() ? arguments[2]->GetIntValue() : 0);
        }
        else if (name == "update")
        {
            if (arguments.size() < 1 + kViewUpdateFieldCount)
            {
                exception = "update() expects all geometry fields.";
                return true;
            }

            ViewUpdate update;
            update.seq = arguments[1]->GetIntValue();
            update.x = arguments[2]->GetDoubleValue();
            update.y = arguments[3]->GetDoubleValue();
            update.width = arguments[4]->GetDoubleValue();
            update.height = arguments[5]->GetDoubleValue();
            update.clipX = arguments[6]->GetDoubleValue();
            update.clipY = arguments[7]->GetDoubleValue();
            update.clipWidth = arguments[8]->GetDoubleValue();
            update.clipHeight = arguments[9]->GetDoubleValue();
            update.innerWidth = arguments[10]->GetDoubleValue();
            update.innerHeight = arguments[11]->GetDoubleValue();
            update.devicePixelRatio = arguments[12]->GetDoubleValue();
            update.visible = arguments[13]->GetBoolValue();
            update.anchorH = arguments[14]->GetIntValue();
            update.anchorV = arguments[15]->GetIntValue();
            update.order = arguments[16]->GetIntValue();
            update.frozen = arguments[17]->GetBoolValue();

            message = CefProcessMessage::Create(kViewUpdateMessageName);
            CefRefPtr<CefListValue> args = message->GetArgumentList();
            args->SetString(0, state->token);
            args->SetInt(1, elementId);
            WriteViewUpdate(args, 2, update);
        }
        else if (name == "navigate" || name == "command")
        {
            if (arguments.size() < 2 || !arguments[1]->IsString())
            {
                exception = "Expected a string argument.";
                return true;
            }

            message = CefProcessMessage::Create(name == "navigate" ? kViewNavigateMessageName : kViewCommandMessageName);
            CefRefPtr<CefListValue> args = message->GetArgumentList();
            args->SetString(0, state->token);
            args->SetInt(1, elementId);
            args->SetString(2, arguments[1]->GetStringValue());
        }
        else if (name == "destroy")
        {
            message = CefProcessMessage::Create(kViewDestroyMessageName);
            CefRefPtr<CefListValue> args = message->GetArgumentList();
            args->SetString(0, state->token);
            args->SetInt(1, elementId);
        }
        else
        {
            return false;
        }

        frame->SendProcessMessage(PID_BROWSER, message);
        retval = CefV8Value::CreateUndefined();
        return true;
    }

private:
    IMPLEMENT_REFCOUNTING(ViewV8Handler);
    DISALLOW_COPY_AND_ASSIGN(ViewV8Handler);
};

void Dispatch(CefRefPtr<CefBrowser> browser, const std::string& token, const std::string& kind, int32_t elementId, CefRefPtr<CefV8Value> a, CefRefPtr<CefV8Value> b)
{
    ViewContextState* state = FindStateForToken(browser, token);
    if (!state || !state->context || !state->dispatch)
    {
        VLOG(1) << "Dropped view message " << kind << " for element " << elementId << " (token = " << token << ").";
        return;
    }

    CefRefPtr<CefV8Context> context = state->context;
    CefRefPtr<CefV8Value> dispatch = state->dispatch;
    if (!context->Enter())
        return;

    CefV8ValueList arguments;
    arguments.push_back(CefV8Value::CreateString(kind));
    arguments.push_back(CefV8Value::CreateInt(elementId));
    arguments.push_back(a ? a : CefV8Value::CreateUndefined());
    arguments.push_back(b ? b : CefV8Value::CreateUndefined());
    dispatch->ExecuteFunction(nullptr, arguments);

    context->Exit();
}

} // namespace

void InstallViewElement(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
    if (!browser || !frame || !frame->IsMain() || !context)
        return;

    CefRefPtr<CefV8Value> factory;
    CefRefPtr<CefV8Exception> exception;
    if (!context->Eval(kViewBootstrapScript, "justcef://view/bootstrap.js", 1, factory, exception) || !factory || !factory->IsFunction())
    {
        LOG(ERROR) << "Failed to bootstrap justcef-view: " << (exception ? exception->GetMessage().ToString() : "unknown error");
        return;
    }

    CefRefPtr<CefV8Handler> handler = new ViewV8Handler();
    CefRefPtr<CefV8Value> native = CefV8Value::CreateObject(nullptr, nullptr);
    const auto attributes = static_cast<CefV8Value::PropertyAttribute>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
    for (const char* name : {"create", "update", "navigate", "command", "destroy", "host"})
        native->SetValue(name, CefV8Value::CreateFunction(name, handler), attributes);

    ViewContextState state;
    state.context = context;
    state.token = CreateDocumentToken();
    g_view_contexts[browser->GetIdentifier()].push_back(state);

    if (!context->Enter())
        return;

    CefV8ValueList arguments;
    arguments.push_back(native);
    CefRefPtr<CefV8Value> dispatch = factory->ExecuteFunction(nullptr, arguments);
    if (!dispatch || !dispatch->IsFunction())
    {
        CefRefPtr<CefV8Exception> error = factory->HasException() ? factory->GetException() : nullptr;
        LOG(ERROR) << "Failed to initialize justcef-view: " << (error ? error->GetMessage().ToString() : "unknown error");
    }
    else if (ViewContextState* stored = FindStateForContext(browser, context))
    {
        stored->dispatch = dispatch;
    }

    context->Exit();
}

bool IsViewContent(CefRefPtr<CefDictionaryValue> extra_info)
{
    return extra_info && extra_info->HasKey(kViewContentExtraInfoKey) && extra_info->GetBool(kViewContentExtraInfoKey);
}

void InstallViewContentScript(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
    if (!browser || !frame || !frame->IsMain() || !context)
        return;

    CefRefPtr<CefV8Value> factory;
    CefRefPtr<CefV8Exception> exception;
    if (!context->Eval(kViewContentScript, "justcef://view/content.js", 1, factory, exception) || !factory || !factory->IsFunction())
    {
        LOG(ERROR) << "Failed to install justcef-view content script: " << (exception ? exception->GetMessage().ToString() : "unknown error");
        return;
    }

    CefRefPtr<CefV8Handler> handler = new ViewContentV8Handler();
    CefRefPtr<CefV8Value> native = CefV8Value::CreateObject(nullptr, nullptr);
    const auto attributes = static_cast<CefV8Value::PropertyAttribute>(V8_PROPERTY_ATTRIBUTE_READONLY | V8_PROPERTY_ATTRIBUTE_DONTDELETE);
    for (const char* name : {"forward", "key"})
        native->SetValue(name, CefV8Value::CreateFunction(name, handler), attributes);

    if (!context->Enter())
        return;

    CefV8ValueList arguments;
    arguments.push_back(native);
    CefRefPtr<CefV8Value> dispatch = factory->ExecuteFunction(nullptr, arguments);
    context->Exit();

    if (dispatch && dispatch->IsFunction())
        g_view_content_contexts[browser->GetIdentifier()].push_back({context, dispatch});
}

void ReleaseViewContentContext(CefRefPtr<CefBrowser> browser, CefRefPtr<CefV8Context> context)
{
    if (!browser || !context)
        return;

    auto it = g_view_content_contexts.find(browser->GetIdentifier());
    if (it == g_view_content_contexts.end())
        return;

    auto& states = it->second;
    states.erase(std::remove_if(states.begin(), states.end(),
                                [&](const ViewContentState& state)
                                {
                                    return state.context && state.context->IsSame(context);
                                }),
                 states.end());
    if (states.empty())
        g_view_content_contexts.erase(it);
}

void ReleaseViewContext(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefV8Context> context)
{
    if (!browser || !frame || !frame->IsMain())
        return;

    auto it = g_view_contexts.find(browser->GetIdentifier());
    if (it == g_view_contexts.end())
        return;

    auto& states = it->second;
    for (auto state = states.begin(); state != states.end(); ++state)
    {
        if (state->context && context && state->context->IsSame(context))
        {
            CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create(kViewContextReleasedMessageName);
            message->GetArgumentList()->SetString(0, state->token);
            frame->SendProcessMessage(PID_BROWSER, message);
            states.erase(state);
            break;
        }
    }

    if (states.empty())
        g_view_contexts.erase(it);
}

bool HandleViewProcessMessage(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefProcessMessage> message)
{
    const std::string name = message->GetName();
    if (name.rfind(kViewMessagePrefix, 0) != 0)
        return false;

    if (name == kViewPingMessageName)
    {
        if (frame)
            frame->SendProcessMessage(PID_BROWSER, CefProcessMessage::Create(kViewPongMessageName));
        return true;
    }

    if (name == kViewLatchMessageName)
    {
        CefRefPtr<CefListValue> latchArgs = message->GetArgumentList();
        CefRefPtr<CefV8Context> context = frame ? frame->GetV8Context() : nullptr;
        auto it = browser ? g_view_content_contexts.find(browser->GetIdentifier()) : g_view_content_contexts.end();
        if (!context || it == g_view_content_contexts.end() || !latchArgs || latchArgs->GetSize() < 1)
            return true;

        for (auto& state : it->second)
        {
            if (!state.context || !state.context->IsSame(context) || !context->Enter())
                continue;

            CefV8ValueList arguments;
            arguments.push_back(CefV8Value::CreateString("latch"));
            arguments.push_back(CefV8Value::CreateInt(latchArgs->GetInt(0)));
            state.dispatch->ExecuteFunction(nullptr, arguments);
            context->Exit();
            break;
        }
        return true;
    }

    CefRefPtr<CefListValue> args = message->GetArgumentList();
    if (!args || args->GetSize() < 3 || args->GetType(0) != VTYPE_STRING || args->GetType(1) != VTYPE_INT)
        return true;

    const std::string token = args->GetString(0);
    const int32_t elementId = args->GetInt(1);

    if (name == kViewEventMessageName && args->GetSize() >= 4)
    {
        Dispatch(browser, token, "event", elementId, CefV8Value::CreateString(args->GetString(2)), CefV8Value::CreateString(args->GetString(3)));
        return true;
    }

    if (name == kViewSnapshotMessageName)
    {
        Dispatch(browser, token, "snapshot", elementId, CefV8Value::CreateString(args->GetString(2)), nullptr);
        return true;
    }

    if (name == kViewKeyScrollMessageName)
    {
        Dispatch(browser, token, "keyscroll", elementId, CefV8Value::CreateString(args->GetString(2)), nullptr);
        return true;
    }

    if (name == kViewTakeFocusMessageName)
    {
        Dispatch(browser, token, "takefocus", elementId, CefV8Value::CreateBool(args->GetType(2) == VTYPE_BOOL && args->GetBool(2)), nullptr);
        return true;
    }

    return true;
}

void ClearViewState(CefRefPtr<CefBrowser> browser)
{
    if (!browser)
        return;

    g_view_contexts.erase(browser->GetIdentifier());
    g_view_content_contexts.erase(browser->GetIdentifier());
}
