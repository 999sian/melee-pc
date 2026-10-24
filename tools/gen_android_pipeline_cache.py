#!/usr/bin/env python3
"""Export device-built GX configurations, never GPU binaries, as an Android starter.

Runtime accepts only the current configuration version and structure size. Record
with the current renderer on real Android hardware before regenerating this file.
"""
import argparse
import gzip
from pathlib import Path
import sqlite3
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def export(source, output):
    with sqlite3.connect(f'{Path(source).resolve().as_uri()}?mode=ro', uri=True) as src:
        rows = src.execute('''SELECT c.type,c.hash,c.config_version,c.config_size,c.config,c.first_frame_used
            FROM pipeline_cache c JOIN pipeline_device d ON c.type=d.type AND c.hash=d.hash
            WHERE c.type=1 ORDER BY c.hash''').fetchall()
    if not rows:
        raise ValueError('No device-built GX configurations to export')
    with tempfile.TemporaryDirectory() as tmp:
        db = Path(tmp)/'starter.db'
        with sqlite3.connect(db) as dst:
            dst.executescript('''CREATE TABLE aurora_schema(value INTEGER);
                INSERT INTO aurora_schema VALUES(1);
                CREATE TABLE melee_portable_starter(value INTEGER);
                INSERT INTO melee_portable_starter VALUES(1);
                CREATE TABLE pipeline_cache(type INTEGER NOT NULL, hash INTEGER NOT NULL,
                config_version INTEGER NOT NULL, config_size INTEGER NOT NULL, config BLOB NOT NULL,
                first_frame_used INTEGER NOT NULL, PRIMARY KEY(type,hash));''')
            dst.executemany('INSERT INTO pipeline_cache VALUES(?,?,?,?,?,?)', rows)
        Path(output).write_bytes(gzip.compress(db.read_bytes(), mtime=0))
    return len(rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', help='A stopped Android app’s pipeline_cache.db')
    parser.add_argument('--output', type=Path, default=ROOT/'tools/android_initial_pipeline_cache.db.gz')
    args = parser.parse_args()
    print(f'Exported {export(args.source, args.output)} device-built configurations to {args.output}')
