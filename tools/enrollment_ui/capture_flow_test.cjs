const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// Mirrors ENROLLMENT_JS_SOURCES in enrollment_app/CMakeLists.txt — the files
// are evaluated one by one here, so top-level wiring in later files only sees
// globals from files listed before them. Keep both lists in sync.
const PAGE_SOURCES = [
  'capture_flow.js', 'ui_common.js', 'preview.js', 'capture_ui.js',
  'navigation.js', 'settings.js', 'faces.js', 'account_refresh.js',
  'enroll_save.js', 'logs.js', 'app_main.js'
];

function fixture() {
  let id = 0;
  const pending = new Map();
  const timers = {
    setInterval(callback) { pending.set(++id, callback); return id; },
    setTimeout(callback) { pending.set(++id, callback); return id; },
    clearInterval(key) { pending.delete(key); },
    clearTimeout(key) { pending.delete(key); }
  };
  const context = vm.createContext({});
  vm.runInContext(fs.readFileSync(path.join(__dirname,
    '../../enrollment_app/capture_flow.js'), 'utf8'), context);
  return { flow: context.createCaptureFlow(timers), pending };
}

// Full-page harness: mock timers/DOM/host, evaluate every module file in
// build order, and expose the pieces tests need to poke at.
function page(overrides) {
  overrides = overrides || {};
  let id = 0;
  const intervals = new Map(), timeouts = new Map(), elements = new Map();
  const timers = {
    setInterval(fn) { intervals.set(++id, fn); return id; },
    clearInterval(key) { intervals.delete(key); },
    setTimeout(fn) { timeouts.set(++id, fn); return id; },
    clearTimeout(key) { timeouts.delete(key); }
  };
  function element(key) {
    if (!elements.has(key)) elements.set(key, {
      checked: true, style: {}, disabled: false, value: '', textContent: '',
      classList: (() => {
        const set = new Set();
        return {
          add(c) { set.add(c); },
          remove(c) { set.delete(c); },
          toggle(c, on) { (on === undefined ? !set.has(c) : on) ? set.add(c) : set.delete(c); },
          contains(c) { return set.has(c); }
        };
      })(),
      addEventListener() {}
    });
    return elements.get(key);
  }
  const state = {
    status: { done: true, livenessPassed: true, collected: 5, target: 5 },
    config: { camera_rotation: 90, ema_learning: true, secret_key: 'keep-me',
              match_threshold: 0.85, anti_spoof_threshold: 0.28,
              learning_distance_gate: 0.65 }
  };
  const calls = { facesJson: 0, setConfig: [] };
  const host = Object.assign({
    GetUsername: () => 'test', GetFaceCount: () => 1,
    CaptureFaceSamples() { return true; },
    GetCaptureStatus: () => JSON.stringify(state.status),
    CancelCapture() {},
    GetFacesJson() { calls.facesJson++; return JSON.stringify([{ id: 1, label: 'test-face' }]); },
    GetConfig: () => JSON.stringify(state.config),
    SetConfig(json) { calls.setConfig.push(json); return true; }
  }, overrides.host);
  const context = vm.createContext({
    ...timers, window: { ...timers, chrome: { webview: { hostObjects: { sync: { host } } } } },
    document: { getElementById: element, querySelectorAll: () => [], addEventListener() {} },
    Image: function() {}, console
  });
  for (const file of PAGE_SOURCES) {
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../../enrollment_app', file), 'utf8'), context);
  }
  return { context, intervals, timeouts, elements, element, calls, state };
}

test('cancelling between angles clears timers and rejects already queued work after retry', () => {
  const { flow, pending } = fixture();
  let oldPolls = 0, oldAngles = 0, newPolls = 0;
  flow.begin(true, 0);
  flow.poll(() => oldPolls++);
  const oldPoll = [...pending.values()][0];
  flow.stopPolling();
  flow.scheduleNext(() => oldAngles++);
  const oldAngle = [...pending.values()][0];
  assert.equal(flow.isActive(), true); // the inter-angle gap is still capture
  flow.end();
  assert.equal(pending.size, 0);
  flow.begin(false, 2);
  flow.poll(() => newPolls++);
  oldPoll();
  oldAngle();
  [...pending.values()][0]();
  assert.deepEqual([oldPolls, oldAngles, newPolls], [0, 0, 1]);
  assert.equal(flow.firstAngle(), 2);
  assert.equal(flow.isMultiAngle(), false);
});

