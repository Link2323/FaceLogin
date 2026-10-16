// The capture state machine and the angle tables are private to this module;
// other files see only the readers below (switchTab/app_main gate on
// isActive/canShowIdleTotal, enroll_save shows the appended angle's name).
var captureCtl = (function() {
  var captureFlow = createCaptureFlow(window);
  var ANGLE_LABELS = ['正面', '左转', '右转'];
  var ANGLE_INSTR = {
    0: '请正对摄像头，保持不动',
    1: '请将头转向左侧约 30°',
    2: '请将头转向右侧约 30°'
  };
  return {
    isActive: function() { return captureFlow.isActive(); },
    canShowIdleTotal: function() { return captureFlow.canShowIdleTotal(); },
    isMultiAngle: function() { return captureFlow.isMultiAngle(); },
    firstAngle: function() { return captureFlow.firstAngle(); },
    angleLabel: function(i) { return ANGLE_LABELS[i]; },
    instr: function(i) { return ANGLE_INSTR[i]; },
    begin: function(multi, firstAngle) { captureFlow.begin(multi, firstAngle); },
    end: function() { captureFlow.end(); },
    stopPolling: function() { captureFlow.stopPolling(); },
    poll: function(fn) { captureFlow.poll(fn); },
    scheduleNext: function(fn) { captureFlow.scheduleNext(fn); },
    // Scan the enrolled labels for the first angle slot not yet taken.
    firstMissingAngle: function(have) {
      for (var a = 0; a < ANGLE_LABELS.length; a++) {
        if (!have[ANGLE_LABELS[a]]) return a;
      }
      return 0;
    }
  };
})();

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
  captureCtl.end();
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
  captureCtl.end();
  try { H.CancelCapture(); } catch(e) {}
  resetCaptureUi();
  hideCaptureProgress();
  setCaption('已采 0 帧');
  showBanner('cam', '已取消采集。', 'ok');
}

function onCapture() {
  // While a capture runs, the primary button doubles as its cancel control.
  if (captureCtl.isActive()) { onCancelCapture(); return; }

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
    captureCtl.begin(multi, firstAngle);
    captureAngleSequence(firstAngle, angles);
  } catch(e) {
    resetCaptureUi();
    hideCaptureProgress();
    showBanner('cam', '开始采集失败: ' + e.message, 'error');
  }
}

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
  return captureCtl.firstMissingAngle(have);
}

// Drive one capture angle at a time: call CaptureFaceSamples(angle), poll
// GetCaptureStatus, show the instruction + live progress, then move on.
function captureAngleSequence(idx, total) {
  if (!captureCtl.isActive()) return;

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
             + (captureCtl.instr(idx) || '采集人脸样本...'));

  // Watchdog: if the server reports no activity for 25s, the capture thread
  // is stuck (e.g. camera stall) — fail visibly instead of hanging forever.
  var pollStart = Date.now();
  var gotProgress = false;

  captureCtl.stopPolling();
  captureCtl.poll(function() {
    var st;
    try { st = JSON.parse(H.GetCaptureStatus()); } catch(e) { return; }

    // A cancel landed server-side while a poll was still in flight — the
    // cancel path has already reset the UI; just stand down.
    if (st.cancelled) {
      captureCtl.stopPolling();
      resetCaptureUi();
      hideCaptureProgress();
      return;
    }

    if (st.collected > 0) gotProgress = true;
    if (!gotProgress && !st.done && st.collected === 0 && Date.now() - pollStart > 25000) {
      captureCtl.stopPolling();
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
      captureCtl.stopPolling();
      resetCaptureUi();
      hideCaptureProgress();
      setCaption('采集失败');
      showBanner('cam', '活体检测失败，请确保是真人并重试。', 'error');
      return;
    }

    // Still collecting this angle — show instruction + progress.
    if (st.collected < st.target) {
      setCaption((total > 1 ? '角度 ' + (idx + 1) + '/' + total + ' · ' : '')
                 + (captureCtl.instr(st.angle) || '采集中') + ' · '
                 + st.collected + '/' + st.target + ' 帧');
      var pct = Math.round(st.collected / st.target * 100);
      document.getElementById('cap-fill').style.width = pct + '%';
      return;
    }

    // This angle is complete — move on to the next one. Paint 100% AND a
    // matching caption: the step text used to keep saying「活体检测中」here,
    // which next to「角度 N · 完成」and a frozen 100% bar read as stuck.
    captureCtl.stopPolling();
    document.getElementById('cap-fill').style.width = '100%';
    setCaption(total > 1 ? '角度 ' + (idx + 1) + '/' + total + ' 完成'
                         : '「' + captureCtl.angleLabel(idx) + '」采集完成');
    if (idx + 1 < total) {
      captureCtl.scheduleNext(function() { captureAngleSequence(idx + 1, total); });
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

  previewCtl.stopLoop();
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
    showAppendScreen(faceCount, captureCtl.isMultiAngle());
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
