"""Run the fixed SpectraPack scoreboard and print count, time and validity per case.

The scoreboard is the definition of done for packing changes: a change must not
lower any count or produce an invalid result, and improvements show up here.

Usage (from the repository root, after a Release build):

    python tools/scoreboard.py                 # all cases
    python tools/scoreboard.py --cases cube64 pryanik2
    python tools/scoreboard.py --record        # also append a row to benchmarks/SCOREBOARD.md
    python tools/scoreboard.py --orientation cube45  # compare the expanded desktop catalog

Per-run artifacts (settings, reports, results, engine output) go to .local/scoreboard/.
"""
import argparse
import datetime
import functools
import hashlib
import itertools
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ENGINE = REPO / 'out/build/windows-ninja-release/bin/spectrapack-engine.exe'
CUBE_CATALOG_SHA256 = 'fe7ba7524cabd7dbb20a4c1acc053d1ca518df6c6c5f2949f78f9b3a0ac1b95a'

# name -> (object, box mm, pair mm, wall mm, pitch mm, budget s, floor, target)
# floor: count the current solver reliably reaches; a change must not drop below it.
# target: the count we are working toward (known layout or spec goal).
CASES = {
    'cube64':     ('gen:cube10', (40, 40, 40), 0.0, 0.0, 1.0, 10, 64, 64),
    'lprism2':    ('gen:lprism', (3, 2, 1), 0.0, 0.0, 0.25, 10, 1, 2),
    'lprism-gap': ('gen:lprism', (3.475, 2.475, 1.35), 0.125, 0.125, 0.25, 10, 2, 2),
    'pryanik2':   ('rc/items/pryanik_2.STL', (400, 340, 285), 1.0, 1.0, 4.0, 60, 425, 426),
    'pryanik2-0': ('rc/items/pryanik_2.STL', (400, 340, 285), 0.0, 1.0, 4.0, 60, 416, 416),
    'ulamok':     ('rc/items/ulamok_2kg_simplified.stl', (400, 350, 285), 0.1, 1.0, 4.0, 60, 36, 36),
    # Density cases, all at 0.1/0.1 mm with a little box slack. Targets are known layouts:
    # the L pair; 14 face-centred spheres (grid 8); stacked 4x4 and 3x3 sphere layers
    # (grid 64); hexagonal sphere layers stacked along Y (grid 504); and one or more
    # squares turned 45 degrees among axis-aligned ones (grids 4 and 9).
    'lprism-01':   ('gen:lprism', (3.35, 2.35, 1.25), 0.1, 0.1, 0.25, 10, 2, 2),
    'sphere14':    ('gen:sphere10', (24.6, 24.6, 24.6), 0.1, 0.1, 0.5, 10, 10, 14),
    'sphere66':    ('gen:sphere10', (40.6, 40.6, 40.6), 0.1, 0.1, 1.0, 10, 64, 66),
    'sphere40-np': ('gen:sphere40', (400, 340, 285), 0.1, 0.1, 4.0, 60, 540, 690),
    'tile5':       ('gen:tile10', (27.5, 27.5, 2.3), 0.1, 0.1, 0.5, 10, 4, 5),
    'tile10':      ('gen:tile10', (37.6, 37.6, 2.3), 0.1, 0.1, 0.5, 10, 9, 10),
}
# Cases whose known layout needs the desktop's 45-degree turns.
CUBE45_CASES = {'tile5', 'tile10'}
# Cases packed as the desktop packs them: the automatic packing envelope, 2 mm pitch.
# A looser clearance must not find fewer copies, so every count here should reach 74;
# yasya-0 still returns 72 in some runs and yasya-wall0 is slow to get there.
# The model is local only; the cases are skipped where it is absent.
CASES.update({
    'yasya-0':     ('rc/items/Yasya-250_V6.stl', (400, 240, 200), 0.0, 0.0, 2.0, 60, 72, 74),
    'yasya-1':     ('rc/items/Yasya-250_V6.stl', (400, 240, 200), 1.0, 1.0, 2.0, 60, 74, 74),
    'yasya-wall0': ('rc/items/Yasya-250_V6.stl', (400, 240, 200), 1.0, 0.0, 2.0, 60, 64, 74),
})
ENVELOPE_CASES = {'yasya-0', 'yasya-1', 'yasya-wall0'}


