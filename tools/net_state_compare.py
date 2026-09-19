#!/usr/bin/env python3
"""Compare final recorded passes on common post-handshake frames.

This compares logged fields, not complete simulation memory. The log has no
confirmation watermark: a final recorded pass can still be speculative. Run on
finished captures; --end-frame can bound comparison to a known confirmed frame.
Single-connection captures only: frame zero selects the connected epoch, avoiding
preconnect/offline records with reused frame numbers. Multiple sessions fail closed.
"""
import argparse
import copy
import json
from itertools import zip_longest
from pathlib import Path
import re

LINE = re.compile(r'^net: (state|bits) f(\d+) (.*)$')
STATE = re.compile(r'seed=([0-9a-f]{8}) pads=([0-9a-f]{4})/(-?\d+),(-?\d+) ([0-9a-f]{4})/(-?\d+),(-?\d+)')
HUMAN = re.compile(r' p(\d+)=\(([^,]+),([^)]*)\) v\(([^,]+),([^)]*)\) kb\(([^,]+),([^)]*)\) f(\S+) (\S+)% m(-?\d+) s(-?\d+)')
BITS = re.compile(r' p(\d+) pos=([0-9a-f]{8})/([0-9a-f]{8})/([0-9a-f]{8}) dir=([0-9a-f]{8}) pct=([0-9a-f]{8}) mid=(-?\d+) st=(-?\d+)')


def fields(state, bits):
    m = STATE.match(state)
    if not m:
        raise ValueError('malformed state header')
    names = ['seed', 'pad0.button', 'pad0.x', 'pad0.y', 'pad1.button', 'pad1.x', 'pad1.y']
    out = dict(zip(names, m.groups()))
    tail = state[m.end():]
    for pattern, labels, prefix in [(HUMAN, ['pos.x', 'pos.y', 'vel.x', 'vel.y', 'kb.x', 'kb.y', 'dir', 'pct', 'mid', 'stocks'], 'state')]:
        while tail:
            m = pattern.match(tail)
            if not m:
                raise ValueError('malformed fighter state')
            out.update((f'{prefix}.p{m[1]}.{name}', value) for name, value in zip(labels, m.groups()[1:]))
            tail = tail[m.end():]
    m = re.match(r'seed=([0-9a-f]{8})', bits)
    if not m or m[1] != out['seed']:
        raise ValueError('missing/inconsistent exact-bits seed')
    out['bits.seed'] = m[1]
    tail = bits[m.end():]
    while tail:
        m = BITS.match(tail)
        if not m:
            raise ValueError('malformed exact-bits fighter')
        out.update((f'bits.p{m[1]}.{name}', value) for name, value in zip(['pos.x', 'pos.y', 'pos.z', 'dir', 'pct', 'mid', 'stocks'], m.groups()[1:]))
        tail = tail[m.end():]
    return out


def scene_transitions(log):
    """Preserve logged transition order, excluding offline/preconnection events."""
    connected = log.split('net: rollback with ', 1)
    if len(connected) != 2:
        raise ValueError('scene log lacks connection boundary')
    session = connected[1].split('net: disconnected at frame ', 1)[0]
    return [tuple(map(int, item)) for item in re.findall(
        r'net: scene (-?\d+) -> (-?\d+) at frame (\d+)', session)]


def compare_scenes(a, b, first, last):
    a, b = [[t for t in seq if first <= t[2] <= last] for seq in (a, b)]
    mismatches = [{'index': i, 'a': x, 'b': y}
                  for i, (x, y) in enumerate(zip_longest(a, b)) if x != y]
    return {'a_transitions': a, 'b_transitions': b,
            'mismatch_count': len(mismatches),
            'first_mismatch': mismatches[0] if mismatches else None}


def scene_injection_check(transitions, first, last):
    selected = [t for t in transitions if first <= t[2] <= last]
    if not selected:
        raise ValueError('scene injection requires a transition in compared range')
    def serialized(seq):
        return 'net: rollback with fixture\n' + ''.join(
            f'net: scene {old} -> {new} at frame {frame}\n' for old, new, frame in seq)
    old, new, frame = selected[0]
    altered = [(old, new + 1, frame)] + selected[1:]
    baseline = compare_scenes(selected, selected, first, last)
    injected = compare_scenes(scene_transitions(serialized(selected)),
                              scene_transitions(serialized(altered)), first, last)
    assert injected['mismatch_count'] - baseline['mismatch_count'] == 1
    assert injected['first_mismatch']['index'] == 0
    return {'passed': True, 'frame': frame, 'field': 'scene.to', 'mismatch_count_delta': 1}


def read_records(state_path):
    records = []
    pending = None
    for number, line in enumerate(Path(state_path).read_text().splitlines(), 1):
        m = LINE.fullmatch(line)
        if not m:
            raise ValueError(f'{state_path}:{number}: malformed record')
        kind, frame, body = m[1], int(m[2]), m[3]
        if kind == 'state':
            if pending is not None:
                raise ValueError('state record missing bits companion')
            pending = (frame, body)
        else:
            if pending is None or pending[0] != frame:
                raise ValueError('bits record missing matching state')
            records.append((frame, fields(pending[1], body), pending[1] + '\n' + body))
            pending = None
    return records, pending


def replay_capture(state_path):
    """Explicit solo mode: no handshake or LAN epoch inference."""
    records, pending = read_records(state_path)
    rows = {frame: (values, raw) for frame, values, raw in records if frame >= 0}
    return {'rows': rows, 'incomplete_tail': pending is not None}


