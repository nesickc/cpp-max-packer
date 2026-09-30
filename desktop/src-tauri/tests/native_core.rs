use serde_json::{json, Value};
use spectrapack_desktop::{
    core::Core,
    model::{ImportRequest, State},
};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

fn fixture() -> (tempfile::TempDir, Core, PathBuf) {
    let engine = PathBuf::from(
        std::env::var_os("SPECTRAPACK_TEST_ENGINE").expect("real test engine required"),
    );
    let source = PathBuf::from(
        std::env::var_os("SPECTRAPACK_TEST_STL").expect("analytic STL fixture required"),
    );
    let root = tempfile::tempdir().unwrap();
    let core = Core::new(engine, root.path().join("private-session")).unwrap();
    (root, core, source)
}
fn wait(core: &Core) -> State {
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let state = core.state();
        if state
            .operation
            .as_ref()
            .is_some_and(|op| op.finished_at.is_some())
        {
            return state;
        }
        assert!(
            Instant::now() < deadline,
            "native operation must finish: {state:?}"
        );
        std::thread::sleep(Duration::from_millis(250));
    }
}
fn settings(budget: f64) -> Value {
    json!({"desktop_version":1,"box_dimensions_mm":[40,40,40],"clearance_mm":{"pair":0,"wall":0},
           "orientation":{"mode":"fixed","quaternion_xyzw":[0,0,0,1]},"pitch_mm":10,"budget_seconds":budget,"seed":"42"})
}
fn import(core: &Core, source: &std::path::Path) {
    core.import_path(
        source,
        ImportRequest {
            units: "mm".into(),
            scale_mm: None,
        },
    )
    .unwrap();
    let state = wait(core);
    assert!(state.last_error.is_none(), "{state:?}");
    assert_eq!(
        state.object.as_ref().unwrap()["report"]["state"],
        "accepted"
    );
}

#[test]
fn real_native_import_is_asynchronous_and_scoped() {
    let (_root, core, source) = fixture();
    let started = Instant::now();
    let receipt = core.import_path(
        &source,
        ImportRequest {
            units: "mm".into(),
            scale_mm: None,
        },
    );
    assert!(
        receipt.is_ok(),
        "real native import must return an operation receipt: {receipt:?}"
    );
    assert!(started.elapsed() < Duration::from_millis(250));
    assert_eq!(
        core.import_path(
            &source,
            ImportRequest {
                units: "mm".into(),
                scale_mm: None
            }
        )
        .unwrap_err()
        .code,
        "JOB_BUSY"
    );
    let state = wait(&core);
    assert!(state.last_error.is_none(), "{state:?}");
    let object = state.object.unwrap();
    assert_eq!(object["report"]["dimensions_mm"], json!([10.0, 10.0, 10.0]));
    let preview = &object["preview"];
    let bytes = core
        .read_preview(preview["preview_id"].as_str().unwrap())
        .unwrap();
    assert_eq!(
        spectrapack_desktop::security::hash(&bytes),
        preview["sha256"]
    );
    assert!(core.read_preview("C:/Windows/system.ini").is_err());
}

#[test]
fn stop_receipt_is_prompt_and_retains_real_native_valid_solution() {
    let (_root, core, source) = fixture();
    import(&core, &source);
    let mut long = settings(30.0);
    long["box_dimensions_mm"] = json!([400, 400, 400]);
    long["pitch_mm"] = 1.into();
    let receipt = core.start(long).unwrap();
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let state = core.state();
        assert!(state.last_error.is_none(), "{state:?}");
        if state.operation.as_ref().unwrap().phase == "running" {
            break;
        }
        assert!(Instant::now() < deadline);
        std::thread::sleep(Duration::from_millis(250));
    }
    std::thread::sleep(Duration::from_millis(500));
    let started = Instant::now();
    let response = core.stop(&receipt.operation_id).unwrap();
    assert!(started.elapsed() < Duration::from_millis(250));
    assert!(response.accepted);
    assert_eq!(core.state().operation.as_ref().unwrap().phase, "stopping");
    assert!(core.stop(&receipt.operation_id).is_ok());
    assert!(core.stop("stale").is_err());
    let state = wait(&core);
    assert!(state.last_error.is_none(), "{state:?}");
    let result = state.result.unwrap();
    assert_eq!(result["document"]["validation"]["status"], "valid");
    assert_eq!(
        result["document"]["metrics"]["termination_reason"],
        "user_stopped"
    );
    assert!(core.stop(&receipt.operation_id).is_err());
}