def write_binary_stl(path, triangles):
    with open(path, 'wb') as out:
        out.write(b'scoreboard generated'.ljust(80, b' '))
        out.write(struct.pack('<I', len(triangles)))
        for tri in triangles:
            out.write(struct.pack('<3f', 0, 0, 0))
            for vertex in tri:
                out.write(struct.pack('<3f', *vertex))
            out.write(b'\0\0')


def voxel_solid(cells, size):
    """Closed outward-oriented boundary of a union of axis-aligned cells of edge(s) `size`."""
    sizes = size if isinstance(size, tuple) else (size,) * 3
    occupied = set(cells)
    triangles = []
    for x, y, z in occupied:
        for axis in range(3):
            for sign in (-1, 1):
                neighbour = [x, y, z]
                neighbour[axis] += sign
                if tuple(neighbour) in occupied:
                    continue
                a, b = [i for i in range(3) if i != axis]
                if sign < 0:
                    a, b = b, a  # flip winding so the normal points along -axis
                corner = [x, y, z]
                if sign > 0:
                    corner[axis] += 1
                quad = []
                for da, db in ((0, 0), (1, 0), (1, 1), (0, 1)):
                    p = list(corner)
                    p[a] += da
                    p[b] += db
                    quad.append(tuple(c * edge for c, edge in zip(p, sizes)))
                triangles += [(quad[0], quad[1], quad[2]), (quad[0], quad[2], quad[3])]
    return triangles


def icosphere(diameter, levels=3):
    """Outward-oriented subdivided icosahedron inscribed in a sphere, lower corner at the origin."""
    t = (1 + 5 ** 0.5) / 2
    points = [(-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0), (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
              (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1)]
    faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11), (1, 5, 9), (5, 11, 4), (11, 10, 2),
             (10, 7, 6), (7, 1, 8), (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9), (4, 9, 5), (2, 4, 11),
             (6, 2, 10), (8, 6, 7), (9, 8, 1)]

    def unit(p):
        length = sum(c * c for c in p) ** 0.5
        return tuple(c / length for c in p)

    points = [unit(p) for p in points]
    for _ in range(levels):
        midpoints, finer = {}, []

        def middle(a, b):
            key = (min(a, b), max(a, b))
            if key not in midpoints:
                points.append(unit(tuple((points[a][i] + points[b][i]) / 2 for i in range(3))))
                midpoints[key] = len(points) - 1
            return midpoints[key]

        for a, b, c in faces:
            ab, bc, ca = middle(a, b), middle(b, c), middle(c, a)
            finer += [(a, ab, ca), (b, bc, ab), (c, ca, bc), (ab, bc, ca)]
        faces = finer
    radius = diameter / 2
    return [tuple(tuple(radius * c + radius for c in points[i]) for i in face) for face in faces]


def generated_stl(name, directory):
    path = directory / f'{name}.stl'
    if not path.exists():
        if name == 'cube10':
            write_binary_stl(path, voxel_solid([(0, 0, 0)], 10.0))
        elif name == 'lprism':
            write_binary_stl(path, voxel_solid([(0, 0, 0), (1, 0, 0), (0, 1, 0)], 1.0))
        elif name == 'tile10':
            write_binary_stl(path, voxel_solid([(0, 0, 0)], (10.0, 10.0, 2.0)))
        elif name in ('sphere10', 'sphere40'):
            write_binary_stl(path, icosphere(float(name[6:])))
        else:
            raise ValueError(name)
    return path


def run(args, **kw):
    return subprocess.run([str(a) for a in args], capture_output=True, text=True, **kw)


def inspect(stl, directory, envelope=False):
    """Inspect once per source content; reuse the accepted report on later runs."""
    digest = hashlib.sha256(stl.read_bytes()).hexdigest()[:16]
    report = directory / f'{stl.stem}-{digest}{"-envelope" if envelope else ""}.report.json'
    if not report.exists():
        options = ['--packing-envelope', 'auto', '--accept-repair', 'auto',
                   '--available-host-bytes', 4 << 30] if envelope else ['--role', 'object']
        done = run([ENGINE, 'inspect', '--stl', stl, '--units', 'mm', '--report', report, *options])
        if done.returncode != 0:
            raise RuntimeError(f'inspect {stl} failed: {done.stdout} {done.stderr}')
    data = json.loads(report.read_text())
    if data['diagnostics']['status'] != 'valid':
        raise RuntimeError(f'{stl} is not an accepted valid solid')
    return report, data


