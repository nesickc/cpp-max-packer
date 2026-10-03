"""ADR0013 / T010-A5 / T011-A1-A5: real retained native process boundary."""
import argparse
import ctypes
import hashlib
import json
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import solve_subprocess_test as fixtures

ENGINE = None
PRACTICAL_REPORT = None
MEMORY_HELPER = None
OVERLAP_EVIDENCE = None


def qpc():
    ticks, frequency = ctypes.c_int64(), ctypes.c_int64()
    assert ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(ticks))
    assert ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(frequency))
    return str(ticks.value), str(frequency.value)


class Session:
    def __init__(self, command=None):
        self.child = subprocess.Popen(command or [str(ENGINE), "desktop-session"], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                      text=True, encoding="utf-8", bufsize=1)
        # The native transport is binary NDJSON. Preserve the exact record limit
        # instead of adding a Windows CR byte to the tested 1 MiB payload.
        self.child.stdin.reconfigure(newline="\n")
        self.records = queue.Queue()
        self.stderr = []
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()
        self.errors = threading.Thread(target=lambda: self.stderr.append(self.child.stderr.read()), daemon=True)
        self.errors.start()

    def _read(self):
        for line in self.child.stdout:
            self.records.put(json.loads(line))
        self.records.put(None)

    def request(self, value, on_phase=None):
        self.child.stdin.write(json.dumps(value) + "\n")
        self.child.stdin.flush()
        phases = []
        while True:
            record = self.records.get(timeout=15)
            assert record is not None, "Native session ended: " + "".join(self.stderr)
            assert record["request_id"] == value["request_id"]
            if record["kind"] == "phase":
                assert record["operation_id"] == value["operation_id"]
                assert not phases or record["sequence"] > phases[-1]["sequence"]
                phases.append(record)
                if on_phase:
                    on_phase(record)
            else:
                return record, phases

    def close(self):
        if not self.child.stdin.closed:
            self.child.stdin.close()
        if self.child.poll() is None:
            try:
                self.child.wait(timeout=6)
            except subprocess.TimeoutExpired:
                self.child.kill()
                self.child.wait()
                raise
        self.reader.join(timeout=1)
        self.errors.join(timeout=1)
        self.child.stdout.close()
        self.child.stderr.close()


