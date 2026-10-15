const assert = require('node:assert/strict');
const { test } = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

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
  let id = 0;
  const intervals = new Map(), timeouts = new Map(), elements = new Map();
  const started = [];
  let status = { done: true, livenessPassed: true, collected: 5, target: 5 };
  const timers = {
    setInterval(fn) { intervals.set(++id, fn); return id; },
    clearInterval(key) { intervals.delete(key); },
    setTimeout(fn) { timeouts.set(++id, fn); return id; },
    clearTimeout(key) { timeouts.delete(key); }
  };
  function element(key) {
    if (!elements.has(key)) elements.set(key, {
      checked: true, style: {}, classList: { add() {}, remove() {}, toggle() {} },
      addEventListener() {}, textContent: '', value: ''
    });
    return elements.get(key);
  }
  const host = {
    GetUsername: () => 'test', GetFaceCount: () => 0,
    CaptureFaceSamples(angle) { started.push(angle); return true; },
    GetCaptureStatus: () => JSON.stringify(status), CancelCapture() {}
  };
  const context = vm.createContext({
    ...timers, window: { ...timers, chrome: { webview: { hostObjects: { sync: { host } } } } },
    document: { getElementById: element, querySelectorAll: () => [], addEventListener() {} },
    Image: function() {}, console
  });
  for (const file of ['capture_flow.js', 'app.js']) {
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../../enrollment_app', file), 'utf8'), context);
  }
  // Skip unrelated page-start timers; exercise real page event handlers.
  intervals.clear(); timeouts.clear();
  context.onCapture();
  [...intervals.values()][0](); // first angle completes, next angle is queued
  const queued = [...timeouts.values()];
  context.onCancelCapture();
  context.onCapture();
  queued.forEach(fn => fn());
  assert.deepEqual(started, [0, 0]);
  assert.equal(context.captureFlow.isActive(), true);
  status = { done: true, livenessPassed: false, collected: 0, target: 5 };
  [...intervals.values()][0]();
  assert.equal(context.captureFlow.isActive(), false);
  assert.equal(intervals.size, 0);
  assert.equal(element('cam-caption').textContent, '采集失败');
});
