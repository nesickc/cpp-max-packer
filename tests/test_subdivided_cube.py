import tempfile
import unittest
from pathlib import Path

from benchmarks.subdivided_cube import generate_triangles, write_stl


class SubdividedCubeTests(unittest.TestCase):
    def check(self, n, expected_volume):
        ts = list(generate_triangles(n))
        edges = {}
        vertices = set()
        volume = 0
        for a, b, c in ts:
            vertices.update((a, b, c))
            ab = tuple(b[i] - a[i] for i in range(3)); ac = tuple(c[i] - a[i] for i in range(3))
            cross = (ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0])
            volume += sum(a[i] * cross[i] for i in range(3))
            for edge in ((a, b), (b, c), (c, a)):
                edges[edge] = edges.get(edge, 0) + 1
        self.assertTrue(all(count == 1 and edges.get((b, a), 0) == 1 for (a, b), count in edges.items()))
        self.assertEqual(len(ts), 12 * n * n)
        self.assertEqual(len(vertices), 6 * n * n + 2)
        self.assertEqual(volume, 6 * expected_volume)
        self.assertEqual(min(v[i] for v in vertices for i in range(3)), 0)
        self.assertEqual(max(v[i] for v in vertices for i in range(3)), n)

    def test_small_cubes(self):
        self.check(2, 8); self.check(3, 27)

    def test_deterministic_bytes_and_exclusive_create(self):
        with tempfile.TemporaryDirectory() as d:
            a = Path(d) / "a.stl"; b = Path(d) / "b.stl"
            write_stl(a, 2); write_stl(b, 2)
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(a.stat().st_size, 84 + 50 * 48)
            before = a.read_bytes()
            with self.assertRaises(FileExistsError): write_stl(a, 2)
            self.assertEqual(a.read_bytes(), before)

    def test_bounds(self):
        for bad in (0, 301, -1, True, 2.5):
            with tempfile.TemporaryDirectory() as d:
                target = Path(d) / "bad.stl"
                with self.assertRaises(ValueError): write_stl(target, bad)
                self.assertFalse(target.exists())


if __name__ == "__main__": unittest.main()
