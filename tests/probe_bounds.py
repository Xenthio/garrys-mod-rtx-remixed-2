"""World-space sections must follow the published probe frame, not local AABBs."""
import math
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from probe_bounds import probe_section


class ProbeSections(unittest.TestCase):
    def test_translated_and_rotated(self):
        probe = dict(minimum=[-2, -1, -3], maximum=[2, 1, 3],
                     worldToLocal=[[0, 1, 0, -20], [-1, 0, 0, 10], [0, 0, 1, -30]])
        section = probe_section(probe, 2, 30)
        self.assertEqual(set(section), {(9, 18), (11, 18), (11, 22), (9, 22)})
        self.assertEqual(probe_section(probe, 2, 0), [])
        self.assertEqual(len(probe_section(probe, 2, 33)), 4)

    def test_oblique_box_produces_hexagon(self):
        a, b, c = 1 / math.sqrt(2), 1 / math.sqrt(6), 1 / math.sqrt(3)
        probe = dict(minimum=[-1] * 3, maximum=[1] * 3,
                     worldToLocal=[[a, b, c, 0], [-a, b, c, 0], [0, -2*b, c, 0]])
        section = probe_section(probe, 2, 0)
        self.assertEqual(len(section), 6)
        self.assertAlmostEqual(max(p[0] for p in section), math.sqrt(2))
        self.assertAlmostEqual(max(p[1] for p in section), math.sqrt(1.5))
        for x, y in section:
            local = [row[0]*x + row[1]*y for row in probe['worldToLocal']]
            self.assertTrue(all(abs(v) <= 1 + 1e-6 for v in local))
            self.assertEqual(sum(abs(abs(v)-1) < 1e-6 for v in local), 2)

    def test_influence_can_intersect_without_projection(self):
        probe = dict(minimum=[-1] * 3, maximum=[1] * 3,
                     influenceMinimum=[-2] * 3, influenceMaximum=[2] * 3)
        self.assertEqual(probe_section(probe, 2, 1.5), [])
        self.assertEqual(set(probe_section(probe, 2, 1.5, influence=True)),
                         {(-2, -2), (-2, 2), (2, -2), (2, 2)})

    def test_nonrigid_frame_is_rejected(self):
        probe = dict(minimum=[-1] * 3, maximum=[1] * 3,
                     worldToLocal=[[2, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]])
        with self.assertRaises(ValueError):
            probe_section(probe, 2, 0)


if __name__ == '__main__':
    unittest.main()
