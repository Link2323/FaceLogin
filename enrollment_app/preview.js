// Cam-screen live preview: frame polling, canvas painting and the preview
// lifecycle. All state is private to this module — other files drive it
// through previewCtl only.
var previewCtl = (function() {
  var viewing = true, lastB64 = '', lastFacesJson = '', timer = 0, faceImg = new Image();
  var camReady = false;

  faceImg.onload = function() {
    drawBackgroundAndOverlay();
    if (!camReady) {
      camReady = true;
      document.getElementById('cam-loading').classList.add('fade');
      setTimeout(function() {
        var el = document.getElementById('cam-loading');
        if (el) el.style.display = 'none';
      }, 350);
    }
  };

  // Compute the contain-fit mapping for the video frame inside the viewport:
  // scale = min(cw/sw, ch/sh), then center the scaled image. Returns {sx,sy,ox,oy}
  // where frame point (x,y) → (x*sx+ox, y*sy+oy). BOTH the background draw and
  // the overlay draw MUST use this same mapping so the face box/landmarks always
  // line up with the visible face. (Changed from stretch → contain in 1.4.1 so a
  // portrait/rotated frame isn't distorted: 90/270 rotation makes the JPEG
  // portrait, and stretching that into a 16:9 box squashes the face.)
  function containMapping(srcW, srcH, cw, ch) {
    if (srcW <= 0 || srcH <= 0 || cw <= 0 || ch <= 0) return { sx: 1, sy: 1, ox: 0, oy: 0 };
    var s = Math.min(cw / srcW, ch / srcH);
    var ox = (cw - srcW * s) / 2;
    var oy = (ch - srcH * s) / 2;
    return { sx: s, sy: s, ox: ox, oy: oy };
  }

  // Draw the background image AND its paired overlay (same frame). The frame is
  // contain-fit over a blurred cover-scaled backdrop, and the overlay uses the
  // SAME contain mapping so the face box/landmarks always line up with the
  // visible face.
  function drawBackgroundAndOverlay() {
    var c = document.getElementById('bg-canvas');
    var vp = document.getElementById('viewport');
    // The viewport stays a flex-fitted full area; the frame is drawn contain-fit
    // on top of a blurred, darkened cover-scaled copy of itself. Ratio mismatch
    // (portrait/rotated cameras, stretched box) must leave padding SOMEWHERE —
    // solid black/white reads as broken letterboxing, so the padding is filled
    // with a continuation of the picture instead: no hard bars, no crop.
    var srcW = faceImg.naturalWidth || 1280;
    var srcH = faceImg.naturalHeight || 720;
    // Backing store at PHYSICAL resolution (devicePixelRatio), drawing coords
    // still CSS px — without this a 125/150% display upscales the 1x bitmap and
    // the overlay text goes fuzzy. CSS pins both canvases to 100% of the
    // viewport, so a larger bitmap renders sharper, never larger.
    var dpr = window.devicePixelRatio || 1;
    var cw = vp.clientWidth, ch = vp.clientHeight;
    c.width = Math.round(cw * dpr);
    c.height = Math.round(ch * dpr);
    var m = containMapping(srcW, srcH, cw, ch);
    var ctx = c.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, cw, ch);
    if (faceImg.naturalWidth > 0) {
      // Backdrop: cover-scale, ~15% oversized so the blur never samples past the
      // image edge into transparency, then darken so the sharp image reads on top.
      ctx.save();
      ctx.filter = 'blur(24px)';
      var cover = Math.max(cw / srcW, ch / srcH) * 1.15;
      ctx.drawImage(faceImg, (cw - srcW * cover) / 2, (ch - srcH * cover) / 2,
                    srcW * cover, srcH * cover);
      ctx.restore();
      ctx.fillStyle = 'rgba(0, 0, 0, 0.35)';
      ctx.fillRect(0, 0, cw, ch);
    }
    ctx.drawImage(faceImg, m.ox, m.oy, srcW * m.sx, srcH * m.sy);
    drawOverlay(lastFacesJson);
  }

  function renderFrame() {
    if (!viewing) return;
    try {
      // Get the frame AND its face overlay atomically (same frame, no desync).
      var combined = H.GetLatestFrameAndFaces();
      var sep = combined.indexOf('\x1e');
      var b64 = sep > 0 ? combined.substring(0, sep) : combined;
      lastFacesJson = sep > 0 ? combined.substring(sep + 1) : '';
      if (b64 && b64 !== lastB64) {
        lastB64 = b64;
        faceImg.src = b64;   // async load; onload redraws bg + overlay
      } else if (faceImg.complete) {
        // Frame unchanged but canvas may be stale (e.g. window resize) — redraw.
        drawBackgroundAndOverlay();
      }
    } catch(e) {}
    timer = setTimeout(renderFrame, 33);
  }

  // Path helper for the guidance pill (arcTo rounding works on every WebView2
  // runtime, unlike ctx.roundRect which needs a newer Chromium).
  function roundedRectPath(ctx, x, y, w, h, r) {
    r = Math.min(r, w / 2, h / 2);
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
  }

  function drawOverlay(json) {
    var canvas = document.getElementById('face-canvas');
    var vp = document.getElementById('viewport');
    var ctx = canvas.getContext('2d');
    // Same physical-resolution backing store as the background canvas — see
    // drawBackgroundAndOverlay. Coordinates below stay in CSS px; only the
    // bitmap is dpr-dense, so text/lines render crisp on scaled displays.
    var dpr = window.devicePixelRatio || 1;
    var cw = vp.clientWidth, ch = vp.clientHeight;
    var bw = Math.round(cw * dpr), bh = Math.round(ch * dpr);
    if (canvas.width !== bw || canvas.height !== bh) {
      canvas.width = bw;
      canvas.height = bh;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    var faces;
    try { faces = JSON.parse(json); } catch(e) { return; }
    if (faces.length === 0) { ctx.clearRect(0, 0, cw, ch); return; }

    // Use the ACTUAL video frame size (from the loaded image), NOT a hardcoded
    // 1280x720 — the camera may negotiate a different resolution. And use the
    // SAME contain mapping as drawBackgroundAndOverlay, so the overlay always
    // lines up with the visible face (a contain/letterbox background would drift
    // from a stretch overlay, so both share containMapping()).
    var srcW = faceImg.naturalWidth || 1280;
    var srcH = faceImg.naturalHeight || 720;
    var m = containMapping(srcW, srcH, cw, ch);
    var sx = m.sx, sy = m.sy, ox = m.ox, oy = m.oy;

    ctx.clearRect(0, 0, cw, ch);

    for (var fi = 0; fi < faces.length; fi++) {
      var f = faces[fi];
      var bx = f.x * sx + ox, by = f.y * sy + oy;
      var bw = f.w * sx, bh = f.h * sy;

      // Gate state drives the frame color: green in gate, amber = keep turning.
      // targetYaw is only present while capturing (backend contract).
      var col = '#1bb253', hint = '';
      if (typeof f.targetYaw !== 'undefined' && typeof f.yaw !== 'undefined') {
        var diff = f.targetYaw - f.yaw;      // >0 keep turning own-left, <0 own-right
        var inGate = Math.abs(diff) <= 10;   // same ±10° gate the collector uses
        col = inGate ? '#1bb253' : '#f59e0b';
        hint = inGate ? '角度到位 · 请保持'
                      : '再' + (diff > 0 ? '左转 ' : '右转 ') +
                      Math.round(Math.abs(diff)) + '°';
      }

      // Viewfinder-style corner brackets instead of the old hard rectangle —
      // same gate color, crisp square ends and near-square corners.
      ctx.save();
      ctx.strokeStyle = col;
      ctx.lineCap = 'butt';
      ctx.lineJoin = 'miter';
      ctx.lineWidth = 3.5;
      var cl = Math.max(20, Math.min(bw, bh) * 0.26);   // bracket arm length (uniform)
      var cr = 6;                                        // corner rounding
      ctx.beginPath();
      ctx.moveTo(bx, by + cl); ctx.lineTo(bx, by + cr);
      ctx.quadraticCurveTo(bx, by, bx + cr, by); ctx.lineTo(bx + cl, by);
      ctx.moveTo(bx + bw - cl, by); ctx.lineTo(bx + bw - cr, by);
      ctx.quadraticCurveTo(bx + bw, by, bx + bw, by + cr); ctx.lineTo(bx + bw, by + cl);
      ctx.moveTo(bx + bw, by + bh - cl); ctx.lineTo(bx + bw, by + bh - cr);
      ctx.quadraticCurveTo(bx + bw, by + bh, bx + bw - cr, by + bh); ctx.lineTo(bx + bw - cl, by + bh);
      ctx.moveTo(bx + cl, by + bh); ctx.lineTo(bx + cr, by + bh);
      ctx.quadraticCurveTo(bx, by + bh, bx, by + bh - cr); ctx.lineTo(bx, by + bh - cl);
      ctx.stroke();
      ctx.restore();

      // Guidance pill above the box (capturing only): tells the user what to DO
      // instead of exposing the raw "+12.3° / 目标 +30°" numbers.
      if (hint) {
        ctx.save();
        ctx.font = '600 13px "Segoe UI", "Microsoft YaHei", sans-serif';
        var tw = ctx.measureText(hint).width;
        var padX = 12, pillH = 26, dotR = 4, dotGap = 8;
        var pillW = padX * 2 + dotR * 2 + dotGap + tw;
        var px = Math.max(8, Math.min(bx + bw / 2 - pillW / 2, cw - pillW - 8));
        var py = by - pillH - 10;
        if (py < 8) py = 8;   // box near the top edge — pin the pill to the top
        roundedRectPath(ctx, px, py, pillW, pillH, 13);
        ctx.fillStyle = 'rgba(15, 23, 42, 0.82)';
        ctx.fill();
        ctx.beginPath();
        ctx.arc(px + padX + dotR, py + pillH / 2, dotR, 0, Math.PI * 2);
        ctx.fillStyle = col;
        ctx.fill();
        ctx.fillStyle = '#fff';
        ctx.textBaseline = 'middle';
        ctx.fillText(hint, px + padX + dotR * 2 + dotGap, py + pillH / 2 + 0.5);
        ctx.restore();
      }
    }
  }

  // Start the cam-screen live preview and kick the frame loop. Every restart
  // site (tab return, save/append/cancel flows, camera switch, boot) shares
  // this and the same banner wording for the failure shapes. viewing is set
  // on success — all callers run while the cam screen is active, which is
  // exactly when the 33ms loop should run.
  function restartPreview() {
    try {
      if (H.StartPreview()) { viewing = true; renderFrame(); return true; }
      showBanner('cam', '预览启动失败：请检查摄像头占用、权限、驱动和模型文件。', 'error', 10000);
    } catch(e) { showBanner('cam', '预览启动失败: ' + e.message, 'error', 10000); }
    return false;
  }

  // Loop guards: viewing/timer are private. The host-side camera off/on call
  // stays at the call sites — tab switch and finishCapture wrap H.StopPreview
  // in different try/catch shapes, which is deliberate.
  function stopLoop() {
    viewing = false;
    clearTimeout(timer);
  }
  function startLoop() {
    viewing = true;
    renderFrame();
  }

  return {
    restartPreview: restartPreview,
    isViewing: function() { return viewing; },
    stopLoop: stopLoop,
    startLoop: startLoop
  };
})();