#[test]
fn mismatched_object_save_is_rejected_without_silent_result_loss() {
    let (root, core, source) = fixture();
    import(&core, &source);
    core.start(settings(0.2)).unwrap();
    let previous = wait(&core);
    assert!(previous.last_error.is_none(), "{previous:?}");
    assert!(previous.result.is_some());
    let replacement = root.path().join("different.stl");
    let mut bytes = std::fs::read(&source).unwrap();
    bytes[0] = b'X';
    std::fs::write(&replacement, bytes).unwrap();
    import(&core, &replacement);
    let saved = core.save_to(&root.path().join("mismatch.spectrapack"), settings(0.2));
    assert_eq!(saved.unwrap_err().code, "ASSET_MISMATCH");
    assert_eq!(core.state().result, previous.result);
}

#[test]
fn save_cannot_overwrite_original_stl_or_hardlink() {
    let (root, core, source) = fixture();
    let owned = root.path().join("original.stl");
    std::fs::copy(source, &owned).unwrap();
    import(&core, &owned);
    let original = std::fs::read(&owned).unwrap();
    assert!(core.save_to(&owned, settings(0.2)).is_err());
    let alias = root.path().join("aliased.spectrapack");
    std::fs::hard_link(&owned, &alias).unwrap();
    assert!(core.save_to(&alias, settings(0.2)).is_err());
    assert_eq!(std::fs::read(owned).unwrap(), original);
}

#[test]
fn real_native_import_at_appdata_depth_keeps_long_artifact_paths_usable() {
    let engine = PathBuf::from(std::env::var_os("SPECTRAPACK_TEST_ENGINE").unwrap());
    let source = PathBuf::from(std::env::var_os("SPECTRAPACK_TEST_STL").unwrap());
    let root = tempfile::tempdir().unwrap();
    let parent = root
        .path()
        .join("io.spectrapack.desktop-sessions-with-the-normal-native-application-data-path-depth");
    std::fs::create_dir(&parent).unwrap();
    let core = Core::new(
        engine,
        parent.join("session-0123456789abcdef0123456789abcdef"),
    )
    .unwrap();
    import(&core, &source);
    core.start(settings(0.2)).unwrap();
    let state = wait(&core);
    assert!(state.last_error.is_none(), "{state:?}");
    assert_eq!(
        state.result.unwrap()["document"]["validation"]["status"],
        "valid"
    );
}

#[test]
fn real_project_move_open_and_checked_stl_export_preserve_native_snapshot() {
    let (root, core, source) = fixture();
    import(&core, &source);
    core.start(settings(0.2)).unwrap();
    let state = wait(&core);
    assert!(state.last_error.is_none(), "{state:?}");
    let document = state.result.as_ref().unwrap()["document"].clone();
    let mut pending = settings(2.0);
    pending["box_dimensions_mm"] = json!([60, 60, 60]);
    let archive = root.path().join("saved.spectrapack");
    core.save_to(&archive, pending.clone()).unwrap();
    let state = wait(&core);
    assert!(state.last_error.is_none(), "{state:?}");
    let bytes = std::fs::read(&archive).unwrap();
    assert_eq!(&bytes[..2], b"PK");
    let directory = root.path().join("перенос with spaces");
    std::fs::create_dir(&directory).unwrap();
    let moved = directory.join("moved.spectrapack");
    std::fs::rename(archive, &moved).unwrap();
    core.new_project().unwrap();
    core.open_path(&moved).unwrap();
    let opened = wait(&core);
    assert!(opened.last_error.is_none(), "{opened:?}");
    assert_eq!(opened.draft_settings, Some(pending));
    let restored = &opened.result.as_ref().unwrap()["document"];
    for field in [
        "placements",
        "count",
        "assets",
        "search",
        "engine",
        "created_at",
        "metrics",
    ] {
        assert_eq!(restored[field], document[field], "{field}");
    }
    let outputs = root.path().join("outputs");
    std::fs::create_dir(&outputs).unwrap();
    core.export_to(&outputs, "stl").unwrap();
    let exported = wait(&core);
    assert!(exported.last_error.is_none(), "{exported:?}");
    let bundle = std::fs::read_dir(outputs)
        .unwrap()
        .next()
        .unwrap()
        .unwrap()
        .path();
    let stl = std::fs::read(bundle.join("packed.stl")).unwrap();
    let triangles = u32::from_le_bytes(stl[80..84].try_into().unwrap());
    assert_eq!(triangles as u64, document["count"].as_u64().unwrap() * 12);
    assert!(bundle.join("packed.stl.json").is_file());
    assert_eq!(std::fs::read(moved).unwrap(), bytes);
}

