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

function escapeHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
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
