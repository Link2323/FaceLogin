
var H = window.chrome.webview.hostObjects.sync.host;

function showLicenseDocument(button) {
  document.querySelectorAll('.license-docs button').forEach(function(item) {
    item.classList.toggle('active', item === button);
  });
  var content = document.getElementById('license-content');
  content.textContent = '正在读取许可文件…';
  try {
    var text = H.GetLicenseDocument(button.dataset.license);
    content.textContent = text;
  } catch (e) {
    content.textContent = '无法读取许可文件：' + (e && e.message ? e.message : String(e));
  }
}

function openLicenseViewer() {
  document.getElementById('license-overlay').classList.add('active');
  showLicenseDocument(document.querySelector('.license-docs button.active'));
}

function closeLicenseViewer() {
  document.getElementById('license-overlay').classList.remove('active');
}
var viewing = true, lastB64 = '', lastFacesJson = '', timer = 0, faceImg = new Image();
var captureFlow = createCaptureFlow(window);
var camReady = false, currentTab = 'cam';
var refreshState = 0;  // account-type-change state shown in the refresh modal (0/1/2)

// Single status line for the cam screen: an overlay pill on the video. All
// capture states (guidance, angle/frame counts, liveness, completion) and the
// idle hint render here — nothing else paints status text on this screen.
function setCaption(text) {
  document.getElementById('cam-caption').textContent = text;
}

// Button modes. Idle = primary「开始采集」; while capturing it becomes an
// enabled outline「取消采集」— previously a started capture was locked in
// until it finished or hit the 25s watchdog.
function enterCaptureCancelMode() {
  var btn = document.getElementById('btn-capture');
  btn.disabled = false;
  btn.classList.remove('btn-primary');
  btn.classList.add('btn-secondary');
  document.getElementById('cap-btn-text').textContent = '取消采集';
  document.getElementById('cap-spinner').style.display = 'none';
}
function resetCaptureUi() {
  captureFlow.end();
  var btn = document.getElementById('btn-capture');
  btn.disabled = false;
  btn.classList.add('btn-primary');
  btn.classList.remove('btn-secondary');
  document.getElementById('cap-btn-text').textContent = '开始采集';
  document.getElementById('cap-spinner').style.display = 'none';
}
function hideCaptureProgress() {
  document.getElementById('cap-progress').style.display = 'none';
  document.getElementById('cap-fill').style.width = '0%';
}

// User pressed the cancel affordance mid-capture: stop the poll first (so no
// later tick paints over the reset), then tell the host to stop the capture
// thread cooperatively (both phases check m_capturing between frames). The
// partial samples stay in host memory exactly like a failed capture does.
function onCancelCapture() {
  captureFlow.end();
  try { H.CancelCapture(); } catch(e) {}
  resetCaptureUi();
  hideCaptureProgress();
  setCaption('已采 0 帧');
  showBanner('cam', '已取消采集。', 'ok');
}

faceImg.onload = function() {
  drawBackgroundAndOverlay();
  if (!camReady) {
    camReady = true;
    document.getElementById('cam-loading').classList.add('fade');
    setTimeout(function() {
      var el = document.getElementById('cam-loading');
      if (el) el.style.display = 'none';
    }, 350);
  }
};

// Compute the contain-fit mapping for the video frame inside the viewport:
// scale = min(cw/sw, ch/sh), then center the scaled image. Returns {sx,sy,ox,oy}
// where frame point (x,y) → (x*sx+ox, y*sy+oy). BOTH the background draw and
// the overlay draw MUST use this same mapping so the face box/landmarks always
// line up with the visible face. (Changed from stretch → contain in 1.4.1 so a
// portrait/rotated frame isn't distorted: 90/270 rotation makes the JPEG
// portrait, and stretching that into a 16:9 box squashes the face.)
function containMapping(srcW, srcH, cw, ch) {
  if (srcW <= 0 || srcH <= 0 || cw <= 0 || ch <= 0) return { sx: 1, sy: 1, ox: 0, oy: 0 };
  var s = Math.min(cw / srcW, ch / srcH);
  var ox = (cw - srcW * s) / 2;
  var oy = (ch - srcH * s) / 2;
  return { sx: s, sy: s, ox: ox, oy: oy };
}

// Draw the background image AND its paired overlay (same frame). The frame is
// contain-fit over a blurred cover-scaled backdrop, and the overlay uses the
// SAME contain mapping so the face box/landmarks always line up with the
// visible face.
function drawBackgroundAndOverlay() {
  var c = document.getElementById('bg-canvas');
  var vp = document.getElementById('viewport');
  // The viewport stays a flex-fitted full area; the frame is drawn contain-fit
  // on top of a blurred, darkened cover-scaled copy of itself. Ratio mismatch
  // (portrait/rotated cameras, stretched box) must leave padding SOMEWHERE —
  // solid black/white reads as broken letterboxing, so the padding is filled
  // with a continuation of the picture instead: no hard bars, no crop.
  var srcW = faceImg.naturalWidth || 1280;
  var srcH = faceImg.naturalHeight || 720;
  // Backing store at PHYSICAL resolution (devicePixelRatio), drawing coords
  // still CSS px — without this a 125/150% display upscales the 1x bitmap and
  // the overlay text goes fuzzy. CSS pins both canvases to 100% of the
  // viewport, so a larger bitmap renders sharper, never larger.
  var dpr = window.devicePixelRatio || 1;
  var cw = vp.clientWidth, ch = vp.clientHeight;
  c.width = Math.round(cw * dpr);
  c.height = Math.round(ch * dpr);
  var m = containMapping(srcW, srcH, cw, ch);
  var ctx = c.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, cw, ch);
  if (faceImg.naturalWidth > 0) {
    // Backdrop: cover-scale, ~15% oversized so the blur never samples past the
    // image edge into transparency, then darken so the sharp image reads on top.
    ctx.save();
    ctx.filter = 'blur(24px)';
    var cover = Math.max(cw / srcW, ch / srcH) * 1.15;
    ctx.drawImage(faceImg, (cw - srcW * cover) / 2, (ch - srcH * cover) / 2,
                  srcW * cover, srcH * cover);
    ctx.restore();
    ctx.fillStyle = 'rgba(0, 0, 0, 0.35)';
    ctx.fillRect(0, 0, cw, ch);
  }
  ctx.drawImage(faceImg, m.ox, m.oy, srcW * m.sx, srcH * m.sy);
  drawOverlay(lastFacesJson);
}

function renderFrame() {
  if (!viewing) return;
  try {
    // Get the frame AND its face overlay atomically (same frame, no desync).
    var combined = H.GetLatestFrameAndFaces();
    var sep = combined.indexOf('\x1e');
    var b64 = sep > 0 ? combined.substring(0, sep) : combined;
    lastFacesJson = sep > 0 ? combined.substring(sep + 1) : '';
    if (b64 && b64 !== lastB64) {
      lastB64 = b64;
      faceImg.src = b64;   // async load; onload redraws bg + overlay
    } else if (faceImg.complete) {
      // Frame unchanged but canvas may be stale (e.g. window resize) — redraw.
      drawBackgroundAndOverlay();
    }
  } catch(e) {}
  timer = setTimeout(renderFrame, 33);
}

