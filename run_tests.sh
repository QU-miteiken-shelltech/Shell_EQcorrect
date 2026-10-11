#!/usr/bin/env bash
# 全テスト: ビルド → テストデータ生成 → 精度検証 → 形式/端のケース → (任意) ビューア
# 必要: g++, python3 + numpy + scipy, java/javac (Java形式の.req生成。無ければその部分を省略)
#       ビューア試験は node + playwright が必要 (無ければ省略)
set -u
cd "$(dirname "$0")"
make || exit 1
D=tests/data; O=tests/out; mkdir -p $O
python3 tools/make_testdata.py $D || exit 1
if command -v javac >/dev/null; then
  mkdir -p tests/jbuild && javac -encoding UTF-8 -d tests/jbuild tools/MakeReq.java tools/rew_stub/roomeqwizard/*.java 2>/dev/null \
    && java -cp tests/jbuild MakeReq $D/eq_text.req $D /dev/stdin < $D/eq_ir.req 2>/dev/null | tail -1 \
    && java -cp tests/jbuild roomeqwizard.MakeRew $D 2>/dev/null | tail -1
fi
fail=0
run() { # 名前 .req [verify追加オプション] -- [eqcorrect追加オプション]
  local n=$1 r=$2; shift 2; local va=(); while [ $# -gt 0 ] && [ "$1" != "--" ]; do va+=("$1"); shift; done; [ $# -gt 0 ] && shift
  ./eqcorrect $D/raw.wav $D/$r.req -o $O/$n.wav -j $O/$n.json "$@" >/dev/null 2>&1 || { echo "  FAIL $n (処理エラー)"; fail=1; return; }
  python3 tools/verify.py $D $O/$n.wav $O/$n.json --tag $n "${va[@]}" | grep -E "ch1|OK|NG" || fail=1
}
echo "== 精度検証 (帯域内で 元音声×基準ゲイン に戻るか)"
run text_full eq_text --min-gain-snr 45
run ir_text eq_ir --min-gain-snr 40
for f in java_arrays java_lists java_points java_wrapped java_hashmap java_ir; do [ -f $D/$f.req ] && run $f $f --min-gain-snr 40; done
[ -f $D/java_matrix.req ] && run java_matrix java_matrix --min-gain-snr 45 -- --java-map 'freq=EqMatrix.data[:,0],mag=EqMatrix.data[:,1],phase=EqMatrix.data[:,2]'
echo "== REW のEQフィルタ形式 (測定ではなく適用するEQ)"
[ -f $D/rew_pk.req ] && ./eqcorrect $D/raw.wav $D/rew_pk.req -o $O/rew_pk.wav -j $O/rew_pk.json >/dev/null 2>&1 && echo "  rew_pk 処理OK (IIRとの照合は下の test_formats で実施)" || { echo "  FAIL rew_pk"; fail=1; }
echo "== 位相なし / 最小位相 / 位相補正なし (振幅偏差)"
run nophase eq_text_nophase --mag-only --max-dev 0.3
run phase_min eq_text --mag-only --max-dev 0.3 -- --phase min
run phase_linear eq_text --mag-only --max-dev 0.3 -- --phase linear
echo "== 形式・チャンネル・端のケース・畳み込みの独立検証"
python3 tests/test_formats.py | grep -E "FAIL|結果" || fail=1
if command -v node >/dev/null && [ -d "${PW_PATH:-/opt/npm-tools/node_modules/playwright}" ]; then
  echo "== ビューア (Chromium)"
  node tests/test_viewer.js | grep -E "FAIL|結果" || fail=1
fi
[ $fail = 0 ] && echo "ALL OK" || { echo "失敗あり"; exit 1; }
