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


def qpc():
    ticks, frequency = ctypes.c_int64(), ctypes.c_int64()
    assert ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(ticks))
    assert ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(frequency))
    return str(ticks.value), str(frequency.value)


class Session:
    def __init__(self):
        self.child = subprocess.Popen([str(ENGINE), "desktop-session"], stdin=subprocess.PIPE,
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

    def request(self, value):
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
                def fixed_run(name,stl=False):
                    output=root/name;output.mkdir();ticks,frequency=qpc()
                    params={'asset_token':token,'settings':fixed,'result_path':str(output/'result.json'),
                            'stop_file':str(output/'stop.marker'),'start_qpc_ticks':ticks,'qpc_frequency_hz':frequency}
                    if stl:params['stl_path']=str(output/'packing.stl')
                    reply,_=session.request({'runtime_version':1,'request_id':name,'operation_id':name+'-op','method':'run','params':params})
                    self.assertTrue(reply['ok'],reply)
                    document=json.loads((output/'result.json').read_text())
                    self.assertGreater(document['count'],0)
                    self.assertEqual(document['validation']['status'],'valid')
                    return document,output
                before,_=fixed_run('before-tamper')
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
    arguments = parser.parse_args()
    ENGINE = arguments.engine.resolve()
    PRACTICAL_REPORT = arguments.practical_report.resolve() if arguments.practical_report else None
    unittest.main(argv=[sys.argv[0]] + (["RuntimeTests." + arguments.case] if arguments.case else []))
