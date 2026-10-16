bindEnterKey('fld-pass', 'btn-save', onSave);
bindEnterKey('fld-face-label', 'btn-save', onSave);
bindEnterKey('append-face-label', 'btn-append', onAppendConfirm);
bindEnterKey('refresh-pass', 'refresh-confirm', onRefreshConfirm);

(function init() {
  try { document.getElementById('fld-user').value = H.GetUsername(); } catch(e) {}
  setTimeout(previewCtl.restartPreview, 100);

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
    if (!previewCtl.isViewing()) return;
    if (captureCtl.isActive()) return;  // sequence in flight — the capture poll owns the tag
    try {
      var st = JSON.parse(H.GetCaptureStatus());
      // Only after the whole sequence has ended do we show the running total.
      // A cancelled capture (st.cancelled) leaves a partial total behind —
      // don't resurrect it after the user asked to reset to zero. Same for
      // an ended capture flow: the total is stale once the
      // confirm/cancel/save screen was left.
      if (st.done && !st.livenessChecking && !st.cancelled && captureCtl.canShowIdleTotal()) {
        setCaption('已采 ' + st.total + ' 帧');
      }
    } catch(e) {}
  }, 300);

  // Auto-refresh log viewer every 2 seconds when logs tab is active
  setInterval(function() {
    if (navCtl.tab() === 'logs') refreshLogs();
  }, 2000);
})();
