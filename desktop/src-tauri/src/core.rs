use crate::{model::*, security};
use serde_json::{json, Value};
use std::{
    collections::HashMap,
    ffi::OsString,
    fs,
    io::Read,
    path::{Path, PathBuf},
    process::{Child, Command, ExitStatus, Stdio},
    sync::{Arc, Mutex},
    thread,
};

const RECORD_LIMIT: u64 = 1 << 20;
const REPORT_LIMIT: u64 = 16 << 20;
const RESULT_LIMIT: u64 = 64 << 20;
const SOURCE_LIMIT: u64 = 256 << 20;
const PREVIEW_LIMIT: u64 = 64 << 20;
// Hash of the canonical native version-1 cube45 catalog, checked by the
// real-engine Start/Save/Open/export test below.
const CUBE45_CATALOG_HASH: &str = "7434d4e2eb8cd12e6a67af3a5d636a7435a4e50da043777586ff0fbff9eae4ef";

struct ChildGuard {
    child: Child,
    reaped: bool,
}
impl ChildGuard {
    fn new(child: Child) -> Self {
        Self {
            child,
            reaped: false,
        }
    }
    fn wait(&mut self) -> std::io::Result<ExitStatus> {
        let status = self.child.wait()?;
        self.reaped = true;
        Ok(status)
    }
}
impl Drop for ChildGuard {
    fn drop(&mut self) {
        if !self.reaped {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

#[derive(Clone)]
struct ObjectFiles {
    id: String,
    report: PathBuf,
    preview: Option<Value>,
    envelope_preview: Option<Value>,
    native_token: Option<String>,
    native_epoch: Option<String>,
    cpu_runtime: Option<Value>,
}
struct PreparedRollback {
    core: Core,
    token: Option<String>,
    epoch: Option<String>,
    preview_ids: Vec<String>,
    committed: bool,
}
impl Drop for PreparedRollback {
    fn drop(&mut self) {
        if !self.committed {
            let _ = self
                .core
                .release_native(self.token.take(), self.epoch.as_deref());
            for preview in self.preview_ids.drain(..) {
                self.core.inner.lock().unwrap().previews.remove(&preview);
            }
        }
    }
}
#[derive(Clone)]
struct ResultFiles {
    id: String,
    root: PathBuf,
    object: ObjectFiles,
}
#[derive(Clone)]
struct PreviewFile {
    root: PathBuf,
    path: PathBuf,
    hash: String,
    bytes: u64,
}
#[derive(Clone)]
struct RepairFiles {
    object_id: String,
    report: PathBuf,
    algorithm: String,
    tolerance: f64,
    token: String,
}
struct Inner {
    repair: Option<RepairFiles>,
    state: State,
    object: Option<ObjectFiles>,
    result: Option<ResultFiles>,
    previews: HashMap<String, PreviewFile>,
    stop_file: Option<PathBuf>,
    protected_sources: Vec<PathBuf>,
    operation_started: Option<std::time::Instant>,
    transitioning: bool,
}

fn accept_live(
    inner: &mut Inner,
    binding: &crate::native_session::Publication,
    operation: &str,
    asset_token: &Value,
    live: Value,
) -> bool {
    if inner.state.session_id != binding.session_id
        || !inner
            .state
            .operation
            .as_ref()
            .is_some_and(|op| op.id == operation && op.finished_at.is_none())
        || !inner.object.as_ref().is_some_and(|object| {
            object.native_epoch.as_deref() == Some(&binding.native_epoch)
                && object.native_token.as_deref() == asset_token.as_str()
        })
    {
        return false;
    }
    if let Some(old) = inner
        .state
        .live_result
        .as_ref()
        .filter(|old| old["operation_id"] == operation)
    {
        if old["settings_id"] != binding.settings_id
            || old["native_epoch"] != binding.native_epoch
            || live["count"].as_u64() < old["count"].as_u64()
            || live["sequence"].as_u64() <= old["sequence"].as_u64()
            || live["solution_revision"].as_u64() <= old["solution_revision"].as_u64()
        {
            return false;
        }
    }
    inner.result = None;
    inner.state.result = None;
    inner.state.live_result = Some(live);
    inner.state.revision += 1;
    true
}
fn physical_value_equal(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Number(_), Value::Number(_)) => a.as_f64() == b.as_f64(),
        (Value::Array(a), Value::Array(b)) => {
            a.len() == b.len() && a.iter().zip(b).all(|(a, b)| physical_value_equal(a, b))
        }
        (Value::Object(a), Value::Object(b)) => {
            a.len() == b.len()
                && a.iter()
                    .all(|(key, a)| b.get(key).is_some_and(|b| physical_value_equal(a, b)))
        }
        _ => a == b,
    }
}
fn physical_orientation_equal(a: &Value, b: &Value) -> bool {
    if a["mode"] != "fixed" || b["mode"] != "fixed" {
        return physical_value_equal(a, b);
    }
    let normalized = |value: &Value| -> Option<[f64; 4]> {
        let mut q: [f64; 4] = serde_json::from_value(value["quaternion_xyzw"].clone()).ok()?;
        let norm = q[0].hypot(q[1]).hypot(q[2].hypot(q[3]));
        if !norm.is_finite() || norm == 0.0 {
            return None;
        }
        for component in &mut q {
            *component /= norm;
        }
        Some(q)
    };
    let (Some(a), Some(b)) = (normalized(a), normalized(b)) else {
        return false;
    };
    // q and -q describe the same rotation. Allow only normalization rounding,
    // not the broader accepted-input unit-norm tolerance; placements remain exact.
    [1.0, -1.0].into_iter().any(|sign| {
        a.iter()
            .zip(b)
            .all(|(a, b)| (a - sign * b).abs() <= 32.0 * f64::EPSILON)
    })
}

fn public_artifact_matches(live: &Value, document: &Value, settings: &Value, object_report: &Value) -> Outcome<()> {
    let resolved = &document["search"]["resolved_settings"];
    let orientation_matches = if settings["orientation"]["mode"] == "cube45" {
        resolved["orientation"]["mode"] == "custom"
            && resolved["resolved"]["orientation_catalog_version"] == 1
            && resolved["resolved"]["orientation_catalog_sha256"] == CUBE45_CATALOG_HASH
    } else {
        physical_orientation_equal(&resolved["orientation"], &settings["orientation"])
    };
    if document["count"] != live["count"]
        || document["placements"] != live["placements"]
        || document["solution_revision"] != live["solution_revision"]
        || !physical_value_equal(&resolved["container"]["dimensions_mm"], &settings["box_dimensions_mm"])
        || !physical_value_equal(&resolved["container"]["tolerance_mm"], &settings["box_tolerance_mm"])
        || !physical_value_equal(&resolved["clearance_mm"], &settings["clearance_mm"])
        || !orientation_matches
        || !physical_value_equal(&resolved["resolved"]["pitch_mm"], &settings["pitch_mm"])
        || resolved["search"]["seed"] != settings["seed"]
    {
        return Err(error(
            "PUBLICATION_MISMATCH",
            "Terminal artifact differs from the displayed public packing or captured settings.",
        ));
    }
    for key in ["source", "accepted_solid", "frame", "dimensions_mm", "repair_record", "packing_envelope"] {
        if document["assets"]["object"][key] != object_report[key] {
            return Err(error(
                "PUBLICATION_MISMATCH",
                "Terminal artifact belongs to a different accepted object.",
            ));
        }
    }
    Ok(())
}

#[derive(Clone)]
pub struct Core {
    engine: PathBuf,
    root: PathBuf,
    inner: Arc<Mutex<Inner>>,
    native: Arc<Mutex<Option<crate::native_session::NativeSession>>>,
    #[cfg(test)]
    before_begin: Arc<Mutex<Option<Box<dyn FnOnce() + Send>>>>,
    #[cfg(test)]
    after_spawn: Arc<Mutex<Option<Box<dyn FnOnce(u32) + Send>>>>,
}

fn write_json(path: &Path, value: &Value) -> Outcome<()> {
    use std::io::Write;
    let bytes =
        serde_json::to_vec_pretty(value).map_err(|e| error("INVALID_DOCUMENT", e.to_string()))?;
    let mut output = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|e| error("OUTPUT_WRITE_FAILED", e.to_string()))?;
    output
        .write_all(&bytes)
        .map_err(|e| error("OUTPUT_WRITE_FAILED", e.to_string()))?;
    output
        .sync_all()
        .map_err(|e| error("OUTPUT_WRITE_FAILED", e.to_string()))
}
fn arguments(values: &[&str]) -> Vec<OsString> {
    values.iter().map(OsString::from).collect()
}
fn pair(args: &mut Vec<OsString>, flag: &str, path: &Path) {
    args.push(flag.into());
    args.push(path.into());
}
fn reported_path(text: &str) -> PathBuf {
    #[cfg(windows)]
    {
        if let Some(rest) = text.strip_prefix("//?/") {
            return PathBuf::from(format!("\\\\?\\{}", rest.replace('/', "\\")));
        }
    }
    PathBuf::from(text)
}

fn drain(mut stream: impl Read, limit: u64) -> Outcome<Vec<u8>> {
    let mut out = Vec::new();
    let mut buffer = [0_u8; 8192];
    let mut overflow = false;
    loop {
        let size = stream
            .read(&mut buffer)
            .map_err(|e| error("ENGINE_TRANSPORT", e.to_string()))?;
        if size == 0 {
            break;
        }
        if out.len() as u64 + size as u64 <= limit {
            out.extend_from_slice(&buffer[..size]);
        } else {
            overflow = true;
        }
    }
    if overflow {
        Err(error(
            "MEMORY_LIMIT",
            "Native output exceeded the desktop transport limit.",
        ))
    } else {
        Ok(out)
    }
}