def block_reference(item, box, pair, wall):
    """Best axis-aligned bounding-box block layout (recursive guillotine); a simple reference, not a bound."""
    item = tuple(b + pair for b in item)
    space = tuple(round(L - 2 * wall + pair, 6) for L in box)

    @functools.lru_cache(maxsize=None)
    def best(space, depth):
        result = 0
        for size in set(itertools.permutations(item)):
            n = [int(space[i] // size[i] + 1e-9) for i in range(3)]
            if 0 in n:
                continue
            used = [n[i] * size[i] for i in range(3)]
            total = n[0] * n[1] * n[2]
            if depth < 3:
                extra = 0
                for order in itertools.permutations(range(3)):
                    rest, bounds, gain = list(space), list(space), 0
                    for axis in order:
                        slab = list(bounds)
                        slab[axis] = round(space[axis] - used[axis], 6)
                        if slab[axis] > 0:
                            gain += best(tuple(slab), depth + 1)
                        bounds[axis] = used[axis]
                    extra = max(extra, gain)
                total += extra
            result = max(result, total)
        return result

    return best(space, 0)


def settings_for(report, box, pair, wall, pitch, budget, threads):
    return {
        'settings_version': 1,
        'clearance_mm': {'pair': pair, 'wall': wall},
        'compute': {'backend': 'cpu', 'thread_count': threads},
        'container': {'kind': 'box', 'dimensions_mm': list(box)},
        'object_asset': {'source_sha256': report['source']['sha256'],
                         'accepted_solid_sha256': report['accepted_solid']['sha256']},
        'orientation': {'mode': 'cube'},
        'resolution': {'mode': 'manual', 'pitch_mm': pitch},
        'resolved': {
            'backend': 'cpu',
            'cpu_runtime': {'version': 1, 'scheduling_policy':
                            'serial-v1' if threads == 1 else 'raster-rows256-fft-lines64-v1'},
            'orientation_catalog_sha256': CUBE_CATALOG_SHA256,
            'orientation_catalog_version': 1,
            'pitch_mm': pitch,
            'thread_count': threads,
        },
        'search': {'budget_scope': 'total_start', 'budget_seconds': budget, 'deterministic': False,
                   'preset': 'desktop-cpu-v1', 'seed': '42'},
    }


def run_case(name, out, threads, orientation='cube'):
    source, box, pair, wall, pitch, budget, floor, target = CASES[name]
    if name in CUBE45_CASES:
        orientation = 'cube45'
    case_dir = out / name
    case_dir.mkdir(parents=True, exist_ok=True)
    stl = generated_stl(source[4:], out.parent / 'inputs') if source.startswith('gen:') else REPO / source
    if not stl.exists():
        return {'case': name, 'count': None, 'status': 'skipped', 'seconds': 0.0, 'termination': f'{source} is absent',
                'time_to_best': None, 'floor': floor, 'target': target, 'reference': '-', 'ok': True}
    report_path, report = inspect(stl, out.parent / 'inputs', name in ENVELOPE_CASES)
    settings = case_dir / 'settings.json'
    resolved = settings_for(report, box, pair, wall, pitch, budget, threads)
    if orientation == 'cube45':
        # Resolve the actual desktop preset natively, while keeping every other
        # scoreboard setting and budget identical to the cube comparison.
        request = case_dir / 'desktop-request.json'
        request.write_text(json.dumps({'desktop_version': 1, 'box_dimensions_mm': list(box),
            'clearance_mm': {'pair': pair, 'wall': wall}, 'orientation': {'mode': orientation},
            'pitch_mm': pitch, 'budget_seconds': budget, 'seed': '42', 'thread_count': threads}))
        prepared = case_dir / 'prepared'
        prepared.mkdir()
        reply = run([ENGINE, 'desktop-prepare', '--request', request, '--object-report', report_path,
                     '--output', prepared], timeout=30)
        if reply.returncode != 0:
            raise RuntimeError(f'orientation preparation failed: {reply.stdout} {reply.stderr}')
        native = json.loads((prepared / 'settings.json').read_text())
        resolved['orientation'] = native['orientation']
        resolved['resolved']['orientation_catalog_sha256'] = native['resolved']['orientation_catalog_sha256']
    settings.write_text(json.dumps(resolved, indent=1))
    result_path = case_dir / 'result.json'
    began = time.perf_counter()
    done = run([ENGINE, 'solve', '--settings', settings, '--object-report', report_path, '--result', result_path],
               timeout=budget + 120)
    elapsed = time.perf_counter() - began
    (case_dir / 'engine-stdout.json').write_text(done.stdout)
    (case_dir / 'engine-stderr.txt').write_text(done.stderr)
    row = {'case': name, 'count': None, 'status': 'no result', 'seconds': round(elapsed, 1), 'termination': None,
           'time_to_best': None, 'floor': floor, 'target': target,
           'reference': block_reference(tuple(report['dimensions_mm']), box, pair, wall)}
    if result_path.exists():
        result = json.loads(result_path.read_text())
        row.update(count=result['count'], status=result['validation']['status'],
                   termination=result['metrics'].get('termination_reason'),
                   time_to_best=result['metrics'].get('time_to_best_seconds'))
        if result['count'] != len(result['placements']):
            row['status'] = 'count/placements mismatch'
    else:
        try:
            row['termination'] = json.loads(done.stdout)['error']['code']
        except (ValueError, KeyError):
            row['termination'] = f'exit {done.returncode}'
    row['ok'] = row['status'] == 'valid' and row['count'] is not None and row['count'] >= floor
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cases', nargs='+', choices=list(CASES), default=list(CASES))
    parser.add_argument('--threads', type=int, default=8)
    parser.add_argument('--orientation', choices=['cube', 'cube45'], default='cube')
    parser.add_argument('--record', action='store_true', help='append the results to benchmarks/SCOREBOARD.md')
    args = parser.parse_args()
    if not ENGINE.exists():
        sys.exit(f'Release engine not found at {ENGINE}; build the windows-ninja-release preset first.')

    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out = REPO / '.local/scoreboard' / stamp
    out.mkdir(parents=True)
    (out.parent / 'inputs').mkdir(exist_ok=True)
    commit = run(['git', 'rev-parse', '--short', 'HEAD'], cwd=REPO).stdout.strip()

    rows = []
    print(f'{"case":<11} {"count":>5} {"floor":>5} {"target":>6} {"blocks":>6} {"best@s":>7} {"total s":>7}  status / termination')
    for name in args.cases:
        row = run_case(name, out, args.threads, args.orientation)
        rows.append(row)
        best_at = '-' if row['time_to_best'] is None else f'{row["time_to_best"]:.1f}'
        print(f'{row["case"]:<11} {row["count"] if row["count"] is not None else "-":>5} {row["floor"]:>5} {row["target"]:>6} '
              f'{row["reference"]:>6} {best_at:>7} {row["seconds"]:>7}  '
              f'{"OK " if row["ok"] else "FAIL "}{row["status"]} / {row["termination"]}', flush=True)
    (out / 'scoreboard.json').write_text(json.dumps({'commit': commit, 'threads': args.threads,
        'orientation': args.orientation, 'rows': rows}, indent=1))
    print(f'\nArtifacts: {out}')
    print('"blocks" = best simple bounding-box block layout for reference; it is not an upper bound.')

    if args.record:
        history = REPO / 'benchmarks/SCOREBOARD.md'
        line = (f'| {datetime.date.today()} | {commit} ({args.orientation}) | ' +
                ' \\| '.join(f'{r["case"]}={r["count"] if r["count"] is not None else "-"}'
                           f'{"" if r["ok"] else "!"}' for r in rows) + ' |\n')
        with open(history, 'a', encoding='utf-8') as f:
            f.write(line)
        print(f'Recorded in {history.relative_to(REPO)}')
    sys.exit(0 if all(r['ok'] for r in rows) else 1)


if __name__ == '__main__':
    main()
