// ビューアの動作確認 (Playwright + Chromium)。使い方: node tests/test_viewer.js
const { chromium } = require(process.env.PW_PATH || '/opt/npm-tools/node_modules/playwright');
const path = require('path');
const fs = require('fs');
const ROOT = path.resolve(__dirname, '..');
const shots = path.join(ROOT, 'tests', 'shots');
fs.mkdirSync(shots, { recursive: true });
// 試験に使う結果ファイル (run_tests.sh が tests/out に作る)。web/result.js として一時配置し、終了時に削除する
const OUT = path.join(ROOT, 'tests', 'out'), RESULT_JS = path.join(ROOT, 'web', 'result.js');
for (const f of ['text_full.js', 'java_arrays.json', 'nophase.json', 'nophase.js', 'rew_pk.json']) {
  if (!fs.existsSync(path.join(OUT, f))) { console.error('tests/out/' + f + ' がありません。先に ./run_tests.sh の精度検証部分を実行してください'); process.exit(2); }
}
fs.copyFileSync(path.join(OUT, 'text_full.js'), RESULT_JS);
process.on('exit', () => { try { fs.unlinkSync(RESULT_JS); } catch (_) { /* 既に無い */ } });
let fails = 0;
const ok = (name, cond, detail = '') => { console.log((cond ? '  PASS ' : '  FAIL ') + name + (detail ? '  - ' + detail : '')); if (!cond) fails++; };

