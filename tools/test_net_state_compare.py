#!/usr/bin/env python3
import tempfile
from pathlib import Path
import unittest
from net_state_compare import capture, compare, fields, injection_check, scene_transitions, compare_scenes, scene_injection_check, replay_capture


def pair(f, mid=5, seed='12345678'):
    return (f'net: state f{f} seed={seed} pads=0000/0,0 0100/-1,2 p0=(1.000,2.000) v(0.000,0.000) kb(0.000,0.000) f1 0.0% m{mid} s4\n'
            f'net: bits f{f} seed={seed} p0 pos=3f800000/40000000/00000000 dir=3f800000 pct=00000000 mid={mid} st=4\n')


class CompareTests(unittest.TestCase):
    def load(self, text, extra='', handshake_frame=1):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'a.state').write_text(text)
            (root/'a.log').write_text(f'net: rollback with 127.0.0.1\nnet: handshake done seed=305419896 start_frame=2 (frame {handshake_frame})\n' + extra)
            return capture(root/'a.state', root/'a.log')['rows']

    def test_epoch_common_final_pass_and_identity(self):
        a = self.load(pair(1, 999) + pair(2, 999) + pair(0) + pair(1) + pair(2, 9) + pair(3) + pair(2) + pair(3) + pair(4))
        b = self.load(pair(0) + pair(1) + pair(2) + pair(3))
        result = compare(a, b)
        self.assertEqual(result['common_frames'], 2)
        self.assertEqual(result['first_common'], 2)
        self.assertEqual(result['byte_identical_frames'], 2)
        self.assertEqual(result['differing_frames'], 0)

    def test_real_text_injection_moves_count_exactly_one(self):
        text = pair(0) + pair(1) + pair(2) + pair(3)
        a = self.load(text)
        # Alter the serialized fixture before parsing, only at one frame.
        b = self.load(text.replace('bits f3 seed=12345678 p0 pos=3f800000', 'bits f3 seed=12345678 p0 pos=3f800001'))
        baseline = compare(a, a)
        result = compare(a, b)
        self.assertEqual(result['differing_frames'] - baseline['differing_frames'], 1)
        self.assertEqual(result['first_difference'], {'frame': 3, 'fields': [{'field': 'bits.p0.pos.x', 'a': '3f800000', 'b': '3f800001'}]})
        self.assertTrue(injection_check(a)['passed'])

    def test_velocity_bits_parsed_and_compared(self):
        # Protocol 9 logs carry vel/kb after st=; a velocity-only divergence
        # must name the field, and the older line without them still parses.
        new = lambda f, vx='00000000': pair(f).replace(' st=4\n', f' st=4 vel={vx}/3f000000/00000000 kb=00000000/00000000/00000000\n')
        a = self.load(new(0) + new(1) + new(2) + new(3))
        b = self.load(new(0) + new(1) + new(2) + new(3, vx='3f800000'))
        self.assertEqual(a[2][0]['bits.p0.vel.y'], '3f000000')
        self.assertEqual(compare(a, b)['first_difference'], {'frame': 3, 'fields': [{'field': 'bits.p0.vel.x', 'a': '00000000', 'b': '3f800000'}]})
        self.assertTrue(injection_check(a)['passed'])
        old = self.load(pair(0) + pair(2))
        self.assertNotIn('bits.p0.vel.x', old[2][0])

    def test_handshake_before_first_tick(self):
        rows = self.load(pair(0) + pair(1) + pair(2), handshake_frame=-1)
        self.assertEqual(set(rows), {2})

    def test_fighter_presence(self):
        a = self.load(pair(0) + pair(2))
        b = self.load(pair(0) + 'net: state f2 seed=12345678 pads=0000/0,0 0100/-1,2\nnet: bits f2 seed=12345678\n')
        self.assertEqual(compare(a, b)['differing_frames'], 1)

    def test_disconnect_and_explicit_bound(self):
        a = self.load(pair(0) + pair(2) + pair(3) + pair(4), 'net: disconnected at frame 4 (status 0)\n')
        self.assertEqual(set(a), {2, 3})
        self.assertEqual(compare(a, a, 2)['common_frames'], 1)

    def test_scene_transition_injection_and_scope(self):
        log = ('net: scene 99 -> 98 at frame 3\nnet: rollback with fixture\n'
               'net: scene -1 -> 45 at frame 0\nnet: scene 45 -> 8 at frame 3\n'
               'net: scene 8 -> 9 at frame 5\nnet: disconnected at frame 6\n'
               'net: scene 9 -> 2 at frame 7\n')
        a = scene_transitions(log)
        self.assertEqual(a, [(-1, 45, 0), (45, 8, 3), (8, 9, 5)])
        self.assertEqual(compare_scenes(a, a, 2, 5)['mismatch_count'], 0)
        b = scene_transitions(log.replace('45 -> 8 at frame 3', '45 -> 8 at frame 4'))
        mismatch = compare_scenes(a, b, 2, 5)
        self.assertEqual(mismatch['mismatch_count'], 1)
        self.assertEqual(mismatch['first_mismatch'], {'index': 0, 'a': (45, 8, 3), 'b': (45, 8, 4)})
        self.assertTrue(scene_injection_check(a, 2, 5)['passed'])
        self.assertEqual(compare_scenes(a, a[:-1], 2, 5)['mismatch_count'], 1)
        self.assertEqual(compare_scenes(a, list(reversed(a)), 2, 5)['mismatch_count'], 2)
        self.assertEqual(compare_scenes(a, b, 5, 5)['mismatch_count'], 0)

    def test_solo_replay_has_no_handshake_filter_and_final_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'solo.state'
            path.write_text(pair(0) + pair(1, 6) + pair(1, 5) + pair(2))
            a = replay_capture(path)['rows']
            self.assertEqual(set(a), {0, 1, 2})
            self.assertEqual(a[1][0]['bits.p0.mid'], '5')
            path.write_text(pair(0) + pair(1, 7))
            b = replay_capture(path)['rows']
            result = compare(a, b)
            self.assertEqual(result['common_frames'], 2)
            self.assertEqual(result['first_common'], 0)
            self.assertEqual(result['first_difference']['frame'], 1)
            self.assertEqual(result['first_difference']['fields'][0]['field'], 'bits.p0.mid')
            self.assertEqual(result['differing_frames'], 1)
            self.assertEqual(injection_check(a)['differing_frame_count_delta'], 1)

    def test_fail_closed(self):
        with self.assertRaises(ValueError):
            self.load(pair(2))
        with self.assertRaises(ValueError):
            self.load(pair(0) + pair(2), 'net: rollback with second\n')
        with self.assertRaises(ValueError):
            fields('seed=12345678 pads=0000/0,0 0000/0,0', 'seed=00000000')
        with self.assertRaises(ValueError):
            compare({}, {})


if __name__ == '__main__':
    unittest.main()
