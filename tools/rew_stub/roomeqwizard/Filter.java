package roomeqwizard;

import java.io.Serializable;

/** テスト用の最小スタブ。実物の REW の roomeqwizard.Filter と同じクラス名/フィールド名/列挙名で直列化される
 *  (eqcorrect のパーサーはクラスファイルを必要とせず、ストリーム内の名前だけを使う)。 */
public class Filter implements Serializable {
    private static final long serialVersionUID = 1L;
    public enum FilterTypes { NONE, PK, LS, HS }
    double Q, a0 = 1, a1, a2, b0 = 1, b1, b2, fc, gain, sampleRate = 48000;
    boolean enabled = true;
    int control = 1;
    FilterTypes filterType = FilterTypes.NONE;

    /** RBJ ピーキング。係数は REW と同様に未正規化 (a0 != 1) で保存する */
    static Filter pk(double fc, double gain, double q, double fs, boolean enabled) {
        Filter f = new Filter(); f.filterType = FilterTypes.PK; f.fc = fc; f.gain = gain; f.Q = q; f.sampleRate = fs; f.enabled = enabled;
        double A = Math.pow(10, gain / 40), w0 = 2 * Math.PI * fc / fs, al = Math.sin(w0) / (2 * q), c = Math.cos(w0);
        f.b0 = 1 + al * A; f.b1 = -2 * c; f.b2 = 1 - al * A; f.a0 = 1 + al / A; f.a1 = -2 * c; f.a2 = 1 - al / A;
        return f;
    }
    static Filter none(double fc) { Filter f = new Filter(); f.fc = fc; f.Q = 10; return f; }
    static Filter other(FilterTypes t, double fc, double gain, double q) { Filter f = pk(fc, gain, q, 48000, true); f.filterType = t; return f; }
}