class RuntimeTests(unittest.TestCase):
    def test_real_field_and_fft_phase_stop_keeps_valid_baseline(self):
        """T011-A4: native fields/FFT, real Stop marker, and checked terminal result."""
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source, report = root/'cube.stl', root/'object.report.json'
            fixtures.write_cube_stl(source, extent=40)
            imported = subprocess.run([str(ENGINE), 'inspect', '--stl', str(source), '--units', 'mm',
                                       '--report', str(report)], capture_output=True, text=True, timeout=15)
            self.assertEqual(imported.returncode, 0, imported.stdout+imported.stderr)
            session = Session()
            try:
                preview = root/'preview'; preview.mkdir()
                prepared, _ = session.request({'runtime_version': 1, 'request_id': 'phase-prepare',
                    'operation_id': 'phase-prepare-op', 'method': 'prepare', 'params': {
                        'object_report': str(report), 'output_directory': str(preview)}})
                self.assertTrue(prepared['ok'], prepared)
                fixture = fixtures.SolveSubprocessTests()
                fixture.object_report_path = report
                settings = fixture.settings(dimensions=(64, 64, 64), candidates=2)
                settings['search']['work_budget']['max_search_passes'] = 2
                settings['resolution']['pitch_mm'] = settings['resolved']['pitch_mm'] = 1
                reference_placements = None
                for target in ('voxelizing', 'planning_fft'):
                    with self.subTest(phase=target):
                        output = root/target; output.mkdir()
                        marker = output/'stop.marker'
                        stopped = []
                        def stop_during_phase(record):
                            if record['phase'] == target and not stopped:
                                # Let the real native phase begin before writing Stop.
                                time.sleep(.02)
                                began = time.perf_counter()
                                marker.write_bytes(b'stop')
                                stopped.append((began, time.perf_counter()-began))
                        ticks, frequency = qpc()
                        reply, phases = session.request({'runtime_version': 1, 'request_id': target,
                            'operation_id': target+'-op', 'method': 'run', 'params': {
                                'asset_token': prepared['result']['asset_token'], 'settings': settings,
                                'result_path': str(output/'result.json'), 'stop_file': str(marker),
                                'start_qpc_ticks': ticks, 'qpc_frequency_hz': frequency}}, stop_during_phase)
                        finished = time.perf_counter()
                        self.assertEqual(len(stopped), 1, (reply, phases))
                        self.assertLessEqual(stopped[0][1], .250)
                        self.assertLessEqual(finished-stopped[0][0], 5)
                        self.assertTrue(reply['ok'], reply)
                        self.assertEqual(reply['result']['termination_reason'], 'user_stopped', reply)
                        saved = json.loads((output/'result.json').read_text())
                        self.assertEqual(saved['validation']['status'], 'valid')
                        self.assertEqual(saved['count'], 1)
                        if reference_placements is None:
                            reference_placements = saved['placements']
                        self.assertEqual(saved['placements'], reference_placements)
                        observed = next(p['elapsed_seconds'] for p in saved['search']['runtime']['phases']
                                        if p['phase'] == target)
                        self.assertGreaterEqual(observed, .02)
                        print(json.dumps({'case': 'native-phase-stop', 'phase': target,
                            'marker_write_seconds': stopped[0][1], 'safe_stop_seconds': finished-stopped[0][0],
                            'observed_phase_seconds': observed, 'reply': reply, 'phases': phases,
                            'settings': settings, 'count': saved['count'], 'placements': saved['placements'],
                            'validation': saved['validation'], 'result_sha256': hashlib.sha256(
                                (output/'result.json').read_bytes()).hexdigest()}), flush=True)
                shutdown, _ = session.request({'runtime_version': 1, 'request_id': 'phase-shutdown',
                                               'method': 'shutdown', 'params': {}})
                self.assertTrue(shutdown['ok'], shutdown)
                self.assertEqual(session.child.wait(timeout=5), 0)
            finally:
                session.close()

    def test_allocation_counted_publication_and_invalid_request_overlap(self):
        if MEMORY_HELPER is None:
            self.skipTest("Allocation overlap requires the existing CLI test helper")
        with tempfile.TemporaryDirectory() as temporary:
            root = OVERLAP_EVIDENCE or pathlib.Path(temporary)
            root.mkdir(parents=True, exist_ok=True)
            source, report = root/'cube.stl', root/'object.report.json'
            fixtures.write_cube_stl(source)
            imported = subprocess.run([str(ENGINE), 'inspect', '--stl', str(source), '--units', 'mm',
                '--report', str(report)], capture_output=True, text=True, timeout=15)
            self.assertEqual(imported.returncode, 0, imported.stdout+imported.stderr)
            gate = root/'gate'; gate.mkdir()
            stats_path = root/'allocations.json'
            session = Session([str(MEMORY_HELPER), 'session-publication-barrier', str(gate), str(stats_path)])
            prepared = []
            try:
                for letter in ('a', 'b'):
                    preview = root/('preview-'+letter*12); preview.mkdir()
                    reply, _ = session.request({'runtime_version': 1, 'request_id': 'prepare-'+letter,
                        'operation_id': 'prepare-'+letter+'-op', 'method': 'prepare', 'params': {
                            'object_report': str(report), 'output_directory': str(preview)}})
                    self.assertTrue(reply['ok'], reply)
                    prepared.append(reply)
                self.assertNotEqual(prepared[0]['result']['asset_token'], prepared[1]['result']['asset_token'])
                self.assertTrue(prepared[1]['result']['preparation_reused'], prepared[1])
                fixture = fixtures.SolveSubprocessTests()
                fixture.object_report_path = report
                settings = fixture.settings(dimensions=(20, 20, 20), candidates=128)
                settings['orientation'] = {'mode': 'custom', 'quaternions_xyzw': [
                    [fixtures.NEAR_UNIT_COMPONENT]*4, [0.0, 0.0, 0.0, 1.0]]}
                settings['resolved']['orientation_catalog_sha256'] = fixtures.NEAR_UNIT_CUSTOM_SHA256
                output = root/('result-'+'r'*12); output.mkdir()
                ticks, frequency = qpc()
                invalid = {'runtime_version': 1, 'request_id': 'overlap-invalid',
                    'operation_id': 'overlap-invalid-op', 'method': 'run',
                    'params': {'payload': [{} for _ in range(2040)], 'text': ''}}
                # Seven non-text nodes plus 2,040 empty objects and text = 2,048 value nodes.
                decoded = sum(len(key) for key in invalid) + sum(len(key) for key in invalid['params'])
                decoded += sum(len(value) for value in invalid.values() if isinstance(value, str))
                invalid['params']['text'] = 'x'*(65536-decoded)
                rejected = []
                def overlap(phase):
                    if phase['phase'] != 'saving':
                        return
                    try:
                        until = time.monotonic()+2
                        while not (gate/'ready').exists() and time.monotonic() < until:
                            time.sleep(.01)
                        self.assertTrue((gate/'ready').exists(), 'Publication barrier did not become ready')
                        session.child.stdin.write(json.dumps(invalid)+'\n'); session.child.stdin.flush()
                        while True:
                            record = session.records.get(timeout=2)
                            self.assertIsNotNone(record, ''.join(session.stderr))
                            if record['request_id'] == invalid['request_id']:
                                self.assertEqual(record['kind'], 'response', record)
                                self.assertFalse(record['ok'], record)
                                self.assertEqual(record['error']['code'], 'INVALID_DOCUMENT', record)
                                rejected.append(record)
                                break
                            self.assertEqual(record['request_id'], 'overlap-run', record)
                            self.assertEqual(record['kind'], 'phase', record)
                    finally:
                        (gate/'release').write_bytes(b'release')
                reply, _ = session.request({'runtime_version': 1, 'request_id': 'overlap-run',
                    'operation_id': 'overlap-run-op', 'method': 'run', 'params': {
                        'asset_token': prepared[1]['result']['asset_token'], 'settings': settings,
                        'result_path': str(output/'result.json'), 'stop_file': str(output/'stop.marker'),
                        'start_qpc_ticks': ticks, 'qpc_frequency_hz': frequency}}, overlap)
                self.assertEqual(len(rejected), 1, reply)
                self.assertTrue(reply['ok'], reply)
                saved = json.loads((output/'result.json').read_text())
                self.assertEqual(saved['validation']['status'], 'valid')
                self.assertEqual(saved['count'], len(saved['placements']))
                shutdown, _ = session.request({'runtime_version': 1, 'request_id': 'overlap-shutdown',
                    'method': 'shutdown', 'params': {}})
                self.assertTrue(shutdown['ok'], shutdown)
            finally:
                (gate/'release').write_bytes(b'release')
                session.close()
            self.assertEqual(session.child.returncode, 0, ''.join(session.stderr))
            stats = json.loads(stats_path.read_text())
            self.assertGreater(stats['publication_baseline_cpp_bytes'], 0)
            self.assertGreaterEqual(stats['publication_peak_cpp_bytes'], stats['publication_baseline_cpp_bytes'])
            self.assertTrue(stats['publication_released_by_fixture'], stats)
            observation = {'allocations': stats, 'prepared': prepared, 'run_reply': reply,
                'invalid_reply': rejected[0], 'invalid_value_nodes': 2048, 'invalid_decoded_bytes': 65536,
                'prepared_reported_heavy_payload_bytes': prepared[1]['retained_native_bytes'],
                'saved_count': saved['count'], 'saved_result_sha256': hashlib.sha256(
                    (output/'result.json').read_bytes()).hexdigest()}
            (root/'observation.json').write_text(json.dumps(observation, indent=2))
            print(json.dumps({'adapter_overlap': stats, 'saved_count': saved['count']}))

    def test_stop_monitor_failure_during_publication(self):
        if MEMORY_HELPER is None:
            self.skipTest("Publication barrier requires the existing CLI test helper")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source, report = root/'cube.stl', root/'object.report.json'
            fixtures.write_cube_stl(source)
            imported = subprocess.run([str(ENGINE),'inspect','--stl',str(source),'--units','mm',
                                       '--report',str(report)],capture_output=True,text=True,timeout=15)
            self.assertEqual(imported.returncode,0,imported.stdout+imported.stderr)
            gate=root/'gate';gate.mkdir()
            session=Session([str(MEMORY_HELPER),'session-publication-barrier',str(gate)])
            try:
                preview=root/'preview';preview.mkdir()
                prepared,_=session.request({'runtime_version':1,'request_id':'prepare-publication',
                    'operation_id':'prepare-publication-op','method':'prepare','params':{
                        'object_report':str(report),'output_directory':str(preview)}})
                self.assertTrue(prepared['ok'],prepared)
                output=root/'run';output.mkdir();marker=output/'stop.marker'
                ticks,frequency=qpc()
                observed=[]
                def during_publication(phase):
                    if phase['phase'] != 'saving':
                        return
                    try:
                        until=time.monotonic()+2
                        while not (gate/'ready').exists() and time.monotonic()<until:
                            time.sleep(.01)
                        self.assertTrue((gate/'ready').exists(),'Publication barrier did not become ready')
                        marker.mkdir()
                        until=time.monotonic()+2
                        while not (gate/'observed').exists() and time.monotonic()<until:
                            time.sleep(.01)
                        observed.append((gate/'observed').exists())
                    finally:
                        (gate/'release').write_bytes(b'release')
                reply,_=session.request({'runtime_version':1,'request_id':'publication-run',
                    'operation_id':'publication-run-op','method':'run','params':{
                        'asset_token':prepared['result']['asset_token'],
                        'settings':{'desktop_version':1,'box_dimensions_mm':[20,20,20],
                            'clearance_mm':{'pair':0,'wall':0},'pitch_mm':10,
                            'orientation':{'mode':'fixed','quaternion_xyzw':[0,0,0,1]},
                            'budget_seconds':10,'seed':'42','thread_count':1,'budget_scope':'total_start'},
                        'result_path':str(output/'result.json'),'stop_file':str(marker),
                        'start_qpc_ticks':ticks,'qpc_frequency_hz':frequency}},during_publication)
                self.assertEqual(observed,[True],f'Stop monitor stopped before publication: {reply}')
                self.assertFalse(reply['ok'],reply)
                self.assertEqual(reply['error']['code'],'STOP_MONITOR_FAILED')
                document=json.loads((output/'result.json').read_text())
                self.assertEqual(document['validation']['status'],'valid')
            finally:
                (gate/'release').write_bytes(b'release')
                session.close()

    def test_increasing_escaped_requests_cache_owned_failure_without_losing_epoch(self):
        session = Session()
        try:
            for length in (30000, 32500):
                name = f"escaped-path-{length}"
                request = {'runtime_version': 1, 'request_id': name, 'operation_id': name + '-op',
                           'method': 'prepare', 'params': {'object_report': '\x01' * length,
                                                        'output_directory': '\x01' * length}}
                reply, _ = session.request(request)
                self.assertFalse(reply['ok'], reply)
                replay, phases = session.request(request)
                self.assertEqual(replay, reply)
                self.assertFalse(phases)
            reply, _ = session.request({'runtime_version': 1, 'request_id': 'after-increasing-escaped-paths',
                                        'method': 'shutdown', 'params': {}})
            self.assertTrue(reply['ok'], reply)
        finally:
            session.close()

    def test_prelexer_rejections_keep_the_session_epoch_usable(self):
        session = Session()
        try:
            records = [("invalid-request", "\t" * ((1 << 20) - 1) + "!", "INVALID_RUNTIME_RECORD")]
            for length in (127, 128, 129):
                name = f"numeric-spelling-{length}"
                raw = ('{"runtime_version":1,"request_id":"' + name +
                       '","operation_id":"' + name + '-op","method":"run","params":{"value":1.' +
                       '0' * (length - 2) + '}}')
                records.append((name, raw, "INVALID_DOCUMENT" if length <= 128 else "INVALID_RUNTIME_RECORD"))
            for identity, raw, code in records:
                session.child.stdin.write(raw + "\n")
                session.child.stdin.flush()
                reply = session.records.get(timeout=5)
                self.assertIsNotNone(reply)
                self.assertEqual(reply['kind'], 'response', reply)
                self.assertFalse(reply['ok'], reply)
                self.assertEqual(reply['request_id'], identity, reply)
                self.assertEqual(reply['error']['code'], code, reply)
            reply, _ = session.request({'runtime_version': 1, 'request_id': 'after-prelexer-rejections',
                                        'method': 'shutdown', 'params': {}})
            self.assertTrue(reply['ok'], reply)
        finally:
            session.close()

    def test_releasing_failed_replacement_preserves_authority_and_allows_next_prepare(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source, report = root / 'cube.stl', root / 'object.report.json'
            fixtures.write_cube_stl(source)
            imported = subprocess.run([str(ENGINE), 'inspect', '--stl', str(source), '--units', 'mm',
                                       '--report', str(report)], capture_output=True, text=True, timeout=15)
            self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
            session = Session()
            try:
                def prepare(name):
                    output = root / name
                    output.mkdir()
                    reply, _ = session.request({'runtime_version': 1, 'request_id': name,
                        'operation_id': name + '-op', 'method': 'prepare', 'params': {
                            'object_report': str(report), 'output_directory': str(output)}})
                    return reply
                first = prepare('prepare-a')
                self.assertTrue(first['ok'], first)
                first_token = first['result']['asset_token']
                second = prepare('prepare-b')
                self.assertTrue(second['ok'], second)
                released, _ = session.request({'runtime_version': 1, 'request_id': 'rollback-b',
                    'method': 'release', 'params': {'asset_token': second['result']['asset_token']}})
                self.assertTrue(released['ok'], released)
                self.assertEqual(released['retained_native_bytes'], first['retained_native_bytes'])
                third = prepare('prepare-c')
                self.assertTrue(third['ok'], third)
                self.assertNotEqual(third['result']['asset_token'], first_token)
                self.assertEqual(third['retained_native_bytes'], first['retained_native_bytes'])
            finally:
                session.close()

    def test_malformed_compute_object_returns_owned_error_without_losing_epoch(self):
        session = Session()
        try:
            reply, _ = session.request({'runtime_version': 1, 'request_id': 'malformed-compute',
                                        'operation_id': 'malformed-compute-op', 'method': 'run',
                                        'params': {'settings': {'compute': 0}}})
            self.assertFalse(reply['ok'], reply)
            self.assertEqual(reply['error']['code'], 'INVALID_DOCUMENT', reply)
            reply, _ = session.request({'runtime_version': 1, 'request_id': 'after-malformed-compute',
                                        'method': 'shutdown', 'params': {}})
            self.assertTrue(reply['ok'], reply)
        finally:
            session.close()

    def test_invalid_request_identity_is_bounded_and_does_not_poison_epoch(self):
        session = Session()
        try:
            request = {'runtime_version': 1, 'request_id': 'x' * 129,
                       'method': 'shutdown', 'params': {}}
            session.child.stdin.write(json.dumps(request) + '\n')
            session.child.stdin.flush()
            reply = session.records.get(timeout=5)
            self.assertIsNotNone(reply)
            self.assertEqual(reply['kind'], 'response', reply)
            self.assertFalse(reply['ok'], reply)
            self.assertEqual(reply['request_id'], 'invalid-request', reply)
            reply, _ = session.request({'runtime_version': 1, 'request_id': 'after-invalid-identity',
                                        'method': 'shutdown', 'params': {}})
            self.assertTrue(reply['ok'], reply)
        finally:
            session.close()

    def test_cold_total_start_interrupts_actual_native_preparation(self):
        if PRACTICAL_REPORT is None:
            self.skipTest('practical report supplied by the T010 acceptance harness')
        record=json.loads(PRACTICAL_REPORT.read_text())
        with tempfile.TemporaryDirectory() as temporary:
            root=pathlib.Path(temporary)
            for stopped in (False,True):
                output=root/('stopped' if stopped else 'deadline');output.mkdir()
                settings={'settings_version':1,'object_asset':{'source_sha256':record['source']['sha256'],
                          'accepted_solid_sha256':record['accepted_solid']['sha256']},
                          'container':{'kind':'box','dimensions_mm':[300,300,200]},'clearance_mm':{'pair':0,'wall':0},
                          'orientation':{'mode':'fixed','quaternion_xyzw':[0,0,0,1]},
                          'search':{'preset':'subprocess','seed':'0','deterministic':False,
                                    'budget_scope':'total_start','budget_seconds':60 if stopped else .35},
                          'resolution':{'mode':'manual','pitch_mm':4},'compute':{'backend':'cpu','thread_count':1},
                          'resolved':{'pitch_mm':4,'orientation_catalog_sha256':fixtures.IDENTITY_CATALOG_SHA256,
                                      'orientation_catalog_version':1,'backend':'cpu','thread_count':1}}
                settings_path=output/'settings.json';settings_path.write_text(json.dumps(settings))
                marker=output/'stop.marker';began=time.perf_counter()
                child=subprocess.Popen([str(ENGINE),'solve','--settings',str(settings_path),'--object-report',str(PRACTICAL_REPORT),
                                        '--result',str(output/'result.json'),'--stop-file',str(marker)],
                                       stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                stop_at=None
                if stopped:
                    time.sleep(.5);stop_at=time.perf_counter();marker.write_bytes(b'stop')
                stdout,stderr=child.communicate(timeout=6);finished=time.perf_counter()
                terminal=json.loads(stdout)
                self.assertNotEqual(child.returncode,0,(stdout,stderr))
                self.assertEqual(terminal['error']['code'],'OPERATION_CANCELLED' if stopped else 'DEADLINE_EXCEEDED',terminal)
                runtime=terminal['error']['details']['runtime']
                preparation=next(p['elapsed_seconds'] for p in runtime['phases'] if p['phase']=='preparing')
                self.assertGreater(preparation,.01,terminal)
                self.assertEqual(runtime['measurement_boundary'],'before_terminal_response')
                self.assertFalse((output/'result.json').exists())
                self.assertLessEqual(finished-(stop_at if stopped else began),5 if stopped else 5.35)
                print(json.dumps({'case':'cold-phase-stop' if stopped else 'cold-phase-deadline',
                                  'elapsed_seconds':finished-began,'safe_stop_seconds':None if stop_at is None else finished-stop_at,
                                  'runtime':runtime}),flush=True)

    def test_phase_qualified_prepare_stop_and_parent_eof(self):
        if PRACTICAL_REPORT is None:
            self.skipTest('practical report supplied by the T010 acceptance harness')
        with tempfile.TemporaryDirectory() as temporary:
            root=pathlib.Path(temporary)
            for eof in (False,True):
                output=root/('eof' if eof else 'stop');output.mkdir()
                session=Session()
                try:
                    marker=output/'stop.marker'
                    request={'runtime_version':1,'request_id':'phase-preparation','operation_id':'phase-preparation-op',
                             'method':'prepare','params':{'object_report':str(PRACTICAL_REPORT),'output_directory':str(output),
                                                        'stop_file':str(marker)}}
                    session.child.stdin.write(json.dumps(request)+'\n');session.child.stdin.flush()
                    while True:
                        phase=session.records.get(timeout=5)
                        self.assertIsNotNone(phase)
                        self.assertEqual(phase['kind'],'phase',phase)
                        if phase['phase']=='preparing':break
                    time.sleep(.02)
                    began=time.perf_counter()
                    if eof:session.child.stdin.close()
                    else:marker.write_bytes(b'stop')
                    while True:
                        reply=session.records.get(timeout=5)
                        self.assertIsNotNone(reply)
                        if reply['kind']=='response':break
                    self.assertFalse(reply['ok'],reply)
                    self.assertEqual(reply['error']['code'],'OPERATION_CANCELLED',reply)
                    self.assertLessEqual(time.perf_counter()-began,5)
                    self.assertEqual(reply['retained_native_bytes'],0)
                    if eof:self.assertEqual(session.child.wait(timeout=5),0)
                    else:
                        reply,_=session.request({'runtime_version':1,'request_id':'after-phase-stop','method':'shutdown','params':{}})
                        self.assertTrue(reply['ok'],reply)
                    print(json.dumps({'case':'phase-eof' if eof else 'phase-stop','safe_seconds':time.perf_counter()-began}),flush=True)
                finally:session.close()

    def test_structural_admission_rejects_adversarial_records_without_poisoning_session(self):
        session=Session()
        try:
            for index,params in enumerate(({'unknown':[0]*2049},{'unknown':'x'*(64*1024+1)},
                                           {'unknown':[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]})):
                request={'runtime_version':1,'request_id':f'adversarial-{index}','method':'shutdown','params':params}
                reply,_=session.request(request)
                self.assertFalse(reply['ok'])
                self.assertEqual(reply['error']['code'],'INVALID_RUNTIME_RECORD',reply)
            reply,_=session.request({'runtime_version':1,'request_id':'after-adversarial','method':'shutdown','params':{}})
            self.assertTrue(reply['ok'],reply)
            self.assertEqual(session.child.wait(timeout=5),0)
        finally:
            session.close()

    def test_auxiliary_allowance_rejects_before_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source, report = root / "cube.stl", root / "object.report.json"
            fixtures.write_cube_stl(source)
            result = subprocess.run([str(ENGINE), "inspect", "--stl", str(source), "--units", "mm",
                                     "--report", str(report), "--available-host-bytes", "1"],
                                    capture_output=True, text=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(json.loads(result.stdout)["error"]["code"], "MEMORY_LIMIT", result.stdout)
            self.assertFalse(report.exists())
            self.assertFalse((root / "assets").exists())

    def test_versioned_session_shutdown(self):
        session = Session()
        try:
            reply, _ = session.request({"runtime_version": 1, "request_id": "shutdown-1",
                                        "method": "shutdown", "params": {}})
            self.assertTrue(reply["ok"], reply)
            self.assertEqual(session.child.wait(timeout=5), 0)
        finally:
            session.close()

    def test_pinned_asset_reuse_stop_and_deadline(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source, report = root / "cube.stl", root / "object.report.json"
            fixtures.write_cube_stl(source)
            imported = subprocess.run([str(ENGINE), "inspect", "--stl", str(source), "--units", "mm",
                                       "--report", str(report)], capture_output=True, text=True, timeout=15)
            self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
            session = Session()
            try:
                preview = root / "preview"
                preview.mkdir()
                prepare = {"runtime_version": 1, "request_id": "prepare-1", "operation_id": "prepare-op",
                           "method": "prepare", "params": {"object_report": str(report),
                                                             "output_directory": str(preview)}}
                prepared, _ = session.request(prepare)
                self.assertTrue(prepared["ok"], prepared)
                token = prepared["result"]["asset_token"]
                initial_preview = (preview / "preview.ply").read_bytes()
                record=json.loads(report.read_text())
                original_source=source.read_bytes()
                preview2=root/'preview2';preview2.mkdir()
                repeated={**prepare,'request_id':'prepare-2','operation_id':'prepare-op-2',
                          'params':{'object_report':str(report),'output_directory':str(preview2)}}
                reused,_=session.request(repeated)
                self.assertTrue(reused['ok'],reused)
                self.assertTrue(reused['result']['preparation_reused'])
                self.assertEqual(reused['retained_native_bytes'],prepared['retained_native_bytes'])
                self.assertNotEqual(reused['result']['asset_token'],token)
                session.request({'runtime_version':1,'request_id':'release-old','method':'release','params':{'asset_token':token}})
                token=reused['result']['asset_token']
                fixed={'settings_version':1,'object_asset':{'source_sha256':record['source']['sha256'],
                       'accepted_solid_sha256':record['accepted_solid']['sha256']},
                       'container':{'kind':'box','dimensions_mm':[20,20,20]},'clearance_mm':{'pair':0,'wall':0},
                       'orientation':{'mode':'fixed','quaternion_xyzw':[0,0,0,1]},
                       'search':{'preset':'subprocess','seed':'0','deterministic':True,
                                 'work_budget':{'max_candidate_evaluations':1,'max_search_passes':1}},
                       'resolution':{'mode':'manual','pitch_mm':10},'compute':{'backend':'cpu'},
                       'resolved':{'pitch_mm':10,'orientation_catalog_sha256':fixtures.IDENTITY_CATALOG_SHA256,
                                   'orientation_catalog_version':1,'backend':'cpu','thread_count':1}}
                def fixed_run(name,stl=False,block_settings_path=False):
                    output=root/name;output.mkdir();ticks,frequency=qpc()
                    if block_settings_path:
                        unrelated=output/'session.settings.json';unrelated.mkdir()
                        (unrelated/'keep.txt').write_bytes(b'unrelated user data')
                    params={'asset_token':token,'settings':fixed,'result_path':str(output/'result.json'),
                            'stop_file':str(output/'stop.marker'),'start_qpc_ticks':ticks,'qpc_frequency_hz':frequency}
                    if stl:params['stl_path']=str(output/'packing.stl')
                    reply,_=session.request({'runtime_version':1,'request_id':name,'operation_id':name+'-op','method':'run','params':params})
                    self.assertTrue(reply['ok'],reply)
                    document=json.loads((output/'result.json').read_text())
                    self.assertGreater(document['count'],0)
                    self.assertEqual(document['validation']['status'],'valid')
                    if block_settings_path:
                        self.assertEqual((unrelated/'keep.txt').read_bytes(),b'unrelated user data')
                    return document,output
                before,_=fixed_run('before-tamper',block_settings_path=True)
                alias_output=root/'marker-output-alias';alias_output.mkdir();ticks,frequency=qpc()
                alias_path=alias_output/'result.json'
                alias_path.write_bytes(b'previous complete result')
                alias_reply,_=session.request({'runtime_version':1,'request_id':'marker-output-alias',
                    'operation_id':'marker-output-alias-op','method':'run','params':{
                        'asset_token':token,'settings':fixed,'result_path':str(alias_path),
                        'stop_file':str(alias_path),'start_qpc_ticks':ticks,'qpc_frequency_hz':frequency}})
                self.assertFalse(alias_reply['ok'],alias_reply)
                self.assertEqual(alias_reply['error']['code'],'INVALID_REQUEST')
                self.assertEqual(alias_path.read_bytes(),b'previous complete result')
                # A token pins independently verified source/accepted bytes; changing disk assertions cannot mint authority.
                source.write_bytes(b"tampered")
                (root/record['source']['path']).write_bytes(b'tampered pinned source')
                (root/record['accepted_solid']['path']).write_bytes(b'tampered accepted ply')
                report.write_text("{}")
                settings = {"desktop_version": 1, "box_dimensions_mm": [20, 20, 20],
                            "clearance_mm": {"pair": 0, "wall": 0},
                            "orientation": {"mode": "fixed", "quaternion_xyzw": [0, 0, 0, 1]},
                            "pitch_mm": 10, "budget_seconds": .01, "seed": "42",
                            "thread_count": 1, "budget_scope": "total_start"}
                for index, stopped in enumerate((True, False)):
                    output = root / f"run-{index}"
                    output.mkdir()
                    marker = output / "stop.marker"
                    if stopped:
                        marker.write_bytes(b"stop")
                    ticks, frequency = qpc()
                    if not stopped:
                        ticks=str(int(ticks)-2*int(frequency))
                    request = {"runtime_version": 1, "request_id": f"run-{index}",
                               "operation_id": f"run-op-{index}", "method": "run", "params": {
                                   "asset_token": token, "settings": settings, "result_path": str(output / "result.json"),
                                   "stop_file": str(marker), "start_qpc_ticks": ticks, "qpc_frequency_hz": frequency}}
                    reply, phases = session.request(request)
                    self.assertTrue(reply["ok"], reply)
                    self.assertTrue(reply["result"]["preparation_reused"])
                    self.assertEqual(reply['result']['runtime']['measurement_boundary'],'before_terminal_response')
                    self.assertGreaterEqual(reply['result']['runtime']['cleanup_seconds'],0)
                    self.assertGreaterEqual(reply['result']['runtime']['deadline_overrun_seconds'],0)
                    self.assertEqual(reply["result"]["termination_reason"], "user_stopped" if stopped else "budget_exhausted")
                    self.assertEqual((preview / "preview.ply").read_bytes(), initial_preview)
                    if stopped:
                        self.assertFalse((output / "result.json").exists())
                        self.assertTrue(reply["result"]["no_nonempty_incumbent"])
                    else:
                        self.assertTrue(phases)
                    replay, replay_phases = session.request(request)
                    self.assertEqual(replay, reply)
                    self.assertFalse(replay_phases)
                    request["params"]["settings"]["seed"] = "43"
                    conflicting, _ = session.request(request)
                    self.assertFalse(conflicting["ok"])
                    self.assertEqual(conflicting["error"]["code"], "REQUEST_ID_CONFLICT")
                    settings["seed"] = "42"
                for index,budget in enumerate((9223372036.854776,1e300)):
                    output=root/f'clock-range-{index}';output.mkdir();ticks,frequency=qpc()
                    rejected={**settings,'budget_seconds':budget}
                    reply,_=session.request({'runtime_version':1,'request_id':f'clock-range-{index}',
                        'operation_id':f'clock-range-{index}-op','method':'run','params':{
                            'asset_token':token,'settings':rejected,'result_path':str(output/'result.json'),
                            'stop_file':str(output/'stop.marker'),'start_qpc_ticks':ticks,'qpc_frequency_hz':frequency}})
                    self.assertFalse(reply['ok'])
                    self.assertEqual(reply['error']['code'],'INVALID_CLOCK_ANCHOR',reply)
                    self.assertFalse((output/'result.json').exists())
                after,output=fixed_run('after-tamper',True)
                self.assertEqual(after['placements'],before['placements'])
                self.assertEqual(after['count'],before['count'])
                self.assertEqual(after['assets']['object']['accepted_solid']['sha256'],record['accepted_solid']['sha256'])
                self.assertEqual((output/after['assets']['object']['source']['path']).read_bytes(),original_source)
                self.assertEqual(hashlib.sha256((output/after['assets']['object']['accepted_solid']['path']).read_bytes()).hexdigest(),record['accepted_solid']['sha256'])
                self.assertEqual((preview2/'preview.ply').read_bytes(),initial_preview)
            finally:
                session.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("engine", type=pathlib.Path)
    parser.add_argument("--case", default=None)
    parser.add_argument('--practical-report',type=pathlib.Path,default=None)
    parser.add_argument('--memory-helper',type=pathlib.Path,default=None)
    parser.add_argument('--overlap-evidence',type=pathlib.Path,default=None)
    arguments = parser.parse_args()
    ENGINE = arguments.engine.resolve()
    PRACTICAL_REPORT = arguments.practical_report.resolve() if arguments.practical_report else None
    MEMORY_HELPER = arguments.memory_helper.resolve() if arguments.memory_helper else None
    OVERLAP_EVIDENCE = arguments.overlap_evidence.resolve() if arguments.overlap_evidence else None
    unittest.main(argv=[sys.argv[0]] + (["RuntimeTests." + arguments.case] if arguments.case else []))
