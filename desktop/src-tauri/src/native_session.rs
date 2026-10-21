//! A single retained native child. The Core serializes requests through its
//! operation lock; Stop remains an independent scoped marker write.
use crate::model::{error, id, DesktopError, Outcome};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    io::{BufRead, BufReader, Read, Write},
    path::Path,
    process::{Child, ChildStdin, Command, Stdio},
    sync::{Arc, Mutex},
    thread,
    time::{Duration, Instant},
};

const RECORD_LIMIT: usize = 1 << 20;

pub(crate) struct NativeSession {
    child: Child,
    input: Option<ChildStdin>,
    records: Option<std::sync::mpsc::Receiver<Outcome<Record>>>,
    output_reader: Option<thread::JoinHandle<()>>,
    error_reader: Option<thread::JoinHandle<()>>,
    pending_writer: Option<thread::JoinHandle<()>>,
    diagnostic: Arc<Mutex<String>>,
    pub epoch: String,
    pub retained_native_bytes: Option<u64>,
    pub admitted_host_cap_bytes: Option<u64>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Record {
    runtime_version: u32,
    request_id: String,
    operation_id: Option<String>,
    kind: String,
    ok: Option<bool>,
    result: Option<Value>,
    error: Option<DesktopError>,
    sequence: Option<u64>,
    phase: Option<String>,
    native_elapsed_seconds: Option<f64>,
    retained_native_bytes: Option<u64>,
    admitted_host_cap_bytes: Option<u64>,
    resource_status: Option<ResourceStatus>,
    publication: Option<Publication>,
    packing: Option<Packing>,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct ResourceStatus {
    admitted_host_cap_bytes: String,
    actual_threads: u32,
    recovery_count: String,
    last_cause: String,
    last_action: String,
}
impl ResourceStatus {
    fn validate(&self) -> Outcome<()> {
        let counter = |text: &str, positive: bool| {
            text.parse::<u64>().is_ok_and(|n| !positive || n > 0)
                && (text == "0" || (!text.starts_with('0') && text.bytes().all(|c| c.is_ascii_digit())))
        };
        if !counter(&self.admitted_host_cap_bytes, true) || !counter(&self.recovery_count, false)
            || !(1..=8).contains(&self.actual_threads) || self.last_cause.len() > 95 || self.last_action.len() > 95 {
            return Err(error("ENGINE_TRANSPORT", "Native resource status is malformed."));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, PartialEq, Eq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Publication {
    pub version: u32,
    pub session_id: String,
    pub native_epoch: String,
    pub settings_id: String,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Packing {
    pub solution_revision: u64,
    pub count: u64,
    pub validation_status: String,
    pub placements: Vec<Pose>,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Pose {
    copy_id: String,
    translation_mm: [f64; 3],
    quaternion_xyzw: [f64; 4],
    local_to_world: [[f64; 4]; 4],
}
impl Packing {
    fn validate(&self) -> Outcome<()> {
        let malformed = || error("ENGINE_TRANSPORT", "Native complete packing is malformed.");
        if self.solution_revision == 0
            || self.count == 0
            || self.count != self.placements.len() as u64
            || self.validation_status != "valid"
        {
            return Err(malformed());
        }
        let mut ids = std::collections::HashSet::new();
        for pose in &self.placements {
            if pose.copy_id.is_empty()
                || !ids.insert(&pose.copy_id)
                || !pose
                    .translation_mm
                    .iter()
                    .chain(pose.quaternion_xyzw.iter())
                    .all(|n| n.is_finite())
            {
                return Err(malformed());
            }
            let [x, y, z, w] = pose.quaternion_xyzw;
            let norm = x * x + y * y + z * z + w * w;
            if (norm - 1.0).abs() > 1e-9 {
                return Err(malformed());
            }
            let [tx, ty, tz] = pose.translation_mm;
            let expected = [
                [
                    1.0 - 2.0 * (y * y + z * z),
                    2.0 * (x * y - z * w),
                    2.0 * (x * z + y * w),
                    tx,
                ],
                [
                    2.0 * (x * y + z * w),
                    1.0 - 2.0 * (x * x + z * z),
                    2.0 * (y * z - x * w),
                    ty,
                ],
                [
                    2.0 * (x * z - y * w),
                    2.0 * (y * z + x * w),
                    1.0 - 2.0 * (x * x + y * y),
                    tz,
                ],
                [0.0, 0.0, 0.0, 1.0],
            ];
            for row in 0..4 {
                for col in 0..4 {
                    let actual = pose.local_to_world[row][col];
                    if !actual.is_finite()
                        || (actual - expected[row][col]).abs() > 1e-9 * (1.0 + expected[row][col].abs())
                    {
                        return Err(malformed());
                    }
                }
            }
        }
        Ok(())
    }
}

impl NativeSession {
    pub fn is_alive(&mut self) -> Outcome<bool> {
        self.child
            .try_wait()
            .map(|status| status.is_none())
            .map_err(|cause| error("ENGINE_TRANSPORT", cause.to_string()))
    }
    #[cfg(test)]
    pub fn kill_for_test(&mut self) {
        self.child.kill().unwrap();
        self.child.wait().unwrap();
    }
    pub fn spawn(engine: &Path, root: &Path) -> Outcome<Self> {
        let mut command = Command::new(engine);
        command
            .arg("desktop-session")
            .current_dir(root)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x08000000);
        }
        Self::from_command(command)
    }
    fn from_command(mut command: Command) -> Outcome<Self> {
        command
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut child = command
            .spawn()
            .map_err(|e| error("ENGINE_START", e.to_string()))?;
        let input = child
            .stdin
            .take()
            .ok_or_else(|| error("ENGINE_TRANSPORT", "Session input unavailable."))?;
        let output = child
            .stdout
            .take()
            .ok_or_else(|| error("ENGINE_TRANSPORT", "Session output unavailable."))?;
        let mut stderr = child
            .stderr
            .take()
            .ok_or_else(|| error("ENGINE_TRANSPORT", "Session diagnostic unavailable."))?;
        let diagnostic = Arc::new(Mutex::new(String::new()));
        let retained = diagnostic.clone();
        let error_reader = thread::spawn(move || {
            let mut buffer = [0u8; 4096];
            while let Ok(size) = stderr.read(&mut buffer) {
                if size == 0 {
                    break;
                }
                let mut log = retained.lock().unwrap();
                let text = String::from_utf8_lossy(&buffer[..size]);
                if log.len() + text.len() <= RECORD_LIMIT {
                    log.push_str(&text);
                }
            }
        });
        let (send, receive) = std::sync::mpsc::sync_channel(1);
        let output_reader = thread::spawn(move || {
            let mut stream = BufReader::new(output);
            loop {
                let mut line = Vec::new();
                let result = stream
                    .by_ref()
                    .take((RECORD_LIMIT + 2) as u64)
                    .read_until(b'\n', &mut line);
                let record = match result {
                    Ok(0) => Err(error("ENGINE_TERMINATED", "Native session output ended.")),
                    Ok(size) if size > RECORD_LIMIT + 1 || line.last() != Some(&b'\n') => {
                        Err(error(
                            "ENGINE_TRANSPORT",
                            "Native runtime record is oversized or truncated.",
                        ))
                    }
                    Ok(_) => serde_json::from_slice(&line)
                        .map_err(|e| error("ENGINE_TRANSPORT", e.to_string())),
                    Err(cause) => Err(error("ENGINE_TRANSPORT", cause.to_string())),
                };
                let failed = record.is_err();
                if send.send(record).is_err() || failed {
                    break;
                }
            }
        });
        Ok(Self {
            child,
            input: Some(input),
            records: Some(receive),
            output_reader: Some(output_reader),
            error_reader: Some(error_reader),
            pending_writer: None,
            diagnostic,
            epoch: id("native"),
            retained_native_bytes: None,
            admitted_host_cap_bytes: None,
        })
    }

    pub fn request(
        &mut self,
        method: &str,
        operation_id: Option<&str>,
        params: Value,
        mut phase: impl FnMut(u64, &str, f64),
    ) -> Outcome<Value> {
        self.request_with_packing(method, operation_id, params, |seq, name, seconds, _| phase(seq, name, seconds), None)
    }
    pub fn request_with_packing(
        &mut self,
        method: &str,
        operation_id: Option<&str>,
        params: Value,
        mut phase: impl FnMut(u64, &str, f64, Option<&ResourceStatus>),
        mut packing_sink: Option<&mut dyn FnMut(&Publication, &Packing, u64)>,
    ) -> Outcome<Value> {
        let request_id = id("request");
        let mut request = serde_json::json!({"runtime_version":1,"request_id":request_id,"method":method,"params":params});
        if let Some(operation) = operation_id {
            request["operation_id"] = Value::String(operation.into());
        }
        let request_started = Instant::now();
        let mut timeout_seconds = 60.0;
        if method == "run" {
            let settings = &request["params"]["settings"];
            let search = if settings.get("desktop_version").is_some() {
                settings
            } else {
                &settings["search"]
            };
            if let Some(budget) = search["budget_seconds"].as_f64() {
                // Windows steady-clock deadlines use signed nanosecond ticks.
                // Check the original budget before any transport write.
                if !budget.is_finite()
                    || budget <= 0.0
                    || budget >= i64::MAX as f64 / 1_000_000_000.0
                {
                    return Err(error(
                        "INVALID_SETTINGS",
                        "Runtime budget exceeds the native clock range.",
                    ));
                }
                let mut remaining = budget;
                if search["budget_scope"] == "total_start" {
                    if let (Some(anchor), Some(frequency), Ok((ticks, actual))) = (
                        request["params"]["start_qpc_ticks"]
                            .as_str()
                            .and_then(|x| x.parse::<u64>().ok()),
                        request["params"]["qpc_frequency_hz"]
                            .as_str()
                            .and_then(|x| x.parse::<u64>().ok()),
                        qpc(),
                    ) {
                        if frequency == actual && ticks >= anchor {
                            remaining -= (ticks - anchor) as f64 / frequency as f64;
                        }
                    }
                }
                // Native solve reserves at most 30 seconds for final validation/publication.
                timeout_seconds = remaining.max(0.0) + 31.0;
                if search["budget_scope"] != "total_start" {
                    timeout_seconds += 60.0;
                }
            }
        }
        if !timeout_seconds.is_finite() || timeout_seconds < 0.0 {
            return Err(error(
                "INVALID_SETTINGS",
                "Runtime timeout is not representable.",
            ));
        }
        let timeout = Duration::try_from_secs_f64(timeout_seconds)
            .map_err(|_| error("INVALID_SETTINGS", "Runtime timeout overflows."))?;
        let mut deadline = request_started
            .checked_add(timeout)
            .ok_or_else(|| error("INVALID_SETTINGS", "Runtime deadline overflows."))?;
        let fixed_work =
            method == "run" && request["params"]["settings"]["search"]["deterministic"] == true;
        let mut bytes =
            serde_json::to_vec(&request).map_err(|e| error("INVALID_REQUEST", e.to_string()))?;
        if bytes.len() > RECORD_LIMIT {
            return Err(error("INVALID_REQUEST", "Runtime record exceeds 1 MiB."));
        }
        bytes.push(b'\n');
        let mut input = self
            .input
            .take()
            .ok_or_else(|| error("ENGINE_TRANSPORT", "Native session is closed."))?;
        let (written, receive) = std::sync::mpsc::sync_channel(1);
        self.pending_writer = Some(
            thread::Builder::new()
                .name("native-runtime-write".into())
                .spawn(move || {
                    let outcome = input
                        .write_all(&bytes)
                        .and_then(|()| input.flush())
                        .map_err(|e| error("ENGINE_TRANSPORT", e.to_string()));
                    let _ = written.send((outcome, input));
                })
                .map_err(|e| error("ENGINE_TRANSPORT", e.to_string()))?,
        );
        let (outcome, input) = receive
            .recv_timeout(
                deadline
                    .saturating_duration_since(Instant::now())
                    .min(Duration::from_secs(1)),
            )
            .map_err(|e| {
                error(
                    "ENGINE_TRANSPORT_TIMEOUT",
                    format!("Native request write stalled: {e}"),
                )
            })?;
        self.input = Some(input);
        if let Some(writer) = self.pending_writer.take() {
            writer
                .join()
                .map_err(|_| error("ENGINE_TRANSPORT", "Native request writer failed."))?;
        }
        outcome?;
        let mut sequence = 0;
        let (mut packing_sequence, mut packing_revision, mut packing_count) = (0, 0, 0);
        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            let record = self
                .records
                .as_ref()
                .ok_or_else(|| error("ENGINE_TRANSPORT", "Native reader is closed."))?
                .recv_timeout(remaining)
                .map_err(|cause| {
                    error(
                        "ENGINE_TRANSPORT_TIMEOUT",
                        format!(
                            "Native response deadline failed: {cause}; {}",
                            self.diagnostic.lock().unwrap()
                        ),
                    )
                })??;
            if record.runtime_version != 1
                || record.request_id != request_id
                || record.operation_id.as_deref() != operation_id
            {
                return Err(error(
                    "ENGINE_IDENTITY",
                    "Native response identity does not match the active operation.",
                ));
            }
            match record.kind.as_str() {
                "packing" => {
                    let binding = record
                        .publication
                        .as_ref()
                        .ok_or_else(|| error("ENGINE_TRANSPORT", "Publication binding missing."))?;
                    if binding.version != 1
                        || binding.native_epoch != self.epoch
                        || serde_json::to_value(binding).unwrap() != request["params"]["publication"]
                    {
                        continue;
                    }
                    let packing = record
                        .packing
                        .as_ref()
                        .ok_or_else(|| error("ENGINE_TRANSPORT", "Packing missing."))?;
                    let next = record
                        .sequence
                        .ok_or_else(|| error("ENGINE_TRANSPORT", "Packing sequence missing."))?;
                    let seconds = record
                        .native_elapsed_seconds
                        .ok_or_else(|| error("ENGINE_TRANSPORT", "Packing time missing."))?;
                    if record.ok.is_some()
                        || record.result.is_some()
                        || record.error.is_some()
                        || record.phase.is_some()
                        || record.retained_native_bytes.is_some()
                        || record.admitted_host_cap_bytes.is_some()
                        || record.resource_status.is_some()
                        || !seconds.is_finite()
                        || seconds < 0.0
                        || next == 0
                    {
                        return Err(error("ENGINE_TRANSPORT", "Packing envelope is malformed."));
                    }
                    packing.validate()?;
                    if next <= packing_sequence
                        || packing.solution_revision <= packing_revision
                        || packing.count < packing_count
                    {
                        continue;
                    }
                    packing_sequence = next;
                    packing_revision = packing.solution_revision;
                    packing_count = packing.count;
                    if let Some(sink) = packing_sink.as_mut() {
                        sink(binding, packing, next);
                    }
                }
                "phase" => {
                    let next = record
                        .sequence
                        .ok_or_else(|| error("ENGINE_TRANSPORT", "Phase sequence missing."))?;
                    let seconds = record
                        .native_elapsed_seconds
                        .ok_or_else(|| error("ENGINE_TRANSPORT", "Phase time missing."))?;
                    let name = record
                        .phase
                        .ok_or_else(|| error("ENGINE_TRANSPORT", "Phase name missing."))?;
                    if next <= sequence
                        || !seconds.is_finite()
                        || seconds < 0.0
                        || record.ok.is_some()
                        || record.result.is_some()
                        || record.error.is_some()
                        || record.publication.is_some()
                        || record.packing.is_some()
                        || record.retained_native_bytes.is_some()
                        || record.admitted_host_cap_bytes.is_some()
                        || ![
                            "loading",
                            "preparing",
                            "voxelizing",
                            "planning_fft",
                            "placing",
                            "improving",
                            "validating",
                            "saving",
                            "cleanup",
                        ]
                        .contains(&name.as_str())
                    {
                        return Err(error(
                            "ENGINE_TRANSPORT",
                            "Native phase is malformed or stale.",
                        ));
                    }
                    sequence = next;
                    if fixed_work {
                        deadline = Instant::now()
                            .checked_add(Duration::from_secs(60))
                            .ok_or_else(|| {
                                error("ENGINE_TRANSPORT", "Progress timeout overflows.")
                            })?;
                    }
                    if let Some(status) = &record.resource_status { status.validate()?; }
                    phase(next, &name, seconds, record.resource_status.as_ref());
                }
                "response"
                    if record.sequence.is_none()
                        && record.phase.is_none()
                        && record.native_elapsed_seconds.is_none()
                        && record.publication.is_none()
                        && record.resource_status.is_none()
                        && record.packing.is_none() =>
                {
                    if ["prepare", "run", "release"].contains(&method) {
                        let cap = record.admitted_host_cap_bytes.filter(|cap| *cap > 0)
                            .ok_or_else(|| error("ENGINE_TRANSPORT", "Native admitted host ceiling is missing or invalid."))?;
                        let bytes = record
                            .retained_native_bytes
                            .filter(|bytes| *bytes <= cap)
                            .ok_or_else(|| {
                                error(
                                    "ENGINE_TRANSPORT",
                                    "Native retained residency is missing or invalid.",
                                )
                            })?;
                        self.retained_native_bytes = Some(bytes);
                        self.admitted_host_cap_bytes = Some(cap);
                    }
                    match (record.ok, record.result, record.error) {
                        (Some(true), Some(result), None) => return Ok(result),
                        (Some(false), None, Some(failure)) => return Err(failure),
                        _ => {
                            return Err(error(
                                "ENGINE_TRANSPORT",
                                "Native terminal fields are contradictory.",
                            ))
                        }
                    }
                }
                _ => {
                    return Err(error(
                        "ENGINE_TRANSPORT",
                        "Native runtime kind is unsupported.",
                    ))
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn t015_complete_native_packing_record_decodes() {
        let value = serde_json::json!({"runtime_version":1,"request_id":"request-live","operation_id":"op-live",
            "kind":"packing","publication":{"version":1,"session_id":"session-live","native_epoch":"native-live","settings_id":"settings-live"},
            "sequence":1,"native_elapsed_seconds":0.1,"packing":{"solution_revision":1,"count":1,"validation_status":"valid",
            "placements":[{"copy_id":"copy-0","translation_mm":[1,2,3],"quaternion_xyzw":[0,0,0,1],
            "local_to_world":[[1,0,0,1],[0,1,0,2],[0,0,1,3],[0,0,0,1]]}]}});
        let record = serde_json::from_value::<Record>(value).expect("ADR0017 native packing rejected");
        let packing = record.packing.unwrap();
        assert!(packing.validate().is_ok());
        let mut duplicate = packing.clone();
        duplicate.count = 2;
        duplicate.placements.push(duplicate.placements[0].clone());
        assert!(duplicate.validate().is_err());
        let mut matrix = packing.clone();
        matrix.placements[0].local_to_world[0][3] = 999.0;
        assert!(matrix.validate().is_err());
        let mut truncated = packing;
        truncated.count = 25;
        assert!(truncated.validate().is_err());
    }

    #[test]
    fn rejected_large_budget_preserves_the_live_transport_epoch() {
        let engine = match std::env::var_os("SPECTRAPACK_TEST_ENGINE") {
            Some(path) => path,
            None => return,
        };
        let root = tempfile::tempdir().unwrap();
        let mut session = NativeSession::spawn(Path::new(&engine), root.path()).unwrap();
        let epoch = session.epoch.clone();
        let (ticks, frequency) = qpc().unwrap();
        let failure=session.request("run",Some("rejected-large-budget"),serde_json::json!({
            "asset_token":"unneeded-for-local-rejection","settings":{"desktop_version":1,"budget_seconds":1e300,"budget_scope":"total_start"},
            "start_qpc_ticks":ticks.to_string(),"qpc_frequency_hz":frequency.to_string()
        }),|_,_,_|{}).unwrap_err();
        assert_eq!(failure.code, "INVALID_SETTINGS");
        session
            .request("shutdown", None, serde_json::json!({}), |_, _, _| {})
            .unwrap();
        assert_eq!(session.epoch, epoch);
    }
    #[test]
    fn transport_delivers_newer_same_count_compaction_poses() {
        let script = r#"
$request = [Console]::ReadLine() | ConvertFrom-Json
foreach ($revision in @(1, 2, 1)) {
    $x = 3 - $revision
    $pose = @{copy_id='one';translation_mm=@($x,1,1);quaternion_xyzw=@(0,0,0,1);
        local_to_world=@(@(1,0,0,$x),@(0,1,0,1),@(0,0,1,1),@(0,0,0,1))}
    @{runtime_version=1;request_id=$request.request_id;operation_id=$request.operation_id;
      kind='packing';sequence=$revision;native_elapsed_seconds=$revision;
      publication=$request.params.publication;
      packing=@{solution_revision=$revision;count=1;validation_status='valid';placements=@($pose)}} |
        ConvertTo-Json -Depth 12 -Compress
}
@{runtime_version=1;request_id=$request.request_id;operation_id=$request.operation_id;
  kind='response';ok=$true;result=@{}} | ConvertTo-Json -Compress
"#;
        let mut command = Command::new("powershell.exe");
        command.args(["-NoProfile", "-NonInteractive", "-Command", script]);
        let mut session = NativeSession::from_command(command).unwrap();
        let binding = serde_json::json!({"version":1,"session_id":"test","native_epoch":session.epoch,
            "settings_id":"settings"});
        let mut translations = Vec::new();
        session.request_with_packing("shutdown", Some("compaction"), serde_json::json!({"publication":binding}),
            |_,_,_,_|{}, Some(&mut |_,packing:&Packing,_| {
                translations.push(packing.placements[0].translation_mm);
            })).unwrap();
        assert_eq!(translations, vec![[2.0,1.0,1.0],[1.0,1.0,1.0]]);
    }
    #[test]
    fn prepared_token_survives_local_budget_rejection_and_runs_again() {
        let (engine,source)=match(std::env::var_os("SPECTRAPACK_TEST_ENGINE"),std::env::var_os("SPECTRAPACK_TEST_STL")) {
            (Some(engine),Some(source))=>(engine,source),_=>return,
        };
        let root=tempfile::tempdir().unwrap();
        let report=root.path().join("object.report.json");
        let imported=Command::new(&engine).arg("inspect").arg("--stl").arg(&source).args(["--units","mm","--report"]).arg(&report).output().unwrap();
        assert!(imported.status.success(),"{}",String::from_utf8_lossy(&imported.stdout));
        let preview=root.path().join("preview");std::fs::create_dir(&preview).unwrap();
        let mut session=NativeSession::spawn(Path::new(&engine),root.path()).unwrap();
        let prepared=session.request("prepare",Some("prepared-for-clock-test"),serde_json::json!({
            "object_report":report,"output_directory":preview}),|_,_,_|{}).unwrap();
        let epoch=session.epoch.clone();
        let output=root.path().join("run");std::fs::create_dir(&output).unwrap();
        let (ticks,frequency)=qpc().unwrap();
        let mut params=serde_json::json!({"asset_token":prepared["asset_token"],"settings":{
            "desktop_version":1,"box_dimensions_mm":[20,20,20],"clearance_mm":{"pair":0,"wall":0},
            "orientation":{"mode":"fixed","quaternion_xyzw":[0,0,0,1]},"pitch_mm":10,
            "budget_seconds":1e300,"seed":"0","thread_count":1,"budget_scope":"total_start"},
            "result_path":output.join("result.json"),"stop_file":output.join("stop.marker"),
            "start_qpc_ticks":ticks.to_string(),"qpc_frequency_hz":frequency.to_string()});
        let failure=session.request("run",Some("rejected-prepared-budget"),params.clone(),|_,_,_|{}).unwrap_err();
        assert_eq!(failure.code,"INVALID_SETTINGS");
        // This success/epoch case must fund real Start resolution, including contract validation in Debug.
        params["settings"]["budget_seconds"]=serde_json::json!(2.0);
        let (ticks,frequency)=qpc().unwrap();params["start_qpc_ticks"]=serde_json::json!(ticks.to_string());params["qpc_frequency_hz"]=serde_json::json!(frequency.to_string());
        let response=session.request("run",Some("valid-after-rejected-budget"),params,|_,_,_|{}).unwrap();
        assert_eq!(response["preparation_reused"],true);
        assert!(output.join("result.json").exists(), "Native terminal without an artifact: {response}");
        let result:Value=serde_json::from_slice(&std::fs::read(output.join("result.json")).unwrap()).unwrap();
        assert_eq!(result["validation"]["status"],"valid");
        assert!(result["count"].as_u64().unwrap()>0);
        assert_eq!(session.epoch,epoch);
    }
    #[test]
    #[ignore]
    fn stalled_stdin_child_helper() {
        thread::sleep(Duration::from_secs(30));
    }
    #[test]
    fn stalled_child_input_is_bounded_transport_failure_and_reclaims_writer() {
        let mut command = Command::new(std::env::current_exe().unwrap());
        command.args([
            "--ignored",
            "--exact",
            "native_session::tests::stalled_stdin_child_helper",
        ]);
        let mut session = NativeSession::from_command(command).unwrap();
        let began = Instant::now();
        let failure = session
            .request(
                "prepare",
                Some("stalled-write"),
                serde_json::json!({"large": "x".repeat(256*1024)}),
                |_, _, _| {},
            )
            .unwrap_err();
        assert_eq!(failure.code, "ENGINE_TRANSPORT_TIMEOUT");
        drop(session);
        assert!(began.elapsed() < Duration::from_secs(7));
    }
}

impl Drop for NativeSession {
    fn drop(&mut self) {
        if let Some(writer) = self.pending_writer.take() {
            let began = Instant::now();
            while !writer.is_finished() && began.elapsed() < Duration::from_millis(250) {
                #[cfg(windows)]
                unsafe {
                    use std::os::windows::io::AsRawHandle;
                    windows_sys::Win32::System::IO::CancelSynchronousIo(writer.as_raw_handle());
                }
                thread::sleep(Duration::from_millis(10));
            }
            if !writer.is_finished() {
                let _ = self.child.kill();
                let _ = self.child.wait();
            }
            let _ = writer.join();
        }
        // EOF requests cooperative cancellation even while native work is active.
        self.input.take();
        self.records.take();
        let started = Instant::now();
        loop {
            match self.child.try_wait() {
                Ok(Some(_)) => break,
                Ok(None) if started.elapsed() < Duration::from_secs(5) => {
                    thread::sleep(Duration::from_millis(10))
                }
                _ => {
                    let _ = self.child.kill();
                    let _ = self.child.wait();
                    break;
                }
            }
        }
        if let Some(reader) = self.output_reader.take() {
            let _ = reader.join();
        }
        if let Some(reader) = self.error_reader.take() {
            let _ = reader.join();
        }
    }
}

pub(crate) fn qpc() -> Outcome<(u64, u64)> {
    #[cfg(windows)]
    {
        use windows_sys::Win32::System::Performance::{
            QueryPerformanceCounter, QueryPerformanceFrequency,
        };
        let mut ticks = 0i64;
        let mut frequency = 0i64;
        unsafe {
            if QueryPerformanceFrequency(&mut frequency) == 0
                || QueryPerformanceCounter(&mut ticks) == 0
                || ticks < 0
                || frequency <= 0
            {
                return Err(error(
                    "CLOCK_UNAVAILABLE",
                    "Windows monotonic Start clock unavailable.",
                ));
            }
        }
        Ok((ticks as u64, frequency as u64))
    }
    #[cfg(not(windows))]
    {
        Err(error(
            "UNSUPPORTED_HOST",
            "Retained desktop runtime requires Windows QPC.",
        ))
    }
}