#[test]
fn failed_open_preserves_previous_native_object_result_and_preview() {
    let (root, core, source) = fixture();
    import(&core, &source);
    core.start(settings(0.2)).unwrap();
    let previous = wait(&core);
    assert!(previous.last_error.is_none(), "{previous:?}");
    let broken = root.path().join("truncated.spectrapack");
    std::fs::write(&broken, b"PK\x03\x04truncated").unwrap();
    core.open_path(&broken).unwrap();
    let failed = wait(&core);
    assert!(failed.last_error.is_some());
    assert_eq!(failed.object, previous.object);
    assert_eq!(failed.result, previous.result);
    let preview = &failed.result.unwrap()["preview"];
    assert!(core
        .read_preview(preview["preview_id"].as_str().unwrap())
        .is_ok());
}

#[test]
fn stop_during_preparation_preserves_previous_complete_result() {
    let (_root, core, source) = fixture();
    import(&core, &source);
    core.start(settings(0.2)).unwrap();
    let previous = wait(&core);
    assert!(previous.last_error.is_none(), "{previous:?}");
    let receipt = core.start(settings(30.0)).unwrap();
    let started = Instant::now();
    core.stop(&receipt.operation_id).unwrap();
    assert!(started.elapsed() < Duration::from_millis(250));
    let state = wait(&core);
    assert!(state.last_error.is_none(), "{state:?}");
    assert_eq!(state.result, previous.result);
}

#[cfg(windows)]
fn native_child_of(parent: u32) -> Option<u32> {
    use windows_sys::Win32::{
        Foundation::{CloseHandle, INVALID_HANDLE_VALUE},
        System::Diagnostics::ToolHelp::*,
    };
    unsafe {
        let snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        assert_ne!(snapshot, INVALID_HANDLE_VALUE);
        let mut entry: PROCESSENTRY32W = std::mem::zeroed();
        entry.dwSize = std::mem::size_of_val(&entry) as u32;
        let mut more = Process32FirstW(snapshot, &mut entry);
        let mut found = None;
        while more != 0 {
            let end = entry
                .szExeFile
                .iter()
                .position(|v| *v == 0)
                .unwrap_or(entry.szExeFile.len());
            if entry.th32ParentProcessID == parent
                && String::from_utf16_lossy(&entry.szExeFile[..end]) == "spectrapack-engine.exe"
            {
                found = Some(entry.th32ProcessID);
                break;
            }
            more = Process32NextW(snapshot, &mut entry);
        }
        CloseHandle(snapshot);
        found
    }
}

// A separate real process is needed to exercise OS handle closure on application shutdown.
#[cfg(windows)]
#[test]
#[ignore = "subprocess fixture invoked by application_shutdown_kills_active_native_child"]
fn application_shutdown_child_helper() {
    let (_root, core, source) = fixture();
    import(&core, &source);
    let mut long = settings(120.0);
    long["box_dimensions_mm"] = json!([400, 400, 400]);
    long["pitch_mm"] = 1.into();
    core.start(long).unwrap();
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        assert!(core.state().last_error.is_none(), "{:?}", core.state());
        if core.state().operation.as_ref().unwrap().phase == "running" {
            if let Some(pid) = native_child_of(std::process::id()) {
                std::thread::sleep(Duration::from_millis(500));
                println!("ACTIVE_NATIVE_PID={pid}");
                use std::io::Write;
                std::io::stdout().flush().unwrap();
                loop {
                    std::thread::park();
                }
            }
        }
        assert!(Instant::now() < deadline);
        std::thread::sleep(Duration::from_millis(20));
    }
}

#[cfg(windows)]
#[test]
fn application_shutdown_kills_active_native_child() {
    use std::io::BufRead;
    use windows_sys::Win32::{
        Foundation::{CloseHandle, WAIT_OBJECT_0},
        System::Threading::*,
    };
    let mut app = std::process::Command::new(std::env::current_exe().unwrap())
        .args([
            "--ignored",
            "--exact",
            "application_shutdown_child_helper",
            "--nocapture",
        ])
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::inherit())
        .spawn()
        .unwrap();
    let lines = std::io::BufReader::new(app.stdout.take().unwrap()).lines();
    let mut pid = None;
    for line in lines {
        let line = line.unwrap();
        if let Some(text) = line.strip_prefix("ACTIVE_NATIVE_PID=") {
            pid = Some(text.parse::<u32>().unwrap());
            break;
        }
    }
    let pid = pid.expect("helper must start a real native solve before application close");
    let process =
        unsafe { OpenProcess(PROCESS_SYNCHRONIZE | PROCESS_TERMINATE, false.into(), pid) };
    assert!(!process.is_null());
    app.kill().unwrap();
    app.wait().unwrap();
    let closed = unsafe { WaitForSingleObject(process, 5000) } == WAIT_OBJECT_0;
    // Clean up even for the intended red run so it cannot leave an unmanaged test engine.
    unsafe {
        if !closed {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 5000);
        }
        CloseHandle(process);
    }
    assert!(
        closed,
        "closing the desktop owner must terminate its active native child"
    );
}