// Path helper for the guidance pill (arcTo rounding works on every WebView2
// runtime, unlike ctx.roundRect which needs a newer Chromium).
function roundedRectPath(ctx, x, y, w, h, r) {
  r = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

function drawOverlay(json) {
  var canvas = document.getElementById('face-canvas');
  var vp = document.getElementById('viewport');
  var ctx = canvas.getContext('2d');
  // Same physical-resolution backing store as the background canvas — see
  // drawBackgroundAndOverlay. Coordinates below stay in CSS px; only the
  // bitmap is dpr-dense, so text/lines render crisp on scaled displays.
  var dpr = window.devicePixelRatio || 1;
  var cw = vp.clientWidth, ch = vp.clientHeight;
  var bw = Math.round(cw * dpr), bh = Math.round(ch * dpr);
  if (canvas.width !== bw || canvas.height !== bh) {
    canvas.width = bw;
    canvas.height = bh;
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

  var faces;
  try { faces = JSON.parse(json); } catch(e) { return; }
  if (faces.length === 0) { ctx.clearRect(0, 0, cw, ch); return; }

  // Use the ACTUAL video frame size (from the loaded image), NOT a hardcoded
  // 1280x720 — the camera may negotiate a different resolution. And use the
  // SAME contain mapping as drawBackgroundAndOverlay, so the overlay always
  // lines up with the visible face (a contain/letterbox background would drift
  // from a stretch overlay, so both share containMapping()).
  var srcW = faceImg.naturalWidth || 1280;
  var srcH = faceImg.naturalHeight || 720;
  var m = containMapping(srcW, srcH, cw, ch);
  var sx = m.sx, sy = m.sy, ox = m.ox, oy = m.oy;

  ctx.clearRect(0, 0, cw, ch);

  for (var fi = 0; fi < faces.length; fi++) {
    var f = faces[fi];
    var bx = f.x * sx + ox, by = f.y * sy + oy;
    var bw = f.w * sx, bh = f.h * sy;

    // Gate state drives the frame color: green in gate, amber = keep turning.
    // targetYaw is only present while capturing (backend contract).
    var col = '#1bb253', hint = '';
    if (typeof f.targetYaw !== 'undefined' && typeof f.yaw !== 'undefined') {
      var diff = f.targetYaw - f.yaw;      // >0 keep turning own-left, <0 own-right
      var inGate = Math.abs(diff) <= 10;   // same ±10° gate the collector uses
      col = inGate ? '#1bb253' : '#f59e0b';
      hint = inGate ? '角度到位 · 请保持'
                    : '再' + (diff > 0 ? '左转 ' : '右转 ') +
                      Math.round(Math.abs(diff)) + '°';
    }

    // Viewfinder-style corner brackets instead of the old hard rectangle —
    // same gate color, crisp square ends and near-square corners.
    ctx.save();
    ctx.strokeStyle = col;
    ctx.lineCap = 'butt';
    ctx.lineJoin = 'miter';
    ctx.lineWidth = 3.5;
    var cl = Math.max(20, Math.min(bw, bh) * 0.26);   // bracket arm length (uniform)
    var cr = 6;                                        // corner rounding
    ctx.beginPath();
    ctx.moveTo(bx, by + cl); ctx.lineTo(bx, by + cr);
    ctx.quadraticCurveTo(bx, by, bx + cr, by); ctx.lineTo(bx + cl, by);
    ctx.moveTo(bx + bw - cl, by); ctx.lineTo(bx + bw - cr, by);
    ctx.quadraticCurveTo(bx + bw, by, bx + bw, by + cr); ctx.lineTo(bx + bw, by + cl);
    ctx.moveTo(bx + bw, by + bh - cl); ctx.lineTo(bx + bw, by + bh - cr);
    ctx.quadraticCurveTo(bx + bw, by + bh, bx + bw - cr, by + bh); ctx.lineTo(bx + bw - cl, by + bh);
    ctx.moveTo(bx + cl, by + bh); ctx.lineTo(bx + cr, by + bh);
    ctx.quadraticCurveTo(bx, by + bh, bx, by + bh - cr); ctx.lineTo(bx, by + bh - cl);
    ctx.stroke();
    ctx.restore();

    // Guidance pill above the box (capturing only): tells the user what to DO
    // instead of exposing the raw "+12.3° / 目标 +30°" numbers.
    if (hint) {
      ctx.save();
      ctx.font = '600 13px "Segoe UI", "Microsoft YaHei", sans-serif';
      var tw = ctx.measureText(hint).width;
      var padX = 12, pillH = 26, dotR = 4, dotGap = 8;
      var pillW = padX * 2 + dotR * 2 + dotGap + tw;
      var px = Math.max(8, Math.min(bx + bw / 2 - pillW / 2, cw - pillW - 8));
      var py = by - pillH - 10;
      if (py < 8) py = 8;   // box near the top edge — pin the pill to the top
      roundedRectPath(ctx, px, py, pillW, pillH, 13);
      ctx.fillStyle = 'rgba(15, 23, 42, 0.82)';
      ctx.fill();
      ctx.beginPath();
      ctx.arc(px + padX + dotR, py + pillH / 2, dotR, 0, Math.PI * 2);
      ctx.fillStyle = col;
      ctx.fill();
      ctx.fillStyle = '#fff';
      ctx.textBaseline = 'middle';
      ctx.fillText(hint, px + padX + dotR * 2 + dotGap, py + pillH / 2 + 0.5);
      ctx.restore();
    }
  }
}

function switchTab(name) {
  if (currentTab === name) return;

  // A capture in flight owns the DirectShow graph — pausing the preview to
  // idle the camera would abort it. Block the switch; cancel first instead.
  if (currentTab === 'cam' && captureFlow.isActive()) {
    showBanner('cam', '正在采集，请先取消采集再切换页面。', 'error');
    return;
  }

  // Camera lifecycle: preview (and its 33ms frame loop) only runs while the
  // cam screen is visible — switching away powers the camera off instead of
  // keeping it live behind screens that need no frames.
  if (currentTab === 'cam') {
    viewing = false;
    clearTimeout(timer);
    try { H.StopPreview(); } catch(e) {}
  }

  var screens = {cam: 'screen-cam', faces: 'screen-faces', settings: 'screen-settings', logs: 'screen-logs'};
  var newScreen = document.getElementById(screens[name]);
  // Fade out current screen
  var all = document.querySelectorAll('.screen');
  for (var i = 0; i < all.length; i++) all[i].classList.remove('active');
  currentTab = name;
  newScreen.classList.add('active');
  var tabNames = {cam: '录入', faces: '人脸', settings: '设置', logs: '日志'};
  var allTabs = document.querySelectorAll('.tabs span');
  for (var i = 0; i < allTabs.length; i++) {
    allTabs[i].classList.toggle('active', allTabs[i].textContent === tabNames[name]);
  }
  if (name === 'cam') {
    // Idempotent server-side (no-op if already running).
    try { if (H.StartPreview()) { viewing = true; renderFrame(); } } catch(e) {}
  }
  if (name === 'settings') loadSettings();
  if (name === 'faces') loadFaces();
  if (name === 'logs') refreshLogs();
}

function loadSettings() {
  try {
    var json = H.GetConfig();
    var cfg = JSON.parse(json);
    // Keep the full parsed config for onSaveSettings: the backend's SetConfig
    // parser starts from defaults, so saving only the UI-managed keys would
    // silently reset non-UI fields (learning gates, alpha, interval, ...).
    window.__loadedConfig = cfg;
    document.getElementById('cfg-rotation').value = String(cfg.camera_rotation || 0);
    document.getElementById('cfg-learning').checked = (cfg.ema_learning !== false);
    document.getElementById('cfg-fastunlock').checked = (cfg.fast_unlock === true);

    // Populate the camera dropdown from the enumerated devices.
    var camSel = document.getElementById('cfg-camera');
    var currentCam = cfg.camera_device || '';
    // Keep the "default" option as first.
    camSel.innerHTML = '<option value="">默认（第一个设备）</option>';
    try {
      var camList = JSON.parse(H.GetCameraList());
      var found = false;
      for (var i = 0; i < camList.length; i++) {
        var opt = document.createElement('option');
        opt.value = camList[i].path;
        opt.textContent = camList[i].name || ('摄像头 ' + (i + 1));
        camSel.appendChild(opt);
        if (currentCam && camList[i].path === currentCam) found = true;
      }
      if (found) {
        camSel.value = currentCam;
      } else if (currentCam) {
        // Configured camera is no longer present — keep it visible so the
        // user knows; the backend refuses to substitute another device.
        var missingOpt = document.createElement('option');
        missingOpt.value = currentCam;
        missingOpt.textContent = '(未找到: ' + currentCam + ')';
        camSel.appendChild(missingOpt);
        camSel.value = currentCam;
      }
    } catch(e) {}

    // match_threshold is now honored in the calibrated band [0.70, 1.00]
    // (EmbeddingThresholdForDim clamps outside values). Slider maps directly
    // to the distance threshold; lower = stricter.
    var matchThr = Number(cfg.match_threshold || 1.00);
    matchThr = Math.max(0.70, Math.min(1.00, matchThr));   // clamp to slider range
    document.getElementById('cfg-threshold').value = matchThr;
    document.getElementById('cfg-threshold-val').textContent = matchThr.toFixed(2);
    // Snap onto the slider's 0.01 grid: configs written by older builds may
    // carry values like 0.281 that the step-0.01 slider can no longer hold.
    var asThr = Number(cfg.anti_spoof_threshold || 0.28);
    asThr = Math.round((asThr - 0.15) / 0.01) * 0.01 + 0.15;
    if (asThr < 0.15) asThr = 0.15;
    if (asThr > 0.50) asThr = 0.50;
    document.getElementById('cfg-antispoof-threshold').value = asThr;
    document.getElementById('cfg-asthr-val').textContent = asThr.toFixed(2);
    // Learning gate cap: slider grid [0.60, 0.68]; hand-edited configs can be
    // stricter (down to 0.35) — the loader honors that, the slider snaps to
    // its own range on display (a settings save then writes the snapped
    // value; the effective gate is min(user p20, cap) so this stays strict).
    var lgThr = Number(cfg.learning_distance_gate || 0.65);
    lgThr = Math.round((lgThr - 0.60) / 0.01) * 0.01 + 0.60;
    if (lgThr < 0.60) lgThr = 0.60;
    if (lgThr > 0.68) lgThr = 0.68;
    document.getElementById('cfg-learn-gate').value = lgThr;
    document.getElementById('cfg-learn-gate-val').textContent = lgThr.toFixed(2);
  } catch(e) { showBanner('settings', '读取设置失败: ' + e.message, 'error'); }
}

function onSaveSettings() {
  // Single UI switch drives both learning channels so "off" means templates
  // are strictly read-only (docs/progressive-learning-v2.md red line 5).
  var learning = document.getElementById('cfg-learning').checked;
  // Round-trip the FULL config: UI-managed keys overridden, every other key
  // preserved from the last load (see loadSettings note on partial saves).
  var cfg = Object.assign({}, window.__loadedConfig || {}, {
    match_threshold: parseFloat(document.getElementById('cfg-threshold').value),
    anti_spoof_threshold: parseFloat(document.getElementById('cfg-antispoof-threshold').value),
    camera_rotation: parseInt(document.getElementById('cfg-rotation').value) || 0,
    camera_device: document.getElementById('cfg-camera').value,
    ema_learning: learning,
    failure_learning: learning,
    fast_unlock: document.getElementById('cfg-fastunlock').checked,
    learning_distance_gate: parseFloat(document.getElementById('cfg-learn-gate').value)
  });
  try {
    var ok = H.SetConfig(JSON.stringify(cfg));
    if (ok) {
      showBanner('settings', '设置已保存并通知后台服务。', 'ok');
    } else {
      showBanner('settings', '设置已写入，但后台服务未确认安全重载；人脸认证将保持关闭，请检查服务和模型文件。', 'error', 10000);
    }
  } catch(e) { showBanner('settings', '保存设置失败: ' + e.message, 'error'); }
}

// Fill the form with display-side defaults (mirrors the backend's
// ConfigFromJson starting point). Nothing is written until the user clicks
// 保存设置, which round-trips window.__loadedConfig for the non-UI keys.
function onResetDefaults() {
  document.getElementById('cfg-rotation').value = '0';
  var camSel = document.getElementById('cfg-camera');
  if (camSel.querySelector('option[value=""]')) camSel.value = '';
  document.getElementById('cfg-learning').checked = false;
  document.getElementById('cfg-fastunlock').checked = false;
  document.getElementById('cfg-threshold').value = 1.00;
  document.getElementById('cfg-threshold-val').textContent = '1.00';
  document.getElementById('cfg-antispoof-threshold').value = 0.28;
  document.getElementById('cfg-asthr-val').textContent = '0.28';
  document.getElementById('cfg-learn-gate').value = 0.65;
  document.getElementById('cfg-learn-gate-val').textContent = '0.65';
  showBanner('settings', '已恢复默认值，点击“保存设置”后生效。', 'ok');
}

function onCapture() {
  // While a capture runs, the primary button doubles as its cancel control.
  if (captureFlow.isActive()) { onCancelCapture(); return; }

  var prog = document.getElementById('cap-progress');

  // Pre-check face slot limit for single-angle mode.
  // Multi-angle always allowed (server replaces old faces via ClearFacesForAccount).
  var faceCount = 0;
  try { faceCount = H.GetFaceCount(); } catch(e) {}
  var multi = false;
  try { multi = document.getElementById('chk-multi-angle').checked; } catch(e) {}
  if (!multi && faceCount >= 3) {
    showBanner('cam', '已达每账号上限（3 张脸）。请用多角度录入替换全部，或先删除旧脸。', 'error', 6000);
    return;
  }

  enterCaptureCancelMode();
  prog.style.display = 'block';
  document.getElementById('cap-fill').style.width = '0%';

  var angles = 1;
  if (multi) angles = 3;

  // Single-angle append: capture the FIRST MISSING angle, not always the
  // front. First enrollment (no faces) -> front angle.
  try {
    var firstAngle = multi ? 0 : pickMissingAngle(faceCount);
    captureFlow.begin(multi, firstAngle);
    captureAngleSequence(firstAngle, angles);
  } catch(e) {
    resetCaptureUi();
    hideCaptureProgress();
    showBanner('cam', '开始采集失败: ' + e.message, 'error');
  }
}

var ANGLE_LABELS = ['正面', '左转', '右转'];

// For a single-angle append, pick the FIRST missing angle (正面/左转/右转)
// based on the labels of faces already enrolled. Old records labeled 脸N
// carry no angle info -> fall back to the front angle.
function pickMissingAngle(faceCount) {
  if (faceCount <= 0) return 0;
  var have = {};
  try {
    var arr = JSON.parse(H.GetFacesJson());
    for (var i = 0; i < arr.length; i++) if (arr[i].label) have[arr[i].label] = true;
  } catch(e) {}
  for (var a = 0; a < ANGLE_LABELS.length; a++) {
    if (!have[ANGLE_LABELS[a]]) return a;
  }
  return 0;
}

var ANGLE_INSTR = {
  0: '请正对摄像头，保持不动',
  1: '请将头转向左侧约 30°',
  2: '请将头转向右侧约 30°'
};

// Drive one capture angle at a time: call CaptureFaceSamples(angle), poll
// GetCaptureStatus, show the instruction + live progress, then move on.
function captureAngleSequence(idx, total) {
  if (!captureFlow.isActive()) return;

  // Start the capture. A false return means a previous capture is still
  // winding down server-side — surface that instead of freezing silently.
  var started = true;
  try { started = H.CaptureFaceSamples(idx); } catch(e) { started = false; }
  if (started === false) {
    resetCaptureUi();
    hideCaptureProgress();
    setCaption('等待上一次采集结束...');
    showBanner('cam', '采集未能启动：上一次采集可能仍在进行，请稍候几秒再试。', 'error');
    return;
  }

  // Reset the bar for the new angle: the completed-angle poll takes the
  // "done" branch and never repaints, so without this the previous angle's
  // last painted value (80% = 4/5 frames) stays frozen through the whole
  // liveness phase of this angle. Paint this angle's instruction right away
  // too — the previous angle's「完成」caption otherwise lingers until the
  // first poll (~200ms), which reads as a stuck bar.
  document.getElementById('cap-fill').style.width = '0%';
  setCaption((total > 1 ? '角度 ' + (idx + 1) + '/' + total + ' · ' : '')
             + (ANGLE_INSTR[idx] || '采集人脸样本...'));

  // Watchdog: if the server reports no activity for 25s, the capture thread
  // is stuck (e.g. camera stall) — fail visibly instead of hanging forever.
  var pollStart = Date.now();
  var gotProgress = false;

  captureFlow.stopPolling();
  captureFlow.poll(function() {
    var st;
    try { st = JSON.parse(H.GetCaptureStatus()); } catch(e) { return; }

    // A cancel landed server-side while a poll was still in flight — the
    // cancel path has already reset the UI; just stand down.
    if (st.cancelled) {
      captureFlow.stopPolling();
      resetCaptureUi();
      hideCaptureProgress();
      return;
    }

    if (st.collected > 0) gotProgress = true;
    if (!gotProgress && !st.done && st.collected === 0 && Date.now() - pollStart > 25000) {
      captureFlow.stopPolling();
      resetCaptureUi();
      hideCaptureProgress();
      setCaption('采集未开始');
      showBanner('cam', '采集超时未开始：摄像头无响应，请检查摄像头后重试。', 'error');
      return;
    }

    // Every angle has its own liveness phase; side-angle COM calls must not
    // inherit a stale proof from a previous capture.
    if (st.livenessChecking) {
      setCaption((total > 1 ? '角度 ' + (idx + 1) + '/' + total + ' · ' : '')
                 + '正在进行活体检测...');
      return;
    }

    // Liveness failed -> abort with the classic error banner.
    if (st.done && !st.livenessPassed) {
      captureFlow.stopPolling();
      resetCaptureUi();
      hideCaptureProgress();
      setCaption('采集失败');
      showBanner('cam', '活体检测失败，请确保是真人并重试。', 'error');
      return;
    }

    // Still collecting this angle — show instruction + progress.
    if (st.collected < st.target) {
      setCaption((total > 1 ? '角度 ' + (idx + 1) + '/' + total + ' · ' : '')
                 + (ANGLE_INSTR[st.angle] || '采集中') + ' · '
                 + st.collected + '/' + st.target + ' 帧');
      var pct = Math.round(st.collected / st.target * 100);
      document.getElementById('cap-fill').style.width = pct + '%';
      return;
    }

    // This angle is complete — move on to the next one. Paint 100% AND a
    // matching caption: the step text used to keep saying「活体检测中」here,
    // which next to「角度 N · 完成」and a frozen 100% bar read as stuck.
    captureFlow.stopPolling();
    document.getElementById('cap-fill').style.width = '100%';
    setCaption(total > 1 ? '角度 ' + (idx + 1) + '/' + total + ' 完成'
                         : '「' + ANGLE_LABELS[idx] + '」采集完成');
    if (idx + 1 < total) {
      captureFlow.scheduleNext(function() { captureAngleSequence(idx + 1, total); });
      return;
    }

    finishCapture();
  });
}

// All angles captured — the original post-capture flow (append vs password).
function finishCapture() {
  resetCaptureUi();
  hideCaptureProgress();
  setCaption('准备就绪');

  viewing = false;
  clearTimeout(timer);
  H.StopPreview();

  // 1.3.0: branch by whether the current account already has faces.
  // First enrollment -> password page; already enrolled -> append page
  // (no password needed — the session identity is the proof).
  var faceCount = 0;
  try { faceCount = H.GetFaceCount(); } catch(e) {}

  if (faceCount > 0) {
    // If the record is a stale account-type one (state 1: MSA→local,
    // state 2: local→MSA), block the append path — AddFace would keep
    // the stale password. Route to the refresh prompt instead.
    try {
      var st = JSON.parse(H.CheckAccountTypeChanged()).state;
      if (st === 1 || st === 2) {
        switchTab('faces');
        showRefreshPrompt(st);
        return;
      }
    } catch(e) {}
    showAppendScreen(faceCount, captureFlow.isMultiAngle());
    return;
  }

  document.getElementById('screen-cam').classList.remove('active');
  document.getElementById('screen-pwd').classList.add('active');
  try { document.getElementById('fld-user').value = H.GetUsername(); } catch(e) {}
  try { document.getElementById('fld-acct-type').value = H.GetAccountType(); } catch(e) {}
  try {
    var sid = H.GetUserSid();
    document.getElementById('fld-sid').textContent = sid;
    document.getElementById('fld-sid-note').style.display = sid ? '' : 'none';
  } catch(e) {}
  try { document.getElementById('fld-face-label').value = ''; } catch(e) {}

  // Label the password field by account type: MSA accounts use the
  // Microsoft account password, local accounts use the Windows password.
  try {
    var acctType = document.getElementById('fld-acct-type').value;
    var passLabel = document.getElementById('fld-pass-label');
    var passInput = document.getElementById('fld-pass');
    if (acctType === 'msa') {
      passLabel.textContent = '微软账号密码';
      passInput.placeholder = '输入你的微软账号密码';
    } else {
      passLabel.textContent = 'Windows 密码';
      passInput.placeholder = '输入你的登录密码';
    }
  } catch(e) {}

  document.getElementById('fld-pass').focus();
}


// ================================================================
// Multi-face management (1.3.0)
// ================================================================

function loadFaces() {
  checkAccountRefresh();  // prompt to refresh a stale MSA→local record if present
  try {
    var faces = JSON.parse(H.GetFacesJson());
    var count = H.GetFaceCount();
    var list = document.getElementById('face-list');
    document.getElementById('faces-count').textContent = count + ' / 3 张';
    try { document.getElementById('faces-user').textContent = H.GetUsername(); } catch(e) {}

    if (faces.length === 0) {
      list.innerHTML = '<div class="face-empty">尚未录入人脸。切换到“录入”页开始录入。</div>';
      return;
    }
    var html = '';
    for (var i = 0; i < faces.length; i++) {
      var f = faces[i];
      html += '<div class="face-item">'
        + '<span class="face-name" title="' + escapeHtml(f.label) + '">' + escapeHtml(f.label) + '</span>'
        + '<span class="face-id">#' + f.id + '</span>'
        + '<button class="btn btn-secondary" onclick="onRenameFace(' + f.id + ')">重命名</button>'
        + '<button class="btn btn-secondary" onclick="onDeleteFace(' + f.id + ')">删除</button>'
        + '</div>';
    }
    list.innerHTML = html;
  } catch(e) { showBanner('faces', '读取人脸失败: ' + e.message, 'error'); }
}

// ================================================================
// In-app modal engine: showAppModal({title, message, okText, cancelText,
// midText, onMid, onOk(value), onCancel}). midText renders an extra secondary
// button (e.g. the「单角度追加」alternative). Hide-then-callback so a handler
// may open another dialog if it needs to.
// ================================================================
var modalCb = null;

function showAppModal(opts) {
  modalCb = { onOk: opts.onOk || null, onCancel: opts.onCancel || null,
              onMid: opts.onMid || null };
  document.getElementById('modal-title').textContent = opts.title || '';
  var msg = document.getElementById('modal-message');
  msg.textContent = opts.message || '';
  msg.style.display = opts.message ? '' : 'none';
  var input = document.getElementById('modal-input');
  input.style.display = opts.input ? '' : 'none';
  input.value = opts.value || '';
  document.getElementById('modal-ok').textContent = opts.okText || '确定';
  document.getElementById('modal-cancel').textContent = opts.cancelText || '取消';
  var mid = document.getElementById('modal-mid');
  mid.style.display = opts.midText ? '' : 'none';
  mid.textContent = opts.midText || '';
  document.getElementById('modal-overlay').classList.add('active');
  if (opts.input) { input.focus(); input.select(); }
}

function hideAppModal() {
  document.getElementById('modal-overlay').classList.remove('active');
}

function onModalOk() {
  var value = document.getElementById('modal-input').value;
  var cb = modalCb;
  hideAppModal();
  modalCb = null;
  if (cb && cb.onOk) cb.onOk(value);
}

function onModalCancel() {
  var cb = modalCb;
  hideAppModal();
  modalCb = null;
  if (cb && cb.onCancel) cb.onCancel();
}

function onModalMid() {
  var cb = modalCb;
  hideAppModal();
  modalCb = null;
  if (cb && cb.onMid) cb.onMid();
}

// Modal keyboard: Enter confirms (including from the text input), Esc cancels.
(function() {
  document.getElementById('modal-input').addEventListener('keydown', function(e) {
    if (e.key === 'Enter') { e.preventDefault(); onModalOk(); }
  });
  document.addEventListener('keydown', function(e) {
    if (e.key === 'Escape' &&
        document.getElementById('modal-overlay').classList.contains('active')) {
      onModalCancel();
    }
    if (e.key === 'Escape' &&
        document.getElementById('license-overlay').classList.contains('active')) {
      closeLicenseViewer();
    }
  });
})();

function goAddFace() {
  var faceCount = 0;
  try { faceCount = H.GetFaceCount(); } catch(e) {}
  var multi = false;
  try { multi = document.getElementById('chk-multi-angle').checked; } catch(e) {}

  // Multi-angle re-enrollment REPLACES all old faces — destructive, so confirm
  // whenever the account already has faces (not only at the per-account cap).
  // Three explicit choices: replace, append a single angle, or back out —
  // "覆盖/追加都要继续" vs "什么都不做" must be separately clickable.
  if (multi && faceCount > 0) {
    showAppModal({
      title: '覆盖重录？',
      message: '当前账号已有 ' + faceCount + ' 张人脸。多角度录入将覆盖全部旧脸（正面/左转/右转各一张）。',
      okText: '覆盖重录',
      midText: '单角度追加',
      onMid: function() {
        try { document.getElementById('chk-multi-angle').checked = false; } catch(e) {}
        switchTab('cam');
        showBanner('cam', '点击「开始采集」即可补录缺失角度（正面/左转/右转）。', 'ok', 6000);
      },
      onOk: function() {
        switchTab('cam');
        showBanner('cam', '多角度重录入：将覆盖当前账号全部旧脸（正面/左转/右转）。', 'ok', 6000);
      }
      // 取消: plain dismiss — no side effect, user stays on the faces screen.
    });
    return;
  }

  // Single-angle append at the per-account cap is impossible — force
  // multi-angle replacement instead.
  if (faceCount >= 3) {
    try { document.getElementById('chk-multi-angle').checked = true; } catch(e) {}
    showAppModal({
      title: '覆盖重录？',
      message: '当前账号已有 3 张人脸，已达上限。继续将使用多角度录入（正面/左转/右转）覆盖全部旧脸。',
      okText: '覆盖重录',
      onOk: function() {
        switchTab('cam');
        showBanner('cam', '多角度重录入：将覆盖当前账号全部旧脸（正面/左转/右转）。', 'ok', 6000);
      }
    });
    return;
  }

  switchTab('cam');
  showBanner('cam', '点击「开始采集」即可为当前账号追加一张新人脸。', 'ok', 6000);
}

function onDeleteFace(id) {
  showAppModal({
    title: '删除这张脸？',
    message: '删除后不可恢复。',
    okText: '删除',
    onOk: function() {
      try {
        if (H.DeleteFace(id)) {
          showBanner('faces', '已删除。', 'ok');
          loadFaces();
        } else {
          showBanner('faces', '删除失败，请重试。', 'error');
        }
      } catch(e) { showBanner('faces', '删除失败: ' + e.message, 'error'); }
    }
  });
}

function onRenameFace(id) {
  showAppModal({
    title: '为这张脸命名',
    input: true,
    okText: '保存',
    onOk: function(name) {
      name = (name || '').trim();
      try {
        if (H.RenameFace(id, name)) {
          showBanner('faces', '已重命名。', 'ok');
          loadFaces();
        } else {
          showBanner('faces', '重命名失败，请重试。', 'error');
        }
      } catch(e) { showBanner('faces', '重命名失败: ' + e.message, 'error'); }
    }
  });
}

function onClearAllFaces() {
  showAppModal({
    title: '清空全部人脸？',
    message: '清空后将无法使用人脸登录。',
    okText: '清空',
    onOk: function() {
      try {
        if (H.ClearAllFaces()) {
          showBanner('faces', '已清空全部人脸。', 'ok');
          loadFaces();
        } else {
          showBanner('faces', '清空失败，请重试。', 'error');
        }
      } catch(e) { showBanner('faces', '清空失败: ' + e.message, 'error'); }
    }
  });
}

// ================================================================
// Stale account-type refresh (1.4.0)
// Detect a record whose identity no longer matches the current account type
// after an MSA↔local conversion (Windows keeps the same SID, so the record is
// matched but carries stale identity/password):
//   state 1 = MSA→local: record UPN is an old Microsoft email.
//   state 2 = local→MSA: record UPN empty/different, current session is MSA.
// Offer to refresh: preserve faces, rewrite the UPN to the current account
// type, re-capture the current password. Detection re-evaluated on every
// faces-screen entry and at startup, so dismissing just hides the prompt.
// ================================================================

function checkAccountRefresh() {
  var s = 0, faces = 0, upn = '';
  try {
    var j = JSON.parse(H.CheckAccountTypeChanged());
    s = j.state;
    faces = j.faces || 0;
    upn = j.upn || '';
  } catch(e) { return; }
  if (s === 1 || s === 2) showRefreshPrompt(s, faces, upn);
}

function showRefreshPrompt(state, faces, upn) {
  refreshState = state;
  var overlay = document.getElementById('refresh-overlay');
  overlay.dataset.refreshState = String(state);  // snapshot at open time
  var info = document.getElementById('refresh-info');
  var title = document.getElementById('refresh-title');
  var dismissBtn = document.getElementById('refresh-dismiss');
  var facesText = (faces > 0) ? '你已录入 ' + faces + ' 张人脸，' : '';
  if (state === 2) {
    // local→MSA: record is local-era, now signed in with a Microsoft account.
    title.textContent = '账号身份已变更';
    info.textContent = facesText
      + '检测到该账号现正使用微软账号（MSA）登录。'
      + '已保存的人脸来自切换前的本地账号，为使人脸登录继续生效，'
      + '需要刷新已保存的账号信息（人脸数据将保留），并更新微软账号密码。'
      + (upn ? '将绑定微软账号：' + upn + '。' : '')
      + '请输入当前微软账号密码。';
    dismissBtn.textContent = '暂不处理';
  } else {
    // MSA→local: record still carries the old Microsoft email.
    title.textContent = '账号身份已变更';
    info.textContent = facesText
      + '检测到该账号曾使用微软账号（MSA）登录，现已切换为本地账号。'
      + '为使人脸登录继续生效，需要刷新已保存的账号信息（人脸数据将保留），'
      + '并更新本地密码。请输入当前 Windows 登录密码。'
      + '也可点击下方「仅清理账号邮箱」立即清除残留邮箱、恢复锁屏登录'
      + '（保留人脸与密码，但不更新已保存的密码）。';
    dismissBtn.textContent = '仅清理账号邮箱';
  }
  document.getElementById('refresh-pass').value = '';
  overlay.classList.add('active');
  setTimeout(function() { document.getElementById('refresh-pass').focus(); }, 50);
}

function onRefreshConfirm() {
  var pass = document.getElementById('refresh-pass').value;
  var info = document.getElementById('refresh-info');
  if (!pass) { info.textContent = '请输入当前登录密码。'; return; }

  var btn = document.getElementById('refresh-confirm');
  var spinner = document.getElementById('refresh-spinner');
  var dismiss = document.getElementById('refresh-dismiss');
  btn.disabled = true; dismiss.disabled = true; spinner.style.display = 'inline-block';

  // Defer the synchronous COM call so the spinner paints (same pattern as
  // onAppendConfirm).
  setTimeout(function() {
    var ok = false;
    try { ok = H.RefreshAccountIdentity(pass); } catch(e) { ok = false; }
    btn.disabled = false; dismiss.disabled = false; spinner.style.display = 'none';
    document.getElementById('refresh-pass').value = '';
    if (ok) {
      document.getElementById('refresh-overlay').classList.remove('active');
      showBanner('faces', '账号信息已刷新，人脸已保留。', 'ok', 6000);
      loadFaces();  // faces-user now shows the current account name
    } else {
      info.textContent =
        '密码验证失败或刷新失败。请确认输入的是当前登录密码（微软账号或本地账号密码），再重试。';
      setTimeout(function() { document.getElementById('refresh-pass').focus(); }, 50);
    }
  }, 50);
}

function onRefreshDismiss() {
  var btn = document.getElementById('refresh-dismiss');
  btn.disabled = true;
  // Defer the synchronous COM call so the disabled state paints first.
  setTimeout(function() {
    btn.disabled = false;
    document.getElementById('refresh-overlay').classList.remove('active');
    if (refreshState !== 1) {
      // state 2 (local→MSA) needs the refresh to write the current email and
      // re-encrypt the password — there is nothing safe to auto-clean.
      showBanner('faces', '已暂不处理。下次进入“人脸”页仍会提示刷新账号信息。', 'ok', 6000);
      return;
    }
    // state 1 (MSA→local): clear the stale MSA email so the lock-screen stops
    // packing the misattributed identity — faces and the stored password are
    // preserved. The full "刷新" path remains the right choice if the Windows
    // password changed since enrollment.
    var ok = false;
    try { ok = H.ClearStaleAccountUpn(); } catch(e) { ok = false; }
    if (ok) {
      showBanner('faces', '已清理残留的账号邮箱，人脸已保留，锁屏登录将改用本地账号身份。若密码已更改，请改用「刷新并保留人脸」。', 'ok', 6000);
      loadFaces();
    } else {
      showBanner('faces', '清理未完成。请点击「刷新并保留人脸」验证密码，或删除该录入后重新录入。', 'error', 6000);
    }
  }, 50);
}

// Show the append-face confirmation screen (current account already has faces).
// When isReplace is true (multi-angle re-enrollment), old faces are cleared and
// the three new angle faces replace them. Otherwise it is a single-angle append.
function showAppendScreen(faceCount, isReplace) {
  document.getElementById('screen-cam').classList.remove('active');
  document.getElementById('screen-append').classList.add('active');
  try { document.getElementById('append-user').value = H.GetUsername(); } catch(e) {}
  try {
    var appendSid = H.GetUserSid();
    document.getElementById('append-sid').textContent = appendSid;
    document.getElementById('append-sid-note').style.display = appendSid ? '' : 'none';
  } catch(e) {}
  try { document.getElementById('append-face-label').value = ''; } catch(e) {}

  if (isReplace) {
    document.getElementById('append-title').textContent = '替换全部人脸';
    document.getElementById('append-label-field').style.display = 'none';
    document.getElementById('append-info').textContent =
      '多角度录入将替换当前账号的 ' + faceCount + ' 张旧脸（正面 / 左转 / 右转各一张）。旧脸数据将被清除。';
    document.getElementById('btn-append-text').textContent = '确认替换';
    showBanner('append', '多角度重录入：旧脸将被替换，账号密码不变。', 'ok');
  } else {
    document.getElementById('append-title').textContent = '添加新人脸';
    document.getElementById('append-label-field').style.display = '';
    document.getElementById('append-info').textContent =
      '当前账号已有 ' + faceCount + ' 张脸，将补录「' + ANGLE_LABELS[captureFlow.firstAngle()] + '」角度。追加无需再次输入密码。';
    document.getElementById('btn-append-text').textContent = '确认追加';
  }
}

function onAppendConfirm() {
  var btn = document.getElementById('btn-append');
  var spinner = document.getElementById('append-spinner');
  var cancelBtn = document.getElementById('btn-append-cancel');
  btn.disabled = true;
  cancelBtn.disabled = true;
  spinner.style.display = 'inline-block';
  var label = '';
  try { label = document.getElementById('append-face-label').value.trim(); } catch(e) {}

  // Defer the synchronous COM call so the spinner paints first.
  setTimeout(function() {
    var ok = false;
    try { ok = H.SaveEnrollmentAppend(label); } catch(e) { ok = false; }
    btn.disabled = false;
    cancelBtn.disabled = false;
    spinner.style.display = 'none';

    if (ok) {
      document.getElementById('screen-append').classList.remove('active');
      document.getElementById('screen-cam').classList.add('active');
      currentTab = 'cam';
      setCaption('准备就绪');
      var okMsg = '新脸已添加成功！可按 Win+L 锁定屏幕测试人脸登录。';
      var hint = enrollmentHint();
      if (hint) okMsg += ' ' + hint;
      showBanner('cam', okMsg, 'ok', hint ? 12000 : undefined);
      try {
        if (H.StartPreview()) { viewing = true; renderFrame(); }
        else showBanner('cam', '预览启动失败：请检查摄像头占用、权限、驱动和模型文件。', 'error', 10000);
      } catch(e) { showBanner('cam', '预览启动失败: ' + e.message, 'error', 10000); }
    } else {
      showBanner('append', '追加失败（已达人脸上限或身份校验失败）。', 'error');
    }
  }, 50);
}

function onAppendCancel() {
  document.getElementById('screen-append').classList.remove('active');
  document.getElementById('screen-cam').classList.add('active');
  currentTab = 'cam';
  // The captured samples were discarded by this cancel — never show the old
  //「已采 N 帧」total again.
  setCaption('已采 0 帧');
  try {
    if (H.StartPreview()) { viewing = true; renderFrame(); }
    else showBanner('cam', '预览启动失败：请检查摄像头占用、权限、驱动和模型文件。', 'error', 10000);
  } catch(e) { showBanner('cam', '预览启动失败: ' + e.message, 'error', 10000); }
}

// Per-screen hide timers: re-showing a banner on the SAME screen must clear
// that screen's previous timer, or the old deadline fires early and hides
// the new banner (a 4s message died at ~2.9s in practice). Timers stay
// per-screen so a banner on another screen is never cut short.
var bannerTimers = {};
function showBanner(screen, text, type, durationMs) {
  var el = document.getElementById(screen + '-banner');
  el.className = 'banner ' + type;
  el.textContent = text;
  // Optional duration override; default 4s. Guidance messages (e.g. the
  // lock-screen password-refresh hint) need longer to be readable.
  var ms = (typeof durationMs === 'number') ? durationMs : 4000;
  if (bannerTimers[screen]) clearTimeout(bannerTimers[screen]);
  bannerTimers[screen] = setTimeout(function() {
    el.className = 'banner';
    bannerTimers[screen] = 0;
  }, ms);
}

// Cancel from the password screen: discard the just-captured samples and
// return to the camera screen, restarting the live preview. Mirrors the
// cleanup done in onSave's success path.
function onPwdCancel() {
  document.getElementById('screen-pwd').classList.remove('active');
  document.getElementById('screen-cam').classList.add('active');
  currentTab = 'cam';
  document.getElementById('fld-pass').value = '';
  setCaption('已采 0 帧');
  try {
    if (H.StartPreview()) { viewing = true; renderFrame(); }
    else showBanner('cam', '预览启动失败：请检查摄像头占用、权限、驱动和模型文件。', 'error', 10000);
  } catch(e) { showBanner('cam', '预览启动失败: ' + e.message, 'error', 10000); }
}

// Soft enrollment-quality hint from the backend (empty when nothing to say).
// Currently set when the saved angle templates sit closer than the measured
// cross-angle minimum — suggests a bigger turn on re-enrollment.
function enrollmentHint() {
  try { return H.GetEnrollmentHint() || ''; } catch(e) { return ''; }
}

function onSave() {
  var pass = document.getElementById('fld-pass').value;
  if (!pass) { showBanner('pwd', '请输入密码。', 'error'); return; }

  var btn = document.getElementById('btn-save');
  var spinner = document.getElementById('save-spinner');
  btn.disabled = true;
  spinner.style.display = 'inline-block';

  // Defer the synchronous COM call to let the browser paint the spinner first
  setTimeout(function() {
    var ok = false;
    var label = '';
    try { label = document.getElementById('fld-face-label').value.trim(); } catch(e) {}
    try { ok = H.ValidatePassword(pass); } catch(e) {}
    if (ok) {
      try { ok = H.SaveEnrollment(pass, label); } catch(e) { ok = false; }
    }

    btn.disabled = false;
    spinner.style.display = 'none';

    if (ok) {
      // Return to the camera (main) screen immediately
      document.getElementById('screen-pwd').classList.remove('active');
      document.getElementById('screen-cam').classList.add('active');
      currentTab = 'cam';
      document.getElementById('fld-pass').value = '';
      setCaption('准备就绪');
      var okMsg = '录入成功！可按 Win+L 锁定屏幕测试人脸登录。';
      var hint = enrollmentHint();
      if (hint) okMsg += ' ' + hint;
      showBanner('cam', okMsg, 'ok', hint ? 12000 : undefined);
      try {
        if (H.StartPreview()) { viewing = true; renderFrame(); }
        else showBanner('cam', '预览启动失败：请检查摄像头占用、权限、驱动和模型文件。', 'error', 10000);
      } catch(e) { showBanner('cam', '预览启动失败: ' + e.message, 'error', 10000); }
    } else {
      // MSA password validation only checks the locally cached credential.
      // If the user changed their Microsoft password elsewhere, the cache is
      // stale and the CURRENT (correct) password fails here. Guide them to
      // unlock once with the new password on the lock screen, which refreshes
      // the cache. Local accounts validate reliably offline.
      var acctType = '';
      try { acctType = document.getElementById('fld-acct-type').value; } catch(e) {}
      if (acctType === 'msa') {
        showBanner('pwd',
          '密码验证失败。若你最近更改过微软账号密码，请先按 Win+L 锁屏，用新密码登录一次，再回来录入。',
          'error', 15000);
      } else {
        showBanner('pwd', '密码错误，请确认是你的 Windows 登录密码。', 'error');
      }
    }
  }, 50);
}

// Learning view throttles to one pipe query per 10s: the public pipe is
// single-instance, so the 2s log tick must not become a 2s poll on it.
var learningLastFetch = 0;
var lastLogSource = '';

// Console-friendly labels for the learner's skip-reason keys (server-side
// keys are stable ASCII; unknown keys fall through verbatim).
var LEARN_REASONS = {
  distance: '距离门', clarity: '落点明确性', cone: '姿态锥',
  norm: '范数地板', interval: '节奏间隔', disabled: '学习已关闭',
  vanished: '模板已移除', error: '其他错误'
};

function renderLearningStatus(force) {
  var now = Date.now();
  if (!force && now - learningLastFetch < 10000) return;
  learningLastFetch = now;
  var viewer = document.getElementById('log-viewer');
  var html;
  try {
    var st = JSON.parse(H.GetLearningStatus());
    if (st.error) {
      html = '<div class="log-warn">学习状态不可用: ' + escapeHtml(st.error) +
             '（需后台服务运行）</div>';
    } else {
      html = '<div class="log-info">[渐进式学习] ' + (st.enabled ? '已开启' : '已关闭') +
             ' · 距离门上限 ' + Number(st.gate_cap).toFixed(2) +
             ' · α=' + Number(st.alpha).toFixed(2) +
             ' · 间隔 ' + st.interval + 's · 每10秒自动刷新</div>';
      html += '<div class="log-debug">[每脸统计]</div>';
      if (!st.faces || !st.faces.length) {
        html += '<div class="log-debug">暂无学习记录（认证成功后此处出现统计）</div>';
      } else {
        for (var i = 0; i < st.faces.length; i++) {
          var f = st.faces[i];
          var p20 = (f.era_p20 >= 0)
            ? '纪元p20 ' + Number(f.era_p20).toFixed(3) + '（' + f.era_rounds + '轮，按槽独立）'
            : '纪元样本不足（' + f.era_rounds + '/5轮，门暂用账号池化/上限）';
          html += '<div class="log-info">' + escapeHtml(f.user || '?') + ' 脸' + f.face +
                  '(' + escapeHtml(f.label || '?') + ') · 累计 ' + f.total + ' 次 · 今日 ' + f.today +
                  ' · 最近接受 ' + (f.last || '从未') + ' · ' + p20 + '</div>';
        }
      }
      html += '<div class="log-debug">[最近事件]（新→旧，最多24条）</div>';
      if (!st.recent || !st.recent.length) {
        html += '<div class="log-debug">暂无事件</div>';
      } else {
        for (var j = 0; j < st.recent.length; j++) {
          var ev = st.recent[j];
          var who = escapeHtml(ev.user || '?') + '/' + escapeHtml(ev.label || '?');
          if (ev.ok) {
            html += '<div class="log-info">' + ev.t + '  [OK]   已更新  d=' +
                    Number(ev.d).toFixed(3) + ' α=' + Number(ev.alpha).toFixed(3) +
                    '  ' + who + '</div>';
          } else {
            var reason = LEARN_REASONS[ev.reason] || ev.reason;
            var detail = (ev.reason === 'distance')
              ? 'd=' + Number(ev.d).toFixed(3) + ' > 门' + Number(ev.gate).toFixed(3)
              : 'd=' + Number(ev.d).toFixed(3);
            html += '<div class="log-warn">' + ev.t + '  [跳过] ' + escapeHtml(reason) +
                    '  ' + detail + '  ' + who + '</div>';
          }
        }
      }
    }
  } catch(e) {
    html = '<div class="log-error">读取学习状态失败: ' + escapeHtml(e.message) + '</div>';
  }
  viewer.innerHTML = html;
}

function refreshLogs() {
  try {
    var viewer = document.getElementById('log-viewer');
    // The 2s auto-refresh repaints innerHTML, which destroys any in-progress
    // text selection — that made it impossible to select a log line to copy.
    // Skip this tick while a selection inside the viewer is active; refreshing
    // resumes on the next tick after the user clicks away.
    var selection = window.getSelection();
    if (selection && !selection.isCollapsed && viewer.contains(selection.anchorNode)) return;
    var src = document.getElementById('log-source').value;
    var force = (src !== lastLogSource);
    lastLogSource = src;
    if (src === 'learning') {
      // No file to clear behind this view — hide the button for it.
      document.getElementById('btn-clear-log').style.display = 'none';
      renderLearningStatus(force);
      return;
    }
    document.getElementById('btn-clear-log').style.display = '';
    var json;
    if (src === 'service') {
      json = H.GetServiceLogLines();
    } else if (src === 'worker') {
      json = H.GetAuthWorkerLogLines();
    } else if (src === 'cp') {
      json = H.GetCredentialProviderLogLines();
    } else {
      json = H.GetLogLines();
    }
    if (!json || json.length < 2 || json[0] !== '[') {
      document.getElementById('log-viewer').innerHTML = '<div class="log-warn">No log data</div>';
      return;
    }
    var lines = JSON.parse(json);
    var html = '';
    for (var i = 0; i < lines.length; i++) {
      var cls = 'log-info';
      if (lines[i].indexOf('[DEBUG]') >= 0) cls = 'log-debug';
      else if (lines[i].indexOf('[WARN]') >= 0) cls = 'log-warn';
      else if (lines[i].indexOf('[ERROR]') >= 0) cls = 'log-error';
      html += '<div class="' + cls + '">' + escapeHtml(lines[i]) + '</div>';
    }
    var atBottom = (viewer.scrollHeight - viewer.scrollTop - viewer.clientHeight) < 60;
    viewer.innerHTML = html;
    if (atBottom) viewer.scrollTop = viewer.scrollHeight;
  } catch(e) {
    document.getElementById('log-viewer').innerHTML = '<div class="log-error">Error loading logs: ' + e.message + '</div>';
  }
}

function escapeHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

// 四个来源行为一致:清空 = 截断当日日志文件(console 同时清内存环形缓冲)。
// writer 以追加模式(FILE_APPEND_DATA)打开文件,截断安全,下一条日志自动落在新末尾;
// 按天轮转的历史文件不受影响。
function clearLogs() {
  var src = document.getElementById('log-source').value;
  if (src === 'learning') return;  // live view, nothing to clear
  try {
    if (src === 'console') H.ClearLog();
    else H.ClearFileLog(src);
  } catch(e) { return; }
  document.getElementById('log-viewer').textContent = '';
  var btn = document.getElementById('btn-clear-log');
  btn.textContent = '已清空';
  setTimeout(function() { btn.textContent = '清空日志'; }, 1500);
}

// Copy exactly what the viewer currently shows (the selected source). The app
// is loaded via NavigateToString (about:blank origin), where the async
// Clipboard API may be unavailable — fall back to execCommand.
function copyLogs() {
  var text = document.getElementById('log-viewer').innerText;
  if (!text) return;
  var btn = document.getElementById('btn-copy-log');
  var done = function() {
    btn.textContent = '已复制';
    setTimeout(function() { btn.textContent = '复制'; }, 1500);
  };
  var fallback = function() {
    var ta = document.createElement('textarea');
    ta.value = text;
    ta.style.position = 'fixed';
    ta.style.opacity = '0';
    document.body.appendChild(ta);
    ta.select();
    try { if (document.execCommand('copy')) done(); } catch(e) {}
    document.body.removeChild(ta);
  };
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).then(done, fallback);
  } else {
    fallback();
  }
}

