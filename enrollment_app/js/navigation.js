// currentTab is owned here: enroll_save returns to the cam screen outside
// switchTab (direct screen swaps), so it goes through navCtl.set.
var navCtl = (function() {
  var currentTab = 'cam';
  return {
    tab: function() { return currentTab; },
    set: function(name) { currentTab = name; }
  };
})();

function switchTab(name) {
  if (navCtl.tab() === name) return;

  // A capture in flight owns the DirectShow graph — pausing the preview to
  // idle the camera would abort it. Block the switch; cancel first instead.
  if (navCtl.tab() === 'cam' && captureCtl.isActive()) {
    showBanner('cam', '正在采集，请先取消采集再切换页面。', 'error');
    return;
  }

  // Camera lifecycle: preview (and its 33ms frame loop) only runs while the
  // cam screen is visible — switching away powers the camera off instead of
  // keeping it live behind screens that need no frames.
  if (navCtl.tab() === 'cam') {
    previewCtl.stopLoop();
    try { H.StopPreview(); } catch(e) {}
  }

  var screens = {cam: 'screen-cam', faces: 'screen-faces', settings: 'screen-settings', logs: 'screen-logs'};
  var newScreen = document.getElementById(screens[name]);
  // Fade out current screen
  var all = document.querySelectorAll('.screen');
  for (var i = 0; i < all.length; i++) all[i].classList.remove('active');
  navCtl.set(name);
  newScreen.classList.add('active');
  var tabNames = {cam: '录入', faces: '人脸', settings: '设置', logs: '日志'};
  var allTabs = document.querySelectorAll('.tabs span');
  for (var i = 0; i < allTabs.length; i++) {
    allTabs[i].classList.toggle('active', allTabs[i].textContent === tabNames[name]);
  }
  if (name === 'cam') {
    // Idempotent server-side (no-op if already running).
    try { if (H.StartPreview()) previewCtl.startLoop(); } catch(e) {}
  }
  if (name === 'settings') loadSettings();
  if (name === 'faces') loadFaces();
  if (name === 'logs') refreshLogs();
}
