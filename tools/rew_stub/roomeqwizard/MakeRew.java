package roomeqwizard;

import java.io.*;

/** テスト用: REW のフィルタファイル (.req) と同じ構造のファイルを作る。使い方: java roomeqwizard.MakeRew 出力フォルダ */
public class MakeRew {
    static void save(String path, Filter[] a) throws IOException {
        try (ObjectOutputStream os = new ObjectOutputStream(new FileOutputStream(path))) {
            os.writeObject("TMreq Filters File:Equaliser:Generic"); os.writeObject("Notes:test"); os.writeObject(a);
        }
    }
    static Filter[] pad(Filter[] head, int total) {
        Filter[] a = new Filter[total];
        for (int i = 0; i < total; i++) a[i] = i < head.length ? head[i] : Filter.none(100 + i);
        return a;
    }
    public static void main(String[] args) throws Exception {
        String d = args[0];
        // 実ファイル (null_EQ_C.req) と同じ 5 個の PK + 未使用スロット
        Filter[] five = { Filter.pk(57.5, -10.1, 2.539, 48000, true), Filter.pk(86.5, -8.2, 4.299, 48000, true), Filter.pk(142, -11.3, 1.599, 48000, true),
                          Filter.pk(277, -6.1, 1.504, 48000, true), Filter.pk(10769, -3.7, 4.155, 48000, true) };
        save(d + "/rew_pk.req", pad(five, 28));
        save(d + "/rew_none.req", pad(new Filter[0], 28));                                             // 有効なフィルタなし
        save(d + "/rew_disabled.req", pad(new Filter[] { five[0], Filter.pk(142, -11.3, 1.599, 48000, false) }, 28)); // 2 つ目は無効
        save(d + "/rew_only_first.req", pad(new Filter[] { five[0] }, 28));                            // 上と同じ結果になるはず
        save(d + "/rew_ls.req", pad(new Filter[] { five[0], Filter.other(Filter.FilterTypes.LS, 100, 3, 0.7) }, 28)); // 未対応種別
        save(d + "/rew_boost.req", pad(new Filter[] { Filter.pk(1000, 18, 2, 48000, true) }, 8));       // ブースト (上限を適用しないことの確認)
        // マルチチャンネル用: チャンネルごとに別のEQ (ファイル名末尾がチャンネル名)
        new File(d, "mc").mkdirs();
        save(d + "/mc/null_EQ_L.req",   pad(new Filter[] { Filter.pk(100, -6, 2, 48000, true), Filter.pk(3000, 3, 1.5, 48000, true) }, 28));
        save(d + "/mc/null_EQ_R.req",   pad(new Filter[] { Filter.pk(120, -4, 3, 48000, true) }, 28));
        save(d + "/mc/null_EQ_C.req",   pad(five, 28));
        save(d + "/mc/null_EQ_LFE.req", pad(new Filter[] { Filter.pk(40, -8, 2, 48000, true), Filter.pk(80, 4, 3, 48000, true) }, 28));
        save(d + "/mc/null_EQ_Lrs.req", pad(new Filter[] { Filter.pk(500, -5, 1, 48000, true) }, 28));
        save(d + "/mc/null_EQ_Rrs.req", pad(new Filter[] { Filter.pk(700, 5, 1, 48000, true) }, 28));
        System.out.println("wrote rew_*.req");
    }
}
