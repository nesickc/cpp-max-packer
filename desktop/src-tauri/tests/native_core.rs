use serde_json::{json, Value};
use spectrapack_desktop::{
    core::Core,
    model::{ImportRequest, State},
};
use std::{
    path::PathBuf,
    time::{Duration, Instant},
};

#[test]
fn native_result_decimals_survive_desktop_json_roundtrip() {
    // AT-14: decimals from the real Ulamok native snapshot that previously
    // changed by one ULP while Save parsed and rewrote the result.
    let decimals = [
        "90.46014404296875",
        "105.50743865966797",
        "-1.5022964477539062",
        "105.69591522216797",
        "143.96210350036623",
    ];
    let root = tempfile::tempdir().unwrap();
    let file = root.path().join("result.json");
    std::fs::write(&file, format!("[{}]", decimals.join(","))).unwrap();
    let parsed = spectrapack_desktop::security::json(&file, 1024).unwrap();
    let saved = serde_json::to_vec(&parsed).unwrap();
    let restored: Vec<f64> = serde_json::from_slice(&saved).unwrap();
    for (decimal, actual) in decimals.iter().zip(restored) {
        let native = decimal.parse::<f64>().unwrap();
        assert_eq!(actual.to_bits(), native.to_bits(), "{decimal}");
    }
}

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
    let deadline = Instant::now()
        + Duration::from_secs(
            if std::env::var_os("SPECTRAPACK_JOURNEY_REQUEST").is_some() {
                1800
            } else {
                60
            },
        );
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

fn wait_native_work(core: &Core, operation_id: &str) {
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let state = core.state();
        assert!(state.last_error.is_none(), "{state:?}");
        let operation = state.operation.as_ref().unwrap();
        assert_eq!(operation.id, operation_id);
        assert!(
            operation.finished_at.is_none(),
            "solve finished before native work was observed"
        );
        assert!(operation.native_completion_elapsed_seconds.is_none());
        if ["placing", "validating"].contains(&operation.phase.as_str())
            && operation.sequence.is_some_and(|seq| seq > 0)
        {
            eprintln!("active native phase: {}, sequence={:?}", operation.phase, operation.sequence);
            return;
        }
        assert!(Instant::now() < deadline, "native work phase not observed");
        std::thread::sleep(Duration::from_millis(20));
    }
}

