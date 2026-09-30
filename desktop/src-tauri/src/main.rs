#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use spectrapack_desktop::{core::Core, model::*, security};
use tauri::{Manager, State as Managed};

#[tauri::command]
fn desktop_state(core: Managed<'_, Core>) -> State {
    core.state()
}

#[tauri::command]
async fn desktop_import_object(
    core: Managed<'_, Core>,
    request: ImportRequest,
) -> Outcome<Option<Receipt>> {
    validate_import(&request)?;
    let path = rfd::AsyncFileDialog::new()
        .add_filter("STL object", &["stl"])
        .pick_file()
        .await;
    path.map(|file| core.import_path(file.path(), request))
        .transpose()
}
#[tauri::command]
fn desktop_start(core: Managed<'_, Core>, request: StartRequest) -> Outcome<Receipt> {
    core.start(request.settings)
}
#[tauri::command]
fn desktop_stop(core: Managed<'_, Core>, request: StopRequest) -> Outcome<StopResponse> {
    core.stop(&request.operation_id)
}
#[tauri::command]
async fn desktop_save_project(
    core: Managed<'_, Core>,
    request: SaveRequest,
) -> Outcome<Option<Receipt>> {
    validate_settings(&request.settings)?;
    let path = rfd::AsyncFileDialog::new()
        .add_filter("SpectraPack project", &["spectrapack"])
        .set_file_name("project.spectrapack")
        .save_file()
        .await;
    path.map(|file| core.save_to(file.path(), request.settings))
        .transpose()
}
#[tauri::command]
async fn desktop_open_project(core: Managed<'_, Core>) -> Outcome<Option<Receipt>> {
    let path = rfd::AsyncFileDialog::new()
        .add_filter("SpectraPack project", &["spectrapack"])
        .pick_file()
        .await;
    path.map(|file| core.open_path(file.path())).transpose()
}
#[tauri::command]
async fn desktop_export(
    core: Managed<'_, Core>,
    request: ExportRequest,
) -> Outcome<Option<Receipt>> {
    if request.format != "json" && request.format != "stl" {
        return Err(error("INVALID_REQUEST", "Unknown export format."));
    }
    let path = rfd::AsyncFileDialog::new()
        .set_title("Choose output folder")
        .pick_folder()
        .await;
    path.map(|file| core.export_to(file.path(), &request.format))
        .transpose()
}
#[tauri::command]
fn desktop_new(core: Managed<'_, Core>) -> Outcome<State> {
    core.new_project()
}
#[tauri::command]
fn desktop_read_preview(
    core: Managed<'_, Core>,
    request: PreviewRequest,
) -> Outcome<tauri::ipc::Response> {
    core.read_preview(&request.preview_id)
        .map(tauri::ipc::Response::new)
}

fn startup_error(failure: impl std::fmt::Display) {
    rfd::MessageDialog::new()
        .set_title("SpectraPack could not start")
        .set_description(format!("Bundled engine or desktop setup failed: {failure}"))
        .set_level(rfd::MessageLevel::Error)
        .show();
}

fn bundled_engine() -> Outcome<std::path::PathBuf> {
    // The configured external binary is fixed beside the application. Verify it
    // before creating a WebView/window; setup callback errors otherwise panic
    // in Tauri's event loop before Builder::run can return an Err.
    let executable = std::env::current_exe().map_err(|e| error("ENGINE_MISSING", e.to_string()))?;
    let engine = executable
        .parent()
        .ok_or_else(|| error("ENGINE_MISSING", "Application executable has no parent."))?
        .join("spectrapack-engine.exe");
    let bytes = security::read(&engine, 256 << 20)?;
    if security::hash(&bytes) != env!("SPECTRAPACK_ENGINE_SHA256") {
        return Err(error(
            "ENGINE_MISMATCH",
            "Bundled engine bytes differ from the staged build.",
        ));
    }
    Ok(engine)
}

fn setup_desktop(app: &mut tauri::App, engine: std::path::PathBuf) -> Outcome<()> {
    let sessions = app
        .path()
        .app_local_data_dir()
        .map_err(|e| error("FILE_ACCESS", e.to_string()))?
        .join("sessions");
    std::fs::create_dir_all(&sessions).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
    let core = Core::new(engine, sessions.join(id("session")))?;
    app.manage(core);
    app.get_webview_window("main")
        .ok_or_else(|| error("DESKTOP_START", "Desktop window is unavailable."))?
        .show()
        .map_err(|e| error("DESKTOP_START", e.to_string()))
}

fn main() {
    let engine = match bundled_engine() {
        Ok(engine) => engine,
        Err(failure) => {
            startup_error(failure);
            return;
        }
    };
    let outcome = tauri::Builder::default()
        .setup(move |app| {
            if let Err(failure) = setup_desktop(app, engine) {
                startup_error(failure);
                // The initially hidden window is never shown on expected setup
                // failure. Request a clean event-loop exit instead of returning
                // an Err that Tauri would convert into a callback panic.
                app.handle().exit(1);
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            desktop_state,
            desktop_import_object,
            desktop_start,
            desktop_stop,
            desktop_save_project,
            desktop_open_project,
            desktop_export,
            desktop_new,
            desktop_read_preview
        ])
        .run(tauri::generate_context!());
    if let Err(failure) = outcome {
        startup_error(failure);
    }
}