test('normal three-angle sequence stays active until confirmation', () => {
  const { flow, pending } = fixture();
  const angles = [];
  flow.begin(true, 0);
  function capture(angle) {
    angles.push(angle);
    if (angle < 2) flow.scheduleNext(() => capture(angle + 1));
    else flow.end();
  }
  capture(0);
  while (pending.size) {
    const [key, callback] = [...pending][0];
    pending.delete(key);
    callback();
  }
  assert.deepEqual(angles, [0, 1, 2]);
  assert.equal(flow.isActive(), false);
  assert.equal(flow.isMultiAngle(), true); // retained for the append screen
  assert.equal(flow.canShowIdleTotal(), false);
});

test('failure or completion prevents stale totals and poll callbacks', () => {
  const { flow, pending } = fixture();
  assert.equal(flow.canShowIdleTotal(), true);
  flow.begin(false, 1);
  assert.equal(flow.canShowIdleTotal(), false);
  let paints = 0;
  flow.poll(() => paints++);
  const queued = [...pending.values()][0];
  flow.end();
  flow.end(); // teardown is idempotent
  queued();
  assert.equal(paints, 0);
  assert.equal(pending.size, 0);
  assert.equal(flow.canShowIdleTotal(), false);
  assert.equal(flow.firstAngle(), 1);
});

test('page cancellation and retry cannot start an old sequence angle', () => {
  const started = [];
  const p = page({ host: { CaptureFaceSamples(angle) { started.push(angle); return true; } } });
  const { context, intervals, timeouts } = p;
  // Skip unrelated page-start timers; exercise real page event handlers.
  intervals.clear(); timeouts.clear();
  context.onCapture();
  [...intervals.values()][0](); // first angle completes, next angle is queued
  const queued = [...timeouts.values()];
  context.onCancelCapture();
  context.onCapture();
  queued.forEach(fn => fn());
  assert.deepEqual(started, [0, 0]);
  assert.equal(context.captureCtl.isActive(), true);
  p.state.status = { done: true, livenessPassed: false, collected: 0, target: 5 };
  [...intervals.values()][0]();
  assert.equal(context.captureCtl.isActive(), false);
  assert.equal(intervals.size, 0);
  assert.equal(p.element('cam-caption').textContent, '采集失败');
});

test('switching to the faces tab loads and renders the face list', () => {
  const p = page();
  p.context.switchTab('faces');
  assert.equal(p.calls.facesJson, 1);
  assert.ok(p.element('face-list').innerHTML.indexOf('test-face') >= 0);
  assert.equal(p.element('faces-count').textContent, '1 / 3 张');
  assert.equal(p.context.navCtl.tab(), 'faces');
  assert.ok(p.element('screen-faces').classList.contains('active'));
  assert.ok(!p.element('screen-cam').classList.contains('active'));
});

test('saving settings round-trips non-UI config keys through the loaded snapshot', () => {
  const p = page();
  p.context.loadSettings();
  assert.equal(p.element('cfg-threshold').value, 0.85);
  assert.equal(p.element('cfg-threshold-val').textContent, '0.85');
  p.element('cfg-rotation').value = '180';
  p.context.onSaveSettings();
  [...p.timeouts.values()].forEach(fn => fn()); // run the deferred SetConfig
  assert.equal(p.calls.setConfig.length, 1);
  const saved = JSON.parse(p.calls.setConfig[0]);
  assert.equal(saved.camera_rotation, 180);   // UI-managed key overridden
  assert.equal(saved.secret_key, 'keep-me');  // non-UI key survives the round trip
});

test('banner re-show clears the previous hide timer for the same screen only', () => {
  const p = page();
  p.timeouts.clear(); // drop the page-start timers, keep only banner timers
  p.context.showBanner('cam', 'first', 'ok');
  const camTimer = [...p.timeouts.keys()][0];
  p.context.showBanner('settings', 'other', 'error');
  assert.equal(p.timeouts.size, 2);
  p.context.showBanner('cam', 'second', 'ok'); // clears cam's old timer, not settings'
  assert.equal(p.timeouts.size, 2);            // one cleared, one added
  assert.ok(!p.timeouts.has(camTimer));
  [...p.timeouts.values()].forEach(fn => fn());
  assert.equal(p.element('cam-banner').className, 'banner');
  assert.equal(p.element('settings-banner').className, 'banner');
});

test('modal ok hands the input value to the callback and closes the dialog', () => {
  const p = page();
  let got = null;
  p.context.showAppModal({ title: 't', message: 'm', onOk(v) { got = v; } });
  assert.ok(p.element('modal-overlay').classList.contains('active'));
  p.element('modal-input').value = 'hello';
  p.context.onModalOk();
  assert.equal(got, 'hello');
  assert.ok(!p.element('modal-overlay').classList.contains('active'));
});
