"""Deterministic binary STL generator for the T-005 subdivided cube fixture."""
import argparse
import math
import struct
from pathlib import Path


def generate_triangles(subdivisions=300):
    _validate(subdivisions)
    n = subdivisions
    maps = (
        lambda u, v: (0, v, u), lambda u, v: (n, u, v),
        lambda u, v: (u, 0, v), lambda u, v: (v, n, u),
        lambda u, v: (v, u, 0), lambda u, v: (u, v, n),
    )
    def rows():
        for map_uv in maps:
            for i in range(n):
                for j in range(n):
                    a, b, c, d = map_uv(i, j), map_uv(i + 1, j), map_uv(i + 1, j + 1), map_uv(i, j + 1)
                    yield a, b, c
                    yield a, c, d
    return rows()


def write_stl(output, subdivisions=300):
    _validate(subdivisions)
    output = Path(output)
    count = 12 * subdivisions * subdivisions
    with output.open("xb") as f:
        f.write(f"SpectraPack subdivided cube n={subdivisions}".encode("ascii").ljust(80, b" "))
        f.write(struct.pack("<I", count))
        for a, b, c in generate_triangles(subdivisions):
            ab = tuple(b[i] - a[i] for i in range(3)); ac = tuple(c[i] - a[i] for i in range(3))
            normal = (ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0])
            length = math.sqrt(sum(x*x for x in normal))
            f.write(struct.pack("<12fH", *(x/length for x in normal), *a, *b, *c, 0))
    expected = 84 + 50 * count
    if output.stat().st_size != expected:
        raise AssertionError("unexpected STL size")


def _validate(subdivisions):
    if isinstance(subdivisions, bool) or not isinstance(subdivisions, int) or not 1 <= subdivisions <= 300:
        raise ValueError("subdivisions must be an integer between 1 and 300")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--subdivisions", type=int, default=300)
    args = parser.parse_args()
    write_stl(args.output, args.subdivisions)
