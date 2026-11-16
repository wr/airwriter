# SPDX-License-Identifier: GPL-3.0-or-later
"""signin.py against preview.py's board with a password (W-1156).

    python3 -m unittest tools/webui/test_signin.py
"""
import http.cookiejar, pathlib, subprocess, sys, tempfile, time, unittest, urllib.error, urllib.request

HERE = pathlib.Path(__file__).parent
PORT = 8098
URL = "http://127.0.0.1:%d" % PORT


def status_with(jar_path):
    jar = http.cookiejar.MozillaCookieJar(str(jar_path))
    jar.load(ignore_discard=True, ignore_expires=True)
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
    try:
        return op.open(URL + "/api/status", timeout=5).status
    except urllib.error.HTTPError as e:
        return e.code


class SignIn(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = subprocess.Popen([sys.executable, str(HERE / "preview.py"), "--port", str(PORT),
                                    "--state", "signed-out"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(50):
            try:
                urllib.request.urlopen(URL + "/api/auth/nonce", timeout=1)
                break
            except OSError:
                time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        cls.srv.terminate()
        cls.srv.wait()

    def run_signin(self, password, jar):
        return subprocess.run([sys.executable, str(HERE / "signin.py"), URL, "--jar", str(jar),
                               "--password-stdin"], input=password + "\n",
                              capture_output=True, text=True, timeout=30)

    def test_status_is_refused_without_signing_in(self):
        with tempfile.TemporaryDirectory() as d:
            jar = pathlib.Path(d) / "jar.txt"
            jar.write_text("# Netscape HTTP Cookie File\n")
            self.assertEqual(status_with(jar), 401)

    def test_right_password_writes_a_jar_curl_can_use(self):
        with tempfile.TemporaryDirectory() as d:
            jar = pathlib.Path(d) / "jar.txt"
            r = self.run_signin("k7m2x9qp", jar)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(status_with(jar), 200)
            self.assertEqual(jar.stat().st_mode & 0o777, 0o600)
            self.assertIn("aw\tpreview", jar.read_text())

    def test_wrong_password_says_so(self):
        with tempfile.TemporaryDirectory() as d:
            jar = pathlib.Path(d) / "jar.txt"
            r = self.run_signin("k7m2x9qq", jar)
            self.assertEqual(r.returncode, 1)
            self.assertIn("Wrong password.", r.stderr)
            self.assertFalse(jar.exists())


if __name__ == "__main__":
    unittest.main()
