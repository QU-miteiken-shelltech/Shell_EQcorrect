/* app.js - eqcorrect 解析 JSON の読み込みとグラフ構成
 * データの読み込み優先順: window.EQ_RESULT (result.js) → result.json の fetch (http 配信時) → ファイル選択/ドロップ
 */
(function () {
  'use strict';
  const $ = (id) => document.getElementById(id);
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };
  const fmt = Chart.fmt;
  let D = null;                           // 現在のデータ
  let charts = {};                        // 名前 → Chart
  const state = { ch: 0, phase: 'wrap', smooth: 0, range: 'all', tab: 'resp' };

  /* ---------- データ整形 ---------- */
  const nan = (a) => (a || []).map((v) => (v === null || v === undefined ? NaN : v));
  function normalize(j) {
    const r = j.response || {};
    const out = {
      meta: j.meta || {}, grid: nan(j.grid), log: j.log || [], stats: j.stats || [], timing: j.timing || {},
      resp: {}, spectrum: (j.spectrum || []).map((s) => ({ channel: s.channel, in_db: nan(s.in_db), out_db: nan(s.out_db), h_db: nan(s.h_db), h_ph: nan(s.h_phase_deg) })),
      wave: (j.waveform || []).map((w) => ({ channel: w.channel, t: nan(w.t), in_min: nan(w.in_min), in_max: nan(w.in_max), out_min: nan(w.out_min), out_max: nan(w.out_max) })),
      ir: j.filter_ir ? { t: nan(j.filter_ir.t_ms), c: nan(j.filter_ir.c) } : null,
    };
    for (const k of Object.keys(r)) out.resp[k] = nan(r[k]);
    return out;
  }
  const wrap180 = (a) => a.map((v) => (isFinite(v) ? ((((v + 180) % 360) + 360) % 360) - 180 : NaN));
  function unwrapDeg(a) {
    const o = a.slice(); let off = 0, prev = NaN;
    for (let i = 0; i < o.length; i++) {
      if (!isFinite(a[i])) continue;
      if (isFinite(prev)) { const d = a[i] - prev; off -= 360 * Math.round(d / 360); }
      prev = a[i]; o[i] = a[i] + off;
    }
    return o;
  }
  const hasData = (sr) => sr.y.some((v) => isFinite(v));
  const keep = (arr) => arr.filter(hasData);
  const phaseView = (a) => (state.phase === 'wrap' ? wrap180(a) : unwrapDeg(a));
  function smoothPower(y, n) {
    if (!n) return y;
    const g = D.grid, ppo = 1 / Math.log2(g[1] / g[0]), half = Math.max(0, Math.round(ppo / n / 2));
    if (half < 1) return y;
    const N = y.length, ps = new Float64Array(N + 1), cs = new Float64Array(N + 1);
    for (let i = 0; i < N; i++) { const f = isFinite(y[i]); ps[i + 1] = ps[i] + (f ? Math.pow(10, y[i] / 10) : 0); cs[i + 1] = cs[i] + (f ? 1 : 0); }
    return y.map((v, i) => {
      if (!isFinite(v)) return NaN;
      const a = Math.max(0, i - half), b = Math.min(N - 1, i + half), c = cs[b + 1] - cs[a];
      return 10 * Math.log10((ps[b + 1] - ps[a]) / c);
    });
  }

  /* ---------- 要約タイル ---------- */
  function tiles() {
    const m = D.meta, host = $('tiles'); host.textContent = '';
    const add = (k, v, s) => { const t = el('div', 'tile'); t.append(el('div', 'k', k), el('div', 'v', v), el('div', 's', s || '')); host.appendChild(t); };
    const lo = m.fmin_used * Math.pow(2, m.edge_oct || 0), hi = m.fmax_used / Math.pow(2, m.edge_oct || 0);
    const rs = []; let mxb = -Infinity, mxc = Infinity;
    D.grid.forEach((f, i) => {
      const r = D.resp.residual_db[i], a = D.resp.correction_actual_db[i];
      if (isFinite(r) && f >= lo && f <= hi) rs.push(r);
      if (isFinite(a) && f >= m.fmin_used && f <= m.fmax_used) { mxb = Math.max(mxb, a); mxc = Math.min(mxc, a); }
    });
    const rms = rs.length ? Math.sqrt(rs.reduce((p, c) => p + c * c, 0) / rs.length) : NaN, mx = rs.length ? Math.max.apply(null, rs.map(Math.abs)) : NaN;
    const hz = fmt.hzShort;
    if (m.req_role === 'correction') {
      const fl = m.req_filters || [];
      add('適用した EQ', fl.length + ' 個 (' + [...new Set(fl.map((f) => f.type))].join(', ') + ')', 'REW のフィルタ設定をそのまま適用 (測定の逆ではない)');
      add('EQ の周波数', fl.length ? hz(Math.min.apply(null, fl.map((f) => f.fc))) + ' – ' + hz(Math.max.apply(null, fl.map((f) => f.fc))) : '—', '各フィルタの中心周波数の範囲');
    } else {
      add('補正帯域', hz(m.fmin_used) + ' – ' + hz(m.fmax_used), '端 ' + (m.edge_oct || 0) + ' oct は補正量を 0 に収束');
      add('設計上の残差 (補正帯域内)', isFinite(rms) ? 'RMS ' + rms.toFixed(2) + ' dB' : '—', isFinite(mx) ? '最大 ' + mx.toFixed(2) + ' dB (測定値+補正フィルタの実特性)' : '');
    }
    const sg = (v) => (Math.abs(v) < 0.05 ? '0.0' : (v > 0 ? '+' : '−') + Math.abs(v).toFixed(1));
    add('補正量の範囲', isFinite(mxb) ? (sg(mxc) + ' 〜 ' + sg(mxb) + ' dB') : '—', '位相方式: ' + ({ full: '測定位相を逆に (完全補正)', min: '最小位相', linear: '位相補正なし', filters: 'EQ フィルタの位相 (最小位相) をそのまま適用' }[m.phase_mode_used] || m.phase_mode_used));
    const pk = D.stats.length ? Math.max.apply(null, D.stats.map((s) => s.out_peak_db)) : NaN;
    add('出力ピーク', isFinite(pk) ? pk.toFixed(2) + ' dBFS' : '—', m.clipped_samples > 0 ? 'クリップ/超過 ' + m.clipped_samples + ' サンプル' : 'クリップなし');
    host.lastChild.querySelector('.s').classList.toggle('warnline', m.clipped_samples > 0);
  }

  /* ---------- 各タブ ---------- */
  const card = (parent) => { const d = el('div'); parent.appendChild(d); return d; };
  function build() {
    Chart.reset(); charts = {};
    ['resp', 'spec', 'filt', 'wave', 'info'].forEach((k) => { $('p-' + k).textContent = ''; });
    const m = D.meta, nyq = m.fs / 2;

    // 補正特性
    const pr = $('p-resp');
    charts.amp = new Chart(card(pr), { title: m.req_role === 'correction' ? '振幅特性: 補正フィルタ (EQ)' : '振幅特性: 測定 / 補正フィルタ / 残差', desc: m.req_role === 'correction' ? '.req の EQ フィルタをそのまま適用した補正フィルタの周波数特性です (測定データはありません)。' : '基準帯域の平均レベルを 0 dB に正規化。残差 = 測定 + 補正フィルタの実特性 (0 dB に近いほど平坦)。', ylabel: '振幅 [dB]', xlabel: '周波数 [Hz]', group: 'f', filename: 'amplitude_response' });
    charts.pha = new Chart(card(pr), { title: m.req_role === 'correction' ? '位相特性: 補正フィルタ (EQ)' : '位相特性: 測定 / 補正フィルタ / 残差', desc: m.req_role === 'correction' ? 'バイクアッド (最小位相) の位相。プリリンギングはありません。' : m.req_has_phase ? '補正フィルタの位相は測定位相の逆。残差が 0° に近いほど位相が揃っています。' : '測定データに位相がないため、測定位相と残差は表示できません (補正フィルタの位相のみ)。', ylabel: '位相 [deg]', xlabel: '周波数 [Hz]', yfmt: fmt.deg, yfmtAx: fmt.degAx, group: 'f', filename: 'phase_response' });
    // スペクトル
    const ps = $('p-spec');
    charts.spec = new Chart(card(ps), { title: '平均パワースペクトル: 補正前 vs 補正後', desc: 'Welch 法 (16384 点, Hann, 50% 重なり)。振幅 1 の正弦波 = 0 dB。信号の内容に依存するため、特性の差は下の伝達関数で確認します。', ylabel: 'レベル [dB]', xlabel: '周波数 [Hz]', group: 'f', filename: 'spectrum_before_after' });
    charts.hdb = new Chart(card(ps), { title: '伝達関数 (実測): 出力 / 入力 の振幅', desc: '実際の入力・出力から推定した H = Gxy / Gxx。補正フィルタの設計値と重なれば、処理が設計どおりに働いています。', ylabel: '振幅 [dB]', xlabel: '周波数 [Hz]', group: 'f', filename: 'transfer_magnitude' });
    charts.hph = new Chart(card(ps), { title: '伝達関数 (実測): 出力 / 入力 の位相', ylabel: '位相 [deg]', xlabel: '周波数 [Hz]', yfmt: fmt.deg, yfmtAx: fmt.degAx, group: 'f', filename: 'transfer_phase' });
    // フィルタ
    const pf = $('p-filt');
    if (D.ir) {
      charts.ir = new Chart(card(pf), { title: '補正フィルタのインパルス応答 (中心 ±4096 サンプル)', desc: '時間 0 が付加遅延を除いた中心。0 より前の成分は位相補正に伴うプリリンギングで、出力では遅延ぶん打ち消されて入力と時間が揃います。', xlog: false, ylabel: '振幅', xlabel: '時間 [ms]', xfmt: fmt.ms, xfmtAx: fmt.msAx, yfmt: fmt.num, yfmtAx: fmt.num, height: 320, filename: 'filter_ir' });
    }
    // 波形
    charts.wave = new Chart(card($('p-wave')), { title: '波形エンベロープ: 補正前 vs 補正後', desc: '時間方向に最小〜最大でまとめて表示 (最大 1600 区間)。補正後は入力と時間を揃えてあります。', xlog: false, ylabel: '振幅 (フルスケール=1)', xlabel: '時間 [s]', xfmt: fmt.sec, xfmtAx: fmt.secAx, yfmt: fmt.num, yfmtAx: fmt.num, height: 320, filename: 'waveform' });
    info();
    refresh();
  }

  function refresh() {
    if (!D) return;
    const g = D.grid, r = D.resp, c = state.ch, S = D.spectrum[c] || D.spectrum[0];
    const m = D.meta;
    // 振幅
    charts.amp.setSeries(keep([
      { id: 'meas', name: '測定 (補正前)', x: g, y: r.measured_db, color: '--c-before' },
      { id: 'corr', name: '補正フィルタ (実特性)', x: g, y: r.correction_actual_db, color: '--c-corr' },
      { id: 'res', name: '残差 (補正後・設計上)', x: g, y: r.residual_db, color: '--c-after' },
      { id: 'tgt', name: '目標補正量 (設計値)', x: g, y: r.correction_target_db, color: '--c-target', visible: false },
    ]));
    // 位相
    const ph = [];
    const bj = state.phase === 'wrap' ? 180 : 0;
    if (r.measured_phase_deg) ph.push({ id: 'meas', name: '測定 (補正前)', x: g, y: phaseView(r.measured_phase_deg), color: '--c-before', breakJump: bj });
    ph.push({ id: 'corr', name: '補正フィルタ (実特性)', x: g, y: phaseView(r.correction_actual_phase_deg), color: '--c-corr', breakJump: bj });
    if (r.residual_phase_deg) ph.push({ id: 'res', name: '残差 (補正後・設計上)', x: g, y: phaseView(r.residual_phase_deg), color: '--c-after', breakJump: bj });
    ph.push({ id: 'tgt', name: '目標補正量 (設計値)', x: g, y: phaseView(r.correction_target_phase_deg), color: '--c-target', visible: false, breakJump: bj });
    Object.assign(charts.pha.o, state.phase === 'wrap' ? { ymin: -180, ymax: 180 } : { ymin: undefined, ymax: undefined });
    charts.pha.setSeries(keep(ph));
    // スペクトル
    if (S) {
      charts.spec.setSeries([
        { id: 'in', name: '補正前 (入力)', x: g, y: smoothPower(S.in_db, state.smooth), color: '--c-before' },
        { id: 'out', name: '補正後 (出力)', x: g, y: smoothPower(S.out_db, state.smooth), color: '--c-after' },
      ]);
      charts.hdb.setSeries([
        { id: 'h', name: '実測 出力/入力', x: g, y: S.h_db, color: '--c-after' },
        { id: 'c', name: '補正フィルタ設計値', x: g, y: r.correction_actual_db, color: '--c-corr' },
      ]);
      Object.assign(charts.hph.o, state.phase === 'wrap' ? { ymin: -180, ymax: 180 } : { ymin: undefined, ymax: undefined });
      charts.hph.setSeries([
        { id: 'h', name: '実測 出力/入力', x: g, y: phaseView(S.h_ph), color: '--c-after', breakJump: bj },
        { id: 'c', name: '補正フィルタ設計値', x: g, y: phaseView(r.correction_actual_phase_deg), color: '--c-corr', breakJump: bj },
      ]);
    }
    if (charts.ir && D.ir) {
      charts.ir.setSeries([{ id: 'c', name: '補正インパルス応答', x: D.ir.t, y: D.ir.c, color: '--c-corr', width: 1.5 }]);
      if (!charts.ir.initialZoom) {   // 初期表示は成分のある範囲 (ピークの 0.1% 超) に合わせる。「範囲リセット」で全範囲
        let pk = 0, ext = 0; D.ir.c.forEach((v) => { pk = Math.max(pk, Math.abs(v)); });
        D.ir.c.forEach((v, i) => { if (Math.abs(v) > pk * 1e-3) ext = Math.max(ext, Math.abs(D.ir.t[i])); });
        ext = Math.max(ext * 1.4, 1); charts.ir.initialZoom = true; charts.ir.xr = [-ext, ext]; charts.ir.draw();
      }
    }
    const W = D.wave[c] || D.wave[0];
    if (W) charts.wave.setSeries([
      { id: 'in', name: '補正前', type: 'band', x: W.t, y: W.in_max, y2: W.in_min, color: '--c-before' },
      { id: 'out', name: '補正後', type: 'band', x: W.t, y: W.out_max, y2: W.out_min, color: '--c-after' },
    ]);
    applyRange();
  }

  function applyRange() {
    const nyq = D.meta.fs / 2;
    const rg = { all: [D.grid[0], D.grid[D.grid.length - 1]], low: [20, 200], mid: [200, 2000], high: [2000, Math.min(20000, nyq)] }[state.range];
    ['amp', 'pha', 'spec', 'hdb', 'hph'].forEach((k) => { if (charts[k] && charts[k].xr) { charts[k].xr = rg.slice(); charts[k].draw(); if (charts[k].tableOpen) charts[k].buildTable(); } });
  }

  /* ---------- 情報タブ ---------- */
  function info() {
    const host = $('p-info'), m = D.meta;
    const c1 = el('div', 'card'); c1.appendChild(el('h2', null, '処理条件'));
    const dl = el('dl', 'kv'); c1.appendChild(dl);
    const kv = (k, v) => { dl.append(el('dt', null, k), el('dd', null, String(v))); };
    const yn = (b) => (b ? 'はい' : 'いいえ');
    kv('入力 WAV', m.input_wav); kv('出力 WAV', m.output_wav); kv('測定ファイル (.req)', m.req_file); kv('.req の判別結果', m.req_detected);
    kv('サンプリング周波数 / ch / 長さ', m.fs + ' Hz / ' + m.channels + ' ch / ' + (m.duration_sec || 0).toFixed(2) + ' 秒 (' + m.frames + ' サンプル)');
    kv('補正フィルタ長', m.taps + ' taps (付加遅延 ' + m.delay_samples + ' サンプル = ' + (1000 * m.delay_samples / m.fs).toFixed(2) + ' ms, 出力では打ち消し済み)');
    kv('位相方式 (要求 → 実際)', m.phase_mode_requested + ' → ' + m.phase_mode_used);
    kv('基準レベル (正規化)', yn(m.normalize) + ' / ' + (m.ref_level_db || 0).toFixed(2) + ' dB');
    kv('補正帯域', (m.fmin_used || 0).toFixed(1) + ' – ' + (m.fmax_used || 0).toFixed(1) + ' Hz, 端テーパ ' + m.edge_oct + ' oct');
    kv('補正量の上限', 'ブースト +' + m.max_boost_db + ' dB / カット −' + m.max_cut_db + ' dB');
    kv('平滑化 / 窓 / 遅延成分の除去', (m.smooth_oct ? '1/' + m.smooth_oct + ' oct' : 'なし') + ' / ' + m.window + ' / ' + (m.remove_delay ? 'する (' + (m.removed_delay_ms || 0).toFixed(3) + ' ms)' : 'しない'));
    kv('出力形式', (m.out_float ? 'float ' : 'PCM ') + m.out_bits + ' bit' + (m.dither ? ', TPDF ディザ' : '') + ', ゲイン ' + m.gain_db + ' dB');
    kv('演算経路 / 所要時間', m.simd + ' / 設計 ' + (D.timing.design_sec || 0).toFixed(2) + ' s, 畳み込み ' + (D.timing.convolve_sec || 0).toFixed(2) + ' s, 合計 ' + (D.timing.total_sec || 0).toFixed(2) + ' s');
    host.appendChild(c1);
    if ((m.req_filters || []).length) {
      const cf = el('div', 'card'); cf.appendChild(el('h2', null, '適用した EQ フィルタ (' + (m.req_role === 'correction' ? 'そのまま適用' : '合成応答を測定とみなして逆特性化') + ')'));
      const sc0 = el('div', 'tbl-scroll'), t0 = el('table', 'data'), h0 = el('tr');
      ['#', '種別', '中心周波数', 'ゲイン', 'Q'].forEach((h) => h0.appendChild(el('th', null, h)));
      const th0 = el('thead'); th0.appendChild(h0); t0.appendChild(th0); const tb0 = el('tbody');
      m.req_filters.forEach((f, i) => { const r = el('tr'); [String(i + 1), f.type, fmt.hzShort(f.fc), (f.gain >= 0 ? '+' : '') + f.gain.toFixed(2) + ' dB', String(+f.q.toFixed(4))].forEach((v) => r.appendChild(el('td', null, v))); tb0.appendChild(r); });
      t0.appendChild(tb0); sc0.appendChild(t0); cf.appendChild(sc0); host.appendChild(cf);
    }

    const c2 = el('div', 'card'); c2.appendChild(el('h2', null, 'チャンネル別 レベル'));
    const sc = el('div', 'tbl-scroll'); const t = el('table', 'data'); const hd = el('tr');
    ['ch', '入力ピーク', '出力ピーク', '入力 RMS', '出力 RMS'].forEach((h) => hd.appendChild(el('th', null, h)));
    const th = el('thead'); th.appendChild(hd); t.appendChild(th); const tb = el('tbody');
    D.stats.forEach((s) => { const r = el('tr'); [s.channel, s.in_peak_db, s.out_peak_db, s.in_rms_db, s.out_rms_db].forEach((v, i) => r.appendChild(el('td', null, i === 0 ? String(v) : (isFinite(v) ? v.toFixed(2) + ' dBFS' : '—')))); tb.appendChild(r); });
    t.appendChild(tb); sc.appendChild(t); c2.appendChild(sc); host.appendChild(c2);

    const c3 = el('div', 'card'); c3.appendChild(el('h2', null, '試聴 (補正前 / 補正後)'));
    const ab = el('div', 'ab'); c3.appendChild(ab);
    const mkAud = (label, color, name) => {
      const box = el('div'); const lab = el('div', 'lab'); const key = el('i'); key.style.borderTopColor = 'var(' + color + ')';
      lab.append(key, el('span', null, label)); box.appendChild(lab);
      const a = document.createElement('audio'); a.controls = true; a.preload = 'none';
      if (location.protocol.indexOf('http') === 0 && name) a.src = encodeURIComponent(name);
      box.appendChild(a);
      const inp = document.createElement('input'); inp.type = 'file'; inp.accept = 'audio/*,.wav'; inp.setAttribute('aria-label', label + ' の WAV を選択');
      inp.addEventListener('change', () => { if (inp.files[0]) a.src = URL.createObjectURL(inp.files[0]); });
      box.appendChild(inp); ab.appendChild(box); return a;
    };
    const aIn = mkAud('補正前 (A): ' + (m.input_wav || ''), '--c-before', m.input_wav), aOut = mkAud('補正後 (B): ' + (m.output_wav || ''), '--c-after', m.output_wav);
    const sw = el('button', 'btn', 'A/B 切替 (同じ位置から再生)'); sw.type = 'button'; sw.style.marginTop = '10px';
    const st = el('span', 'hint', ' '); st.style.marginLeft = '8px';
    sw.addEventListener('click', () => {
      const cur = !aIn.paused ? aIn : !aOut.paused ? aOut : aIn, oth = cur === aIn ? aOut : aIn;
      oth.currentTime = cur.currentTime; const was = !cur.paused; cur.pause();
      if (was || true) oth.play().catch(() => { st.textContent = ' 再生できません (WAV を選択するか、サーバー経由で開いてください)'; });
      st.textContent = ' 再生中: ' + (oth === aIn ? '補正前 (A)' : '補正後 (B)');
    });
    c3.append(sw, st);
    c3.appendChild(el('div', 'hint', 'ファイルを直接開いている場合は、上の各欄で WAV を選んでください。サーバー経由 (tools/serve.py) なら自動で読み込みます。'));
    host.appendChild(c3);

    const c4 = el('div', 'card'); c4.appendChild(el('h2', null, '処理ログ'));
    const pre = el('pre', 'log'); D.log.forEach((l) => { const line = el('div', /警告|注意/.test(l) ? 'warnline' : '', l); pre.appendChild(line); });
    c4.appendChild(pre); host.appendChild(c4);
  }

  /* ---------- UI 配線 ---------- */
  function setupTabs() {
    const tabs = [['resp', '補正特性'], ['spec', '補正前後スペクトル'], ['filt', '補正フィルタ'], ['wave', '波形'], ['info', '条件・試聴・ログ']];
    const host = $('tabs'); host.textContent = '';
    tabs.forEach(([k, label]) => {
      const b = el('button', 'tab', label); b.type = 'button'; b.setAttribute('role', 'tab'); b.id = 't-' + k; b.setAttribute('aria-selected', k === state.tab ? 'true' : 'false');
      b.addEventListener('click', () => selectTab(k)); host.appendChild(b);
    });
    host.addEventListener('keydown', (e) => {
      if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
      const i = tabs.findIndex((t) => t[0] === state.tab), n = (i + (e.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length;
      selectTab(tabs[n][0]); $('t-' + tabs[n][0]).focus();
    });
  }
  function selectTab(k) {
    state.tab = k;
    document.querySelectorAll('.tab').forEach((b) => b.setAttribute('aria-selected', b.id === 't-' + k ? 'true' : 'false'));
    document.querySelectorAll('.panel').forEach((p) => { p.hidden = p.id !== 'p-' + k; });
    Chart.redrawAll();
  }

  function load(json, label) {
    D = normalize(json);
    if (!D.grid.length || !D.resp.measured_db) { alert('このファイルは eqcorrect の解析 JSON ではないようです。'); D = null; return; }
    $('empty').hidden = true; $('app').hidden = false;
    $('subtitle').textContent = (D.meta.input_wav || '') + ' → ' + (D.meta.output_wav || '') + '  |  ' + (D.meta.req_file || '') + (label ? '  (' + label + ')' : '');
    const sel = $('fCh'); sel.textContent = '';
    const nch = Math.max(D.spectrum.length, 1);
    for (let i = 0; i < nch; i++) { const o = el('option', null, 'ch ' + (i + 1)); o.value = String(i); sel.appendChild(o); }
    state.ch = 0; sel.value = '0';
    tiles(); setupTabs(); build(); selectTab(state.tab);
  }
  function parseText(text) {
    const t = text.replace(/^﻿/, '').trim();
    const body = t.startsWith('window.EQ_RESULT') ? t.replace(/^window\.EQ_RESULT\s*=\s*/, '').replace(/;\s*$/, '') : t;
    return JSON.parse(body);
  }
  function readFile(f) {
    const rd = new FileReader();
    rd.onload = () => { try { load(parseText(String(rd.result)), f.name); } catch (e) { alert('読み込みに失敗しました: ' + e.message); } };
    rd.readAsText(f);
  }

  function init() {
    $('btnOpen').addEventListener('click', () => $('fileJson').click());
    $('fileJson').addEventListener('change', (e) => { if (e.target.files[0]) readFile(e.target.files[0]); });
    ['dragover', 'dragenter'].forEach((ev) => window.addEventListener(ev, (e) => { e.preventDefault(); $('empty').classList.add('drag'); }));
    ['dragleave', 'drop'].forEach((ev) => window.addEventListener(ev, (e) => { e.preventDefault(); $('empty').classList.remove('drag'); }));
    window.addEventListener('drop', (e) => { const f = e.dataTransfer && e.dataTransfer.files[0]; if (f) readFile(f); });
    $('fCh').addEventListener('change', (e) => { state.ch = +e.target.value; refresh(); });
    $('fRange').addEventListener('change', (e) => { state.range = e.target.value; applyRange(); });
    $('fPhase').addEventListener('change', (e) => { state.phase = e.target.value; refresh(); });
    $('fSmooth').addEventListener('change', (e) => { state.smooth = +e.target.value; refresh(); });

    // テーマ: 自動 → ライト → ダーク
    const modes = ['auto', 'light', 'dark'], names = { auto: '自動', light: 'ライト', dark: 'ダーク' };
    let mode = 'auto'; try { mode = localStorage.getItem('eq-theme') || 'auto'; } catch (_) { /* 保存できない環境 */ }
    const applyTheme = () => { if (mode === 'auto') document.documentElement.removeAttribute('data-theme'); else document.documentElement.setAttribute('data-theme', mode); $('btnTheme').textContent = 'テーマ: ' + names[mode]; requestAnimationFrame(Chart.redrawAll); };
    $('btnTheme').addEventListener('click', () => { mode = modes[(modes.indexOf(mode) + 1) % 3]; try { localStorage.setItem('eq-theme', mode); } catch (_) { /* noop */ } applyTheme(); });
    if (window.matchMedia) window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => Chart.redrawAll());
    applyTheme();

    if (window.EQ_RESULT) { load(window.EQ_RESULT, 'result.js'); return; }
    if (location.protocol.indexOf('http') === 0) {
      fetch('result.json', { cache: 'no-store' }).then((r) => { if (!r.ok) throw new Error(r.status); return r.json(); }).then((j) => load(j, 'result.json')).catch(() => { $('empty').hidden = false; });
    } else $('empty').hidden = false;
  }
  init();
})();