impl Core {
    pub fn new(engine: PathBuf, root: PathBuf) -> Outcome<Self> {
        #[cfg(windows)]
        crate::native_process::bind_owner()?;
        security::regular_file(&engine)?;
        security::directory(
            engine
                .parent()
                .ok_or_else(|| error("ENGINE_MISSING", "Bundled engine has no directory."))?,
        )?;
        let engine =
            fs::canonicalize(engine).map_err(|e| error("ENGINE_MISSING", e.to_string()))?;
        fs::create_dir(&root).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
        security::directory(&root)?;
        let root = fs::canonicalize(root).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
        let state = State {
            desktop_version: 1,
            session_id: id("session"),
            revision: 0,
            object: None,
            repair: None,
            draft_settings: None,
            result: None,
            live_result: None,
            operation: None,
            last_error: None,
            cpu_runtime: None,
        };
        let core = Self {
            engine,
            root,
            native: Arc::new(Mutex::new(None)),
            #[cfg(test)]
            before_begin: Arc::new(Mutex::new(None)),
            #[cfg(test)]
            after_spawn: Arc::new(Mutex::new(None)),
            inner: Arc::new(Mutex::new(Inner {
                state,
                repair: None,
                object: None,
                result: None,
                previews: HashMap::new(),
                stop_file: None,
                protected_sources: Vec::new(),
                operation_started: None,
                transitioning: false,
            })),
        };
        core.run(&arguments(&["capabilities", "--json"]), &core.root)?;
        Ok(core)
    }
    pub fn state(&self) -> State {
        self.inner.lock().unwrap().state.clone()
    }