// Enter submits the form the field belongs to. All screens share this
// document (hidden ones only via CSS), but a hidden field can never take
// focus, so one binding per field is enough. Ignore Enter while the target
// button is disabled (a save is already in flight) and during IME composition
// (Chinese face-label input commits with Enter).
function bindEnterKey(fieldId, btnId, submit) {
  document.getElementById(fieldId).addEventListener('keydown', function(e) {
    if (e.key !== 'Enter' || e.isComposing) return;
    var btn = document.getElementById(btnId);
    if (btn && btn.disabled) return;
    submit();
  });
}
bindEnterKey('fld-pass', 'btn-save', onSave);
bindEnterKey('fld-face-label', 'btn-save', onSave);
bindEnterKey('append-face-label', 'btn-append', onAppendConfirm);
bindEnterKey('refresh-pass', 'refresh-confirm', onRefreshConfirm);

// ==================================================================
// First-run camera picker: if config has no camera_device (fresh install
// or the user never touched settings) AND more than one camera is present,
// prompt the user to pick one. Saves via SetConfig so subsequent launches
// and the service use the chosen device.
// ==================================================================
function showCamPicker() {
  var sel = document.getElementById('cam-picker-select');
  sel.innerHTML = '';
  var camList = [];
  try { camList = JSON.parse(H.GetCameraList()); } catch(e) {}
  if (camList.length < 2) return;  // nothing to choose from / no multi-cam
  for (var i = 0; i < camList.length; i++) {
    var opt = document.createElement('option');
    opt.value = camList[i].path;
    opt.textContent = camList[i].name || ('摄像头 ' + (i + 1));
    sel.appendChild(opt);
  }
  document.getElementById('cam-picker-overlay').classList.add('active');
}

