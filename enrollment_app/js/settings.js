// Settings screen + first-run camera picker. The last config loaded by
// loadSettings is kept whole for onSaveSettings — it is this module's only
// shared state. deps: {host, showBanner, previewCtl, withBusy}.
function createSettingsModule(deps) {
  var H = deps.host;
  var showBanner = deps.showBanner;
  var previewCtl = deps.previewCtl;
  var withBusy = deps.withBusy;

  var loadedConfig = null;

  function loadSettings() {
    try {
      var json = H.GetConfig();
      var cfg = JSON.parse(json);
      // Keep the full parsed config for onSaveSettings: the backend's SetConfig
      // parser starts from defaults, so saving only the UI-managed keys would
      // silently reset non-UI fields (learning gates, alpha, interval, ...).
      loadedConfig = cfg;
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
    // are strictly read-only (docs/work/in-progress/progressive-learning-v2.md red line 5).
    var learning = document.getElementById('cfg-learning').checked;
    // Round-trip the FULL config: UI-managed keys overridden, every other key
    // preserved from the last load (see loadSettings note on partial saves).
    var cfg = Object.assign({}, loadedConfig || {}, {
      match_threshold: parseFloat(document.getElementById('cfg-threshold').value),
      anti_spoof_threshold: parseFloat(document.getElementById('cfg-antispoof-threshold').value),
      camera_rotation: parseInt(document.getElementById('cfg-rotation').value) || 0,
      camera_device: document.getElementById('cfg-camera').value,
      ema_learning: learning,
      failure_learning: learning,
      fast_unlock: document.getElementById('cfg-fastunlock').checked,
      learning_distance_gate: parseFloat(document.getElementById('cfg-learn-gate').value)
    });
    // withBusy defers the synchronous COM call so the locked button and
    // spinner paint first: SetConfig replaces the auth worker and waits out
    // its handshake (up to ~25s), during which the UI thread is blocked
    // (repeated clicks can no longer stack SetConfig round trips).
    withBusy([document.getElementById('btn-save-settings')],
             document.getElementById('settings-spinner'),
    function() {
      try {
        var ok = H.SetConfig(JSON.stringify(cfg));
        if (ok) {
          showBanner('settings', '设置已保存并通知后台服务。', 'ok');
        } else {
          showBanner('settings', '设置已写入，但后台服务未确认安全重载；人脸认证将保持关闭，请检查服务和模型文件。', 'error', 10000);
        }
      } catch(e) { showBanner('settings', '保存设置失败: ' + e.message, 'error'); }
    });
  }

  // Fill the form with display-side defaults (mirrors the backend's
  // ConfigFromJson starting point). Nothing is written until the user clicks
  // 保存设置, which round-trips the loaded config for the non-UI keys.
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
    // withBusy locks the button first so the click visibly registers before
    // SetConfig blocks the UI thread (same shape as onSaveSettings).
    withBusy([document.getElementById('cam-picker-confirm')], null, function() {
      try {
        var cfg = JSON.parse(H.GetConfig());
        cfg.camera_device = path;
        if (H.SetConfig(JSON.stringify(cfg))) {
          document.getElementById('cam-picker-overlay').classList.remove('active');
          showBanner('cam', '已选择摄像头，正在启动预览...', 'ok');
          // Restart preview with the newly chosen camera.
          try { H.StopPreview(); } catch(e) {}
          setTimeout(previewCtl.restartPreview, 100);
        } else {
          showBanner('cam', '保存摄像头设置失败，请重试。', 'error');
        }
      } catch(e) {
        showBanner('cam', '选择摄像头失败: ' + e.message, 'error');
      }
    });
  }

  function onCamPickerSkip() {
    document.getElementById('cam-picker-overlay').classList.remove('active');
    showBanner('cam', '已使用默认摄像头，可在设置页随时更改。', 'ok');
  }

  return {
    load: loadSettings,
    save: onSaveSettings,
    resetDefaults: onResetDefaults,
    showCamPicker: showCamPicker,
    onCamPickerConfirm: onCamPickerConfirm,
    onCamPickerSkip: onCamPickerSkip
  };
}