fn assert_active_solve(core: &Core, operation_id: &str) {
    let state = core.state();
    assert!(state.last_error.is_none(), "{state:?}");
    let operation = state.operation.as_ref().unwrap();
    assert_eq!(operation.id, operation_id);
    assert!(operation.native_completion_elapsed_seconds.is_none());
    assert!(
        operation.finished_at.is_none(),
        "solve finished before lifecycle action"
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
    // T011-A4/A5, SOL-06: native baseline work emits placing/validating.
    wait_native_work(&core, &receipt.operation_id);
    std::thread::sleep(Duration::from_millis(500));
    assert_active_solve(&core, &receipt.operation_id);
    let started = Instant::now();
    let response = core.stop(&receipt.operation_id).unwrap();
    let receipt_elapsed = started.elapsed();
    assert!(receipt_elapsed < Duration::from_millis(250));
    assert!(response.accepted);
    assert_eq!(core.state().operation.as_ref().unwrap().phase, "stopping");
    assert!(core.stop(&receipt.operation_id).is_ok());
    assert!(core.stop("stale").is_err());
    let deadline = started + Duration::from_secs(5);
    let state = loop {
        let state = core.state();
        if state.operation.as_ref().unwrap().finished_at.is_some() {
            break state;
        }
        assert!(
            Instant::now() < deadline,
            "safe Stop must complete within 5 seconds"
        );
        std::thread::sleep(Duration::from_millis(20));
    };
    assert!(started.elapsed() <= Duration::from_secs(5));
    assert!(state.last_error.is_none(), "{state:?}");
    let result = state.result.unwrap();
    assert_eq!(result["document"]["validation"]["status"], "valid");
    let count = result["document"]["count"].as_u64().unwrap();
    assert!(
        count > 0,
        "Stop must retain a nonempty native-valid incumbent"
    );
    assert_eq!(
        result["document"]["placements"].as_array().unwrap().len() as u64,
        count
    );
    eprintln!("native Stop: receipt={receipt_elapsed:?}, completion={:?}, valid_count={count}", started.elapsed());
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
    let request = std::env::var_os("SPECTRAPACK_JOURNEY_REQUEST")
        .map(|path| serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap())
        .unwrap_or_else(|| settings(0.2));
    project_roundtrip(request, false, true);
}

#[test]
fn retained_resource_failure_survives_save_move_open_and_exports() {
    // T009-A1/A3: real unsupported field work keeps a positive analytic incumbent.
    let mut request = settings(1.0);
    request["box_dimensions_mm"] = json!([20, 10, 10]);
    request["pitch_mm"] = json!(1e-9);
    project_roundtrip(request, true, true);
}

#[test]
#[ignore = "requires the pinned full Ulamok STL and a real native engine"]
fn ulamok_retained_resource_json_project_roundtrip() {
    assert_eq!(project_roundtrip(ulamok_request(), true, false), 36);
}

#[test]
#[ignore = "T010 pending: full36 checked STL hits EXPORT_IMPORT_WORK_LIMIT"]
fn ulamok_retained_resource_checked_stl_qualification() {
    assert_eq!(project_roundtrip(ulamok_request(), true, true), 36);
}

fn ulamok_request() -> Value {
    // AT-14 / T009-A6: the actual GUI regression, including rotated poses.
    let source = PathBuf::from(std::env::var_os("SPECTRAPACK_TEST_STL").unwrap());
    assert_eq!(
        spectrapack_desktop::security::hash(&std::fs::read(source).unwrap()),
        "39bcc1c3a5849176fc83473ce191ec5106854ea68836aed412c7c93b11b5bb7e"
    );
    json!({"desktop_version":1,"box_dimensions_mm":[400,350,285],
        "clearance_mm":{"pair":0.1,"wall":1},"orientation":{"mode":"cube"},
        "pitch_mm":1,"budget_seconds":600,"seed":"0"})
}

fn project_roundtrip(request: Value, expect_failure: bool, checked_stl: bool) -> u64 {
    let (root, core, source) = fixture();
    let source_bytes = std::fs::read(&source).unwrap();
    import(&core, &source);
    let started = Instant::now();
    core.start(request).unwrap();
    let state = wait(&core);
    let start_seconds = started.elapsed().as_secs_f64();
    let document = state.result.as_ref().unwrap()["document"].clone();
    let minimum: u64 = std::env::var("SPECTRAPACK_JOURNEY_MIN_COUNT")
        .unwrap_or_else(|_| "1".into())
        .parse()
        .unwrap();
    assert!(document["count"].as_u64().unwrap() >= minimum, "{document}");
    if expect_failure {
        let error = state
            .last_error
            .as_ref()
            .expect("resource failure required");
        assert_eq!(error.code, "RESOURCE_LIMIT");
        assert_eq!(document["metrics"]["termination_reason"], "resource_limit");
        let failure = &error.details["failure"];
        assert!(failure["cause_code"]
            .as_str()
            .is_some_and(|code| !code.is_empty()));
        assert!(failure["phase"]
            .as_str()
            .is_some_and(|phase| !phase.is_empty()));
        assert!(failure["resource"]["required"].as_str().is_some());
        assert!(failure["resource"]["limit"].as_str().is_some());
        assert_eq!(document["search"]["diagnostics"]["failure"], *failure);
        assert_eq!(
            document["search"]["run_segments"][0]["diagnostics"]["failure"],
            *failure
        );
    } else {
        assert!(state.last_error.is_none(), "{state:?}");
    }
    let evidence = std::env::var_os("SPECTRAPACK_JOURNEY_OUTPUT").map(PathBuf::from);
    let destination = evidence.as_deref().unwrap_or(root.path());
    std::fs::create_dir_all(destination).unwrap();
    if evidence.is_some() {
        std::fs::write(
            destination.join("document.json"),
            serde_json::to_vec_pretty(&document).unwrap(),
        )
        .unwrap();
        std::fs::write(
            destination.join("start-seconds.json"),
            start_seconds.to_string(),
        )
        .unwrap();
    }
    let mut pending = settings(2.0);
    pending["box_dimensions_mm"] = json!([60, 60, 60]);
    let archive = destination.join("saved.spectrapack");
    core.save_to(&archive, pending.clone()).unwrap();
    let state = wait(&core);
    assert!(state.last_error.is_none(), "{state:?}");
    let bytes = std::fs::read(&archive).unwrap();
    assert_eq!(&bytes[..2], b"PK");
    let directory = destination.join("перенос with spaces");
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
    let outputs = destination.join("exports");
    std::fs::create_dir(&outputs).unwrap();
    core.export_to(&outputs, "json").unwrap();
    let exported = wait(&core);
    assert!(exported.last_error.is_none(), "{exported:?}");
    if checked_stl {
        core.export_to(&outputs, "stl").unwrap();
        let exported = wait(&core);
        assert!(exported.last_error.is_none(), "{exported:?}");
    }
    let mut stl_count = 0;
    for entry in std::fs::read_dir(outputs).unwrap() {
        let bundle = entry.unwrap().path();
        let exported: Value =
            serde_json::from_slice(&std::fs::read(bundle.join("result.json")).unwrap()).unwrap();
        for field in ["search", "metrics", "placements", "count"] {
            assert_eq!(exported[field], document[field], "{field}");
        }
        if let Ok(stl) = std::fs::read(bundle.join("packed.stl")) {
            stl_count += 1;
            let triangles = u32::from_le_bytes(stl[80..84].try_into().unwrap());
            let per_copy = document["assets"]["object"]["accepted_solid"]["triangle_count"]
                .as_u64()
                .unwrap();
            assert_eq!(
                triangles as u64,
                document["count"].as_u64().unwrap() * per_copy
            );
            assert!(bundle.join("packed.stl.json").is_file());
        }
    }
    if checked_stl {
        assert_eq!(stl_count, 1, "the requested checked STL must exist");
    }
    assert_eq!(std::fs::read(moved).unwrap(), bytes);
    assert_eq!(std::fs::read(source).unwrap(), source_bytes);
    document["count"].as_u64().unwrap()
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
    let receipt = core.start(long).unwrap();
    wait_native_work(&core, &receipt.operation_id);
    std::thread::sleep(Duration::from_millis(500));
    assert_active_solve(&core, &receipt.operation_id);
    let pid = native_child_of(std::process::id()).expect("active real native child required");
    println!("ACTIVE_NATIVE_PID={pid}");
    use std::io::Write;
    std::io::stdout().flush().unwrap();
    loop {
        std::thread::park();
    }
}

#[cfg(windows)]
#[test]
fn application_shutdown_kills_active_native_child() {
    use std::io::BufRead;
    use windows_sys::Win32::{
        Foundation::{CloseHandle, WAIT_OBJECT_0, WAIT_TIMEOUT},
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
    let was_active = unsafe { WaitForSingleObject(process, 0) } == WAIT_TIMEOUT;
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
    assert!(was_active, "native child must still be active immediately before owner exit");
    assert!(
        closed,
        "closing the desktop owner must terminate its active native child"
    );
    eprintln!("native shutdown: child_pid={pid}, exited_within_5s={closed}");
}
