import java.io.*;
import java.util.*;

/** テスト用: テキスト .req (freq \t dB \t deg) から、様々な形の Java シリアライズ .req を作る。 */
public class MakeReq {
    static class EqData implements Serializable {
        private static final long serialVersionUID = 1L;
        String name = "測定 test"; int sampleRate = 48000;
        double[] frequency, gainDb, phaseDeg;
    }
    static class EqLists implements Serializable {
        private static final long serialVersionUID = 2L;
        ArrayList<Double> freqs = new ArrayList<>(), gains = new ArrayList<>(), phases = new ArrayList<>();
    }
    static class EqMatrix implements Serializable {
        private static final long serialVersionUID = 3L;
        String title = "matrix"; double[][] data;
    }
    static class Point implements Serializable {
        private static final long serialVersionUID = 4L;
        double hz, db, deg;
        Point(double a, double b, double c) { hz = a; db = b; deg = c; }
    }
    static class EqPoints implements Serializable {
        private static final long serialVersionUID = 5L;
        List<Point> points = new ArrayList<>(); float scale = 1f;
    }
    static class Opaque implements Serializable {
        private static final long serialVersionUID = 6L;
        double[] a, b, c;
    }
    static class Wrapper implements Serializable {
        private static final long serialVersionUID = 7L;
        String comment = "wrapped"; int version = 3; EqData inner; long stamp = 1700000000000L;
        EqData same; // 同一オブジェクトを二重参照 (TC_REFERENCE の検証)
    }
    static class EqIR implements Serializable {
        private static final long serialVersionUID = 8L;
        int sampleRate = 48000; double[] impulse;
    }

    static void save(String path, Object o) throws IOException {
        try (ObjectOutputStream os = new ObjectOutputStream(new FileOutputStream(path))) { os.writeObject(o); }
    }

    public static void main(String[] a) throws Exception {
        String src = a[0], dir = a[1];
        List<double[]> rows = new ArrayList<>();
        for (String line : new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get(src)), "UTF-8").split("\n")) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] t = line.split("\\s+");
            rows.add(new double[]{Double.parseDouble(t[0]), Double.parseDouble(t[1]), Double.parseDouble(t[2])});
        }
        int n = rows.size();
        EqData d = new EqData();
        d.frequency = new double[n]; d.gainDb = new double[n]; d.phaseDeg = new double[n];
        EqLists l = new EqLists(); EqMatrix m = new EqMatrix(); m.data = new double[n][];
        EqPoints p = new EqPoints(); Opaque o = new Opaque(); o.a = d.frequency; o.b = d.gainDb; o.c = d.phaseDeg;
        for (int i = 0; i < n; i++) {
            double[] r = rows.get(i);
            d.frequency[i] = r[0]; d.gainDb[i] = r[1]; d.phaseDeg[i] = r[2];
            l.freqs.add(r[0]); l.gains.add(r[1]); l.phases.add(r[2]);
            m.data[i] = r; p.points.add(new Point(r[0], r[1], r[2]));
        }
        save(dir + "/java_arrays.req", d);
        save(dir + "/java_lists.req", l);
        save(dir + "/java_matrix.req", m);
        save(dir + "/java_points.req", p);
        save(dir + "/java_opaque.req", o);
        Wrapper w = new Wrapper(); w.inner = d; w.same = d; save(dir + "/java_wrapped.req", w);
        HashMap<String, Object> hm = new HashMap<>();
        hm.put("freq", d.frequency); hm.put("gain", d.gainDb); hm.put("phase", d.phaseDeg); hm.put("sampleRate", 48000); hm.put("name", "map");
        save(dir + "/java_hashmap.req", hm);
        if (a.length > 2) { // IR (1列テキスト) → Java
            List<Double> ir = new ArrayList<>();
            for (String line : new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get(a[2])), "UTF-8").split("\n"))
                if (!line.trim().isEmpty()) ir.add(Double.parseDouble(line.trim()));
            EqIR e = new EqIR(); e.impulse = new double[ir.size()];
            for (int i = 0; i < ir.size(); i++) e.impulse[i] = ir.get(i);
            save(dir + "/java_ir.req", e);
        }
        System.out.println("wrote java_*.req (" + n + " points)");
    }
}
