// Log viewer + the learning status view. deps: {host, escapeHtml}. Learning
// view throttles to one pipe query per 10s: the public pipe is
// single-instance, so the 2s log tick must not become a 2s poll on it. The
// last-source memory forces one immediate repaint when the dropdown switches
// views. Both are private to this module.
function createLogsModule(deps) {
  var H = deps.host;
  var escapeHtml = deps.escapeHtml;

  var learningLastFetch = 0;
  var lastLogSource = '';
  function learningFetchDue(now) { return now - learningLastFetch >= 10000; }
  function markLearningFetch(now) { learningLastFetch = now; }
  function logSourceChanged(src) {
    var changed = (src !== lastLogSource);
    lastLogSource = src;
    return changed;
  }

  // Console-friendly labels for the learner's skip-reason keys (server-side
  // keys are stable ASCII; unknown keys fall through verbatim).
  var LEARN_REASONS = {
    distance: '距离门', clarity: '落点明确性', cone: '姿态锥',
    norm: '范数地板', interval: '节奏间隔', disabled: '学习已关闭',
    vanished: '模板已移除', error: '其他错误'
  };

  function renderLearningStatus(force) {
    var now = Date.now();
    if (!force && !learningFetchDue(now)) return;
    markLearningFetch(now);
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
      var force = logSourceChanged(src);
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

  return {
    refresh: refreshLogs,
    clearLogs: clearLogs,
    copyLogs: copyLogs
  };
}
