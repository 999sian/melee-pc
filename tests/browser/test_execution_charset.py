import pathlib, sys, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / 'tools/browser'))
from execution_charset import cp932_literals

class CharsetTest(unittest.TestCase):
    def test_native_name_bytes(self):
        self.assertEqual(cp932_literals('"あＪ－A"'), '"' + ''.join('\\%03o' % b for b in 'あＪ－'.encode('cp932')) + 'A"')
    def test_preserves_escapes_and_unicode_literals(self):
        source = r'"hello\n\\\"" u8"あ" L"あ"'
        self.assertEqual(cp932_literals(source), source)

if __name__ == '__main__': unittest.main()
