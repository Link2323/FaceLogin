// Shared UI primitives: license viewer, the in-app modal engine, banners,
// the cam-screen caption, Enter-key binding, HTML escaping, and the withBusy
// deferral every COM-writing button goes through. deps: {host} — nothing
// else crosses this module's boundary.
function createUiCommon(deps) {
  var H = deps.host;

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

  // Single status line for the cam screen: an overlay pill on the video. All
  // capture states (guidance, angle/frame counts, liveness, completion) and
  // the idle hint render here — nothing else paints status text on this screen.
  function setCaption(text) {
    document.getElementById('cam-caption').textContent = text;
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

  // Lock button(s) — and show the spinner, if any — then run work after a
  // short deferral: synchronous host-object COM calls block the UI thread,
  // and the 50ms gap lets the locked controls paint first so the click
  // visibly registers before the freeze (SetConfig can block ~25s while the
  // auth worker is replaced). Restoration lives in finally so no outcome —
  // early return or a thrown host error — can leave the page stuck busy.
  // This replaces the per-call-site disable/spinner/defer sequence every
  // COM-writing button used to repeat.
  function withBusy(buttons, spinner, work) {
    for (var i = 0; i < buttons.length; i++) buttons[i].disabled = true;
    if (spinner) spinner.style.display = 'inline-block';
    setTimeout(function() {
      try { work(); }
      finally {
        for (var j = 0; j < buttons.length; j++) buttons[j].disabled = false;
        if (spinner) spinner.style.display = 'none';
      }
    }, 50);
  }

  return {
    showLicenseDocument: showLicenseDocument,
    openLicenseViewer: openLicenseViewer,
    closeLicenseViewer: closeLicenseViewer,
    showAppModal: showAppModal,
    hideAppModal: hideAppModal,
    onModalOk: onModalOk,
    onModalCancel: onModalCancel,
    onModalMid: onModalMid,
    showBanner: showBanner,
    escapeHtml: escapeHtml,
    setCaption: setCaption,
    bindEnterKey: bindEnterKey,
    withBusy: withBusy
  };
}
