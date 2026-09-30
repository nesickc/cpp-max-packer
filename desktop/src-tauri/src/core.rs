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
struct Inner {
    state: State,
    object: Option<ObjectFiles>,
    result: Option<ResultFiles>,
    previews: HashMap<String, PreviewFile>,
    stop_file: Option<PathBuf>,
    protected_sources: Vec<PathBuf>,
}

#[derive(Clone)]
pub struct Core {
    engine: PathBuf,
    root: PathBuf,
    inner: Arc<Mutex<Inner>>,
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
            draft_settings: None,
            result: None,
            operation: None,
            last_error: None,
        };
        let core = Self {
            engine,
            root,
            #[cfg(test)]
            before_begin: Arc::new(Mutex::new(None)),
            #[cfg(test)]
            after_spawn: Arc::new(Mutex::new(None)),
            inner: Arc::new(Mutex::new(Inner {
                state,
                object: None,
                result: None,
                previews: HashMap::new(),
                stop_file: None,
                protected_sources: Vec::new(),
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
        let captured = snapshot(&inner)?;
        let operation_id = id("operation");
        let directory = self.root.join(&operation_id);
        fs::create_dir(&directory).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
        inner.stop_file = if kind == "solve" {
            Some(directory.join("stop.marker"))
        } else {
            None
        };
        inner.state.operation = Some(Operation {
            id: operation_id.clone(),
            kind: kind.into(),
            phase: "preparing".into(),
            started_at: now(),
            finished_at: None,
            result_id: None,
            detail: None,
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
                let op = inner.state.operation.as_mut().unwrap();
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
        let mut command = Command::new(&self.engine);
        command
            .args(args)
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
    fn prepare(
        &self,
        object: &ObjectFiles,
        settings: &Value,
        root: &Path,
    ) -> Outcome<(PathBuf, Option<Value>)> {
        validate_settings(settings)?;
        let request = root.join("desktop.request.json");
        write_json(&request, settings)?;
        let output = root.join("prepared");
        fs::create_dir(&output).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
        let mut args = arguments(&["desktop-prepare"]);
        pair(&mut args, "--object-report", &object.report);
        pair(&mut args, "--request", &request);
        pair(&mut args, "--output", &output);
        let response = self.run(&args, root)?;
        let path = reported_path(
            response["settings_path"]
                .as_str()
                .ok_or_else(|| error("ENGINE_TRANSPORT", "Settings path missing."))?,
        );
        Ok((
            security::scoped(&output, &path)?,
            self.register_preview(&output, &response)?,
        ))
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
            if let Some(scale) = request.scale_mm { args.extend(["--scale-mm".into(), scale.to_string().into()]); }
            core.run(&args, &root)?;
            let report = security::json(&report_path, REPORT_LIMIT)?; let object_id = id("object");
            let mut object = ObjectFiles { id: object_id.clone(), report: report_path, preview: None };
            let longest = report["dimensions_mm"].as_array()
                .map(|v| v.iter().filter_map(Value::as_f64).fold(1.0_f64, f64::max)).unwrap_or(10.0);
            let settings = json!({"desktop_version":1,"box_dimensions_mm":[100.0,100.0,100.0],
                "clearance_mm":{"pair":1.0,"wall":1.0},"orientation":{"mode":"fixed","quaternion_xyzw":[0,0,0,1]},
                "pitch_mm":longest/64.0,"budget_seconds":60.0,"seed":"42"});
            if report["state"] == "accepted" && report["diagnostics"]["status"] == "valid" {
                let (_, preview) = core.prepare(&object, &settings, &root)?; object.preview = preview;
            }
            let mut inner = core.inner.lock().unwrap(); inner.object = Some(object.clone());
            inner.protected_sources.push(source.clone());
            inner.state.object = Some(json!({"id":object_id,"display_name":name,"report":report,"preview":object.preview}));
            inner.state.draft_settings = Some(settings); inner.state.revision += 1; drop(inner);
            core.phase(&op, "validating"); Ok(())
        });
        Ok(receipt)
    }

    pub fn start(&self, settings: Value) -> Outcome<Receipt> {
        validate_settings(&settings)?;
        let (receipt, root, object) =
            self.begin_with("solve", Some(settings.clone()), |inner| {
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
                inner
                    .object
                    .clone()
                    .ok_or_else(|| error("ASSET_REQUIRED", "Current object files are unavailable."))
            })?;
        self.spawn(&receipt, move |core, op| {
            let (resolved, preview) = core.prepare(&object, &settings, &root)?;
            let marker = root.join("stop.marker");
            if marker.exists() {
                core.inner
                    .lock()
                    .unwrap()
                    .state
                    .operation
                    .as_mut()
                    .unwrap()
                    .detail =
                    Some("Stopped during preparation; previous complete result retained.".into());
                return Ok(());
            }
            let output = root.join("solution");
            fs::create_dir(&output).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
            core.phase(&op, "running");
            let mut args = arguments(&["solve"]);
            pair(&mut args, "--settings", &resolved);
            pair(&mut args, "--object-report", &object.report);
            pair(&mut args, "--result", &output.join("result.json"));
            pair(&mut args, "--stop-file", &marker);
            let solve_outcome = core.run(&args, &root);
            core.phase(&op, "validating");
            if output.join("result.json").is_file() {
                let document = security::json(&output.join("result.json"), RESULT_LIMIT)?;
                if document["validation"]["status"] != "valid" {
                    return Err(error("INVALID_RESULT", "Native result is not validated."));
                }
                security::copy(
                    &object.report,
                    &output.join("object.report.json"),
                    REPORT_LIMIT,
                )?;
                core.copy_report_dependencies(&object.report, &output)?;
                write_json(
                    &output.join("settings.json"),
                    &document["search"]["resolved_settings"],
                )?;
                let result_id = id("result");
                let saved_object = ObjectFiles {
                    id: object.id.clone(),
                    report: output.join("object.report.json"),
                    preview: preview.clone(),
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
        inner.object = None;
        inner.result = None;
        inner.previews.clear();
        inner.stop_file = None;
        inner.state.object = None;
        inner.state.result = None;
        inner.state.draft_settings = None;
        inner.state.last_error = None;
        inner.state.operation = None;
        inner.state.revision += 1;
        Ok(inner.state.clone())
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
            let result = inner.result.clone();
            if let Some(result) = &result {
                let original = security::json(&result.object.report, REPORT_LIMIT)?;
                let current = &inner.state.object.as_ref().unwrap()["report"];
                for key in ["source", "accepted_solid", "frame", "dimensions_mm", "repair_record"] {
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
            let mut object = ObjectFiles { id: object_id.clone(), report: report_path, preview: None };
            let (_, preview) = core.prepare(&object, &draft, &root)?; object.preview = preview.clone();
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
                security::copy(&object.report, &output.join("object.report.json"), REPORT_LIMIT)?;
                core.copy_report_dependencies(&object.report, &output)?;
                write_json(&output.join("settings.json"), &restored["search"]["resolved_settings"])?;
                let result_id = id("result");
                result_files = Some(ResultFiles { id: result_id.clone(), root: output, object: object.clone() });
                result_snapshot = Some(json!({"id":result_id,"document":restored,"preview":preview,"origin":"project"}));
            }
            let mut inner = core.inner.lock().unwrap(); inner.object = Some(object.clone()); inner.result = result_files;
            inner.state.object = Some(json!({"id":object_id,"display_name":name,"report":report,"preview":object.preview}));
            inner.state.result = result_snapshot; inner.state.draft_settings = Some(draft);
            let result_id = inner.result.as_ref().map(|r| r.id.clone());
            inner.state.operation.as_mut().unwrap().result_id = result_id; inner.state.revision += 1; Ok(())
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
