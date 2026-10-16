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
// deps: {host, showBanner, withBusy}. The faces-list reload after a refresh
// or cleanup is NOT a dep — faces depends on this module for the entry check,
// so the reload is injected the other way via onChanged (registered in the
// composition root).
// ================================================================
function createAccountRefreshModule(deps) {
  var H = deps.host;
  var showBanner = deps.showBanner;
  var withBusy = deps.withBusy;

  // Account-type-change state shown in the refresh modal (0/1/2).
  var refreshState = 0;
  var reloadListeners = [];

  function notifyReload() {
    for (var i = 0; i < reloadListeners.length; i++) reloadListeners[i]();
  }

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

    withBusy([document.getElementById('refresh-confirm'),
              document.getElementById('refresh-dismiss')],
             document.getElementById('refresh-spinner'),
    function() {
      var ok = false;
      try { ok = H.RefreshAccountIdentity(pass); } catch(e) { ok = false; }
      document.getElementById('refresh-pass').value = '';
      if (ok) {
        document.getElementById('refresh-overlay').classList.remove('active');
        showBanner('faces', '账号信息已刷新，人脸已保留。', 'ok', 6000);
        notifyReload();  // faces-user now shows the current account name
      } else {
        info.textContent =
          '密码验证失败或刷新失败。请确认输入的是当前登录密码（微软账号或本地账号密码），再重试。';
        setTimeout(function() { document.getElementById('refresh-pass').focus(); }, 50);
      }
    });
  }

  function onRefreshDismiss() {
    withBusy([document.getElementById('refresh-dismiss')], null, function() {
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
        notifyReload();
      } else {
        showBanner('faces', '清理未完成。请点击「刷新并保留人脸」验证密码，或删除该录入后重新录入。', 'error', 6000);
      }
    });
  }

  return {
    check: checkAccountRefresh,
    showRefreshPrompt: showRefreshPrompt,
    onRefreshConfirm: onRefreshConfirm,
    onRefreshDismiss: onRefreshDismiss,
    onChanged: function(fn) { reloadListeners.push(fn); }
  };
}
