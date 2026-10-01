"""T011-A6 serial retained-session measurements. Standard library, Windows only."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import queue
import re
import statistics
import subprocess
import sys
import threading
import time
import traceback
import warnings

ROOT = Path(__file__).resolve().parents[2]
WORK = {'max_candidate_evaluations': 1, 'max_search_passes': 1}
LIMIT = 1 << 20


def require(condition, message):
    if not condition:
        raise ValueError(message)


def write(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def verify_settings(settings, profile, frozen_settings):
    request = profile['request']
    require(settings['container'] == {'kind': 'box', 'dimensions_mm': request['box_dimensions_mm']},
            'Box differs from frozen T011 profile (analytic uses 22x18x12)')
    for name in ('clearance_mm', 'orientation'):
        require(settings[name] == request[name], 'Changed ' + name)
    require(settings['object_asset'] == frozen_settings['object_asset'], 'Changed asset IDs')
    require(settings['object_asset']['source_sha256'] == profile['source_sha256'], 'Wrong source')
    require(settings['resolution'] == {'mode': 'manual', 'pitch_mm': request['pitch_mm']}, 'Changed pitch')
    require(settings['resolved'] == frozen_settings['resolved'], 'Changed resolved pitch/catalog/thread/backend')
    require(settings['resolved']['thread_count'] == 1 and settings['resolved']['backend'] == 'cpu', 'Not CPU thread1')
    require(settings['compute']['backend'] == 'cpu' and settings['compute'].get('thread_count', 1) == 1,
            'Changed compute settings')
    require(settings['search']['seed'] == request['seed'] and settings['search']['deterministic'] is True,
            'Changed seed/determinism')
    require(settings['search']['work_budget'] == WORK and 'budget_seconds' not in settings['search'],
            'Not fixed one-candidate/one-pass work')


def verify_result(result, profile, baseline, settings):
    """Check native evidence; these comparisons do not perform geometric validation."""
    require(result['count'] == 1 and len(result['placements']) == 1, 'Expected exactly one native-valid copy')
    require(result['label'] == 'best_found', 'Wrong result label')
    require(result['validation']['status'] == 'valid', 'Native validation did not accept result')
    require(result['validation']['checks'] == baseline['validation']['checks'], 'Changed native checks')
    require(result['validation']['authoritative_geometry_sha256'] ==
            baseline['validation']['authoritative_geometry_sha256'], 'Changed authoritative IDs')
    require(result['placements'] == baseline['placements'], 'Changed ordered poses')
    work = result['search']['work_counts']
    require({k: work[k] for k in baseline['work']} == baseline['work'], 'Changed fixed work')
    require(work.get('spectral_correlations', 0) == 0 and work.get('spectral_pages_examined', 0) == 0,
            'Unexpected spectral work in preparation-only comparison')
    obj = result['assets']['object']
    require(obj['source']['sha256'] == profile['source_sha256'], 'Changed result source ID')
    require(obj['accepted_solid']['sha256'] == settings['object_asset']['accepted_solid_sha256'],
            'Changed result accepted-solid ID')
    require(result['constraints']['orientation_catalog_sha256'] == settings['resolved']['orientation_catalog_sha256'],
            'Changed result catalog')
    require(result['container'] == settings['container'], 'Changed result box')
    for name in ('clearance_mm', 'orientation'):
        require(result['constraints'][name] == settings[name], 'Changed result ' + name)
    verify_settings(result['search']['resolved_settings'], profile, settings)
    require(result['search']['pitch_levels_mm'] == [profile['request']['pitch_mm']], 'Changed pitch levels')


def pin_artifacts(report_path, pins):
    """Hash actual artifact bytes, including repair artifacts recursively; never rewrite a report."""
    report_path = report_path.resolve()
    require(report_path.stat().st_size <= 16 << 20, 'Report exceeds harness 16MiB inspection bound')
    report = read(report_path)
    pins[str(report_path)] = digest(report_path)
    visited = {report_path}
    nodes = 0

    def visit(value, base, depth=0):
        nonlocal nodes
        nodes += 1
        require(depth <= 64 and nodes <= 100000 and len(visited) <= 4096, 'Artifact inspection bound exceeded')
        if isinstance(value, dict):
            if 'path' in value and 'sha256' in value:
                path = (base / value['path']).resolve()
                actual = digest(path)
                require(actual == value['sha256'], 'Artifact hash mismatch: ' + str(path))
                pins[str(path)] = actual
                if path.suffix.lower() == '.json' and path not in visited:
                    require(path.stat().st_size <= 16 << 20, 'Referenced JSON exceeds harness 16MiB bound')
                    visited.add(path)
                    visit(read(path), path.parent, depth + 1)
            for item in value.values():
                visit(item, base, depth + 1)
        elif isinstance(value, list):
            for item in value:
                visit(item, base, depth + 1)

    visit(report, report_path.parent)
    return report


def qpc():
    ticks, frequency = ctypes.c_int64(), ctypes.c_int64()
    require(ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(ticks)), 'QPC failed')
    require(ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(frequency)), 'QPF failed')
    return str(ticks.value), str(frequency.value)


class Counters(ctypes.Structure):
    _fields_ = [('cb', wintypes.DWORD), ('PageFaultCount', wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in ('PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
        'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage', 'QuotaNonPagedPoolUsage', 'PagefileUsage',
        'PeakPagefileUsage')]


class Session:
    """One real serial child. Raw streams, every timing and process high-water observations persist."""
    def __init__(self, engine, directory, watchdog, *, transport_command=None):
        directory.mkdir(parents=True, exist_ok=False)
        self.directory, self.watchdog = directory, watchdog
        self.records = queue.Queue()
        self.stop_sampler = threading.Event()
        self.lock = threading.Lock()
        self.threads = []
        self.peak_ws = self.peak_private = self.observations = 0
        self.faults = []
        self.killed = False
        self.closed = False
        self.writer = None
        self.memory = ctypes.WinDLL('psapi').GetProcessMemoryInfo
        self.memory.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        self.memory.restype = wintypes.BOOL
        # A real nonreading child may characterize pipe reclamation; CLI measurements
        # always use the explicit held native engine and cannot select this test seam.
        self.invocation = {'argv': transport_command or [str(engine), 'desktop-session'], 'cwd': str(ROOT),
                           'started_perf_ns': time.perf_counter_ns()}
        write(directory / 'invocation.json', self.invocation)
        self.child = subprocess.Popen(self.invocation['argv'], cwd=ROOT, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.invocation['pid'] = self.child.pid
        write(directory / 'invocation.json', self.invocation)
        try:
            self.snapshot('process-launched')
            for target in (self._reader, self._stderr, self._sampler):
                worker = threading.Thread(target=target, daemon=True)
                worker.start()
                self.threads.append(worker)
        except BaseException:
            self.close(failed=True)
            raise

    def snapshot(self, boundary):
        with self.lock:
            counters = Counters()
            counters.cb = ctypes.sizeof(counters)
            ok = bool(self.memory(int(self.child._handle), ctypes.byref(counters), counters.cb))
            record = {'perf_ns': time.perf_counter_ns(), 'boundary': boundary, 'available': ok}
            if ok:
                record.update(peak_working_set_bytes=counters.PeakWorkingSetSize,
                              peak_pagefile_bytes=counters.PeakPagefileUsage,
                              working_set_bytes=counters.WorkingSetSize, pagefile_bytes=counters.PagefileUsage)
                self.peak_ws = max(self.peak_ws, counters.PeakWorkingSetSize)
                self.peak_private = max(self.peak_private, counters.PeakPagefileUsage)
                self.observations += 1
            with (self.directory / 'memory.jsonl').open('a', encoding='utf-8') as stream:
                stream.write(json.dumps(record) + '\n')
            return record

    def _sampler(self):
        try:
            while not self.stop_sampler.wait(.01):
                self.snapshot('10ms-observation')
        except BaseException as error:
            self.faults.append(repr(error))

    def _stderr(self):
        try:
            with (self.directory / 'stderr.log').open('wb') as stream:
                while block := self.child.stderr.read(65536):
                    stream.write(block)
        except BaseException as error:
            self.faults.append(repr(error))

    def _reader(self):
        try:
            with (self.directory / 'stdout.ndjson').open('wb') as raw, \
                    (self.directory / 'receipts.jsonl').open('w', encoding='utf-8') as receipts:
                while line := self.child.stdout.readline(LIMIT + 2):
                    received = time.perf_counter_ns()  # complete terminal/phase receipt, before parsing/logging
                    raw.write(line)
                    raw.flush()
                    require(line.endswith(b'\n') and len(line) - 1 <= LIMIT, 'Invalid native NDJSON framing')
                    value = json.loads(line.decode('utf-8'))
                    receipts.write(json.dumps({'perf_ns': received, 'record': value}) + '\n')
                    receipts.flush()
                    self.records.put((received, value))
        except BaseException as error:
            self.faults.append(repr(error))
        finally:
            self.records.put((time.perf_counter_ns(), None))

    def request(self, method, params, directory):
        directory.mkdir(parents=True, exist_ok=False)
        identity = directory.name
        value = {'runtime_version': 1, 'request_id': identity, 'method': method, 'params': params}
        if method in ('prepare', 'run'):
            value['operation_id'] = identity + '-op'
        write(directory / 'request.json', value)
        raw = json.dumps(value, allow_nan=False).encode('utf-8') + b'\n'
        require(len(raw) - 1 <= LIMIT, 'Oversized request')
        self.snapshot(identity + '-before')
        began = time.perf_counter_ns()
        timing = {'send_perf_ns': began, 'method': method}
        phases = []
        try:
            # Pipe writes can block too. Include sending under the same watchdog.
            def send():
                try:
                    remaining = memoryview(raw)
                    while remaining:
                        written = self.child.stdin.write(remaining)
                        require(written is not None and written > 0, 'Incomplete stdin write')
                        remaining = remaining[written:]
                    self.child.stdin.flush()
                except BaseException as error:
                    self.faults.append(repr(error))
                    self.records.put((time.perf_counter_ns(), None))
            self.writer = threading.Thread(target=send, daemon=True)
            self.writer.start()
            self.threads.append(self.writer)
            while True:
                remaining = self.watchdog - (time.perf_counter_ns() - began) / 1e9
                if remaining <= 0:
                    raise TimeoutError('Watchdog: failed sample, not cooperative Stop')
                try:
                    received, record = self.records.get(timeout=remaining)
                except queue.Empty as error:
                    raise TimeoutError('Watchdog: failed sample, not cooperative Stop') from error
                require(record is not None, 'Native EOF before terminal receipt; ' + repr(self.faults))
                require(record.get('runtime_version') == 1 and record.get('request_id') == identity,
                        'Mismatched runtime/request identity')
                if 'operation_id' in value:
                    require(record.get('operation_id') == value['operation_id'], 'Mismatched operation identity')
                if record.get('kind') == 'phase':
                    require(not phases or record['sequence'] > phases[-1]['sequence'], 'Nonincreasing phase sequence')
                    phases.append(record)
                    continue
                require(record.get('kind') == 'response', 'Unexpected native record')
                timing.update(terminal_receipt_perf_ns=received, seconds=(received - began) / 1e9)
                write(directory / 'response.json', record)
                require(record.get('ok') is True, 'Native failure: ' + json.dumps(record))
                self.writer.join(timeout=1)
                require(not self.writer.is_alive(), 'Unreclaimed stdin writer')
                require(not self.faults, 'Reader/sampler failure: ' + repr(self.faults))
                require(self.observations > 0, 'No valid Windows process memory observation; sample cannot pass')
                return record, timing
        except BaseException as error:
            timing['failure'] = repr(error)
            raise
        finally:
            timing['boundary_snapshot'] = self.snapshot(identity + '-after')
            write(directory / 'phases.json', phases)
            write(directory / 'timing.json', timing)

    def close(self, failed=False):
        if self.closed:
            return
        self.closed = True
        errors = []
        try:
            if not failed and self.child.poll() is None:
                try:
                    self.request('shutdown', {}, self.directory / 'shutdown')
                except BaseException as error:
                    errors.append(repr(error))
                    failed = True
            if failed and self.writer is not None and self.writer.is_alive():
                self.killed = True
                self.child.kill()
                self.child.wait(timeout=6)
                errors.append('Forced reclamation of blocked stdin: failed sample')
            self.child.stdin.close()
            try:
                self.child.wait(timeout=6)
            except subprocess.TimeoutExpired:
                self.killed = True
                self.child.kill()
                self.child.wait(timeout=6)
                errors.append('Forced child reclamation: failed sample; never cooperative Stop')
        finally:
            self.stop_sampler.set()
            for worker in self.threads:
                worker.join(timeout=6)
                if worker.is_alive():
                    errors.append('Unreclaimed reader/sampler thread')
            self.snapshot('process-exit')
            for stream in (self.child.stdout, self.child.stderr):
                stream.close()
            self.child._handle.Close()
            self.invocation.update(exit_code=self.child.returncode, exited_perf_ns=time.perf_counter_ns(),
                                   forced_kill=self.killed, errors=errors, thread_faults=self.faults,
                                   memory_observations=self.observations, peak_working_set_bytes=self.peak_ws,
                                   peak_pagefile_bytes=self.peak_private)
            write(self.directory / 'invocation.json', self.invocation)
        require(not errors and not self.faults and self.child.returncode == 0 and self.observations > 0,
                'Child/reader reclamation or exit failure: ' + json.dumps(self.invocation))


def timed_sample(session, directory, profile, baseline, settings, report, old_token=None, cold_origin=None):
    directory.mkdir(parents=True, exist_ok=False)
    sample = {'status': 'failed', 'profile': profile['id'], 'process_pid': session.child.pid,
              'mode': 'cold' if cold_origin is not None else 'retained', 'result_path': str(directory / 'result.json')}
    origin = cold_origin if cold_origin is not None else time.perf_counter_ns()
    try:
        preview = directory / 'preview'
        preview.mkdir()
        prepared, prep = session.request('prepare', {'object_report': str(report), 'output_directory': str(preview)},
                                         directory / (directory.name + '-prepare'))
        token = prepared['result']['asset_token']
        require(prepared['result']['preparation_reused'] is (old_token is not None), 'Unexpected native preparation reuse')
        release_seconds = 0
        if old_token is not None:
            _, released = session.request('release', {'asset_token': old_token},
                                           directory / (directory.name + '-release'))
            release_seconds = released['seconds']
            prepared_terminal = released['terminal_receipt_perf_ns']
        else:
            prepared_terminal = prep['terminal_receipt_perf_ns']
        sample.update(prepare_command_seconds=prep['seconds'], old_token_release_seconds=release_seconds,
                      preparation_total_seconds=(prepared_terminal - origin) / 1e9,
                      old_token_release_included_in_preparation_total=True,
                      retained_native_bytes=prepared['retained_native_bytes'], native_prepare=prepared['result'])
        ticks, frequency = qpc()
        params = {'asset_token': token, 'settings': settings, 'result_path': sample['result_path'],
                  'stop_file': str(directory / 'stop.marker'), 'start_qpc_ticks': ticks, 'qpc_frequency_hz': frequency}
        terminal, run = session.request('run', params, directory / (directory.name + '-run'))
        sample.update(run_command_terminal_seconds=run['seconds'],
                      fixed_work_start_terminal_seconds=(run['terminal_receipt_perf_ns'] - origin) / 1e9,
                      native_terminal=terminal, terminal_receipt_perf_ns=run['terminal_receipt_perf_ns'])
        result_path = Path(sample['result_path'])
        result = read(result_path)
        verify_result(result, profile, baseline, settings)
        require(terminal['result']['count'] == 1 and terminal['result']['preparation_reused'] is True,
                'Terminal count/reuse mismatch')
        runtime = result['search']['runtime']
        require(runtime['measurement_boundary'] == 'before_result_commit', 'Unexpected saved timing boundary')
        terminal_runtime = terminal['result'].get('runtime')
        if terminal_runtime is not None:
            require(terminal_runtime['measurement_boundary'] == 'before_terminal_response',
                    'Unexpected terminal timing boundary')
        for key in ('completion_elapsed_seconds', 'native_completion_elapsed_seconds'):
            seconds = terminal['result'][key]
            require(isinstance(seconds, (int, float)) and math.isfinite(seconds) and seconds >= 0,
                    'Invalid native completion timing: ' + key)
        require(terminal['result']['native_completion_elapsed_seconds'] <=
                terminal['result']['completion_elapsed_seconds'], 'Native completion exceeds total completion')
        native_work = terminal['result']['diagnostics']['work']
        require({key: native_work[key] for key in baseline['work']} == baseline['work'], 'Terminal work differs')
        sample.update(status='complete', count=result['count'], work=result['search']['work_counts'],
                      placements=result['placements'], validation=result['validation'], engine=result['engine'],
                      result_sha256=digest(result_path), native_search_seconds=result['search']['elapsed_seconds'],
                      saved_precommit_runtime=runtime, native_terminal_runtime=terminal_runtime,
                      native_terminal_runtime_available=terminal_runtime is not None,
                      native_completion_seconds=terminal['result']['completion_elapsed_seconds'],
                      native_process_completion_seconds=terminal['result']['native_completion_elapsed_seconds'],
                      native_tracked_payload_peak_bytes=terminal['result']['diagnostics']['tracked_working_bytes_peak'],
                      native_retained_bytes_after_run=terminal['retained_native_bytes'],
                      process_memory_observations=session.observations,
                      process_peak_working_set_bytes=session.peak_ws, process_peak_pagefile_bytes=session.peak_private)
        # Read actual exported asset bytes too; a report assertion alone is insufficient identity evidence.
        output_pins = {}
        pin_artifacts(result_path, output_pins)
        sample['result_artifacts'] = output_pins
        return token, sample
    except BaseException as error:
        sample.update(status='failed', failure=repr(error), traceback=traceback.format_exc())
        raise
    finally:
        write(directory / 'sample.json', sample)


def load_inputs(args, output):
    pins = {}
    fixture = ROOT / 'tests/fixtures/t010/preparation-profiles.json'
    baseline_path = ROOT / 'tests/fixtures/t010/start-baseline.json'
    profiles, baseline = read(fixture), read(baseline_path)
    for path in (fixture, baseline_path, Path(__file__).resolve(), args.old_root / 'summary.json'):
        pins[str(path.resolve())] = digest(path)
    require(profiles['baseline_engine_sha256'] == baseline['baseline_engine_sha256'], 'Baseline engine mismatch')
    old_summary = read(args.old_root / 'summary.json')
    require(old_summary['engine_sha256'] == baseline['baseline_engine_sha256'], 'Old raw-series engine mismatch')
    names = {'analytic-medium': ROOT / '.local/t009/integration/cli-qualified/object.report.json',
             'ulamok': ROOT / '.local/t010/inventory/desktop-prepare-old-final/ulamok-h16/import/object.report.json',
             'pryanik-1': ROOT / '.local/t010/inventory/desktop-prepare-old-final/pryanik-1-h4/import/object.report.json',
             'pryanik-2': ROOT / '.local/t010/inventory/desktop-prepare-old-final/pryanik-2-h4/import/object.report.json'}
    for override in args.report:
        key, value = override.split('=', 1)
        require(key in names, 'Unknown report profile: ' + key)
        names[key] = Path(value).resolve()
    cases = []
    for profile in profiles['profiles']:
        name = profile['id']
        if args.profile and name not in args.profile:
            continue
        frozen = next(p for p in baseline['profiles'] if p['id'] == name)
        target = next(p for p in profiles['frozen_targets']['profiles'] if p['id'] == name)
        old = next(p for p in old_summary['profiles'] if p['id'] == name)
        require(len(old['samples']) == 6, 'Missing old warmup/five samples')
        old_private = 0
        for index, frozen_sample in enumerate(old['samples']):
            case = args.old_root / name / ('warmup' if index == 0 else f'sample-{index}')
            record = case / 'sample.json'
            actual_sha = digest(record)
            require(actual_sha == frozen['sample_record_sha256'][index], 'Old sample pin mismatch: ' + str(record))
            pins[str(record.resolve())] = actual_sha
            actual = read(record)
            require(actual == frozen_sample, 'Raw old summary/sample mismatch')
            old_private = max(old_private, actual['prepare']['peak_pagefile_bytes'], actual['solve']['peak_pagefile_bytes'])
            settings_path, result_path = case / 'settings.json', case / 'result.json'
            settings = read(settings_path)
            require(settings == actual['settings'], 'Old settings differ from hash-pinned baseline sample')
            verify_settings(settings, profile, actual['settings'])
            verify_result(read(result_path), profile, frozen, settings)
            for path in (settings_path, result_path, case / 'solve-evidence/stdout.json'):
                pins[str(path.resolve())] = digest(path)
        settings = read(args.old_root / name / 'sample-1/settings.json')
        settings['compute']['thread_count'] = 1
        report_path = names[name].resolve()
        report = pin_artifacts(report_path, pins)
        require(report['source']['sha256'] == profile['source_sha256'], 'Report source mismatch')
        require(report['accepted_solid']['sha256'] == settings['object_asset']['accepted_solid_sha256'],
                'Report accepted mismatch')
        require(report['source']['units'] == 'mm' and report['source']['unit_scale_mm'] == 1, 'Changed physical scale')
        cases.append((profile, frozen, settings, report_path,
                      {**target, 'old_peak_pagefile_bytes': old_private,
                       'maximum_peak_pagefile_bytes': old_private + profiles['frozen_targets']['maximum_peak_increase_bytes']}))
    write(output / 'input-pins-before.json', pins)
    return cases, pins


def summarize(cold, warm, sessions, target, smoke):
    result = {'status': 'smoke_only' if smoke else 'complete_measurements', 'targets': target,
              'cold_samples': cold, 'retained_samples': warm}
    for mode, samples in (('cold', cold), ('retained', warm)):
        timed = samples if smoke else samples[1:]
        result[mode + '_medians'] = {key: statistics.median(s[key] for s in timed) for key in (
            'prepare_command_seconds', 'old_token_release_seconds', 'preparation_total_seconds',
            'run_command_terminal_seconds', 'fixed_work_start_terminal_seconds', 'native_search_seconds')}
    ws = max(s.peak_ws for s in sessions)
    private = max(s.peak_private for s in sessions)
    result.update(peak_working_set_bytes=ws, peak_pagefile_bytes=private)
    if not smoke:
        median = result['retained_medians']
        result['comparisons'] = {
            'preparation_target_met': median['preparation_total_seconds'] <= target['warm_preparation_target_seconds'],
            'fixed_work_start_target_met': median['fixed_work_start_terminal_seconds'] <= target['warm_fixed_work_start_target_seconds'],
            'peak_working_set_limit_met': ws <= target['maximum_peak_working_set_bytes'],
            'peak_pagefile_limit_met': private <= target['maximum_peak_pagefile_bytes']}
        result['all_frozen_targets_met'] = all(result['comparisons'].values())
    return result


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)  # never overwrite success or failure evidence
    summary = {'status': 'failed', 'scope': 'T011-A6 preparation and fixed-work Start; no full spectral/threading claim',
               'argv': sys.argv, 'host': {'platform': platform.platform(), 'machine': platform.machine(),
                                        'processor': platform.processor(), 'logical_cpu_count': os.cpu_count()},
               'engine': str(args.engine.resolve()), 'expected_engine_sha256': args.engine_sha256,
               'warmups': 1, 'timed_samples': 5, 'profiles': [],
               'cold_definition': 'fresh native process/context; OS file cache is NOT reset',
               'retained_definition': 'one native process per profile; bootstrap prepare excluded; prepare/release/run serial',
               'timing': 'complete NDJSON terminal receipt; combined Start includes prepare and old-token release; cold includes process launch; result checks and shutdown excluded',
               'memory': '10ms observations plus operation boundaries; PeakWorkingSetSize/PeakPagefileUsage are process historical high-water marks, never per-request deltas',
               'baseline_timing': 'old prepare+solve through process exit (baseline includes polling/reclamation); new through complete terminal receipt; raw old samples retained',
               'smoke': args.smoke}
    pins = None
    engine_before = None
    failure = None
    try:
        write(output / 'provenance.json', summary)
        engine_before = digest(args.engine)
        require(engine_before == args.engine_sha256, 'Held executable SHA-256 mismatch')
        summary['engine_sha256_before'] = engine_before
        cases, pins = load_inputs(args, output)
        if args.check_inputs:
            summary['status'] = 'inputs_checked_no_engine_run'
        else:
            require(sys.platform == 'win32', 'Windows QPC/process memory measurement required')
            count = 1 if args.smoke else 6
            for profile, baseline, settings, report, target in cases:
                name = profile['id']
                directory = output / name
                directory.mkdir()
                write(directory / 'profile.json', profile)
                write(directory / 'settings.json', settings)
                cold, warm, sessions = [], [], []
                # Fresh process/context on every cold repetition; startup included in sample origin.
                for index in range(count):
                    ordinal = 'smoke' if args.smoke else ('warmup' if index == 0 else f'sample-{index}')
                    origin = time.perf_counter_ns()
                    session = Session(args.engine.resolve(), directory / ('cold-process-' + ordinal), args.watchdog_seconds)
                    sessions.append(session)
                    failed = True
                    try:
                        _, sample = timed_sample(session, directory / ('cold-' + ordinal), profile, baseline,
                                                 settings, report, cold_origin=origin)
                        cold.append(sample)
                        failed = False
                    finally:
                        session.close(failed)
                session = Session(args.engine.resolve(), directory / 'retained-process', args.watchdog_seconds)
                sessions.append(session)
                failed = True
                try:
                    preview = directory / 'bootstrap-preview'
                    preview.mkdir()
                    bootstrap, _ = session.request('prepare', {'object_report': str(report), 'output_directory': str(preview)},
                                                    directory / 'bootstrap-prepare')
                    token = bootstrap['result']['asset_token']
                    for index in range(count):
                        ordinal = 'smoke' if args.smoke else ('warmup' if index == 0 else f'sample-{index}')
                        token, sample = timed_sample(session, directory / ('retained-' + ordinal), profile, baseline,
                                                     settings, report, old_token=token)
                        warm.append(sample)
                    failed = False
                finally:
                    session.close(failed)
                entry = summarize(cold, warm, sessions, target, args.smoke)
                entry['id'] = name
                summary['profiles'].append(entry)
                write(output / 'summary-partial.json', summary)
            summary['status'] = 'smoke_only' if args.smoke else 'complete_measurements'
            summary['overall_target_verdict'] = 'not_evaluated_smoke'
            if not args.smoke:
                met = all(entry['all_frozen_targets_met'] for entry in summary['profiles'])
                all_four = len(summary['profiles']) == 4
                summary.update(overall_target_verdict='met' if met else 'missed',
                               all_four_profiles_measured=all_four, four_profile_acceptance_met=met and all_four)
                if not met:
                    summary['status'] = 'complete_measurements_targets_missed'
                    failure = ValueError('One or more frozen targets missed; raw completed measurements retained')
    except BaseException as error:
        failure = error
        summary['status'] = 'failed'
        write(output / 'failure.json', {'error': repr(error), 'traceback': traceback.format_exc(),
                                        'watchdog_or_kill_is_failure': True})
    finally:
        try:
            engine_after = digest(args.engine)
            summary['engine_sha256_after'] = engine_after
            require(engine_before == engine_after == args.engine_sha256, 'Executable changed during series')
            if pins is not None:
                after = {path: digest(Path(path)) for path in pins}
                write(output / 'input-pins-after.json', after)
                require(after == pins, 'An immutable input changed during series')
        except BaseException as error:
            summary['status'] = 'failed'
            write(output / 'identity-failure.json', {'error': repr(error), 'traceback': traceback.format_exc()})
            failure = failure or error
        write(output / 'summary.json', summary)
    if failure:
        print('Failed series preserved at ' + str(output) + ': ' + repr(failure), file=sys.stderr)
        return 1
    print(json.dumps({'status': summary['status'], 'evidence': str(output)}))
    return 0


def main():
    warnings.simplefilter('error', ResourceWarning)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', required=True, type=Path, help='Explicit held Release desktop-session executable')
    parser.add_argument('--engine-sha256', required=True, help='Pinned exact executable SHA-256')
    parser.add_argument('--output', required=True, type=Path, help='Fresh evidence directory; existing paths refused')
    parser.add_argument('--old-root', type=Path, default=ROOT / '.local/t010/start-baseline', help='Frozen old raw series')
    parser.add_argument('--report', action='append', default=[], metavar='ID=PATH', help='Immutable report override')
    parser.add_argument('--profile', action='append', choices=['analytic-medium', 'ulamok', 'pryanik-1', 'pryanik-2'],
                        help='Subset (default all four); a subset cannot close the four-profile gate')
    parser.add_argument('--watchdog-seconds', type=float, default=180, help='Per-request watchdog; expiry is failure')
    parser.add_argument('--check-inputs', action='store_true', help='Hash/verify provenance only; never launch engine')
    parser.add_argument('--smoke', action='store_true', help='One analytic cold/retained pair; no target verdict')
    args = parser.parse_args()
    parser.error('Invalid SHA-256') if not re.fullmatch('[0-9a-f]{64}', args.engine_sha256) else None
    if not math.isfinite(args.watchdog_seconds) or args.watchdog_seconds <= 0:
        parser.error('watchdog must be positive and finite')
    if args.smoke:
        if args.profile and args.profile != ['analytic-medium']:
            parser.error('smoke permits only analytic-medium')
        args.profile = ['analytic-medium']
    try:
        return run(args)
    except FileExistsError:
        print('Evidence directory exists; choose a fresh --output path.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
