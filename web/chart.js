/* chart.js - 依存なしの Canvas 折れ線/帯グラフ (対数周波数軸対応)
 *
 * 機能: 十字線 + 全系列ツールチップ / ドラッグで範囲拡大 / ダブルクリックでリセット / Ctrl+ホイール拡大
 *       キーボード(←→)でも同じ読み取り / 凡例クリックで表示切替 / 表ビュー・CSV / ライト・ダーク追従 / 軸リンク
 * 注意: 系列名など外部由来の文字列は必ず textContent で DOM に入れる (innerHTML 不使用)
 */
(function () {
  'use strict';

  const DPR = () => Math.max(1, Math.min(3, window.devicePixelRatio || 1));
  const css = (el, name) => getComputedStyle(el).getPropertyValue(name).trim();

  function niceStep(span, target) {
    const raw = span / Math.max(1, target);
    const p = Math.pow(10, Math.floor(Math.log10(raw)));
    const f = raw / p;
    return (f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10) * p;
  }
  function linTicks(lo, hi, target) {
    const st = niceStep(hi - lo, target);
    const out = [];
    for (let v = Math.ceil(lo / st - 1e-9) * st; v <= hi + st * 1e-9; v += st) out.push(Math.abs(v) < st * 1e-9 ? 0 : v);
    return { ticks: out, step: st };
  }
  function logTicks(lo, hi) {
    const major = [], minor = [];
    for (let e = Math.floor(Math.log10(lo)) - 1; e <= Math.ceil(Math.log10(hi)); e++) {
      for (const m of [1, 2, 3, 4, 5, 6, 7, 8, 9]) {
        const v = m * Math.pow(10, e);
        if (v < lo * 0.9999 || v > hi * 1.0001) continue;
        (m === 1 || m === 2 || m === 5 ? major : minor).push(v);
      }
    }
    return { major, minor };
  }

  const fmt = {
    hz: (v) => {
      if (v >= 1000) { const k = v / 1000; return (Number.isInteger(k) ? String(k) : String(+k.toFixed(2))) + 'k'; }
      return String(+(v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2)));
    },
    hzShort: (v) => (v >= 1000 ? String(+(v / 1000).toFixed(2)) + ' kHz' : String(+v.toFixed(v >= 100 ? 0 : 1)) + ' Hz'),
    hzFull: (v) => (v >= 1000 ? (v / 1000).toFixed(3) + ' kHz' : v.toFixed(v >= 100 ? 1 : 2) + ' Hz'),
    db: (v) => v.toFixed(2) + ' dB',
    dbAx: (v) => (Math.abs(v) < 1e-9 ? '0' : Number(v.toFixed(2)).toString()),
    deg: (v) => v.toFixed(1) + '°',
    degAx: (v) => Number(v.toFixed(1)).toString() + '°',
    num: (v) => (Math.abs(v) >= 1000 || (Math.abs(v) < 0.01 && v !== 0) ? v.toExponential(2) : Number(v.toPrecision(4)).toString()),
    sec: (v) => v.toFixed(v < 10 ? 3 : 2) + ' s',
    secAx: (v) => Number(v.toFixed(3)).toString(),
    ms: (v) => v.toFixed(3) + ' ms',
    msAx: (v) => Number(v.toFixed(2)).toString(),
  };

  const groups = {};

  class Chart {
    /* opts: {title, desc, xlog, xlabel, ylabel, xfmt, xfmtAx, yfmt, yfmtAx, ymin, ymax, yTickStep, group, height, filename} */
    constructor(host, opts) {
      this.o = Object.assign({ xlog: true, height: 300, xfmt: fmt.hzFull, xfmtAx: fmt.hz, yfmt: fmt.db, yfmtAx: fmt.dbAx, ylabel: '', xlabel: '', filename: 'chart' }, opts);
      this.series = [];
      this.xr = null;          // 現在の表示範囲 [x0, x1]
      this.fullX = [0, 1];
      this.hover = null;       // {x, px}
      this.drag = null;
      this.tableOpen = false;
      this.build(host);
      if (this.o.group) (groups[this.o.group] = groups[this.o.group] || []).push(this);
      this.ro = new ResizeObserver(() => this.draw());
      this.ro.observe(this.wrap);
      Chart.all.push(this);
    }

    build(host) {
      host.classList.add('card');
      const head = document.createElement('div'); head.className = 'head';
      const h2 = document.createElement('h2'); h2.textContent = this.o.title || ''; head.appendChild(h2);
      const grow = document.createElement('span'); grow.className = 'grow'; head.appendChild(grow);
      const mk = (label, fn, pressed) => {
        const b = document.createElement('button'); b.className = 'btn small'; b.type = 'button'; b.textContent = label;
        if (pressed !== undefined) b.setAttribute('aria-pressed', 'false');
        b.addEventListener('click', fn); head.appendChild(b); return b;
      };
      mk('範囲リセット', () => this.resetRange());
      this.tblBtn = mk('表で見る', () => this.toggleTable(), true);
      mk('CSV', () => this.downloadCsv());
      host.appendChild(head);
      if (this.o.desc) { const d = document.createElement('div'); d.className = 'desc'; d.textContent = this.o.desc; host.appendChild(d); }
      this.wrap = document.createElement('div'); this.wrap.className = 'cv-wrap';
      this.cv = document.createElement('canvas'); this.cv.tabIndex = 0;
      this.cv.setAttribute('role', 'img'); this.cv.setAttribute('aria-label', (this.o.title || 'グラフ') + ' (表ビューで数値を確認できます)');
      this.cv.style.height = this.o.height + 'px';
      this.tip = document.createElement('div'); this.tip.className = 'tip'; this.tip.hidden = true;
      this.wrap.append(this.cv, this.tip); host.appendChild(this.wrap);
      this.legend = document.createElement('ul'); this.legend.className = 'legend'; host.appendChild(this.legend);
      this.tblHost = document.createElement('div'); this.tblHost.hidden = true; host.appendChild(this.tblHost);
      const hint = document.createElement('div'); hint.className = 'hint';
      hint.textContent = 'ドラッグで範囲拡大 / ダブルクリックでリセット / Ctrl+ホイールでズーム / ←→キーで読み取り';
      host.appendChild(hint);
      this.host = host;

      const cv = this.cv;
      cv.addEventListener('pointermove', (e) => this.onMove(e));
      cv.addEventListener('pointerleave', () => { if (!this.drag) { this.hover = null; this.draw(); } });
      cv.addEventListener('pointerdown', (e) => this.onDown(e));
      cv.addEventListener('pointerup', (e) => this.onUp(e));
      cv.addEventListener('pointercancel', () => { this.drag = null; this.draw(); });
      cv.addEventListener('dblclick', () => this.resetRange());
      cv.addEventListener('wheel', (e) => this.onWheel(e), { passive: false });
      cv.addEventListener('keydown', (e) => this.onKey(e));
      cv.addEventListener('blur', () => { this.hover = null; this.draw(); });
    }

    /* series: [{id,name,x,y,y2?,color,visible,type:'line'|'band',width,breakJump}] color は CSS 変数名 ('--c-after') */
    setSeries(series) {
      const vis = {}; this.series.forEach((s) => { vis[s.id] = s.visible; });
      this.series = series.map((s) => Object.assign({ visible: true, type: 'line', width: 2 }, s, vis[s.id] !== undefined ? { visible: vis[s.id] } : {}));
      let lo = Infinity, hi = -Infinity;
      for (const s of this.series) { for (let i = 0; i < s.x.length; i++) { const v = s.x[i]; if (isFinite(v) && (!this.o.xlog || v > 0)) { if (v < lo) lo = v; if (v > hi) hi = v; } } }
      if (!isFinite(lo)) { lo = 0; hi = 1; }
      this.fullX = this.o.fullX || [lo, hi];
      if (!this.xr) this.xr = this.fullX.slice();
      this.buildLegend(); this.draw(); if (this.tableOpen) this.buildTable();
    }

    buildLegend() {
      this.legend.textContent = '';
      if (this.series.length < 2) return;       // 1 系列なら凡例なし (タイトルが示す)
      for (const s of this.series) {
        const li = document.createElement('li'); const b = document.createElement('button'); b.type = 'button';
        b.setAttribute('aria-pressed', s.visible ? 'true' : 'false');
        const sw = document.createElement('span'); sw.className = 'sw' + (s.type === 'band' ? ' band' : '');
        sw.style.borderTopColor = s.type === 'band' ? 'transparent' : 'var(' + s.color + ')';
        if (s.type === 'band') sw.style.background = 'var(' + s.color + ')';
        const t = document.createElement('span'); t.textContent = s.name;
        b.append(sw, t);
        b.addEventListener('click', () => { s.visible = !s.visible; b.setAttribute('aria-pressed', s.visible ? 'true' : 'false'); this.draw(); if (this.tableOpen) this.buildTable(); });
        li.appendChild(b); this.legend.appendChild(li);
      }
    }

    setRange(r, fromGroup) {
      this.xr = r.slice(); this.draw();
      if (this.tableOpen) this.buildTable();
      if (!fromGroup && this.o.group) groups[this.o.group].forEach((c) => { if (c !== this) c.setRange(r, true); });
    }
    resetRange() { this.setRange(this.fullX); }

    /* ---- 座標変換 ---- */
    layout() {
      const w = this.cv.clientWidth, h = this.cv.clientHeight;
      return { w, h, l: 62, r: 14, t: 10, b: 36 };
    }
    X(v, L) {
      const [a, b] = this.xr, pw = L.w - L.l - L.r;
      return L.l + (this.o.xlog ? (Math.log(v) - Math.log(a)) / (Math.log(b) - Math.log(a)) : (v - a) / (b - a)) * pw;
    }
    invX(px, L) {
      const [a, b] = this.xr, pw = L.w - L.l - L.r, f = (px - L.l) / pw;
      return this.o.xlog ? Math.exp(Math.log(a) + f * (Math.log(b) - Math.log(a))) : a + f * (b - a);
    }
    Y(v, L) { const [a, b] = this.yr, ph = L.h - L.t - L.b; return L.t + (1 - (v - a) / (b - a)) * ph; }

    computeY() {
      if (this.o.ymin !== undefined && this.o.ymax !== undefined) { this.yr = [this.o.ymin, this.o.ymax]; return; }
      let lo = Infinity, hi = -Infinity;
      const [x0, x1] = this.xr;
      for (const s of this.series) {
        if (!s.visible) continue;
        const arrs = s.type === 'band' ? [s.y, s.y2] : [s.y];
        for (const arr of arrs) for (let i = 0; i < arr.length; i++) {
          const x = s.x[i], y = arr[i];
          if (x < x0 || x > x1 || !isFinite(y)) continue;
          if (y < lo) lo = y; if (y > hi) hi = y;
        }
      }
      if (!isFinite(lo)) { lo = -1; hi = 1; }
      if (hi - lo < 1e-12) { hi += 0.5; lo -= 0.5; }
      const pad = (hi - lo) * 0.08;
      this.yr = [this.o.ymin !== undefined ? this.o.ymin : lo - pad, this.o.ymax !== undefined ? this.o.ymax : hi + pad];
    }

    /* ---- 描画 ---- */
    draw() {
      if (!this.cv.isConnected) return;
      const cv = this.cv, dpr = DPR(), L = this.layout();
      if (L.w < 10) return;
      cv.width = Math.round(L.w * dpr); cv.height = Math.round(L.h * dpr);
      const g = cv.getContext('2d'); g.setTransform(dpr, 0, 0, dpr, 0, 0);
      const surf = css(this.host, '--surface'), ink2 = css(this.host, '--ink-2'), ink3 = css(this.host, '--ink-3'), grid = css(this.host, '--grid'), axis = css(this.host, '--axis');
      g.fillStyle = surf; g.fillRect(0, 0, L.w, L.h);
      this.computeY();
      const pw = L.w - L.l - L.r, ph = L.h - L.t - L.b;
      g.font = '11.5px ' + css(this.host, '--font'); g.textBaseline = 'middle';

      // 格子 (実線ヘアライン) と軸ラベル
      g.lineWidth = 1;
      const yt = linTicks(this.yr[0], this.yr[1], Math.max(3, Math.round(ph / 46)));
      g.textAlign = 'right'; g.fillStyle = ink3;
      for (const v of yt.ticks) {
        const y = Math.round(this.Y(v, L)) + 0.5; if (y < L.t - 1 || y > L.t + ph + 1) continue;
        g.strokeStyle = Math.abs(v) < 1e-9 ? axis : grid; g.beginPath(); g.moveTo(L.l, y); g.lineTo(L.l + pw, y); g.stroke();
        g.fillText(this.o.yfmtAx(v), L.l - 6, y);
      }
      g.textAlign = 'center';
      let xTicks;
      if (this.o.xlog) {
        const lt = logTicks(this.xr[0], this.xr[1]);
        g.strokeStyle = grid;
        for (const v of lt.minor) { const x = Math.round(this.X(v, L)) + 0.5; g.globalAlpha = 0.5; g.beginPath(); g.moveTo(x, L.t); g.lineTo(x, L.t + ph); g.stroke(); g.globalAlpha = 1; }
        xTicks = lt.major;
      } else xTicks = linTicks(this.xr[0], this.xr[1], Math.max(3, Math.round(pw / 90))).ticks;
      let lastRight = -1e9;
      for (const v of xTicks) {
        const x = Math.round(this.X(v, L)) + 0.5; if (x < L.l - 1 || x > L.l + pw + 1) continue;
        g.strokeStyle = grid; g.beginPath(); g.moveTo(x, L.t); g.lineTo(x, L.t + ph); g.stroke();
        const lab = this.o.xfmtAx(v), tw = g.measureText(lab).width;
        if (x - tw / 2 > lastRight + 6) { g.fillStyle = ink3; g.fillText(lab, x, L.t + ph + 14); lastRight = x + tw / 2; }
      }
      g.strokeStyle = axis; g.beginPath(); g.moveTo(L.l, L.t + ph + 0.5); g.lineTo(L.l + pw, L.t + ph + 0.5); g.stroke();
      g.fillStyle = ink2; g.textAlign = 'center';
      if (this.o.xlabel) g.fillText(this.o.xlabel, L.l + pw / 2, L.h - 8);
      if (this.o.ylabel) { g.save(); g.translate(12, L.t + ph / 2); g.rotate(-Math.PI / 2); g.fillText(this.o.ylabel, 0, 0); g.restore(); }

      // データ (プロット領域でクリップ)
      g.save(); g.beginPath(); g.rect(L.l, L.t, pw, ph); g.clip();
      for (const s of this.series) if (s.visible) this.drawSeries(g, s, L, pw);
      g.restore();

      // 十字線とマーカー
      if (this.hover && !this.drag) this.drawHover(g, L, surf);
      // ドラッグ選択
      if (this.drag && Math.abs(this.drag.cur - this.drag.start) > 3) {
        const a = Math.min(this.drag.start, this.drag.cur), b = Math.max(this.drag.start, this.drag.cur);
        g.fillStyle = css(this.host, '--accent'); g.globalAlpha = 0.14; g.fillRect(a, L.t, b - a, ph); g.globalAlpha = 1;
      }
    }

    drawSeries(g, s, L, pw) {
      const col = css(this.host, s.color), [x0, x1] = this.xr;
      // 表示範囲に対応するインデックス
      let i0 = this.lowerBound(s.x, x0), i1 = Math.min(s.x.length - 1, this.lowerBound(s.x, x1) + 1);
      i0 = Math.max(0, i0 - 1);
      if (s.type === 'band') {
        g.fillStyle = col; g.globalAlpha = 0.34;
        const n = i1 - i0 + 1, step = Math.max(1, Math.floor(n / (pw * 2)));
        g.beginPath(); let started = false;
        for (let i = i0; i <= i1; i += step) { if (!isFinite(s.y[i])) continue; const x = this.X(s.x[i], L), y = this.Y(s.y[i], L); if (!started) { g.moveTo(x, y); started = true; } else g.lineTo(x, y); }
        for (let i = i1; i >= i0; i -= step) { if (!isFinite(s.y2[i])) continue; g.lineTo(this.X(s.x[i], L), this.Y(s.y2[i], L)); }
        g.closePath(); g.fill(); g.globalAlpha = 1;
        g.strokeStyle = col; g.lineWidth = 1;
        for (const arr of [s.y, s.y2]) { g.beginPath(); started = false; for (let i = i0; i <= i1; i += step) { if (!isFinite(arr[i])) continue; const x = this.X(s.x[i], L), y = this.Y(arr[i], L); if (!started) { g.moveTo(x, y); started = true; } else g.lineTo(x, y); } g.stroke(); }
        return;
      }
      g.strokeStyle = col; g.lineWidth = s.width; g.lineJoin = 'round'; g.lineCap = 'round';
      g.beginPath();
      let pen = false, prevY = NaN;
      const n = i1 - i0 + 1, dense = n > pw * 3;
      if (dense) {   // 画素列ごとの min/max で間引く (形状を保つ)
        let curPx = null, mn = 0, mx = 0, first = 0, last = 0;
        const flush = () => { if (curPx === null) return; const xs = curPx + 0.5; if (!pen) { g.moveTo(xs, this.Y(first, L)); pen = true; } else g.lineTo(xs, this.Y(first, L)); g.lineTo(xs, this.Y(mn, L)); g.lineTo(xs, this.Y(mx, L)); g.lineTo(xs, this.Y(last, L)); };
        for (let i = i0; i <= i1; i++) {
          const y = s.y[i]; if (!isFinite(y)) { flush(); curPx = null; pen = false; continue; }
          const px = Math.floor(this.X(s.x[i], L));
          if (px !== curPx) { flush(); curPx = px; mn = mx = first = last = y; } else { if (y < mn) mn = y; if (y > mx) mx = y; last = y; }
        }
        flush();
      } else {
        for (let i = i0; i <= i1; i++) {
          const y = s.y[i];
          if (!isFinite(y)) { pen = false; continue; }
          if (s.breakJump && pen && Math.abs(y - prevY) > s.breakJump) pen = false;
          const px = this.X(s.x[i], L), py = this.Y(y, L);
          if (!pen) { g.moveTo(px, py); pen = true; } else g.lineTo(px, py);
          prevY = y;
        }
      }
      g.stroke();
    }

    lowerBound(arr, v) { let lo = 0, hi = arr.length; while (lo < hi) { const m = (lo + hi) >> 1; if (arr[m] < v) lo = m + 1; else hi = m; } return lo; }
    nearest(s, x) {
      const i = this.lowerBound(s.x, x);
      if (i <= 0) return 0; if (i >= s.x.length) return s.x.length - 1;
      return (this.o.xlog ? Math.abs(Math.log(s.x[i] / x)) < Math.abs(Math.log(x / s.x[i - 1])) : Math.abs(s.x[i] - x) < Math.abs(x - s.x[i - 1])) ? i : i - 1;
    }

    drawHover(g, L, surf) {
      const vis = this.series.filter((s) => s.visible && s.x.length); if (!vis.length) return;
      const ref = vis[0], ri = this.nearest(ref, this.hover.x), xv = ref.x[ri], px = this.X(xv, L);
      if (px < L.l || px > L.w - L.r) return;
      g.strokeStyle = css(this.host, '--ink-3'); g.lineWidth = 1; g.beginPath(); g.moveTo(Math.round(px) + 0.5, L.t); g.lineTo(Math.round(px) + 0.5, L.h - L.b); g.stroke();
      const rows = [];
      for (const s of vis) {
        const i = this.nearest(s, xv), y = s.y[i];
        if (s.type === 'band') { rows.push({ s, text: this.o.yfmt(s.y2[i]) + ' 〜 ' + this.o.yfmt(y) }); continue; }
        if (!isFinite(y)) { rows.push({ s, text: '—' }); continue; }
        const py = this.Y(y, L);
        g.fillStyle = surf; g.beginPath(); g.arc(this.X(s.x[i], L), py, 6, 0, 7); g.fill();      // 2px のサーフェスリング
        g.fillStyle = css(this.host, s.color); g.beginPath(); g.arc(this.X(s.x[i], L), py, 4, 0, 7); g.fill();
        rows.push({ s, text: this.o.yfmt(y) });
      }
      this.showTip(this.o.xfmt(xv), rows, px, L);
    }

    showTip(xtext, rows, px, L) {
      const tip = this.tip; tip.textContent = '';
      const hx = document.createElement('div'); hx.className = 'x'; hx.textContent = xtext; tip.appendChild(hx);
      for (const r of rows) {
        const d = document.createElement('div'); d.className = 'row';
        const k = document.createElement('span'); k.className = 'key'; k.style.borderTopColor = 'var(' + r.s.color + ')';
        const v = document.createElement('span'); v.className = 'val'; v.textContent = r.text;
        const n = document.createElement('span'); n.className = 'nm'; n.textContent = r.s.name;
        d.append(k, v, n); tip.appendChild(d);
      }
      tip.hidden = false;
      const tw = tip.offsetWidth, th = tip.offsetHeight;
      let left = px + 14; if (left + tw > L.w - 4) left = px - tw - 14; if (left < 4) left = 4;
      tip.style.left = left + 'px'; tip.style.top = Math.max(4, Math.min(L.t + 6, L.h - th - 4)) + 'px';
    }

    /* ---- 入力 ---- */
    px(e) { const r = this.cv.getBoundingClientRect(); return e.clientX - r.left; }
    onMove(e) {
      const L = this.layout(), p = this.px(e);
      if (this.drag) { this.drag.cur = Math.max(L.l, Math.min(L.w - L.r, p)); this.tip.hidden = true; this.draw(); return; }
      if (p < L.l || p > L.w - L.r) { this.hover = null; this.tip.hidden = true; this.draw(); return; }
      this.hover = { x: this.invX(p, L) }; this.draw();
    }
    onDown(e) {
      const L = this.layout(), p = this.px(e);
      if (e.button !== 0 || p < L.l || p > L.w - L.r) return;
      this.cv.setPointerCapture(e.pointerId); this.drag = { start: p, cur: p };
    }
    onUp(e) {
      if (!this.drag) return;
      const L = this.layout(), d = this.drag; this.drag = null;
      try { this.cv.releasePointerCapture(e.pointerId); } catch (_) { /* noop */ }
      if (Math.abs(d.cur - d.start) > 6) {
        const a = this.invX(Math.min(d.start, d.cur), L), b = this.invX(Math.max(d.start, d.cur), L);
        this.setRange([a, b]);
      } else this.draw();
    }
    onWheel(e) {
      if (!(e.ctrlKey || e.metaKey)) return;       // 通常のスクロールは奪わない
      e.preventDefault();
      const L = this.layout(), p = this.px(e), c = this.invX(Math.max(L.l, Math.min(L.w - L.r, p)), L), k = Math.exp(Math.max(-1, Math.min(1, e.deltaY * 0.004)));
      let [a, b] = this.xr;
      if (this.o.xlog) { a = Math.exp(Math.log(c) + (Math.log(a) - Math.log(c)) * k); b = Math.exp(Math.log(c) + (Math.log(b) - Math.log(c)) * k); }
      else { a = c + (a - c) * k; b = c + (b - c) * k; }
      a = Math.max(a, this.fullX[0]); b = Math.min(b, this.fullX[1]); if (b <= a) return;
      this.setRange([a, b]);
    }
    onKey(e) {
      if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') { if (e.key === 'Escape') { this.hover = null; this.draw(); } return; }
      e.preventDefault();
      const vis = this.series.filter((s) => s.visible && s.x.length); if (!vis.length) return;
      const ref = vis[0], L = this.layout();
      let i = this.hover ? this.nearest(ref, this.hover.x) : this.nearest(ref, this.invX(L.l + (L.w - L.l - L.r) / 2, L));
      i = Math.max(0, Math.min(ref.x.length - 1, i + (e.key === 'ArrowRight' ? 1 : -1) * (e.shiftKey ? 10 : 1)));
      this.hover = { x: ref.x[i] }; this.draw();
    }

    /* ---- 表ビュー / CSV ---- */
    toggleTable() {
      this.tableOpen = !this.tableOpen; this.tblBtn.setAttribute('aria-pressed', String(this.tableOpen));
      this.tblHost.hidden = !this.tableOpen; if (this.tableOpen) this.buildTable();
    }
    rowsFor(limit) {
      const vis = this.series; if (!vis.length) return { head: [], rows: [] };
      const ref = vis[0], [x0, x1] = this.xr, idx = [];
      for (let i = 0; i < ref.x.length; i++) if (ref.x[i] >= x0 && ref.x[i] <= x1) idx.push(i);
      const step = limit && idx.length > limit ? idx.length / limit : 1, pick = [];
      for (let k = 0; k < idx.length; k += step) pick.push(idx[Math.floor(k)]);
      const head = [this.o.xlabel || 'x'], cols = [];
      for (const s of vis) { if (s.type === 'band') { head.push(s.name + ' 最小', s.name + ' 最大'); cols.push(s, s); } else { head.push(s.name); cols.push(s); } }
      const rows = pick.map((i) => {
        const r = [ref.x[i]];
        for (const s of vis) { if (s.type === 'band') { r.push(s.y2[i], s.y[i]); } else r.push(s.y[i]); }
        return r;
      });
      return { head, rows };
    }
    buildTable() {
      const { head, rows } = this.rowsFor(400);
      this.tblHost.textContent = '';
      const note = document.createElement('div'); note.className = 'hint';
      note.textContent = '表示範囲内の値 (最大 400 行に間引き。全点は CSV で取得できます)'; this.tblHost.appendChild(note);
      const sc = document.createElement('div'); sc.className = 'tbl-scroll';
      const t = document.createElement('table'); t.className = 'data';
      const tr = document.createElement('tr'); head.forEach((h) => { const th = document.createElement('th'); th.textContent = h; tr.appendChild(th); });
      const thd = document.createElement('thead'); thd.appendChild(tr); t.appendChild(thd);
      const tb = document.createElement('tbody');
      for (const r of rows) { const row = document.createElement('tr'); r.forEach((v, i) => { const td = document.createElement('td'); td.textContent = isFinite(v) ? (i === 0 ? this.o.xfmtAx(v) : fmt.num(v)) : '—'; row.appendChild(td); }); tb.appendChild(row); }
      t.appendChild(tb); sc.appendChild(t); this.tblHost.appendChild(sc);
    }
    downloadCsv() {
      const saveXr = this.xr; this.xr = this.fullX; const { head, rows } = this.rowsFor(0); this.xr = saveXr;
      const esc = (s) => '"' + String(s).replace(/"/g, '""') + '"';
      const csv = '﻿' + [head.map(esc).join(',')].concat(rows.map((r) => r.map((v) => (isFinite(v) ? v : '')).join(','))).join('\r\n');
      const a = document.createElement('a'); a.href = URL.createObjectURL(new Blob([csv], { type: 'text/csv;charset=utf-8' }));
      a.download = this.o.filename + '.csv'; document.body.appendChild(a); a.click(); setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 500);
    }
  }
  Chart.all = [];
  Chart.reset = () => { Chart.all.forEach((c) => c.ro.disconnect()); Chart.all.length = 0; Object.keys(groups).forEach((k) => delete groups[k]); };
  Chart.fmt = fmt;
  Chart.redrawAll = () => Chart.all.forEach((c) => c.draw());
  window.Chart = Chart;
})();
