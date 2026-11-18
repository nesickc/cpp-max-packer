//! libFuzzer entry point for opening a portable project (`archive::open`).
//! tests/fuzz/CMakeLists.txt builds this as a static library and links it into
//! the `fuzz_project_archive` executable of the windows-ninja-fuzz preset.
use spectrapack_desktop::archive;
use std::{fs, path::Path, sync::OnceLock};

extern "C" {
    // The C runtime's abort: libFuzzer catches it and keeps the input.
    fn abort() -> !;
    fn atexit(callback: extern "C" fn()) -> i32;
}

static ROOT: OnceLock<tempfile::TempDir> = OnceLock::new();

// A finished run leaves nothing behind; an aborted one keeps its last extraction.
extern "C" fn remove_root() {
    if let Some(root) = ROOT.get() {
        let _ = fs::remove_dir_all(root.path());
    }
}

fn finding(message: &str) -> ! {
    eprintln!("fuzz invariant failed: {message}");
    unsafe { abort() }
}

fn check(archive_bytes: &[u8]) {
    let root = ROOT
        .get_or_init(|| {
            unsafe { atexit(remove_root) };
            tempfile::tempdir().expect("temporary directory")
        })
        .path();
    let input = root.join("input.spectrapack");
    let destination = root.join("project");
    if destination.exists() {
        fs::remove_dir_all(&destination).expect("remove the previous extraction");
    }
    fs::create_dir(&destination).expect("create the extraction directory");
    fs::write(&input, archive_bytes).expect("write the archive");

    if let Ok(manifest) = archive::open(&input, &destination) {
        // An opened project is exactly its manifest: every listed file, nothing else.
        let files = manifest["files"].as_array().expect("manifest files");
        for file in files {
            let path = destination.join(file["path"].as_str().expect("file path"));
            let size = fs::metadata(&path).map(|metadata| metadata.len()).ok();
            if size != file["size_bytes"].as_u64() {
                finding("an extracted file differs from its manifest size");
            }
        }
        if count_files(&destination) != files.len() + 1 {
            finding("extraction wrote a file the manifest does not list");
        }
    }
    // Opened or refused, nothing is written beside the extraction directory.
    if fs::read_dir(root).expect("list the root").count() != 2 {
        finding("open wrote outside its extraction directory");
    }
}

fn count_files(directory: &Path) -> usize {
    fs::read_dir(directory)
        .expect("list the extraction")
        .map(|entry| entry.expect("extraction entry").path())
        .map(|path| if path.is_dir() { count_files(&path) } else { 1 })
        .sum()
}

/// # Safety
/// libFuzzer passes `size` readable bytes at `data`.
#[no_mangle]
pub unsafe extern "C" fn LLVMFuzzerTestOneInput(data: *const u8, size: usize) -> i32 {
    let bytes = if size == 0 {
        &[][..]
    } else {
        std::slice::from_raw_parts(data, size)
    };
    if std::panic::catch_unwind(|| check(bytes)).is_err() {
        finding("opening the archive panicked");
    }
    0
}