function onCamPickerConfirm() {
  var sel = document.getElementById('cam-picker-select');
  var path = sel.value;
  if (!path) return;
  try {
    var cfg = JSON.parse(H.GetConfig());
    cfg.camera_device = path;
    if (H.SetConfig(JSON.stringify(cfg))) {
      document.getElementById('cam-picker-overlay').classList.remove('active');
      showBanner('cam', '已选择摄像头，正在启动预览...', 'ok');
      // Restart preview with the newly chosen camera.
      try { H.StopPreview(); } catch(e) {}
      setTimeout(function() {
        try {
          if (H.StartPreview()) renderFrame();
          else showBanner('cam', '预览启动失败：请检查摄像头占用、权限、驱动和模型文件。', 'error', 10000);
        } catch(e) { showBanner('cam', '预览启动失败: ' + e.message, 'error', 10000); }
      }, 100);
    } else {
      showBanner('cam', '保存摄像头设置失败，请重试。', 'error');
    }
  } catch(e) {
    showBanner('cam', '选择摄像头失败: ' + e.message, 'error');
  }
}

function onCamPickerSkip() {
  document.getElementById('cam-picker-overlay').classList.remove('active');
  showBanner('cam', '已使用默认摄像头，可在设置页随时更改。', 'ok');
}