def capture(state_path, log_path):
    log = Path(log_path).read_text(errors='replace')
    handshakes = re.findall(r'net: handshake done seed=(\d+) start_frame=(\d+) \(frame (-?\d+)\)', log)
    if len(handshakes) != 1 or log.count('net: rollback with ') != 1:
        raise ValueError('require exactly one connection and one completed handshake')
    seed, start, done = map(int, handshakes[0])
    lower = max(start, done)
    disconnect = re.findall(r'net: disconnected at frame (\d+)', log)
    upper = int(disconnect[0]) - 1 if disconnect else None
    records, pending = read_records(state_path)
    # A buffered capture can end with an incomplete final pair; do not use it.
    zeros = [i for i, row in enumerate(records) if row[0] == 0]
    if not zeros:
        raise ValueError('no frame-zero connected epoch; cannot disambiguate offline frames')
    rows = {}
    for frame, values, raw in records[zeros[-1]:]:
        if frame >= lower and (upper is None or frame <= upper):
            rows[frame] = (values, raw)
    return {'seed': seed, 'start': start, 'lower': lower, 'rows': rows, 'incomplete_tail': pending is not None, 'scenes': scene_transitions(log)}


def compare(a, b, end=None):
    common = sorted(set(a) & set(b))
    if end is not None:
        common = [f for f in common if f <= end]
    if not common:
        raise ValueError('no common eligible frames')
    differences = []
    identical = 0
    for frame in common:
        av, ar = a[frame]
        bv, br = b[frame]
        if ar == br:
            identical += 1
        # Exact bit fields first, then human-readable fields absent from bits.
        names = sorted(set(av) | set(bv), key=lambda k: (not k.startswith('bits.'), k))
        changes = [{'field': k, 'a': av.get(k), 'b': bv.get(k)} for k in names if av.get(k) != bv.get(k)]
        if changes:
            differences.append({'frame': frame, 'fields': changes})
        elif ar != br:
            differences.append({'frame': frame, 'fields': [{'field': 'record_bytes', 'a': ar, 'b': br}]})
    return {'common_frames': len(common), 'first_common': common[0], 'last_common': common[-1],
            'byte_identical_frames': identical, 'differing_frames': len(differences),
            'first_difference': differences[0] if differences else None}


def injection_check(rows):
    """One changed motion count must move the differing-frame count exactly one."""
    baseline = compare(rows, rows)
    altered = copy.deepcopy(rows)
    frame = next((f for f in sorted(rows) if 'bits.p0.mid' in rows[f][0]), None)
    if frame is None:
        raise ValueError('injection requires at least one fighter frame')
    values, raw = altered[frame]
    old = values['bits.p0.mid']
    state, bits = raw.split('\n')
    injected_bits, count = re.subn(r'( p0 .*? mid=)' + re.escape(old) + r'(?= st=)',
                                  lambda m: m[1] + str(int(old) + 1), bits, count=1)
    assert count == 1
    altered[frame] = (fields(state, injected_bits), state + '\n' + injected_bits)
    result = compare(rows, altered)
    assert result['differing_frames'] - baseline['differing_frames'] == 1
    assert result['first_difference']['frame'] == frame
    assert result['first_difference']['fields'] == [{'field': 'bits.p0.mid', 'a': old, 'b': str(int(old) + 1)}]
    return {'passed': True, 'frame': frame, 'field': 'bits.p0.mid', 'differing_frame_count_delta': 1}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('work', type=Path, help='LAN capture directory, or first state file with --replay')
    p.add_argument('second_state', nargs='?', type=Path)
    p.add_argument('--replay', action='store_true', help='compare two solo state files without LAN handshake filtering')
    p.add_argument('--end-frame', type=int)
    args = p.parse_args()
    try:
        if args.replay:
            if args.second_state is None:
                raise ValueError('--replay requires two state files')
            a, b = [replay_capture(path) for path in (args.work, args.second_state)]
            result = compare(a['rows'], b['rows'], args.end_frame)
            result['mode'] = 'replay'
            result['injection'] = injection_check(a['rows'])
            result['limits'] = 'Solo replay: all common nonnegative frames, final complete recorded pass; logged fields only, no handshake or scene-log validation.'
            print(json.dumps(result, indent=2))
            return int(result['differing_frames'] > 0)
        if args.second_state is not None:
            raise ValueError('two state files require explicit --replay')
        a, b = [capture(args.work / f'{peer}.state', args.work / f'{peer}.log') for peer in 'ab']
        if (a['seed'], a['start']) != (b['seed'], b['start']):
            raise ValueError('peers disagree on handshake seed/start_frame')
        lower = max(a['lower'], b['lower'])
        ar, br = [{f: r for f, r in cap['rows'].items() if f >= lower} for cap in (a, b)]
        result = compare(ar, br, args.end_frame)
        result['injection'] = injection_check(ar)
        result['scenes'] = compare_scenes(a['scenes'], b['scenes'],
                                          result['first_common'], result['last_common'])
        result['scene_injection'] = scene_injection_check(
            a['scenes'], result['first_common'], result['last_common'])
        result['limits'] = 'Logged fields only; final recorded passes, not proven confirmed. Speculative tail and missing/buffered records are not certified.'
        print(json.dumps(result, indent=2))
        return int(result['differing_frames'] > 0 or result['scenes']['mismatch_count'] > 0)
    except (ValueError, OSError) as e:
        p.error(str(e))


if __name__ == '__main__':
    raise SystemExit(main())
