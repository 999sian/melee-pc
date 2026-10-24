#!/usr/bin/env python3
"""Check the credits table's real allocation and clear on native pointers."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class StaffRollInitialization(unittest.TestCase):
    def test_entire_native_table_is_zeroed(self):
        source = (ROOT / 'src/melee/gm/gmstaffroll.c').read_text()
        entry = re.search(r'static struct gmstaffroll_staffInfo_t \{.*?\}\* staffInfo;',
                          source, re.S).group()
        legacy = re.search(r'struct staffInfo_t \{.*?\};', source, re.S).group()
        allocation = re.search(r'staffInfo = HSD_MemAlloc\((.*?)\);', source).group(1)
        clearing = re.search(r'memzero\(staffInfo, (.*?)\);', source).group(1)
        with tempfile.TemporaryDirectory(prefix='staffroll-init-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text('''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef void HSD_Text;
''' + legacy + '\n' + entry + '''
int main(void) {
    size_t size = ''' + allocation + ''';
    staffInfo = malloc(size);
    assert(staffInfo);
    memset(staffInfo, 0xa5, size);
    memset(staffInfo, 0, ''' + clearing + ''');
    for (size_t i = 0; i < size; ++i)
        assert(((unsigned char*)staffInfo)[i] == 0);
    free(staffInfo);
    return 0;
}
''')
            subprocess.run(['cc', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
