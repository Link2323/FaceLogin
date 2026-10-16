// Screen and tab navigation; owns the current-tab tracker. deps:
// {host, captureCtl, previewCtl, showBanner}. Tab-entry loaders (settings/
// faces/logs) are deliberately NOT deps — each of those modules needs
// switchTab, so depending on them here would be a cycle; they register their
// loaders through onEnter in the composition root instead.
function createNavigation(deps) {
  var H = deps.host;
  var captureCtl = deps.captureCtl;
  var previewCtl = deps.previewCtl;
  var showBanner = deps.showBanner;

  var currentTab = 'cam';
  var enterHooks = {};
  var SCREENS = {cam: 'screen-cam', pwd: 'screen-pwd', faces: 'screen-faces',
                 append: 'screen-append', settings: 'screen-settings', logs: 'screen-logs'};
  var TAB_NAMES = {cam: '录入', faces: '人脸', settings: '设置', logs: '日志'};

  // Unguarded direct swap for the flow screens (password/append and the
  // return to cam after save/cancel — a capture has always just ended there,
  // so the switchTab guards would only be noise). Tab screens re-anchor the
  // tracker; flow screens leave it alone — their tab bar still reads 录入 and
  // switchTab's capture/camera guards key off the cam tab. Replaces the
  // pairwise class-swap + tracker-set lines each flow used to repeat.
  function showScreen(name) {
    var all = document.querySelectorAll('.screen');
    for (var i = 0; i < all.length; i++) all[i].classList.remove('active');
    document.getElementById(SCREENS[name]).classList.add('active');
    if (TAB_NAMES[name]) currentTab = name;
  }

  function switchTab(name) {
    if (currentTab === name) return;

    // A capture in flight owns the DirectShow graph — pausing the preview to
    // idle the camera would abort it. Block the switch; cancel first instead.
    if (currentTab === 'cam' && captureCtl.isActive()) {
      showBanner('cam', '正在采集，请先取消采集再切换页面。', 'error');
      return;
    }

    // Camera lifecycle: preview (and its 33ms frame loop) only runs while the
    // cam screen is visible — switching away powers the camera off instead of
    // keeping it live behind screens that need no frames.
    if (currentTab === 'cam') {
      previewCtl.stopLoop();
      try { H.StopPreview(); } catch(e) {}
    }

    showScreen(name);

    var allTabs = document.querySelectorAll('.tabs span');
    for (var i = 0; i < allTabs.length; i++) {
      allTabs[i].classList.toggle('active', allTabs[i].textContent === TAB_NAMES[name]);
    }
    if (name === 'cam') {
      // Idempotent server-side (no-op if already running).
      try { if (H.StartPreview()) previewCtl.startLoop(); } catch(e) {}
    }
    if (enterHooks[name]) enterHooks[name]();
  }

  return {
    switchTab: switchTab,
    showScreen: showScreen,
    tab: function() { return currentTab; },
    onEnter: function(name, fn) { enterHooks[name] = fn; }
  };
}
