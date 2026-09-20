#!/usr/bin/env python3
"""Fail closed when the full-set fixture skips a scene or changes peer timing."""
import unittest
import net_test

class Peer:
    def __init__(self, name, port, peer_port):
        self.name, self.port = name, port
        self.log = (f'lan: connect 127.0.0.1:{peer_port} as P1\n'
                    'lobby: entering CSS at frame 10, seed 42\n'
                    '[FileCache] HIT: MnSlChr.usd\n'
                    '[FileCache] HIT: MnSlMap.usd\n'
                    '[FileCache] HIT: GrNLa.dat\n'
                    'sss: picks P1=30 P2=30 -> 30\n'
                    'sss: picks P1=30 P2=30 -> 30\n')
        for frame, scene in enumerate(('CSS', 'SSS', 'VS', 'RESULTS', 'CSS', 'SSS', 'VS')):
            self.log += f'online: enter {scene} at frame {frame + 10}\n'
    def text(self):
        return self.log

class SetFlow(unittest.TestCase):
    def setUp(self):
        self.a = Peer('a', 5000, 5001)
        self.b = Peer('b', 5001, 5000)
    def test_complete(self):
        self.assertEqual(net_test.check_scenes(self.a, self.b), [])
    def test_pick_only_is_failure(self):
        for peer in (self.a, self.b):
            peer.log = peer.log.split('online: enter')[0]
        self.assertTrue(net_test.check_scenes(self.a, self.b))
    def test_peer_scene_timing(self):
        self.b.log = self.b.log.replace('VS at frame 12', 'VS at frame 13')
        self.assertTrue(net_test.check_scenes(self.a, self.b))
    def test_empty_stage(self):
        for peer in (self.a, self.b):
            peer.log = peer.log.replace('GrNLa.dat', 'Gr.dat')
        self.assertTrue(net_test.check_scenes(self.a, self.b))
    def test_preloaded_stage_resolution(self):
        for peer in (self.a, self.b):
            peer.log = peer.log.replace('[FileCache] HIT: GrNLa.dat\n', '')
            peer.log += 'sss: resolved cell 1 stage 3\n' * 2
        self.assertEqual(net_test.check_scenes(self.a, self.b), [])
        self.b.log = self.b.log.replace('resolved cell 1 stage 3', 'resolved cell 30 stage 0')
        self.assertTrue(net_test.check_scenes(self.a, self.b))
    def test_missing_results(self):
        for peer in (self.a, self.b):
            peer.log = peer.log.replace('online: enter RESULTS at frame 13\n', '')
        self.assertTrue(net_test.check_scenes(self.a, self.b))

if __name__ == '__main__':
    unittest.main()
