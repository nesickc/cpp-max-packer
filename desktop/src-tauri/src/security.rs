use crate::model::{error, Outcome};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
};

pub fn regular_file(path: &Path) -> Outcome<()> {
    let metadata = fs::symlink_metadata(path).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
    if !metadata.is_file() || metadata.file_type().is_symlink() || is_reparse(&metadata) {
        return Err(error(
            "PATH_INVALID",
            "Expected a regular file without link or reparse redirection.",
        ));
    }
    Ok(())
}

#[cfg(windows)]
pub fn is_reparse(metadata: &fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt;
    metadata.file_attributes() & 0x400 != 0
}
#[cfg(not(windows))]
pub fn is_reparse(_: &fs::Metadata) -> bool {
    false
}

pub fn directory(path: &Path) -> Outcome<()> {
    let mut cursor = Some(path);
    while let Some(part) = cursor {
        if part.as_os_str().is_empty() {
            break;
        }
        let metadata =
            fs::symlink_metadata(part).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
        if !metadata.is_dir() || metadata.file_type().is_symlink() || is_reparse(&metadata) {
            return Err(error(
                "PATH_INVALID",
                "Directory ancestry contains a link or reparse point.",
            ));
        }
        cursor = part.parent();
    }
    Ok(())
}

pub fn scoped(root: &Path, path: &Path) -> Outcome<PathBuf> {
    directory(root)?;
    let root = fs::canonicalize(root).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
    let target = fs::canonicalize(path).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
    if !target.starts_with(&root) {
        return Err(error(
            "PATH_INVALID",
            "Artifact escapes its private directory.",
        ));
    }
    regular_file(path)?;
    directory(
        path.parent()
            .ok_or_else(|| error("PATH_INVALID", "Artifact has no parent."))?,
    )?;
    Ok(target)
}

pub fn read(path: &Path, limit: u64) -> Outcome<Vec<u8>> {
    regular_file(path)?;
    directory(
        path.parent()
            .ok_or_else(|| error("PATH_INVALID", "File has no parent."))?,
    )?;
    let mut bytes = Vec::new();
    fs::File::open(path)
        .map_err(|e| error("FILE_ACCESS", e.to_string()))?
        .take(limit + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| error("FILE_ACCESS", e.to_string()))?;
    if bytes.len() as u64 > limit {
        return Err(error(
            "MEMORY_LIMIT",
            "File exceeds desktop admission limit.",
        ));
    }
    Ok(bytes)
}

pub fn json(path: &Path, limit: u64) -> Outcome<serde_json::Value> {
    serde_json::from_slice(&read(path, limit)?)
        .map_err(|e| error("INVALID_DOCUMENT", e.to_string()))
}

pub fn hash(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

pub fn aliases(a: &Path, b: &Path) -> Outcome<bool> {
    if !a.exists() || !b.exists() {
        return Ok(false);
    }
    if fs::canonicalize(a).map_err(|e| error("FILE_ACCESS", e.to_string()))?
        == fs::canonicalize(b).map_err(|e| error("FILE_ACCESS", e.to_string()))?
    {
        return Ok(true);
    }
    #[cfg(windows)]
    {
        use std::os::windows::io::AsRawHandle;
        use windows_sys::Win32::Storage::FileSystem::{
            GetFileInformationByHandle, BY_HANDLE_FILE_INFORMATION,
        };
        let identity = |path: &Path| -> Outcome<(u32, u32, u32)> {
            let file = fs::File::open(path).map_err(|e| error("FILE_ACCESS", e.to_string()))?;
            let mut info: BY_HANDLE_FILE_INFORMATION = unsafe { std::mem::zeroed() };
            if unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut info) } == 0 {
                return Err(error("FILE_ACCESS", "Unable to verify file identity."));
            }
            Ok((
                info.dwVolumeSerialNumber,
                info.nFileIndexHigh,
                info.nFileIndexLow,
            ))
        };
        return Ok(identity(a)? == identity(b)?);
    }
    #[cfg(not(windows))]
    {
        Ok(false)
    }
}

pub fn copy(source: &Path, target: &Path, limit: u64) -> Outcome<()> {
    regular_file(source)?;
    directory(
        source
            .parent()
            .ok_or_else(|| error("PATH_INVALID", "Source has no parent."))?,
    )?;
    directory(
        target
            .parent()
            .ok_or_else(|| error("PATH_INVALID", "Output has no parent."))?,
    )?;
    let mut input = fs::File::open(source)
        .map_err(|e| error("FILE_ACCESS", e.to_string()))?
        .take(limit + 1);
    let mut output = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(target)
        .map_err(|e| error("OUTPUT_WRITE_FAILED", e.to_string()))?;
    let count = std::io::copy(&mut input, &mut output)
        .map_err(|e| error("OUTPUT_WRITE_FAILED", e.to_string()))?;
    if count > limit {
        return Err(error("MEMORY_LIMIT", "Source exceeds admission limit."));
    }
    output
        .flush()
        .map_err(|e| error("OUTPUT_WRITE_FAILED", e.to_string()))?;
    Ok(())
}
