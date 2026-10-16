// Post-capture flows: the append-face screen (account already has faces) and
// the first-enrollment password screen. deps: {host, showBanner, setCaption,
// withBusy, captureCtl, showScreen, previewCtl}.
function createEnrollSaveModule(deps) {
  var H = deps.host;
  var showBanner = deps.showBanner, setCaption = deps.setCaption;
  var withBusy = deps.withBusy;
  var captureCtl = deps.captureCtl;
  var showScreen = deps.showScreen;
  var previewCtl = deps.previewCtl;

  // Show the append-face confirmation screen (current account already has
  // faces). When isReplace is true (multi-angle re-enrollment), old faces are
  // cleared and the three new angle faces replace them. Otherwise it is a
  // single-angle append.
  function showAppendScreen(faceCount, isReplace) {
    showScreen('append');
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
        '当前账号已有 ' + faceCount + ' 张脸，将补录「' + captureCtl.angleLabel(captureCtl.firstAngle()) + '」角度。追加无需再次输入密码。';
      document.getElementById('btn-append-text').textContent = '确认追加';
    }
  }

  function onAppendConfirm() {
    var label = '';
    try { label = document.getElementById('append-face-label').value.trim(); } catch(e) {}
    withBusy([document.getElementById('btn-append'),
              document.getElementById('btn-append-cancel')],
             document.getElementById('append-spinner'),
    function() {
      var ok = false;
      try { ok = H.SaveEnrollmentAppend(label); } catch(e) { ok = false; }

      if (ok) {
        showScreen('cam');
        setCaption('准备就绪');
        var okMsg = '新脸已添加成功！可按 Win+L 锁定屏幕测试人脸登录。';
        var hint = enrollmentHint();
        if (hint) okMsg += ' ' + hint;
        showBanner('cam', okMsg, 'ok', hint ? 12000 : undefined);
        previewCtl.restartPreview();
      } else {
        showBanner('append', '追加失败（已达人脸上限或身份校验失败）。', 'error');
      }
    });
  }

  function onAppendCancel() {
    showScreen('cam');
    // The captured samples were discarded by this cancel — never show the old
    //「已采 N 帧」total again.
    setCaption('已采 0 帧');
    previewCtl.restartPreview();
  }

  // Cancel from the password screen: discard the just-captured samples and
  // return to the camera screen, restarting the live preview. Mirrors the
  // cleanup done in onSave's success path.
  function onPwdCancel() {
    showScreen('cam');
    document.getElementById('fld-pass').value = '';
    setCaption('已采 0 帧');
    previewCtl.restartPreview();
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

    withBusy([document.getElementById('btn-save')],
             document.getElementById('save-spinner'),
    function() {
      var ok = false;
      var label = '';
      try { label = document.getElementById('fld-face-label').value.trim(); } catch(e) {}
      try { ok = H.ValidatePassword(pass); } catch(e) {}
      if (ok) {
        try { ok = H.SaveEnrollment(pass, label); } catch(e) { ok = false; }
      }

      if (ok) {
        // Return to the camera (main) screen immediately
        showScreen('cam');
        document.getElementById('fld-pass').value = '';
        setCaption('准备就绪');
        var okMsg = '录入成功！可按 Win+L 锁定屏幕测试人脸登录。';
        var hint = enrollmentHint();
        if (hint) okMsg += ' ' + hint;
        showBanner('cam', okMsg, 'ok', hint ? 12000 : undefined);
        previewCtl.restartPreview();
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
    });
  }

  return {
    showAppendScreen: showAppendScreen,
    onAppendConfirm: onAppendConfirm,
    onAppendCancel: onAppendCancel,
    onPwdCancel: onPwdCancel,
    onSave: onSave
  };
}