    fn begin(&self, kind: &str, settings: Option<Value>) -> Outcome<(Receipt, PathBuf)> {
        self.begin_with(kind, settings, |_| Ok(()))
            .map(|(receipt, root, ())| (receipt, root))
    }
    fn begin_with<T>(
        &self,
        kind: &str,
        settings: Option<Value>,
        snapshot: impl FnOnce(&Inner) -> Outcome<T>,
    ) -> Outcome<(Receipt, PathBuf, T)> {
        #[cfg(test)]
        if let Some(hook) = self.before_begin.lock().unwrap().take() {
            hook();
        }
        let mut inner = self.inner.lock().unwrap();
        if inner
            .state
            .operation
            .as_ref()
            .is_some_and(|op| op.finished_at.is_none())
        {
            return Err(error(
                "JOB_BUSY",
                "Wait for the active operation to finish.",
            ));
        }
        if inner.transitioning {
            return Err(error("JOB_BUSY", "Project transition is active."));
        }
        let operation_started = std::time::Instant::now();
        let captured = snapshot(&inner)?;
        let operation_id = id("operation");
        let directory = self.root.join(&operation_id);
        fs::create_dir(&directory).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
        inner.stop_file = if kind == "solve" {
            Some(directory.join("stop.marker"))
        } else {
            None
        };
        inner.operation_started = Some(operation_started);
        inner.state.operation = Some(Operation {
            id: operation_id.clone(),
            kind: kind.into(),
            phase: "preparing".into(),
            started_at: now(),
            finished_at: None,
            result_id: None,
            detail: None,
            sequence: None,
            completion_elapsed_seconds: None,
            native_completion_elapsed_seconds: None,
        });
        if let Some(settings) = settings {
            inner.state.draft_settings = Some(settings);
        }
        inner.state.last_error = None;
        inner.state.revision += 1;
        Ok((Receipt { operation_id }, directory, captured))
    }
    fn phase(&self, operation_id: &str, phase: &str) {
        let mut inner = self.inner.lock().unwrap();
        if let Some(op) = &mut inner.state.operation {
            if op.id == operation_id && op.finished_at.is_none() && op.phase != "stopping" {
                op.phase = phase.into();
                inner.state.revision += 1;
            }
        }
    }
    fn spawn(
        &self,
        receipt: &Receipt,
        work: impl FnOnce(Core, String) -> Outcome<()> + Send + 'static,
    ) {
        let core = self.clone();
        let operation_id = receipt.operation_id.clone();
        thread::spawn(move || {
            let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                work(core.clone(), operation_id.clone())
            }))
            .unwrap_or_else(|_| {
                Err(error(
                    "INTERNAL_ERROR",
                    "Desktop worker panicked; previous complete state retained.",
                ))
            });
            let mut inner = core.inner.lock().unwrap();
            if inner
                .state
                .operation
                .as_ref()
                .is_some_and(|op| op.id == operation_id)
            {
                let (phase, failure) = match outcome {
                    Ok(()) => ("finished", None),
                    Err(e) => ("failed", Some(e)),
                };
                let completion = inner
                    .operation_started
                    .map(|start| start.elapsed().as_secs_f64());
                let op = inner.state.operation.as_mut().unwrap();
                op.completion_elapsed_seconds = completion;
                op.phase = phase.into();
                op.finished_at = Some(now());
                if let Some(e) = &failure {
                    op.detail = Some(e.message.clone());
                }
                inner.state.last_error = failure;
                inner.state.revision += 1;
                inner.stop_file = None;
            }
        });
    }
    fn run(&self, args: &[OsString], cwd: &Path) -> Outcome<Value> {
        security::directory(cwd)?;
        security::regular_file(&self.engine)?;
        let mut bounded_args = args.to_vec();
        if args.first().is_some_and(|command| {
            ["inspect", "desktop-prepare", "desktop-restore"]
                .iter()
                .any(|x| command == *x)
        }) {
            if args.iter().any(|arg| arg == "--available-host-bytes") {
                return Err(error(
                    "INVALID_REQUEST",
                    "Helper memory allowance is owned by the desktop coordinator.",
                ));
            }
            bounded_args.push("--available-host-bytes".into());
            bounded_args.push(self.auxiliary_host_allowance()?.to_string().into());
        }
        let mut command = Command::new(&self.engine);
        command
            .args(&bounded_args)
            .current_dir(cwd)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            let flags = 0x0800_0000;
            // Hold the real child at creation for the deterministic owner-exit
            // boundary regression. Production never suspends or detaches it.
            #[cfg(test)]
            let flags = flags
                | if self.after_spawn.lock().unwrap().is_some() {
                    0x0000_0004
                } else {
                    0
                };
            command.creation_flags(flags);
        }
        let child = command
            .spawn()
            .map_err(|e| error("ENGINE_START", e.to_string()))?;
        // Reap before an abnormal return or unwind can publish failed state.
        // The owner job separately covers exit during process creation itself.
        let mut child = ChildGuard::new(child);
        #[cfg(test)]
        {
            let hook = self.after_spawn.lock().unwrap().take();
            if let Some(hook) = hook {
                hook(child.child.id());
            }
        }
        let stdout = child.child.stdout.take().unwrap();
        let stderr = child.child.stderr.take().unwrap();
        let out_reader = thread::Builder::new()
            .name("native-stdout".into())
            .spawn(move || drain(stdout, RECORD_LIMIT))
            .map_err(|e| error("ENGINE_TRANSPORT", e.to_string()))?;
        let err_reader = thread::Builder::new()
            .name("native-stderr".into())
            .spawn(move || drain(stderr, RECORD_LIMIT))
            .map_err(|e| error("ENGINE_TRANSPORT", e.to_string()))?;
        let status = child
            .wait()
            .map_err(|e| error("ENGINE_TRANSPORT", e.to_string()))?;
        let out = out_reader
            .join()
            .map_err(|_| error("ENGINE_TRANSPORT", "Native stdout reader failed."))??;
        let err = err_reader
            .join()
            .map_err(|_| error("ENGINE_TRANSPORT", "Native stderr reader failed."))?;
        let text = std::str::from_utf8(&out)
            .map_err(|_| error("ENGINE_TRANSPORT", "Native stdout is not UTF-8."))?;
        if text.lines().count() != 1 || !text.ends_with('\n') {
            return Err(error(
                "ENGINE_TRANSPORT",
                "Expected one complete terminal JSON record.",
            ));
        }
        let record: Value = serde_json::from_str(text)
            .map_err(|e| error("ENGINE_TRANSPORT", format!("Invalid native JSON: {e}")))?;
        if record.get("ok") == Some(&Value::Bool(false)) {
            let failure: DesktopError =
                serde_json::from_value(record.get("error").cloned().unwrap_or(Value::Null))
                    .map_err(|_| {
                        error("ENGINE_TRANSPORT", "Native failure record is malformed.")
                    })?;
            if status.success() {
                return Err(error(
                    "ENGINE_TRANSPORT",
                    "Native failure record had a successful exit.",
                ));
            }
            return Err(failure);
        }
        if !status.success() {
            return Err(error(
                "ENGINE_FAILED",
                format!(
                    "Native process exited {}; {}",
                    status,
                    err.ok()
                        .map(|bytes| String::from_utf8_lossy(&bytes).into_owned())
                        .unwrap_or_default()
                ),
            ));
        }
        if args.first().is_some_and(|x| x == "capabilities") {
            let includes = |value: &Value, expected: &Value| {
                value.as_array().is_some_and(|a| a.contains(expected))
            };
            if !includes(&record["protocol_versions"], &json!(1))
                || ["settings", "assets", "results"]
                    .iter()
                    .any(|name| !includes(&record["schema_versions"][name], &json!(1)))
                || ["inspect", "solve", "desktop-prepare", "desktop-restore"]
                    .iter()
                    .any(|name| !includes(&record["implemented_commands"], &json!(name)))
            {
                return Err(error(
                    "ENGINE_INCOMPATIBLE",
                    "Bundled engine does not support the desktop's version 1 native contracts.",
                ));
            }
            return Ok(record);
        }
        if args.first().is_some_and(|x| x == "inspect") {
            if record.get("report_path").and_then(Value::as_str).is_none()
                || record.get("state").is_none()
            {
                return Err(error(
                    "ENGINE_TRANSPORT",
                    "Native inspection terminal record is malformed.",
                ));
            }
            return Ok(record);
        }
        if record.get("ok") != Some(&Value::Bool(true)) {
            return Err(error(
                "ENGINE_TRANSPORT",
                "Native success record is malformed.",
            ));
        }
        Ok(record.get("result").cloned().unwrap_or(record))
    }
    fn register_preview(&self, root: &Path, response: &Value) -> Outcome<Option<Value>> {
        let metadata = &response["preview"];
        if metadata.is_null() {
            if !response["preview_path"].is_null() {
                return Err(error("ENGINE_TRANSPORT", "Preview metadata/path mismatch."));
            }
            return Ok(None);
        }
        let path = reported_path(
            response["preview_path"]
                .as_str()
                .ok_or_else(|| error("ENGINE_TRANSPORT", "Preview path missing."))?,
        );
        let path = security::scoped(root, &path)?;
        let length = metadata["byte_length"]
            .as_u64()
            .ok_or_else(|| error("ENGINE_TRANSPORT", "Preview size missing."))?;
        let hash = metadata["sha256"]
            .as_str()
            .ok_or_else(|| error("ENGINE_TRANSPORT", "Preview hash missing."))?;
        let bytes = security::read(&path, PREVIEW_LIMIT)?;
        if bytes.len() as u64 != length
            || security::hash(&bytes) != hash
            || metadata["format"] != "ply"
            || metadata["coordinate_frame"] != "object_local_mm"
            || !metadata["triangle_count"].is_u64()
        {
            return Err(error(
                "ASSET_MISMATCH",
                "Native preview bytes do not match metadata.",
            ));
        }
        let preview_id = id("preview");
        let mut value = metadata.clone();
        value["preview_id"] = preview_id.clone().into();
        self.inner.lock().unwrap().previews.insert(
            preview_id,
            PreviewFile {
                root: root.into(),
                path,
                hash: hash.into(),
                bytes: length,
            },
        );
        Ok(Some(value))
    }
    fn auxiliary_host_allowance(&self) -> Outcome<u64> {
        // Before a retained session exists, use the same half-available-RAM
        // policy as native preparation, rather than an unrelated 512 MiB cap.
        #[cfg(windows)]
        let initial_cap = unsafe {
            use windows_sys::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
            let mut memory: MEMORYSTATUSEX = std::mem::zeroed();
            memory.dwLength = std::mem::size_of::<MEMORYSTATUSEX>() as u32;
            if GlobalMemoryStatusEx(&mut memory) == 0 {
                return Err(error("MEMORY_LIMIT", "Available physical RAM could not be queried."));
            }
            memory.ullAvailPhys / 2
        };
        #[cfg(not(windows))]
        let initial_cap = 512 << 20;
        const ADAPTER: u64 = 16 << 20;
        let (retained, session_live, cap) = {
            let mut native = self.native.lock().unwrap();
            if let Some(child) = native.as_mut() {
                if child.is_alive()? {
                    let cap = child.admitted_host_cap_bytes.filter(|cap| *cap > 0)
                        .ok_or_else(|| error("MEMORY_LIMIT", "Native admitted host ceiling is unavailable."))?;
                    (child.retained_native_bytes.filter(|bytes|*bytes<=cap)
                        .ok_or_else(||error("MEMORY_LIMIT","Retained native residency is unavailable; helper admission cannot proceed."))?,true,cap)
                } else {
                    native.take();
                    (0, false, initial_cap)
                }
            } else {
                (0, false, initial_cap)
            }
        };
        let inner = self.inner.lock().unwrap();
        let viewer = if inner.state.object.is_some() || inner.state.result.is_some() {
            64 << 20
        } else {
            0
        };
        let live = retained
            .checked_add(viewer)
            .and_then(|bytes| bytes.checked_add(ADAPTER))
            .and_then(|bytes| bytes.checked_add(if session_live { ADAPTER } else { 0 }))
            .ok_or_else(|| error("MEMORY_LIMIT", "Helper retained-owner admission overflows."))?;
        cap.checked_sub(live)
            .filter(|bytes| *bytes > 0)
            .ok_or_else(|| {
                error(
                    "MEMORY_LIMIT",
                    "Retained native owners, viewer and adapters exhaust the helper allowance.",
                )
            })
    }
    fn native_request(
        &self,
        method: &str,
        operation: Option<&str>,
        params: Value,
        allow_spawn: bool,
    ) -> Outcome<Value> {
        let mut native = self.native.lock().unwrap();
        if native
            .as_mut()
            .is_some_and(|child| child.is_alive().is_ok_and(|alive| !alive))
        {
            native.take();
        }
        if native.is_none() {
            if !allow_spawn {
                return Err(error("ENGINE_SESSION_LOST", "Prepared native session is unavailable; import or Open verified geometry again."));
            }
            *native = Some(crate::native_session::NativeSession::spawn(
                &self.engine,
                &self.root,
            )?);
        }
        let live_settings = params["settings"].clone();
        let asset_token = params["asset_token"].clone();
        let preview = self
            .inner
            .lock()
            .unwrap()
            .object
            .as_ref()
            .and_then(|object| object.preview.clone());
        let mut packing_sink =
            |binding: &crate::native_session::Publication, packing: &crate::native_session::Packing, sequence| {
                let mut live = serde_json::to_value(packing).unwrap();
                live["publication_version"] = json!(binding.version);
                live["session_id"] = json!(binding.session_id);
                live["native_epoch"] = json!(binding.native_epoch);
                live["settings_id"] = json!(binding.settings_id);
                live["operation_id"] = json!(operation);
                live["sequence"] = json!(sequence);
                live["settings"] = live_settings.clone();
                live["preview"] = json!(preview);
                if let Some(operation) = operation {
                    accept_live(&mut self.inner.lock().unwrap(), binding, operation, &asset_token, live);
                }
            };
        let result = native.as_mut().unwrap().request_with_packing(
            method,
            operation,
            params,
            |sequence, phase, _, resource| {
                if let Some(operation_id) = operation {
                    let mut inner = self.inner.lock().unwrap();
                    if let Some(active) = inner.state.operation.as_mut() {
                        if active.id == operation_id
                            && active.finished_at.is_none()
                            && active.sequence.unwrap_or(0) < sequence
                        {
                            active.sequence = Some(sequence);
                            if active.phase != "stopping" {
                                active.phase = phase.into();
                            }
                            if let (Some(cpu), Some(resource)) = (inner.state.cpu_runtime.as_mut(), resource) {
                                cpu["resource_status"] = serde_json::to_value(resource).unwrap();
                            }
                            inner.state.revision += 1;
                        }
                    }
                }
            },
            Some(&mut packing_sink),
        );

        if result.as_ref().err().is_some_and(|failure| {
            [
                "ENGINE_TRANSPORT",
                "ENGINE_TERMINATED",
                "ENGINE_IDENTITY",
                "ENGINE_TRANSPORT_TIMEOUT",
            ]
            .contains(&failure.code.as_str())
        }) {
            native.take();
        }
        result
    }
    fn prepare(
        &self,
        object: &mut ObjectFiles,
        root: &Path,
    ) -> Outcome<(PathBuf, Option<Value>)> {
        let output = root.join("prepared");
        fs::create_dir(&output).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
        let operation = self
            .inner
            .lock()
            .unwrap()
            .state
            .operation
            .as_ref()
            .map(|op| op.id.clone())
            .ok_or_else(|| error("INTERNAL_ERROR", "Preparation has no operation."))?;
        let response = self.native_request(
            "prepare",
            Some(&operation),
            json!({"object_report":object.report,"output_directory":output}),
            true,
        )?;
        object.native_token = Some(
            response["asset_token"]
                .as_str()
                .ok_or_else(|| error("ENGINE_TRANSPORT", "Prepared native token missing."))?
                .into(),
        );
        object.native_epoch = self
            .native
            .lock()
            .unwrap()
            .as_ref()
            .map(|native| native.epoch.clone());
        let mut rollback = PreparedRollback {
            core: self.clone(),
            token: object.native_token.clone(),
            epoch: object.native_epoch.clone(),
            preview_ids: Vec::new(),
            committed: false,
        };
        let maximum = response["supported_thread_count_max"]
            .as_u64()
            .ok_or_else(|| error("ENGINE_TRANSPORT", "CPU support missing."))?;
        if !(1..=8).contains(&maximum) {
            return Err(error("ENGINE_TRANSPORT", "CPU support range invalid."));
        }
        object.cpu_runtime = Some(
            json!({"version":1,"supported_min":1,"supported_max":maximum,
            "default_thread_count":maximum.min(4),"working_set_estimate_bytes":response["working_set_estimate_bytes"]}),
        );
        let preview;
        let report = security::json(&object.report, REPORT_LIMIT)?;
        let report_root = object.report.parent().unwrap();
        object.envelope_preview = self.import_preview(report_root, &report["accepted_solid"],
            report["accepted_solid"]["triangle_count"].as_u64().unwrap_or(0))?;
        if let Some(id)=object.envelope_preview.as_ref().and_then(|p|p["preview_id"].as_str()) {rollback.preview_ids.push(id.into());}
        if !report["packing_envelope"].is_null() {
            let source = &report["packing_envelope"]["source_mesh"];
            preview = self.import_preview(report_root, source, source["triangle_count"].as_u64().unwrap_or(0))?;
        }
        else { preview = self.register_preview(&output, &response)?; }
        if let Some(id)=preview.as_ref().and_then(|p|p["preview_id"].as_str()) {rollback.preview_ids.push(id.into());}
        rollback.committed = true;
        Ok((output, preview))
    }
    fn release_native(&self, token: Option<String>, epoch: Option<&str>) -> Outcome<()> {
        let Some(token) = token else {
            return Ok(());
        };
        let mut native = self.native.lock().unwrap();
        let Some(child) = native.as_mut() else {
            return Ok(());
        };
        if epoch != Some(child.epoch.as_str()) {
            return Ok(());
        }
        if !child.is_alive()? {
            native.take();
            return Ok(());
        }
        child.request("release", None, json!({"asset_token":token}), |_, _, _| {})?;
        Ok(())
    }
    // Public path adapters are called only after Rust dialogs or by native integration tests.
    pub fn import_path(&self, source: &Path, request: ImportRequest) -> Outcome<Receipt> {
        validate_import(&request)?;
        security::regular_file(source)?;
        let source = source.to_path_buf();
        let name = source
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        let (receipt, root) = self.begin("import", None)?;
        self.spawn(&receipt, move |core, op| {
            let copy = root.join("selected.stl"); security::copy(&source, &copy, SOURCE_LIMIT)?;
            let report_path = root.join("object.report.json"); let mut args = arguments(&["inspect", "--units", &request.units]);
            pair(&mut args, "--stl", &copy); pair(&mut args, "--report", &report_path);
            args.extend(arguments(&["--packing-envelope", "auto", "--accept-repair", "auto"]));
            if let Some(scale) = request.scale_mm { args.extend(["--scale-mm".into(), scale.to_string().into()]); }
            core.run(&args, &root)?;
            let report = security::json(&report_path, REPORT_LIMIT)?; let object_id = id("object");
            let mut object = ObjectFiles { id: object_id.clone(), report: report_path, preview: None, envelope_preview: None, native_token: None, native_epoch: None, cpu_runtime: None };
            if report["state"] == "accepted" && report["diagnostics"]["status"] == "valid" {
                let (_, preview) = core.prepare(&mut object, &root)?; object.preview = preview;
            } else {
                object.preview = core.import_preview(&root, &report["preview"], report["diagnostics"]["import"]["triangle_count"].as_u64().unwrap_or(0))?;
            }
            let mut prepared_guard=PreparedRollback {core:core.clone(),token:object.native_token.clone(),epoch:object.native_epoch.clone(),preview_ids:[object.preview.as_ref(),object.envelope_preview.as_ref()].into_iter().flatten().filter_map(|p|p["preview_id"].as_str().map(str::to_owned)).collect(),committed:false};
            let old_object=core.inner.lock().unwrap().object.clone();
            if let Some(old)=old_object {core.release_native(old.native_token,old.native_epoch.as_deref())?;}
            let mut inner = core.inner.lock().unwrap(); inner.state.cpu_runtime=object.cpu_runtime.clone(); inner.object = Some(object.clone());
            prepared_guard.committed=true;
            inner.protected_sources.push(source.clone());
            inner.state.live_result=None;
            inner.repair=None; inner.state.repair=None;
            inner.result=None; inner.state.result=None;
            inner.state.object = Some(json!({"id":object_id,"display_name":name,"report":report,"preview":object.preview,"envelope_preview":object.envelope_preview}));
            // Imports own geometry only. The editor owns unsaved settings; the
            // backend retains its last submitted snapshot until Start, Save or Open.
            inner.state.revision += 1; drop(inner);
            core.prune_previews();
            core.phase(&op, "validating"); Ok(())
        });
        Ok(receipt)
    }

    fn import_preview(&self, root: &Path, reference: &Value, triangles: u64) -> Outcome<Option<Value>> {
        let relative = reference["path"].as_str().ok_or_else(||error("ASSET_MISMATCH", "Import preview missing."))?;
        let path = security::scoped(root, &root.join(relative))?;
        let bytes = security::read(&path, PREVIEW_LIMIT)?;
        self.register_preview(root, &json!({"preview_path":path,"preview":{
            "format":"ply","sha256":reference["sha256"],"byte_length":bytes.len(),
            "triangle_count":triangles,"coordinate_frame":"object_local_mm"}}))
    }

    pub fn repair(&self, request: RepairRequest) -> Outcome<Receipt> {
        if let RepairRequest::Propose {algorithm, tolerance_mm} = &request {
            if !["weld", "manifold"].contains(&algorithm.as_str()) || !tolerance_mm.is_finite() || *tolerance_mm <= 0.0 {
                return Err(error("INVALID_REQUEST", "Choose a supported repair and positive millimetre tolerance."));
            }
        }
        let (receipt, root, (old, record, pending)) = self.begin_with("repair", None, |inner| {
            let old = inner.object.clone().ok_or_else(||error("ASSET_REQUIRED", "Import an object first."))?;
            let record = inner.state.object.clone().unwrap();
            if let RepairRequest::Accept {token} | RepairRequest::Reject {token} = &request {
                if !inner.repair.as_ref().is_some_and(|p|p.token == *token && p.object_id == old.id) {
                    return Err(error("ASSET_MISMATCH", "Repair proposal is no longer current."));
                }
            }
            Ok((old, record, inner.repair.clone()))
        })?;
        self.spawn(&receipt, move |core, op| {
            if matches!(&request, RepairRequest::Reject {..}) {
                let mut inner = core.inner.lock().unwrap(); inner.repair=None; inner.state.repair=None;
                inner.state.revision+=1; drop(inner); core.prune_previews(); return Ok(());
            }
            let (algorithm, tolerance, token) = match request {
                RepairRequest::Propose {algorithm,tolerance_mm} => (algorithm,tolerance_mm,None),
                RepairRequest::Envelope => ("envelope".to_owned(),0.0,None),
                RepairRequest::Accept {token} => {
                    let pending = pending.unwrap();
                    // Retained proposal bytes must still match the displayed token.
                    let report = security::json(&pending.report, REPORT_LIMIT)?;
                    if report["repair_proposal"]["sha256"] != token {return Err(error("ASSET_MISMATCH","Proposal changed."));}
                    (pending.algorithm,pending.tolerance,Some(token))
                },
                _ => unreachable!(),
            };
            let source_report=security::json(&old.report, REPORT_LIMIT)?;
            let source=security::scoped(old.report.parent().unwrap(), &old.report.parent().unwrap().join(source_report["source"]["path"].as_str().unwrap()))?;
            let report_path=root.join("object.report.json");
            let mut args=arguments(&["inspect","--units",source_report["source"]["units"].as_str().unwrap()]);
            pair(&mut args,"--stl",&source); pair(&mut args,"--report",&report_path);
            if source_report["source"]["units"]=="custom" {
                args.extend(["--scale-mm".into(),source_report["source"]["unit_scale_mm"].to_string().into()]);
            }
            if algorithm=="envelope" { args.extend(["--packing-envelope".into(),"auto".into()]); } else {
            args.extend([if algorithm=="weld" {"--weld-tolerance-mm"} else {"--reconstruct-resolution-mm"}.into(),tolerance.to_string().into()]); }
            if let Some(token)=&token {args.extend(["--accept-repair".into(),token.into()]);}
            core.run(&args,&root).map_err(|mut failure| {
                if failure.code == "MEMORY_LIMIT" {
                    failure.message.push_str(" Free some RAM and retry; a larger reconstruction resolution may also fit, but can lose small features.");
                }
                failure
            })?;
            let report=security::json(&report_path,REPORT_LIMIT)?;
            if token.is_none() {
                let proposal=&report["repair_proposal"];
                let token=proposal["sha256"].as_str().ok_or_else(||error("ENGINE_TRANSPORT","Proposal token missing."))?.to_owned();
                let before=core.import_preview(&root,&proposal["before"],report["diagnostics"]["import"]["triangle_count"].as_u64().unwrap_or(0))?;
                let after=match core.import_preview(&root,&proposal["after"],proposal["after_diagnostics"]["import"]["triangle_count"].as_u64().unwrap_or(0)) {
                    Ok(preview)=>preview,
                    Err(failure)=>{
                        if let Some(id)=before.as_ref().and_then(|p|p["preview_id"].as_str()) {
                            core.inner.lock().unwrap().previews.remove(id);
                        }
                        return Err(failure);
                    }
                };
                let mut inner=core.inner.lock().unwrap();
                inner.repair=Some(RepairFiles{object_id:old.id.clone(),report:report_path,algorithm:algorithm.clone(),tolerance,token:token.clone()});
                inner.state.repair=Some(json!({"object_id":old.id,"token":token,"algorithm":algorithm,"tolerance_mm":if algorithm=="envelope" {Value::Null} else {json!(tolerance)},
                    "report":report,"before":before,"after":after}));
                inner.state.revision+=1;drop(inner);core.prune_previews();return Ok(());
            }
            let mut object=ObjectFiles{id:id("object"),report:report_path,preview:None,envelope_preview:None,native_token:None,native_epoch:None,cpu_runtime:None};
            let (_,preview)=core.prepare(&mut object,&root)?;object.preview=preview;
            let mut guard=PreparedRollback{core:core.clone(),token:object.native_token.clone(),epoch:object.native_epoch.clone(),
                preview_ids:[object.preview.as_ref(),object.envelope_preview.as_ref()].into_iter().flatten().filter_map(|p|p["preview_id"].as_str().map(str::to_owned)).collect(),committed:false};
            core.release_native(old.native_token,old.native_epoch.as_deref())?;
            let mut inner=core.inner.lock().unwrap();
            inner.state.cpu_runtime=object.cpu_runtime.clone();
            inner.state.object=Some(json!({"id":object.id,"display_name":record["display_name"],"report":report,"preview":object.preview,"envelope_preview":object.envelope_preview}));
            inner.object=Some(object);inner.result=None;inner.state.result=None;inner.state.live_result=None;
            inner.repair=None;inner.state.repair=None;inner.state.revision+=1;guard.committed=true;
            drop(inner);core.prune_previews();core.phase(&op,"validating");Ok(())
        });
        Ok(receipt)
    }

    pub fn start(&self, mut settings: Value) -> Outcome<Receipt> {
        settings["budget_scope"] = json!("total_start");
        validate_settings(&settings)?;
        let (receipt, root, (object,clock)) =
            self.begin_with("solve", Some(settings.clone()), |inner| {
                if inner.repair.is_some() {
                    return Err(error("REPAIR_PENDING", "Accept or reject the repair preview before packing."));
                }
                let record =
                    inner.state.object.as_ref().ok_or_else(|| {
                        error("ASSET_REQUIRED", "Import an accepted object first.")
                    })?;
                if record["report"]["state"] != "accepted"
                    || record["report"]["diagnostics"]["status"] != "valid"
                {
                    return Err(error(
                        "INVALID_SOLID",
                        "Current object is not accepted native geometry.",
                    ));
                }
                let object=inner.object.clone().ok_or_else(||error("ASSET_REQUIRED","Current object files are unavailable."))?;
                if object.native_token.is_none() { return Err(error("ENGINE_SESSION_LOST","Current geometry has no pinned native authority.")); }
                if let Some(runtime)=&inner.state.cpu_runtime {
                    let maximum=runtime["supported_max"].as_u64().unwrap_or(1);
                    if settings.get("thread_count").and_then(Value::as_u64).unwrap_or(1)>maximum {
                        let mut failure=error("UNSUPPORTED_THREAD_COUNT","Explicit CPU count exceeds actual native support.");
                        failure.details=json!({"supported_min":1,"supported_max":maximum,"requested":settings["thread_count"]});
                        return Err(failure);
                    }
                }
                Ok((object,crate::native_session::qpc()?))
            })?;
        self.spawn(&receipt, move |core, op| {
            let preview=object.preview.clone();
            let marker=root.join("stop.marker");
            let output=root.join("solution");
            fs::create_dir(&output).map_err(|e|error("FILE_ACCESS",e.to_string()))?;
            let current_epoch=core.native.lock().unwrap().as_ref().map(|native|native.epoch.clone());
            if object.native_epoch!=current_epoch { return Err(error("ENGINE_SESSION_LOST","Prepared token belongs to an expired native epoch.")); }
            let publication=json!({"version":1,"session_id":core.inner.lock().unwrap().state.session_id,
                "native_epoch":current_epoch,"settings_id":id("settings")});
            let solve_outcome=core.native_request("run",Some(&op),json!({"asset_token":object.native_token,"settings":settings,"publication":publication,
                "result_path":output.join("result.json"),"stop_file":marker,"start_qpc_ticks":clock.0.to_string(),"qpc_frequency_hz":clock.1.to_string()}),false);
            if let Err(failure)=&solve_outcome {
                let mut inner=core.inner.lock().unwrap();
                if let Some(operation)=&mut inner.state.operation {
                    if operation.id==op {operation.native_completion_elapsed_seconds=failure.details["native_completion_elapsed_seconds"].as_f64();}
                }
            }
            if let Ok(response)=&solve_outcome {
                let mut inner=core.inner.lock().unwrap();
                if let Some(operation)=&mut inner.state.operation {
                    if operation.id==op {
                        operation.native_completion_elapsed_seconds=response["native_completion_elapsed_seconds"].as_f64();
                        if response["no_nonempty_incumbent"]==true {operation.detail=Some("No nonempty incumbent.".into());}
                    }
                }
                if let Some(runtime)=inner.state.cpu_runtime.as_mut() {runtime["requested_thread_count"]=settings.get("thread_count").cloned().unwrap_or(json!(1));runtime["resolved_thread_count"]=response["resolved_thread_count"].clone();}
            }
            core.phase(&op, "validating");
            if output.join("result.json").is_file() {
                let document = security::json(&output.join("result.json"), RESULT_LIMIT)?;
                if document["validation"]["status"] != "valid" {
                    return Err(error("INVALID_RESULT", "Native result is not validated."));
                }
                let live = core.inner.lock().unwrap().state.live_result.clone();
                if document["count"] == 0 && (live.is_some() || core.inner.lock().unwrap().state.result.is_some()) {
                    return solve_outcome.map(|_| ());
                }
                if document["count"].as_u64().unwrap_or(0) > 0 {
                    let live = live.as_ref().filter(|live| live["operation_id"] == op).ok_or_else(|| {
                        error(
                            "PUBLICATION_MISMATCH",
                            "Terminal packing has no matching complete public snapshot.",
                        )
                    })?;
                    let object_report = security::json(&object.report, REPORT_LIMIT)?;
                    public_artifact_matches(live, &document, &settings, &object_report)?;
                }

                let result_report=output.join("object.report.json");
                write_json(&result_report,&document["assets"]["object"])?;
                core.copy_report_dependencies(&result_report, &output)?;
                write_json(
                    &output.join("settings.json"),
                    &document["search"]["resolved_settings"],
                )?;
                let result_id = id("result");
                let saved_object = ObjectFiles {
                    id: object.id.clone(),
                    report: output.join("object.report.json"),
                    preview: preview.clone(),
                    envelope_preview: object.envelope_preview.clone(),
                    native_token: object.native_token.clone(),
                    native_epoch: object.native_epoch.clone(),
                    cpu_runtime: object.cpu_runtime.clone(),
                };
                let mut inner = core.inner.lock().unwrap();
                inner.result = Some(ResultFiles {
                    id: result_id.clone(),
                    root: output,
                    object: saved_object,
                });
                inner.state.result = Some(
                    json!({"id":result_id,"document":document,"preview":preview,"origin":"solve"}),
                );
                inner.state.operation.as_mut().unwrap().result_id = Some(result_id);
                inner.state.revision += 1;
                drop(inner);core.prune_previews();
            }
            solve_outcome.map(|_| ())
        });
        Ok(receipt)
    }

    pub fn stop(&self, operation_id: &str) -> Outcome<StopResponse> {
        let mut inner = self.inner.lock().unwrap();
        let op = inner
            .state
            .operation
            .as_ref()
            .ok_or_else(|| error("OPERATION_STALE", "No solve operation is active."))?;
        if op.id != operation_id || op.kind != "solve" || op.finished_at.is_some() {
            return Err(error(
                "OPERATION_STALE",
                "Stop ID does not identify the active solve.",
            ));
        }
        let marker = inner
            .stop_file
            .clone()
            .ok_or_else(|| error("OPERATION_STALE", "Stop marker is unavailable."))?;
        security::directory(marker.parent().unwrap())
            .map_err(|e| error("STOP_FAILED", e.message))?;
        match fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&marker)
        {
            Ok(_) => {}
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => {
                security::regular_file(&marker).map_err(|e| error("STOP_FAILED", e.message))?;
            }
            Err(e) => return Err(error("STOP_FAILED", e.to_string())),
        }
        if inner.state.operation.as_ref().unwrap().phase != "stopping" {
            inner.state.operation.as_mut().unwrap().phase = "stopping".into();
            inner.state.revision += 1;
        }
        Ok(StopResponse {
            operation_id: operation_id.into(),
            accepted: true,
        })
    }

    pub fn read_preview(&self, preview_id: &str) -> Outcome<Vec<u8>> {
        let file = self
            .inner
            .lock()
            .unwrap()
            .previews
            .get(preview_id)
            .cloned()
            .ok_or_else(|| {
                error(
                    "ASSET_UNKNOWN",
                    "Preview handle is not registered in this session.",
                )
            })?;
        let path = security::scoped(&file.root, &file.path)?;
        let bytes = security::read(&path, PREVIEW_LIMIT)?;
        if bytes.len() as u64 != file.bytes || security::hash(&bytes) != file.hash {
            return Err(error(
                "ASSET_MISMATCH",
                "Preview changed since registration.",
            ));
        }
        Ok(bytes)
    }

    pub fn new_project(&self) -> Outcome<State> {
        let mut inner = self.inner.lock().unwrap();
        if inner
            .state
            .operation
            .as_ref()
            .is_some_and(|op| op.finished_at.is_none())
        {
            return Err(error(
                "JOB_BUSY",
                "Wait for the active operation before starting a new project.",
            ));
        }
        if inner.transitioning {
            return Err(error("JOB_BUSY", "Project transition is active."));
        }
        inner.transitioning = true;
        let object = inner.object.clone();
        drop(inner);
        let outcome = object.map_or(Ok(()), |old| {
            self.release_native(old.native_token, old.native_epoch.as_deref())
        });
        let mut inner = self.inner.lock().unwrap();
        inner.transitioning = false;
        outcome?;
        inner.object = None;
        inner.repair = None; inner.state.repair = None;
        inner.result = None;
        inner.previews.clear();
        inner.stop_file = None;
        inner.state.object = None;
        inner.state.result = None;
        inner.state.live_result = None;
        inner.state.draft_settings = None;
        inner.state.cpu_runtime = None;
        inner.state.last_error = None;
        inner.state.operation = None;
        inner.state.revision += 1;
        Ok(inner.state.clone())
    }

    fn prune_previews(&self) {
        let mut inner = self.inner.lock().unwrap();
        let ids = [
            inner.object.as_ref().and_then(|o|o.envelope_preview.as_ref()).and_then(|p|p["preview_id"].as_str()).map(str::to_owned),
            inner.state.repair.as_ref().and_then(|r|r["before"]["preview_id"].as_str()).map(str::to_owned),
            inner.state.repair.as_ref().and_then(|r|r["after"]["preview_id"].as_str()).map(str::to_owned),
            inner.state.live_result.as_ref().and_then(|live|live["preview"]["preview_id"].as_str()).map(str::to_owned),
            inner
                .object
                .as_ref()
                .and_then(|object| object.preview.as_ref())
                .and_then(|preview| preview["preview_id"].as_str())
                .map(str::to_owned),
            inner
                .state
                .result
                .as_ref()
                .and_then(|result| result["preview"]["preview_id"].as_str())
                .map(str::to_owned),
        ];
        inner
            .previews
            .retain(|key, _| ids.iter().any(|id| id.as_ref() == Some(key)));
    }

    fn copy_report_dependencies(&self, report_path: &Path, target: &Path) -> Outcome<()> {
        let report = security::json(report_path, REPORT_LIMIT)?;
        let source_root = report_path.parent().unwrap();
        fn collect(value: &Value, paths: &mut Vec<String>) {
            if let Some(object) = value.as_object() {
                if let Some(path) = object.get("path").and_then(Value::as_str) {
                    paths.push(path.into());
                }
                if let Some(hash) = object.get("mesh_sha256").and_then(Value::as_str) {
                    paths.push(format!("assets/{hash}.ply"));
                }
                for value in object.values() {
                    collect(value, paths);
                }
            } else if let Some(array) = value.as_array() {
                for value in array {
                    collect(value, paths);
                }
            }
        }
        let mut paths = Vec::new();
        collect(&report, &mut paths);
        let mut cursor = 0;
        let mut copied = std::collections::HashSet::new();
        while cursor < paths.len() {
            let name = paths[cursor].clone();
            cursor += 1;
            if !copied.insert(name.clone()) {
                continue;
            }
            if !name.starts_with("assets/")
                || name.contains("..")
                || name.contains('\\')
                || name.contains(':')
            {
                return Err(error(
                    "PATH_INVALID",
                    "Report dependency is not a content-addressed portable asset.",
                ));
            }
            let source = security::scoped(source_root, &source_root.join(&name))?;
            let destination = target.join(&name);
            if !destination.parent().unwrap().exists() {
                fs::create_dir(destination.parent().unwrap())
                    .map_err(|e| error("FILE_ACCESS", e.to_string()))?;
            }
            if !destination.exists() {
                security::copy(&source, &destination, SOURCE_LIMIT)?;
            } else if security::read(&source, SOURCE_LIMIT)?
                != security::read(&destination, SOURCE_LIMIT)?
            {
                return Err(error(
                    "ASSET_MISMATCH",
                    "Snapshot dependency conflicts with exported asset.",
                ));
            }
            if name.ends_with(".json") {
                collect(&security::json(&source, REPORT_LIMIT)?, &mut paths);
            }
            if paths.len() > 1024 {
                return Err(error("MEMORY_LIMIT", "Too many report dependencies."));
            }
        }
        Ok(())
    }

    pub fn export_to(&self, target: &Path, format: &str) -> Outcome<Receipt> {
        if format != "json" && format != "stl" {
            return Err(error(
                "INVALID_REQUEST",
                "Export format must be json or stl.",
            ));
        }
        security::directory(target)?;
        let target = fs::canonicalize(target).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
        let format = format.to_owned();
        let (receipt, root, result) = self.begin_with("export", None, |inner| {
            inner.result.clone().ok_or_else(|| {
                error(
                    "RESULT_REQUIRED",
                    "A complete validated result is required for export.",
                )
            })
        })?;
        self.inner
            .lock()
            .unwrap()
            .state
            .operation
            .as_mut()
            .unwrap()
            .result_id = Some(result.id.clone());
        self.spawn(&receipt, move |core, op| {
            let output = target.join(id("SpectraPack-export"));
            fs::create_dir(&output).map_err(|e| error("OUTPUT_WRITE_FAILED", e.to_string()))?;
            core.phase(&op, "validating");
            let mut args = arguments(&["desktop-restore"]);
            pair(&mut args, "--object-report", &result.object.report);
            pair(&mut args, "--result", &result.root.join("result.json"));
            pair(&mut args, "--output", &output);
            if format == "stl" {
                pair(&mut args, "--stl", &output.join("packed.stl"));
            }
            let outcome = core.run(&args, &root);
            core.inner
                .lock()
                .unwrap()
                .state
                .operation
                .as_mut()
                .unwrap()
                .detail = Some(format!("Output: {}", output.display()));
            outcome.map(|_| ())
        });
        Ok(receipt)
    }
    pub fn save_to(&self, target: &Path, settings: Value) -> Outcome<Receipt> {
        validate_settings(&settings)?;
        security::directory(
            target
                .parent()
                .ok_or_else(|| error("PATH_INVALID", "Archive destination has no parent."))?,
        )?;
        let (receipt, root, (object, result)) = self.begin_with("save", Some(settings.clone()), |inner| {
            for source in &inner.protected_sources {
                if security::aliases(source, target)? { return Err(error("PATH_INVALID", "Save would overwrite an original imported STL.")); }
            }
            let object = inner.object.clone().ok_or_else(|| error("ASSET_REQUIRED", "Import an accepted object before Save."))?;
            if inner.state.object.as_ref().unwrap()["report"]["state"] != "accepted" {
                return Err(error("INVALID_SOLID", "Only accepted objects can be saved."));
            }
            if inner.state.live_result.is_some() && inner.result.is_none() {
                return Err(error("PUBLIC_RESULT_UNAVAILABLE","The displayed packing has no matching finalized files; Save is unavailable."));
            }
            let result = inner.result.clone();
            if let Some(result) = &result {
                let original = security::json(&result.object.report, REPORT_LIMIT)?;
                let current = &inner.state.object.as_ref().unwrap()["report"];
                for key in ["source", "accepted_solid", "frame", "dimensions_mm", "repair_record", "packing_envelope"] {
                    if original[key] != current[key] {
                        return Err(error("ASSET_MISMATCH", "Displayed result belongs to a different object; use New before saving the replacement."));
                    }
                }
            }
            Ok((object, result))
        })?;
        let target = target.to_path_buf();
        self.spawn(&receipt, move |core, op| {
            core.phase(&op, "saving");
            let snapshot = root.join("snapshot");
            fs::create_dir(&snapshot).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
            let report = result
                .as_ref()
                .map(|r| &r.object.report)
                .unwrap_or(&object.report);
            security::copy(report, &snapshot.join("object.report.json"), REPORT_LIMIT)?;
            core.copy_report_dependencies(report, &snapshot)?;
            if let Some(result) = &result {
                let document = security::json(&result.root.join("result.json"), RESULT_LIMIT)?;
                let mut dependency_free = document.clone();
                dependency_free.as_object_mut().unwrap().remove("artifacts");
                write_json(&snapshot.join("result.json"), &dependency_free)?;
                write_json(
                    &snapshot.join("settings.json"),
                    &document["search"]["resolved_settings"],
                )?;
                core.inner
                    .lock()
                    .unwrap()
                    .state
                    .operation
                    .as_mut()
                    .unwrap()
                    .result_id = Some(result.id.clone());
            }
            crate::archive::save(&snapshot, &target, &settings, result.is_some())
        });
        Ok(receipt)
    }

    pub fn open_path(&self, path: &Path) -> Outcome<Receipt> {
        security::regular_file(path)?;
        let path = path.to_path_buf();
        let name = path
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        let (receipt, root) = self.begin("open", None)?;
        self.spawn(&receipt, move |core, op| {
            let extracted = root.join("extracted"); fs::create_dir(&extracted).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
            let manifest = crate::archive::open(&path, &extracted)?;
            let draft = manifest["draft_settings"].clone(); validate_settings(&draft)?;
            let report_path = extracted.join("object.report.json"); let report = security::json(&report_path, REPORT_LIMIT)?;
            let object_id = id("object");
            let mut object = ObjectFiles { id: object_id.clone(), report: report_path, preview: None, envelope_preview: None, native_token: None, native_epoch: None, cpu_runtime: None };
            let (_, preview) = core.prepare(&mut object, &root)?; object.preview = preview.clone();
            let mut prepared_guard=PreparedRollback {core:core.clone(),token:object.native_token.clone(),epoch:object.native_epoch.clone(),preview_ids:[object.preview.as_ref(),object.envelope_preview.as_ref()].into_iter().flatten().filter_map(|p|p["preview_id"].as_str().map(str::to_owned)).collect(),committed:false};
            let mut result_files = None; let mut result_snapshot = None;
            if !manifest["best_result"].is_null() {
                core.phase(&op, "validating");
                let stored = security::json(&extracted.join("result.json"), RESULT_LIMIT)?;
                let settings = security::json(&extracted.join("settings.json"), RECORD_LIMIT)?;
                if settings != stored["search"]["resolved_settings"] {
                    return Err(error("ASSET_MISMATCH", "Saved settings differ from the result's immutable original settings."));
                }
                let output = root.join("restored"); fs::create_dir(&output).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
                let mut args = arguments(&["desktop-restore"]); pair(&mut args, "--object-report", &object.report);
                pair(&mut args, "--result", &extracted.join("result.json")); pair(&mut args, "--output", &output);
                core.run(&args, &root)?; let restored = security::json(&output.join("result.json"), RESULT_LIMIT)?;
                let result_report=output.join("object.report.json");
                write_json(&result_report,&restored["assets"]["object"])?;
                core.copy_report_dependencies(&result_report, &output)?;
                write_json(&output.join("settings.json"), &restored["search"]["resolved_settings"])?;
                let result_id = id("result");
                result_files = Some(ResultFiles { id: result_id.clone(), root: output, object: object.clone() });
                result_snapshot = Some(json!({"id":result_id,"document":restored,"preview":preview,"origin":"project"}));
            }
            let old_object=core.inner.lock().unwrap().object.clone();
            if let Some(old)=old_object {core.release_native(old.native_token,old.native_epoch.as_deref())?;}
            let mut inner = core.inner.lock().unwrap(); inner.state.cpu_runtime=object.cpu_runtime.clone(); inner.object = Some(object.clone()); inner.result = result_files;
            prepared_guard.committed=true;
            inner.state.live_result=None;
            inner.repair=None; inner.state.repair=None;
            inner.state.object = Some(json!({"id":object_id,"display_name":name,"report":report,"preview":object.preview,"envelope_preview":object.envelope_preview}));
            inner.state.result = result_snapshot; inner.state.draft_settings = Some(draft);
            let result_id = inner.result.as_ref().map(|r| r.id.clone());
            inner.state.operation.as_mut().unwrap().result_id = result_id; inner.state.revision += 1;
            drop(inner);core.prune_previews();Ok(())
        });
        Ok(receipt)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(windows)]
    #[test]
    fn post_spawn_failure_reaps_child_before_failed_operation_is_published() {
        use windows_sys::Win32::{
            Foundation::{CloseHandle, WAIT_OBJECT_0},
            System::Threading::*,
        };
        let (_root, core) = fixture();
        let (handle_tx, handle_rx) = std::sync::mpsc::channel();
        *core.after_spawn.lock().unwrap() = Some(Box::new(move |pid| {
            let handle = unsafe { OpenProcess(PROCESS_SYNCHRONIZE | PROCESS_TERMINATE, 0, pid) };
            assert!(!handle.is_null());
            handle_tx.send(handle as usize).unwrap();
            panic!("injected post-spawn transport failure");
        }));
        let (receipt, root) = core.begin("import", None).unwrap();
        core.spawn(&receipt, move |core, _| {
            core.run(&arguments(&["capabilities", "--json"]), &root)
                .map(|_| ())
        });
        let handle = handle_rx
            .recv_timeout(std::time::Duration::from_secs(5))
            .unwrap() as windows_sys::Win32::Foundation::HANDLE;
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        loop {
            if core.state().operation.unwrap().finished_at.is_some() {
                break;
            }
            assert!(std::time::Instant::now() < deadline);
            thread::sleep(std::time::Duration::from_millis(10));
        }
        assert_eq!(core.state().last_error.unwrap().code, "INTERNAL_ERROR");
        let exited = unsafe { WaitForSingleObject(handle, 0) } == WAIT_OBJECT_0;
        unsafe {
            if !exited {
                TerminateProcess(handle, 1);
                WaitForSingleObject(handle, 5000);
            }
            CloseHandle(handle);
        }
        assert!(
            exited,
            "failed operation may be published only after its native child is reaped"
        );
    }
    #[cfg(windows)]
    #[test]
    #[ignore = "subprocess fixture invoked by exit_at_spawn_boundary_cannot_orphan_native_child"]
    fn spawn_boundary_child_helper() {
        let (_root, core) = fixture();
        *core.after_spawn.lock().unwrap() = Some(Box::new(|pid| {
            println!("SPAWN_BOUNDARY_PID={pid}");
            use std::io::Write;
            std::io::stdout().flush().unwrap();
            loop {
                std::thread::park();
            }
        }));
        core.run(&arguments(&["capabilities", "--json"]), &core.root)
            .unwrap();
    }
    #[cfg(windows)]
    #[test]
    fn exit_at_spawn_boundary_cannot_orphan_native_child() {
        use std::io::BufRead;
        use windows_sys::Win32::{
            Foundation::{CloseHandle, WAIT_OBJECT_0},
            System::Threading::*,
        };
        let mut owner = Command::new(std::env::current_exe().unwrap())
            .args([
                "--ignored",
                "--exact",
                "core::tests::spawn_boundary_child_helper",
                "--nocapture",
            ])
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .unwrap();
        let mut pid = None;
        for line in std::io::BufReader::new(owner.stdout.take().unwrap()).lines() {
            let line = line.unwrap();
            if let Some(value) = line.strip_prefix("SPAWN_BOUNDARY_PID=") {
                pid = Some(value.parse::<u32>().unwrap());
                break;
            }
        }
        let process =
            unsafe { OpenProcess(PROCESS_SYNCHRONIZE | PROCESS_TERMINATE, 0, pid.unwrap()) };
        assert!(!process.is_null());
        owner.kill().unwrap();
        owner.wait().unwrap();
        let exited = unsafe { WaitForSingleObject(process, 5000) } == WAIT_OBJECT_0;
        unsafe {
            if !exited {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 5000);
            }
            CloseHandle(process);
        }
        assert!(
            exited,
            "owner exit at the child-creation boundary must leave no native process"
        );
    }
    #[test]
    fn t015_public_binding_gate_keeps_exact_poses_and_resets_only_for_current_run() {
        use crate::native_session::Publication;
        let binding = Publication {
            version: 1,
            session_id: "session".into(),
            native_epoch: "native".into(),
            settings_id: "settings".into(),
        };
        let state = State {
            desktop_version: 1,
            session_id: "session".into(),
            revision: 0,
            object: None,
            repair: None,
            draft_settings: None,
            result: None,
            live_result: None,
            last_error: None,
            cpu_runtime: None,
            operation: Some(Operation {
                id: "operation".into(),
                kind: "solve".into(),
                phase: "placing".into(),
                started_at: now(),
                finished_at: None,
                result_id: None,
                detail: None,
                sequence: None,
                completion_elapsed_seconds: None,
                native_completion_elapsed_seconds: None,
            }),
        };
        let mut inner = Inner {
            repair: None,
            state,
            object: Some(ObjectFiles {
                id: "object".into(),
                report: PathBuf::new(),
                preview: None,
                envelope_preview: None,
                native_token: Some("asset".into()),
                native_epoch: Some("native".into()),
                cpu_runtime: None,
            }),
            result: None,
            previews: HashMap::new(),
            stop_file: None,
            protected_sources: Vec::new(),
            operation_started: None,
            transitioning: false,
        };
        let value = |count: u32, revision, sequence| {
            json!({"operation_id":"operation","settings_id":"settings","native_epoch":"native",
            "solution_revision":revision,"sequence":sequence,"count":count,
            "placements":(0..count).map(|i|json!({"copy_id":format!("copy-{i}"),"translation_mm":[i*2+1,revision,1],
                "quaternion_xyzw":[0,0,0,1],"local_to_world":[[1,0,0,i*2+1],[0,1,0,revision],[0,0,1,1],[0,0,0,1]]})).collect::<Vec<_>>()})
        };
        let token = json!("asset");
        assert!(accept_live(&mut inner, &binding, "operation", &token, value(24, 1, 1)));
        let a24 = inner.state.live_result.clone();
        assert!(!accept_live(&mut inner, &binding, "operation", &token, value(24, 1, 2)));
        assert_eq!(inner.state.live_result, a24);
        for field in ["session", "native", "settings"] {
            let mut foreign = binding.clone();
            match field {
                "session" => foreign.session_id = "old".into(),
                "native" => foreign.native_epoch = "old".into(),
                _ => foreign.settings_id = "old".into(),
            }
            assert!(!accept_live(&mut inner, &foreign, "operation", &token, value(25, 2, 2)));
        }
        assert!(!accept_live(
            &mut inner,
            &binding,
            "old-operation",
            &token,
            value(25, 2, 2)
        ));
        assert!(!accept_live(
            &mut inner,
            &binding,
            "operation",
            &json!("old-asset"),
            value(25, 2, 2)
        ));
        assert!(accept_live(&mut inner, &binding, "operation", &token, value(25, 2, 2)));
        let d25 = inner.state.live_result.clone();
        assert!(!accept_live(&mut inner, &binding, "operation", &token, value(26, 1, 3)));
        assert!(!accept_live(&mut inner, &binding, "operation", &token, value(26, 3, 2)));
        assert_eq!(inner.state.live_result, d25);
        assert!(accept_live(&mut inner, &binding, "operation", &token, value(26, 3, 3)));
        assert!(accept_live(&mut inner, &binding, "operation", &token, value(26, 4, 4)));
        assert!(!accept_live(&mut inner, &binding, "operation", &token, value(25, 5, 5)));
        inner.state.operation.as_mut().unwrap().id = "fresh".into();
        assert!(accept_live(
            &mut inner,
            &binding,
            "fresh",
            &token,
            json!({"operation_id":"fresh","count":1,"solution_revision":1,"sequence":1})
        ));
    }
    #[test]
    fn t015_terminal_equal_count_pose_change_is_a_publication_error() {
        let settings = settings();
        let live = json!({"count":1,"solution_revision":2,"placements":[{"copy_id":"first","translation_mm":[1,2,3],"quaternion_xyzw":[0,0,0,1]}]});
        let mut document = json!({"count":1,"solution_revision":2,"placements":live["placements"],
            "container":{"dimensions_mm":settings["box_dimensions_mm"]},"assets":{"object":{}},
            "search":{"resolved_settings":{"container":{"dimensions_mm":settings["box_dimensions_mm"]},
                "clearance_mm":settings["clearance_mm"],"orientation":settings["orientation"],
                "resolved":{"pitch_mm":settings["pitch_mm"]},"search":{"seed":settings["seed"]}}}});
        assert!(public_artifact_matches(&live, &document, &settings, &json!({})).is_ok());
        document["placements"][0]["translation_mm"] = json!([4, 5, 6]);
        assert_eq!(
            public_artifact_matches(&live, &document, &settings, &json!({}))
                .unwrap_err()
                .code,
            "PUBLICATION_MISMATCH"
        );
    }

    #[test]
    fn t015_canonical_fixed_settings_preserve_exact_public_placement_matching() {
        let live = json!({"count":1,"solution_revision":1,"placements":[{"copy_id":"one",
            "translation_mm":[1,2,3],"quaternion_xyzw":[0,0,0,1]}]});
        let mut requested = settings();
        let mut document = json!({"count":1,"solution_revision":1,"placements":live["placements"],
            "container":{"dimensions_mm":requested["box_dimensions_mm"]},"assets":{"object":{}},
            "search":{"resolved_settings":{"container":{"dimensions_mm":requested["box_dimensions_mm"]},
                "clearance_mm":requested["clearance_mm"],
                "orientation":requested["orientation"],"resolved":{"pitch_mm":requested["pitch_mm"]},
                "search":{"seed":requested["seed"]}}}});
        for q in [[0.0, 0.0, 0.0, -1.0], [0.0, 0.0, 0.0, 1.0000000005]] {
            requested["orientation"]["quaternion_xyzw"] = json!(q);
            assert!(
                public_artifact_matches(&live, &document, &requested, &json!({})).is_ok(),
                "{q:?}"
            );
        }
        requested["orientation"]["quaternion_xyzw"] = json!([0, 0, 1e-10, 1]);
        assert!(public_artifact_matches(&live, &document, &requested, &json!({})).is_err());
        requested["orientation"]["quaternion_xyzw"] = json!([0, 0, 0, -1]);
        document["placements"][0]["quaternion_xyzw"] = json!([0, 0, 0, -1]);
        assert_eq!(
            public_artifact_matches(&live, &document, &requested, &json!({}))
                .unwrap_err()
                .code,
            "PUBLICATION_MISMATCH"
        ); // Physical settings equivalence never relaxes canonical pose equality.
    }

    #[test]
    fn t015_delivery_failure_keeps_real_public_poses_and_disables_old_file_fallback() {
        use crate::native_session::Publication;
        let (root, core) = fixture();
        import_fixture(&core);
        core.start(settings()).unwrap();
        let previous = complete(&core);
        assert!(previous.last_error.is_none(), "{previous:?}");
        assert!(previous.result.is_some());
        let mut live = previous.live_result.unwrap();
        let (receipt, _) = core.begin("solve", None).unwrap();
        let (binding, token) = {
            let inner = core.inner.lock().unwrap();
            let object = inner.object.as_ref().unwrap();
            (
                Publication {
                    version: 1,
                    session_id: inner.state.session_id.clone(),
                    native_epoch: object.native_epoch.clone().unwrap(),
                    settings_id: "fresh-settings".into(),
                },
                json!(object.native_token),
            )
        };
        live["operation_id"] = json!(receipt.operation_id);
        live["settings_id"] = json!(binding.settings_id);
        live["solution_revision"] = json!(1);
        live["sequence"] = json!(1);
        assert!(accept_live(
            &mut core.inner.lock().unwrap(),
            &binding,
            &receipt.operation_id,
            &token,
            live.clone()
        ));
        // This is the same operational error returned by the actual writer-refusal test.
        core.spawn(&receipt, |_, _| {
            Err(error(
                "PUBLICATION_RESOURCE_LIMIT",
                "Complete record cannot be delivered.",
            ))
        });
        let failed = complete(&core);
        assert_eq!(failed.last_error.unwrap().code, "PUBLICATION_RESOURCE_LIMIT");
        assert_eq!(failed.live_result, Some(live));
        assert!(failed.result.is_none());
        assert_eq!(
            core.save_to(&root.path().join("unavailable.spectrapack"), settings())
                .unwrap_err()
                .code,
            "PUBLIC_RESULT_UNAVAILABLE"
        );
        for format in ["json", "stl"] {
            assert_eq!(core.export_to(root.path(), format).unwrap_err().code, "RESULT_REQUIRED");
        }
        assert!(!root.path().join("unavailable.spectrapack").exists());
    }

    fn fixture() -> (tempfile::TempDir, Core) {
        let root = tempfile::tempdir().unwrap();
        let engine = PathBuf::from(
            std::env::var_os("SPECTRAPACK_TEST_ENGINE").expect("real engine required"),
        );
        let core = Core::new(engine, root.path().join("session")).unwrap();
        (root, core)
    }
    fn settings() -> Value {
        json!({"desktop_version":1,"box_dimensions_mm":[40,40,40],"clearance_mm":{"pair":0,"wall":0},
            "orientation":{"mode":"fixed","quaternion_xyzw":[0,0,0,1]},"pitch_mm":10,"budget_seconds":1,"seed":"42"})
    }
    fn complete(core: &Core) -> State {
        let until = std::time::Instant::now() + std::time::Duration::from_secs(15);
        loop {
            let state = core.state();
            if state
                .operation
                .as_ref()
                .is_some_and(|operation| operation.finished_at.is_some())
            {
                return state;
            }
            assert!(
                std::time::Instant::now() < until,
                "operation failed to finish: {state:?}"
            );
            thread::sleep(std::time::Duration::from_millis(20));
        }
    }
    fn import_fixture(core: &Core) {
        let source = PathBuf::from(
            std::env::var_os("SPECTRAPACK_TEST_STL").expect("real analytic fixture required"),
        );
        core.import_path(
            &source,
            ImportRequest {
                units: "mm".into(),
                scale_mm: None,
            },
        )
        .unwrap();
        let state = complete(core);
        assert!(state.last_error.is_none(), "{state:?}");
    }
    #[test]
    fn cube45_start_save_open_and_export_preserve_native_catalog() {
        let (root, core) = fixture();
        import_fixture(&core);
        let mut requested = settings();
        requested["orientation"] = json!({"mode":"cube45"});
        core.start(requested.clone()).unwrap();
        let solved = complete(&core);
        assert!(solved.last_error.is_none(), "{solved:?}");
        let document = &solved.result.as_ref().unwrap()["document"];
        assert!(document["count"].as_u64().unwrap() > 0);
        let resolved = &document["search"]["resolved_settings"];
        assert_eq!(resolved["orientation"]["mode"], "custom");
        assert_eq!(resolved["orientation"]["quaternions_xyzw"].as_array().unwrap().len(), 96);
        assert_eq!(resolved["resolved"]["orientation_catalog_sha256"], CUBE45_CATALOG_HASH);
        let mut wrong_catalog = document.clone();
        wrong_catalog["search"]["resolved_settings"]["resolved"]["orientation_catalog_sha256"] = json!("0".repeat(64));
        assert!(public_artifact_matches(solved.live_result.as_ref().unwrap(), &wrong_catalog,
            &requested, &document["assets"]["object"]).is_err());
        let project = root.path().join("diagonal.spectrapack");
        core.save_to(&project, requested.clone()).unwrap();
        let saved = complete(&core);
        assert!(saved.last_error.is_none(), "{saved:?}");
        core.new_project().unwrap();
        core.open_path(&project).unwrap();
        let opened = complete(&core);
        assert!(opened.last_error.is_none(), "{opened:?}");
        assert_eq!(opened.draft_settings.as_ref().unwrap()["orientation"], requested["orientation"]);
        assert_eq!(&opened.result.as_ref().unwrap()["document"], document);
        core.export_to(root.path(), "stl").unwrap();
        let exported = complete(&core);
        assert!(exported.last_error.is_none(), "{exported:?}");
    }
    #[test]
    fn child_death_allows_new_and_verified_reimport_without_releasing_foreign_epoch() {
        let (_root, core) = fixture();
        import_fixture(&core);
        // T011-A5 / SOL-06: characterize recovery with a complete nonempty result.
        core.start(settings()).unwrap();
        let previous = complete(&core);
        assert!(previous.last_error.is_none(), "{previous:?}");
        let document = &previous.result.as_ref().unwrap()["document"];
        assert_eq!(document["validation"]["status"], "valid");
        let count = document["count"].as_u64().unwrap();
        assert!(count > 0);
        assert_eq!(
            document["placements"].as_array().unwrap().len() as u64,
            count
        );
        let preview_id = previous.result.as_ref().unwrap()["preview"]["preview_id"]
            .as_str()
            .unwrap();
        let preview_bytes = core.read_preview(preview_id).unwrap();
        core.native
            .lock()
            .unwrap()
            .as_mut()
            .unwrap()
            .kill_for_test();
        core.start(settings()).unwrap();
        let failed = complete(&core);
        assert_eq!(failed.last_error.as_ref().unwrap().code, "ENGINE_SESSION_LOST");
        assert_eq!(failed.object, previous.object);
        assert_eq!(failed.result, previous.result);
        assert_eq!(core.read_preview(preview_id).unwrap(), preview_bytes);
        eprintln!("native child death: prior_valid_count={count}, result_and_preview_retained=true");
        core.new_project().unwrap();
        assert!(core.state().object.is_none());
        assert!(core.state().result.is_none());
        assert!(core.read_preview(preview_id).is_err());
        import_fixture(&core);
        let old_epoch = core
            .inner
            .lock()
            .unwrap()
            .object
            .as_ref()
            .unwrap()
            .native_epoch
            .clone();
        core.native
            .lock()
            .unwrap()
            .as_mut()
            .unwrap()
            .kill_for_test();
        import_fixture(&core);
        assert_ne!(
            core.inner
                .lock()
                .unwrap()
                .object
                .as_ref()
                .unwrap()
                .native_epoch,
            old_epoch
        );
        let state=core.state();
        for key in ["preview","envelope_preview"] {
            let id=state.object.as_ref().unwrap()[key]["preview_id"].as_str().unwrap();
            assert!(!core.read_preview(id).unwrap().is_empty());
        }
    }
    #[test]
    fn failed_stop_marker_does_not_publish_accepted_cancellation() {
        let (_root, core) = fixture();
        let (receipt, directory) = core.begin("solve", None).unwrap();
        let marker = directory.join("stop.marker");
        fs::create_dir(&marker).unwrap();
        let before = core.state();
        assert!(core.stop(&receipt.operation_id).is_err());
        let failed = core.state();
        assert_eq!(failed.operation.unwrap().phase, "preparing");
        assert_eq!(failed.revision, before.revision);
        fs::remove_dir(&marker).unwrap();
        assert!(core.stop(&receipt.operation_id).unwrap().accepted);
        assert_eq!(core.state().operation.unwrap().phase, "stopping");
    }
    #[test]
    fn new_between_request_and_registration_cannot_use_a_stale_snapshot() {
        for kind in ["solve", "export", "save"] {
            let (root, core) = fixture();
            let object = ObjectFiles {
                id: "old-object".into(),
                report: root.path().join("report.json"),
                preview: None,
                envelope_preview: None,
                native_token: None,
                native_epoch: None,
                cpu_runtime: None,
            };
            {
                let mut inner = core.inner.lock().unwrap();
                inner.object = Some(object.clone());
                inner.state.object =
                    Some(json!({"report":{"state":"accepted","diagnostics":{"status":"valid"}}}));
                if kind == "export" {
                    inner.result = Some(ResultFiles {
                        id: "old-result".into(),
                        root: root.path().into(),
                        object,
                    });
                }
            }
            let (reached_tx, reached_rx) = std::sync::mpsc::channel();
            let (resume_tx, resume_rx) = std::sync::mpsc::channel();
            *core.before_begin.lock().unwrap() = Some(Box::new(move || {
                reached_tx.send(()).unwrap();
                resume_rx.recv().unwrap();
            }));
            let task = core.clone();
            let target = root.path().to_path_buf();
            let request = thread::spawn(move || match kind {
                "solve" => task.start(settings()),
                "export" => task.export_to(&target, "json"),
                _ => task.save_to(&target.join("saved.spectrapack"), settings()),
            });
            reached_rx
                .recv_timeout(std::time::Duration::from_secs(5))
                .unwrap();
            core.new_project().unwrap();
            resume_tx.send(()).unwrap();
            let code = request.join().unwrap().unwrap_err().code;
            assert_eq!(
                code,
                if kind == "export" {
                    "RESULT_REQUIRED"
                } else {
                    "ASSET_REQUIRED"
                },
                "{kind}"
            );
            assert!(core.state().operation.is_none());
        }
    }
}