(function init() {
  try { document.getElementById('fld-user').value = H.GetUsername(); } catch(e) {}
  setTimeout(function() {
    try {
      if (H.StartPreview()) renderFrame();
      else showBanner('cam', '预览启动失败：请检查摄像头占用、权限、驱动和模型文件。', 'error', 10000);
    } catch(e) { showBanner('cam', '预览启动失败: ' + e.message, 'error', 10000); }
  }, 100);

  // If the account was converted from MSA→local with a stale record, surface
  // the refresh prompt at startup (before the camera preview takes over).
  setTimeout(function() { checkAccountRefresh(); }, 150);

  // First-run camera picker: if config has no camera_device AND multiple
  // cameras exist, prompt the user to choose one. Fresh installs (or users
  // who never touched settings) may not know the camera dropdown exists.
  setTimeout(function() {
    try {
      var cfg = JSON.parse(H.GetConfig());
      if (!cfg.camera_device) showCamPicker();
    } catch(e) {}
  }, 300);

  setInterval(function() {
    if (!viewing) return;
    if (captureFlow.isActive()) return;  // sequence in flight — the capture poll owns the tag
    try {
      var st = JSON.parse(H.GetCaptureStatus());
      // Only after the whole sequence has ended do we show the running total.
      // A cancelled capture (st.cancelled) leaves a partial total behind —
      // don't resurrect it after the user asked to reset to zero. Same for
      // an ended capture flow: the total is stale once the
      // confirm/cancel/save screen was left.
      if (st.done && !st.livenessChecking && !st.cancelled && captureFlow.canShowIdleTotal()) {
        setCaption('已采 ' + st.total + ' 帧');
      }
    } catch(e) {}
  }, 300);

  // Auto-refresh log viewer every 2 seconds when logs tab is active
  setInterval(function() {
    if (currentTab === 'logs') refreshLogs();
  }, 2000);
})();
