// ================================================================
// Multi-face management (1.3.0). deps: {host, showBanner, showAppModal,
// escapeHtml, switchTab, checkAccountRefresh} — the account refresh check
// runs on every load; the reverse direction (reload the list after a
// refresh) is injected the other way via accountRefresh.onChanged in the
// composition root, avoiding a construction cycle.
// ================================================================
function createFacesModule(deps) {
  var H = deps.host;
  var showBanner = deps.showBanner, showAppModal = deps.showAppModal, escapeHtml = deps.escapeHtml;
  var switchTab = deps.switchTab;
  var checkAccountRefresh = deps.checkAccountRefresh;

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

  return {
    load: loadFaces,
    goAddFace: goAddFace,
    onDeleteFace: onDeleteFace,
    onRenameFace: onRenameFace,
    onClearAllFaces: onClearAllFaces
  };
}
