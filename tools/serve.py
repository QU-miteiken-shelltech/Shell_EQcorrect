#!/usr/bin/env python3
"""ビューア用の簡易ローカルサーバー (標準ライブラリのみ)。

  python3 tools/serve.py [--data 出力フォルダ] [--port 8000] [--no-open]

・web/ をルートとして配信します。--data を指定すると、web/ に無いファイル
  (result.json や補正前後の WAV) をそのフォルダから探して配信します。
・127.0.0.1 にだけバインドします (外部には公開されません)。
・サーバー経由なら、result.json の自動読み込みと補正前後 WAV の試聴 (A/B) が使えます。
  サーバーなしで使う場合は web/index.html を直接開き、JSON をドロップするか、result.js を web/ に置きます。
"""
import argparse, http.server, os, socketserver, threading, webbrowser

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=None, help="result.json / WAV のあるフォルダ")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--no-open", action="store_true")
    a = ap.parse_args()
    web = os.path.abspath(ROOT)
    data = os.path.abspath(a.data) if a.data else None

    class H(http.server.SimpleHTTPRequestHandler):
        def translate_path(self, path):
            p = super().translate_path(path)
            rel = os.path.relpath(p, os.getcwd())
            if data and not os.path.exists(p):
                alt = os.path.join(data, rel)
                if os.path.commonpath([data, os.path.abspath(alt)]) == data and os.path.exists(alt):
                    return alt
            return p

        def end_headers(self):
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

    os.chdir(web)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", a.port), H) as srv:
        url = "http://127.0.0.1:%d/index.html" % a.port
        print("配信中: %s   (Ctrl+C で終了)" % url)
        if not a.no_open:
            threading.Timer(0.5, lambda: webbrowser.open(url)).start()
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print("\n終了します")


if __name__ == "__main__":
    main()