(async () => {
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 }, colorScheme: 'light' });
  const page = await ctx.newPage();
  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push('pageerror: ' + e.message));

  console.log('== file:// で直接開く (result.js を自動読み込み)');
  await page.goto('file://' + path.join(ROOT, 'web', 'index.html'));
  await page.waitForSelector('#app:not([hidden])', { timeout: 5000 });
  ok('result.js から自動で表示された', await page.isVisible('#app'));
  ok('タイルが4つ', (await page.locator('.tile').count()) === 4);
  ok('タブが5つ', (await page.locator('.tab').count()) === 5);
  ok('補正特性タブに 2 つのグラフ', (await page.locator('#p-resp canvas').count()) === 2);
  await page.screenshot({ path: path.join(shots, '1_resp_light.png'), fullPage: true });

  console.log('== ホバー (十字線 + 全系列ツールチップ)');
  const cv = page.locator('#p-resp canvas').first();
  const box = await cv.boundingBox();
  await page.mouse.move(box.x + box.width * 0.5, box.y + 120);
  const tip = page.locator('#p-resp .tip').first();
  ok('ツールチップが表示される', await tip.isVisible());
  const tipText = await tip.innerText();
  ok('ツールチップに全系列の名前 (測定/補正フィルタ/残差)', /測定/.test(tipText) && /補正フィルタ/.test(tipText) && /残差/.test(tipText), tipText.replace(/\n/g, ' | '));
  ok('値に dB 単位', /dB/.test(tipText));
  await page.screenshot({ path: path.join(shots, '2_hover.png'), clip: { x: box.x - 10, y: box.y - 40, width: box.width + 20, height: box.height + 130 } });

  console.log('== ドラッグで範囲拡大 / ダブルクリックでリセット / 軸リンク');
  const beforeTicks = await page.evaluate(() => Chart.all[0].xr.slice());
  await page.mouse.move(box.x + box.width * 0.3, box.y + 100); await page.mouse.down();
  await page.mouse.move(box.x + box.width * 0.6, box.y + 100, { steps: 6 }); await page.mouse.up();
  const zoomed = await page.evaluate(() => [Chart.all[0].xr.slice(), Chart.all[1].xr.slice()]);
  ok('ドラッグで範囲が狭まる', zoomed[0][1] / zoomed[0][0] < beforeTicks[1] / beforeTicks[0] / 2, JSON.stringify(zoomed[0].map((v) => +v.toFixed(1))));
  ok('同じグループの位相グラフにも範囲が連動', Math.abs(zoomed[1][0] - zoomed[0][0]) < 1e-6);
  await page.mouse.dblclick(box.x + box.width * 0.5, box.y + 100);
  const reset = await page.evaluate(() => Chart.all[0].xr.slice());
  ok('ダブルクリックで全範囲に戻る', Math.abs(reset[0] - beforeTicks[0]) < 1e-9 && Math.abs(reset[1] - beforeTicks[1]) < 1e-9);

  console.log('== キーボード操作 / 凡例 / 表ビュー');
  await cv.focus(); await page.keyboard.press('ArrowRight'); await page.keyboard.press('ArrowRight');
  ok('矢印キーでもツールチップが出る', await tip.isVisible());
  const leg = page.locator('#p-resp .legend button').first();
  await leg.click();
  ok('凡例クリックで系列を非表示 (aria-pressed=false)', (await leg.getAttribute('aria-pressed')) === 'false');
  await leg.click();
  await page.locator('#p-resp button:has-text("表で見る")').first().click();
  const rows = await page.locator('#p-resp table.data tbody tr').count();
  ok('表ビューに行が出る (最大400行)', rows > 10 && rows <= 400, rows + ' 行');
  const cell = await page.locator('#p-resp table.data tbody tr').nth(5).innerText();
  ok('表の値が数値で出る', /\d/.test(cell), cell.replace(/\s+/g, ' '));
  const dl = page.waitForEvent('download');
  await page.locator('#p-resp button:has-text("CSV")').first().click();
  const d = await dl; ok('CSV がダウンロードされる', /\.csv$/.test(d.suggestedFilename()), d.suggestedFilename());

  console.log('== フィルタ');
  await page.selectOption('#fRange', 'mid');
  const mid = await page.evaluate(() => Chart.all[0].xr.slice());
  ok('周波数範囲 200Hz–2kHz', mid[0] === 200 && mid[1] === 2000, JSON.stringify(mid));
  await page.selectOption('#fRange', 'all');
  await page.selectOption('#fPhase', 'unwrap');
  const unw = await page.evaluate(() => { const c = Chart.all[1]; return [c.o.ymin, c.series[0].y.some((v) => v < -190)]; });
  ok('位相を連続表示にするとY軸固定が外れ、-180°を超える値が出る', unw[0] === undefined && unw[1] === true);
  await page.selectOption('#fPhase', 'wrap');

  console.log('== 各タブ');
  for (const [id, canvases, name] of [['spec', 3, '補正前後スペクトル'], ['filt', 1, '補正フィルタ'], ['wave', 1, '波形']]) {
    await page.click('#t-' + id);
    ok(name + ': グラフ ' + canvases + ' 個が表示', (await page.locator('#p-' + id + ' canvas:visible').count()) === canvases);
    await page.waitForTimeout(150);
    await page.screenshot({ path: path.join(shots, '3_' + id + '_light.png'), fullPage: true });
  }
  await page.click('#t-spec'); await page.selectOption('#fSmooth', '6');
  const sm = await page.evaluate(() => { const s = Chart.all.find((c) => c.o.title.startsWith('平均パワー')).series[0].y; let d = 0, n = 0; for (let i = 1; i < s.length; i++) if (isFinite(s[i]) && isFinite(s[i - 1])) { d += Math.abs(s[i] - s[i - 1]); n++; } return d / n; });
  await page.selectOption('#fSmooth', '0');
  const raw = await page.evaluate(() => { const s = Chart.all.find((c) => c.o.title.startsWith('平均パワー')).series[0].y; let d = 0, n = 0; for (let i = 1; i < s.length; i++) if (isFinite(s[i]) && isFinite(s[i - 1])) { d += Math.abs(s[i] - s[i - 1]); n++; } return d / n; });
  ok('1/6oct 平滑化でスペクトルの凸凹が小さくなる', sm < raw, sm.toFixed(3) + ' < ' + raw.toFixed(3));
  await page.click('#t-info');
  ok('情報タブ: 条件・ログ・audio が表示', (await page.locator('#p-info audio').count()) === 2 && (await page.locator('pre.log div').count()) > 3);
  await page.screenshot({ path: path.join(shots, '4_info_light.png'), fullPage: true });

  console.log('== ダークテーマ (手動切替)');
  await page.click('#btnTheme'); await page.click('#btnTheme');
  ok('テーマ=ダーク', (await page.getAttribute('html', 'data-theme')) === 'dark');
  await page.click('#t-resp'); await page.waitForTimeout(200);
  const bg = await page.evaluate(() => getComputedStyle(document.body).backgroundColor);
  ok('背景がダーク', bg === 'rgb(13, 13, 13)', bg);
  await page.screenshot({ path: path.join(shots, '5_resp_dark.png'), fullPage: true });

  console.log('== 空状態 (result.js なし) とドロップ読み込み');
  const page2 = await ctx.newPage();
  const errs2 = []; page2.on('pageerror', (e) => errs2.push(e.message));
  await page2.route('**/result.js', (r) => r.abort());
  await page2.goto('file://' + path.join(ROOT, 'web', 'index.html'));
  await page2.waitForSelector('#empty:not([hidden])');
  ok('result.js がなければ案内が出る', await page2.isVisible('#empty'));
  await page2.setInputFiles('#fileJson', path.join(ROOT, 'tests', 'out', 'java_arrays.json'));
  await page2.waitForSelector('#app:not([hidden])');
  ok('ファイル選択 (JSON) から読み込める', (await page2.locator('.tile').count()) === 4);
  await page2.setInputFiles('#fileJson', path.join(ROOT, 'tests', 'out', 'nophase.js'));
  await page2.waitForTimeout(300);
  ok('result.js 形式のファイルも読める', (await page2.locator('#subtitle').innerText()).includes('nophase.js'));
  ok('ページエラーなし', errs2.length === 0, errs2.join(' / '));

  console.log('== 位相データなし .req の結果 / モノラル相当');
  await page2.setInputFiles('#fileJson', path.join(ROOT, 'tests', 'out', 'nophase.json'));
  await page2.waitForTimeout(300);
  const ph = await page2.evaluate(() => Chart.all[1].series.map((s) => s.name));
  ok('位相なしでは測定位相/残差の系列を出さない', !ph.some((n) => /測定|残差/.test(n)), ph.join(', '));

  console.log('== REW の EQ フィルタ結果 (測定データなし)');
  await page2.setInputFiles('#fileJson', path.join(ROOT, 'tests', 'out', 'rew_pk.json'));
  await page2.waitForTimeout(400);
  const t1 = await page2.locator('.tile').first().innerText();
  ok('先頭タイルが「適用した EQ  5 個」', /適用した EQ/.test(t1) && /5 個/.test(t1), t1.replace(/\n/g, ' '));
  const names = await page2.evaluate(() => Chart.all[0].series.map((s) => s.name));
  ok('振幅グラフに測定/残差の系列を出さない (補正フィルタのみ)', !names.some((n) => /測定|残差/.test(n)) && names.some((n) => /補正フィルタ/.test(n)), names.join(', '));
  const rng = (await page2.locator('.tile').nth(2).innerText()).replace(/\n/g, ' ');
  ok('補正量の範囲に「+-」のような符号の重複がない', !/\+-|−−|\+\+/.test(rng), rng);
  await page2.click('#t-info');
  ok('情報タブに EQ フィルタ一覧 (5 行)', (await page2.locator('#p-info table.data').first().locator('tbody tr').count()) === 5);
  await page2.screenshot({ path: path.join(shots, '6_rew_info.png'), fullPage: true });
  ok('REW 表示でページエラーなし', errs2.length === 0, errs2.join(' / '));

  ok('コンソールエラー/ページエラーなし (メインページ)', errors.length === 0, errors.join(' / '));
  await browser.close();
  console.log('\n結果: ' + (fails ? '失敗 ' + fails + ' 件' : '全テスト合格'));
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(2); });
