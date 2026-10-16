// Owns one capture sequence, including the pause between angles. Generation
// checks also discard callbacks already queued when a sequence was cancelled.
function createCaptureFlow(timers) {
  var phase = 'idle';
  var generation = 0;
  var poll = 0, nextAngle = 0;
  var multi = false, angle = 0;

  function stopPolling() {
    if (poll) timers.clearInterval(poll);
    poll = 0;
  }
  function clearTimers() {
    stopPolling();
    if (nextAngle) timers.clearTimeout(nextAngle);
    nextAngle = 0;
  }
  function isActive() { return phase === 'capturing'; }
  function guarded(callback) {
    var expected = generation;
    return function() {
      if (expected === generation && isActive()) callback();
    };
  }

  return {
    begin: function(multiAngle, firstAngle) {
      clearTimers();
      generation++;
      multi = multiAngle;
      angle = firstAngle;
      phase = 'capturing';
    },
    end: function() {
      clearTimers();
      generation++;
      phase = 'ended';
    },
    isActive: isActive,
    canShowIdleTotal: function() { return phase === 'idle'; },
    isMultiAngle: function() { return multi; },
    firstAngle: function() { return angle; },
    stopPolling: stopPolling,
    poll: function(callback) {
      stopPolling();
      poll = timers.setInterval(guarded(callback), 200);
    },
    scheduleNext: function(callback) {
      if (nextAngle) timers.clearTimeout(nextAngle);
      nextAngle = timers.setTimeout(guarded(function() {
        nextAngle = 0;
        callback();
      }), 400);
    }
  };
}
