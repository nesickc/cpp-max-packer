//! Version-one portable transport. Native preparation/restore remains the validity authority.
use crate::{
    model::{error, now, validate_settings, DesktopError, Outcome},
    security,
};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs::{self, File, OpenOptions},
    io::{Read, Seek, SeekFrom, Write},
    path::Path,
};
use zip::{write::SimpleFileOptions, CompressionMethod, ZipArchive, ZipWriter};

const MANIFEST_LIMIT: u64 = 1 << 20;
const TOTAL_LIMIT: u64 = 2 << 30;
const ENTRY_LIMIT: usize = 1024; // Includes project.json, which is also a regular entry.
const REPORT_LIMIT: u64 = 16 << 20;
const RESULT_LIMIT: u64 = 64 << 20;

fn invalid(message: impl Into<String>) -> DesktopError {
    error("ARCHIVE_INVALID", message)
}
fn io_error(e: impl std::fmt::Display) -> DesktopError {
    invalid(e.to_string())
}
fn limit() -> DesktopError {
    error(
        "MEMORY_LIMIT",
        "Portable project exceeds an admission limit.",
    )
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct ProjectFile {
    path: String,
    sha256: String,
    size_bytes: u64,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Manifest {
    project_version: u32,
    format: String,
    saved_at: String,
    object_report: String,
    resolved_settings: Option<String>,
    best_result: Option<String>,
    draft_settings: Value,
    files: Vec<ProjectFile>,
}

fn valid_hash(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn portable(path: &str) -> Outcome<()> {
    if path.is_empty()
        || path.starts_with('/')
        || path.contains(['\\', ':', '\0'])
        || path
            .chars()
            .any(|c| c.is_control() || "<>\"|?*".contains(c))
    {
        return Err(invalid(
            "Entry path is not a portable Windows-relative path.",
        ));
    }
    for part in path.split('/') {
        if part.is_empty() || part == "." || part == ".." || part.ends_with(['.', ' ']) {
            return Err(invalid(
                "Entry path has an empty, dot or trailing-dot/space segment.",
            ));
        }
        let base = part
            .split('.')
            .next()
            .unwrap()
            .trim_end_matches(' ')
            .to_uppercase();
        if matches!(
            base.as_str(),
            "CON" | "PRN" | "AUX" | "NUL" | "CLOCK$" | "CONIN$" | "CONOUT$"
        ) || ["COM", "LPT"].iter().any(|prefix| {
            base.strip_prefix(prefix).is_some_and(|suffix| {
                matches!(
                    suffix,
                    "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" | "¹" | "²" | "³"
                )
            })
        }) {
            return Err(invalid("Entry path contains a Windows device name."));
        }
    }
    Ok(())
}

fn asset_hash(path: &str) -> Outcome<&str> {
    portable(path)?;
    let name = path
        .strip_prefix("assets/")
        .ok_or_else(|| invalid("Dependency is outside assets/."))?;
    if name.len() < 68
        || !name.is_ascii()
        || name.contains('/')
        || !valid_hash(&name[..64])
        || !matches!(&name[64..], ".stl" | ".ply" | ".repair.json" | ".json")
    {
        return Err(invalid(
            "Dependency must use a content-addressed asset filename.",
        ));
    }
    Ok(&name[..64])
}

fn document_limit(path: &str) -> u64 {
    match path {
        "result.json" => RESULT_LIMIT,
        "settings.json" | "project.json" => MANIFEST_LIMIT,
        _ => REPORT_LIMIT,
    }
}

fn validate_manifest(manifest: &Manifest) -> Outcome<BTreeMap<String, ProjectFile>> {
    if manifest.project_version != 1 || manifest.format != "spectrapack-project" {
        return Err(error(
            "UNSUPPORTED_VERSION",
            "Unsupported portable project version or format.",
        ));
    }
    time::OffsetDateTime::parse(
        &manifest.saved_at,
        &time::format_description::well_known::Rfc3339,
    )
    .map_err(|_| invalid("Manifest saved_at is not RFC3339."))?;
    validate_settings(&manifest.draft_settings)?;
    let has_result = manifest.best_result.as_deref() == Some("result.json");
    if manifest.object_report != "object.report.json"
        || manifest.best_result.is_some() != has_result
        || manifest.resolved_settings.as_deref()
            != if has_result {
                Some("settings.json")
            } else {
                None
            }
    {
        return Err(invalid(
            "Manifest snapshot filenames or result/settings pairing is invalid.",
        ));
    }
    if manifest.files.is_empty() || manifest.files.len() + 1 > ENTRY_LIMIT {
        return Err(limit());
    }
    let mut files = BTreeMap::new();
    let mut names = BTreeSet::new();
    let mut hashes = BTreeSet::new();
    let mut size = 0_u64;
    for record in &manifest.files {
        portable(&record.path)?;
        if !valid_hash(&record.sha256) || !names.insert(record.path.to_uppercase()) {
            return Err(invalid(
                "Manifest has invalid hashes or duplicate entry paths.",
            ));
        }
        match record.path.as_str() {
            "object.report.json" => (),
            "result.json" | "settings.json" if has_result => (),
            path => {
                if asset_hash(path)? != record.sha256 || !hashes.insert(record.sha256.clone()) {
                    return Err(invalid(
                        "Asset address differs from its hash or an asset is stored twice.",
                    ));
                }
            }
        }
        if record.path.ends_with(".json") && record.size_bytes > document_limit(&record.path) {
            return Err(limit());
        }
        size = size.checked_add(record.size_bytes).ok_or_else(limit)?;
        if size > TOTAL_LIMIT {
            return Err(limit());
        }
        files.insert(record.path.clone(), record.clone());
    }
    if !files.contains_key("object.report.json")
        || has_result
            && (!files.contains_key("result.json") || !files.contains_key("settings.json"))
    {
        return Err(invalid("Manifest omits a required snapshot file."));
    }
    Ok(files)
}

fn parse_manifest(bytes: &[u8]) -> Outcome<Manifest> {
    // Typed deserialization rejects duplicate keys as well as unknown fields. Nullable keys are required.
    let manifest: Manifest = serde_json::from_slice(bytes).map_err(io_error)?;
    let value: Value = serde_json::from_slice(bytes).map_err(io_error)?;
    if !["resolved_settings", "best_result"]
        .iter()
        .all(|key| value.get(key).is_some())
    {
        return Err(invalid("Manifest must include nullable snapshot keys."));
    }
    Ok(manifest)
}

fn stream(reader: &mut impl Read, writer: &mut impl Write, maximum: u64) -> Outcome<(u64, String)> {
    let mut total = 0_u64;
    let mut hash = Sha256::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let count = reader.read(&mut buffer).map_err(io_error)?;
        if count == 0 {
            break;
        }
        total = total.checked_add(count as u64).ok_or_else(limit)?;
        if total > maximum {
            return Err(limit());
        }
        hash.update(&buffer[..count]);
        writer.write_all(&buffer[..count]).map_err(io_error)?;
    }
    Ok((total, format!("{:x}", hash.finalize())))
}

fn inventory(root: &Path) -> Outcome<BTreeMap<String, ProjectFile>> {
    security::directory(root)?;
    let mut files = BTreeMap::new();
    let mut total = 0_u64;
    let mut queue = vec![(root.to_path_buf(), String::new())];
    while let Some((directory, prefix)) = queue.pop() {
        security::directory(&directory)?;
        for entry in fs::read_dir(&directory).map_err(io_error)? {
            let entry = entry.map_err(io_error)?;
            let name = entry
                .file_name()
                .into_string()
                .map_err(|_| invalid("Snapshot name is not UTF-8."))?;
            let relative = format!("{prefix}{name}");
            portable(&relative)?;
            let metadata = fs::symlink_metadata(entry.path()).map_err(io_error)?;
            if metadata.is_dir() && relative == "assets" && !security::is_reparse(&metadata) {
                queue.push((entry.path(), "assets/".into()));
                continue;
            }
            security::scoped(root, &entry.path())?;
            if files.len() + 2 > ENTRY_LIMIT || metadata.len() > TOTAL_LIMIT - total {
                return Err(limit());
            }
            let (size_bytes, sha256) = stream(
                &mut File::open(entry.path()).map_err(io_error)?,
                &mut std::io::sink(),
                TOTAL_LIMIT - total,
            )?;
            total += size_bytes;
            files.insert(
                relative.clone(),
                ProjectFile {
                    path: relative,
                    sha256,
                    size_bytes,
                },
            );
        }
    }
    Ok(files)
}

fn collect_references(value: &Value, refs: &mut BTreeMap<String, String>) -> Outcome<()> {
    match value {
        Value::Object(object) => {
            if let Some(path) = object.get("path") {
                let path = path
                    .as_str()
                    .ok_or_else(|| invalid("Dependency path is not text."))?;
                let hash = object
                    .get("sha256")
                    .and_then(Value::as_str)
                    .ok_or_else(|| invalid("Dependency reference has no SHA256."))?;
                if asset_hash(path)? != hash {
                    return Err(invalid(
                        "Dependency reference disagrees with its content address.",
                    ));
                }
                if refs
                    .insert(path.into(), hash.into())
                    .is_some_and(|old| old != hash)
                {
                    return Err(invalid("Conflicting dependency hashes."));
                }
            }
            if let Some(hash) = object.get("mesh_sha256") {
                let hash = hash
                    .as_str()
                    .filter(|hash| valid_hash(hash))
                    .ok_or_else(|| invalid("Repair mesh hash is invalid."))?;
                refs.insert(format!("assets/{hash}.ply"), hash.into());
            }
            for child in object.values() {
                collect_references(child, refs)?;
            }
        }
        Value::Array(array) => {
            for child in array {
                collect_references(child, refs)?;
            }
        }
        _ => (),
    }
    Ok(())
}

fn validate_graph(
    files: &BTreeMap<String, ProjectFile>,
    has_result: bool,
    mut load: impl FnMut(&str) -> Outcome<Value>,
) -> Outcome<()> {
    let report = load("object.report.json")?;
    if report["schema_version"] != 1
        || report["role"] != "object"
        || report["state"] != "accepted"
        || !report["source"].is_object()
        || !report["accepted_solid"].is_object()
    {
        return Err(invalid(
            "Project does not retain an accepted version-one object report.",
        ));
    }
    let mut refs = BTreeMap::new();
    collect_references(&report, &mut refs)?;
    if has_result {
        let result = load("result.json")?;
        let settings = load("settings.json")?;
        if result["schema_version"] != 1
            || settings["settings_version"] != 1
            || result["search"].get("resolved_settings") != Some(&settings)
        {
            return Err(error(
                "ASSET_MISMATCH",
                "Resolved settings differ from the saved result.",
            ));
        }
        if result["assets"]["object"] != report {
            return Err(error(
                "ASSET_MISMATCH",
                "Result object differs from its immutable inspection report.",
            ));
        }
        collect_references(&result, &mut refs)?;
    }
    let mut visited = BTreeSet::new();
    loop {
        let next = refs.keys().find(|path| !visited.contains(*path)).cloned();
        let Some(path) = next else { break };
        let record = files
            .get(&path)
            .ok_or_else(|| invalid("Dependency graph references an absent file."))?;
        if record.sha256 != refs[&path] {
            return Err(invalid("Dependency hash differs from the manifest."));
        }
        visited.insert(path.clone());
        if path.ends_with(".json") {
            collect_references(&load(&path)?, &mut refs)?;
        }
    }
    for path in files.keys() {
        if path.starts_with("assets/") && !visited.contains(path) {
            return Err(invalid("Project contains an unreferenced asset."));
        }
    }
    Ok(())
}

fn u16_at(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(bytes[offset..offset + 2].try_into().unwrap())
}
fn u32_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}
fn read_at(file: &mut File, offset: u64, size: usize) -> Outcome<Vec<u8>> {
    file.seek(SeekFrom::Start(offset)).map_err(io_error)?;
    let mut bytes = vec![0; size];
    file.read_exact(&mut bytes).map_err(io_error)?;
    Ok(bytes)
}
fn extra_fields(bytes: &[u8]) -> Outcome<()> {
    let mut offset = 0;
    while offset < bytes.len() {
        if bytes.len() - offset < 4 {
            return Err(invalid("Truncated ZIP extra field."));
        }
        let kind = u16_at(bytes, offset);
        let size = u16_at(bytes, offset + 2) as usize;
        if size > bytes.len() - offset - 4 {
            return Err(invalid("Truncated ZIP extra field data."));
        }
        // ZIP64, Unicode alternate paths, Unix hardlink metadata and AES are not part of v1 transport.
        if !matches!(kind, 0x5455 | 0x000a | 0x7875) {
            return Err(invalid("Unsupported ZIP extra field."));
        }
        offset += 4 + size;
    }
    Ok(())
}

// ZipArchive maps filenames to entries and silently drops exact duplicates. Check raw headers first,
// before its allocation, with a bounded central directory. ZIP64/multidisk are outside the v1 subset.
fn preflight(file: &mut File) -> Outcome<BTreeMap<String, u64>> {
    let length = file.metadata().map_err(io_error)?.len();
    if length > TOTAL_LIMIT {
        return Err(limit());
    }
    if length < 22 {
        return Err(invalid("Truncated ZIP archive."));
    }
    let tail_len = length.min(22 + 65535) as usize;
    let tail = read_at(file, length - tail_len as u64, tail_len)?;
    let end = (0..=tail.len() - 22)
        .rev()
        .find(|&i| {
            tail[i..i + 4] == [0x50, 0x4b, 0x05, 0x06]
                && i + 22 + u16_at(&tail, i + 20) as usize == tail.len()
        })
        .ok_or_else(|| invalid("Missing complete ZIP end record."))?;
    let eocd = &tail[end..];
    let end_offset = length - tail_len as u64 + end as u64;
    let count = u16_at(eocd, 10) as usize;
    let start = u32_at(eocd, 16) as u64;
    let size = u32_at(eocd, 12) as u64;
    if u16_at(eocd, 4) != 0
        || u16_at(eocd, 6) != 0
        || u16_at(eocd, 8) as usize != count
        || count == 65535
        || start == u32::MAX as u64
        || size == u32::MAX as u64
    {
        return Err(invalid("ZIP64 and multidisk archives are unsupported."));
    }
    if count == 0 || count > ENTRY_LIMIT || size > ENTRY_LIMIT as u64 * (46 + 3 * 65535) {
        return Err(limit());
    }
    if start.checked_add(size) != Some(end_offset) {
        return Err(invalid("ZIP central directory bounds are inconsistent."));
    }
    let mut cursor = start;
    let mut names = BTreeSet::new();
    let mut files = BTreeMap::new();
    let mut spans = Vec::new();
    let mut total = 0_u64;
    for _ in 0..count {
        if end_offset - cursor < 46 {
            return Err(invalid("Truncated central ZIP header."));
        }
        let central = read_at(file, cursor, 46)?;
        if central[..4] != [0x50, 0x4b, 0x01, 0x02] {
            return Err(invalid("Invalid central ZIP signature."));
        }
        let flags = u16_at(&central, 8);
        let method = u16_at(&central, 10);
        let compressed = u32_at(&central, 20) as u64;
        let expanded = u32_at(&central, 24) as u64;
        let name_len = u16_at(&central, 28) as usize;
        let extra_len = u16_at(&central, 30) as usize;
        let comment_len = u16_at(&central, 32) as u64;
        let local_offset = u32_at(&central, 42) as u64;
        let attrs = u32_at(&central, 38);
        let kind = (attrs >> 16) & 0o170000;
        if flags & !0x080e != 0
            || !matches!(method, 0 | 8)
            || u16_at(&central, 6) > 20
            || u16_at(&central, 34) != 0
            || (kind != 0 && kind != 0o100000)
            || attrs & 0x410 != 0
        {
            return Err(invalid("ZIP entry has unsupported flags, compression, encryption or link/reparse attributes."));
        }
        let next = cursor + 46 + name_len as u64 + extra_len as u64 + comment_len;
        if next > end_offset {
            return Err(invalid("Central ZIP entry extends outside its directory."));
        }
        let raw_name = read_at(file, cursor + 46, name_len)?;
        let name = std::str::from_utf8(&raw_name)
            .map_err(|_| invalid("ZIP entry names must be UTF-8."))?
            .to_owned();
        portable(&name)?;
        if !names.insert(name.to_uppercase()) {
            return Err(invalid("Duplicate or case-colliding ZIP entry."));
        }
        extra_fields(&read_at(file, cursor + 46 + name_len as u64, extra_len)?)?;
        total = total.checked_add(expanded).ok_or_else(limit)?;
        if total > TOTAL_LIMIT {
            return Err(limit());
        }
        if local_offset.checked_add(30).is_none_or(|end| end > start) {
            return Err(invalid("Local ZIP header escapes its data region."));
        }
        let local = read_at(file, local_offset, 30)?;
        if local[..4] != [0x50, 0x4b, 0x03, 0x04]
            || u16_at(&local, 4) != u16_at(&central, 6)
            || u16_at(&local, 6) != flags
            || u16_at(&local, 8) != method
            || u16_at(&local, 26) as usize != name_len
        {
            return Err(invalid("Local and central ZIP headers disagree."));
        }
        let local_extra = u16_at(&local, 28) as usize;
        let data_start = local_offset + 30 + name_len as u64 + local_extra as u64;
        let mut data_end = data_start.checked_add(compressed).ok_or_else(limit)?;
        if data_end > start || read_at(file, local_offset + 30, name_len)? != raw_name {
            return Err(invalid("ZIP data bounds or local name disagree."));
        }
        extra_fields(&read_at(
            file,
            local_offset + 30 + name_len as u64,
            local_extra,
        )?)?;
        if flags & 8 == 0 {
            if local[14..26] != central[16..28] {
                return Err(invalid(
                    "Local ZIP CRC/sizes disagree with central metadata.",
                ));
            }
        } else {
            if data_end + 12 > start {
                return Err(invalid("Truncated ZIP data descriptor."));
            }
            let first = read_at(file, data_end, 4)?;
            let signature = first == [0x50, 0x4b, 0x07, 0x08];
            let descriptor = read_at(file, data_end + if signature { 4 } else { 0 }, 12)?;
            if descriptor != central[16..28] {
                return Err(invalid(
                    "ZIP data descriptor disagrees with central metadata.",
                ));
            }
            data_end += if signature { 16 } else { 12 };
            if data_end > start {
                return Err(invalid("ZIP data descriptor escapes its region."));
            }
        }
        spans.push((local_offset, data_end));
        files.insert(name, expanded);
        cursor = next;
    }
    if cursor != end_offset {
        return Err(invalid("Unlisted central directory bytes."));
    }
    spans.sort_unstable();
    let mut position = 0;
    for (start, end) in spans {
        if start != position {
            return Err(invalid(
                "Overlapping, prefixed or unlisted local ZIP entries.",
            ));
        }
        position = end;
    }
    if position != start {
        return Err(invalid("Unlisted ZIP data before central directory."));
    }
    Ok(files)
}

fn zip_json(archive: &mut ZipArchive<File>, name: &str) -> Outcome<Value> {
    let mut entry = archive.by_name(name).map_err(io_error)?;
    let mut bytes = Vec::new();
    stream(&mut entry, &mut bytes, document_limit(name))?;
    serde_json::from_slice(&bytes).map_err(io_error)
}

fn scoped_output(root: &Path, path: &Path, file: &File) -> Outcome<()> {
    security::scoped(root, path)?;
    let metadata = file.metadata().map_err(io_error)?;
    if !metadata.is_file() || security::is_reparse(&metadata) {
        return Err(invalid("Extraction output is not a regular file."));
    }
    #[cfg(windows)]
    {
        use std::os::windows::{ffi::OsStringExt, io::AsRawHandle};
        use windows_sys::Win32::Storage::FileSystem::GetFinalPathNameByHandleW;
        let required =
            unsafe { GetFinalPathNameByHandleW(file.as_raw_handle(), std::ptr::null_mut(), 0, 0) };
        if required == 0 || required > 32768 {
            return Err(invalid("Cannot resolve extraction output handle."));
        }
        let mut buffer = vec![0_u16; required as usize + 1];
        let count = unsafe {
            GetFinalPathNameByHandleW(
                file.as_raw_handle(),
                buffer.as_mut_ptr(),
                buffer.len() as u32,
                0,
            )
        };
        if count == 0 || count as usize >= buffer.len() {
            return Err(invalid("Extraction handle path resolution failed."));
        }
        let actual =
            std::path::PathBuf::from(std::ffi::OsString::from_wide(&buffer[..count as usize]));
        if !actual.starts_with(root) {
            return Err(invalid(
                "Extraction output handle escapes the private directory.",
            ));
        }
    }
    Ok(())
}

fn check_archive(path: &Path, destination: Option<&Path>) -> Outcome<Value> {
    security::regular_file(path)?;
    security::directory(
        path.parent()
            .ok_or_else(|| invalid("Archive has no directory."))?,
    )?;
    let mut input = File::open(path).map_err(io_error)?;
    let entries = preflight(&mut input)?;
    let mut archive = ZipArchive::new(input).map_err(io_error)?;
    if entries.len() != archive.len() {
        return Err(invalid("ZIP metadata entry count changed."));
    }
    let mut entry = archive.by_name("project.json").map_err(io_error)?;
    let mut bytes = Vec::new();
    let (manifest_size, _) = stream(&mut entry, &mut bytes, MANIFEST_LIMIT)?;
    drop(entry);
    let manifest = parse_manifest(&bytes)?;
    let files = validate_manifest(&manifest)?;
    if entries.len() != files.len() + 1
        || entries.get("project.json") != Some(&manifest_size)
        || files
            .iter()
            .any(|(name, record)| entries.get(name) != Some(&record.size_bytes))
    {
        return Err(invalid(
            "ZIP entries differ from the manifest's complete file list.",
        ));
    }
    let mut total = manifest_size;
    for (name, record) in &files {
        let mut entry = archive.by_name(name).map_err(io_error)?;
        if entry.encrypted() || !entry.is_file() {
            return Err(invalid("Unsupported encrypted or nonregular entry."));
        }
        let mut output = if let Some(root) = destination {
            let path = root.join(name);
            let parent = path.parent().unwrap();
            if !parent.exists() {
                fs::create_dir(parent).map_err(io_error)?;
            }
            security::directory(parent)?;
            let output = OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&path)
                .map_err(io_error)?;
            scoped_output(root, &path, &output)?;
            Some(output)
        } else {
            None
        };
        let maximum = record.size_bytes.min(TOTAL_LIMIT - total);
        let (size, hash) = if let Some(output) = &mut output {
            stream(&mut entry, output, maximum)?
        } else {
            stream(&mut entry, &mut std::io::sink(), maximum)?
        };
        if size != record.size_bytes || hash != record.sha256 {
            return Err(invalid("Entry size or SHA256 does not match the manifest."));
        }
        if let Some(output) = &mut output {
            output.sync_all().map_err(io_error)?;
        }
        total += size;
        if let Some(root) = destination {
            security::scoped(root, &root.join(name))?;
        }
    }
    validate_graph(&files, manifest.best_result.is_some(), |name| {
        zip_json(&mut archive, name)
    })?;
    if let Some(root) = destination {
        let mut output = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(root.join("project.json"))
            .map_err(io_error)?;
        scoped_output(root, &root.join("project.json"), &output)?;
        output.write_all(&bytes).map_err(io_error)?;
        output.sync_all().map_err(io_error)?;
        security::scoped(root, &root.join("project.json"))?;
    }
    serde_json::to_value(manifest).map_err(io_error)
}

#[cfg(windows)]
fn replace(temporary: &Path, target: &Path) -> Outcome<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::Storage::FileSystem::{
        MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
    };
    let from: Vec<u16> = temporary.as_os_str().encode_wide().chain(Some(0)).collect();
    let to: Vec<u16> = target.as_os_str().encode_wide().chain(Some(0)).collect();
    if unsafe {
        MoveFileExW(
            from.as_ptr(),
            to.as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    } == 0
    {
        return Err(error(
            "OUTPUT_WRITE_FAILED",
            std::io::Error::last_os_error().to_string(),
        ));
    }
    Ok(())
}
#[cfg(not(windows))]
fn replace(temporary: &Path, target: &Path) -> Outcome<()> {
    fs::rename(temporary, target).map_err(io_error)
}

pub fn save(
    source_root: &Path,
    target: &Path,
    draft_settings: &Value,
    has_result: bool,
) -> Outcome<()> {
    save_internal(source_root, target, draft_settings, has_result, || Ok(()))
}
fn save_internal(
    source_root: &Path,
    target: &Path,
    draft_settings: &Value,
    has_result: bool,
    before_replace: impl FnOnce() -> Outcome<()>,
) -> Outcome<()> {
    validate_settings(draft_settings)?;
    let parent = target
        .parent()
        .ok_or_else(|| invalid("Archive target has no directory."))?;
    security::directory(parent)?;
    if target.exists() {
        security::regular_file(target)?;
    }
    let files = inventory(source_root)?;
    for path in files.keys() {
        if security::aliases(&source_root.join(path), target)? {
            return Err(invalid("Archive target aliases a snapshot input."));
        }
    }
    let manifest = Manifest {
        project_version: 1,
        format: "spectrapack-project".into(),
        saved_at: now(),
        object_report: "object.report.json".into(),
        resolved_settings: has_result.then(|| "settings.json".into()),
        best_result: has_result.then(|| "result.json".into()),
        draft_settings: draft_settings.clone(),
        files: files.values().cloned().collect(),
    };
    validate_manifest(&manifest)?;
    validate_graph(&files, has_result, |name| {
        security::json(&source_root.join(name), document_limit(name))
    })?;
    let manifest_bytes = serde_json::to_vec(&manifest).map_err(io_error)?;
    if manifest_bytes.len() as u64 > MANIFEST_LIMIT {
        return Err(limit());
    }
    let temporary = parent.join(format!(
        ".spectrapack-{}.tmp",
        uuid::Uuid::new_v4().simple()
    ));
    let output = OpenOptions::new()
        .write(true)
        .read(true)
        .create_new(true)
        .open(&temporary)
        .map_err(io_error)?;
    let outcome = (|| {
        let mut writer = ZipWriter::new(output);
        let options = SimpleFileOptions::default()
            .compression_method(CompressionMethod::Deflated)
            .unix_permissions(0o600);
        writer
            .start_file("project.json", options)
            .map_err(io_error)?;
        writer.write_all(&manifest_bytes).map_err(io_error)?;
        let mut total = manifest_bytes.len() as u64;
        for (name, record) in &files {
            let path = security::scoped(source_root, &source_root.join(name))?;
            writer.start_file(name, options).map_err(io_error)?;
            let (size, hash) = stream(
                &mut File::open(path).map_err(io_error)?,
                &mut writer,
                record.size_bytes.min(TOTAL_LIMIT - total),
            )?;
            if size != record.size_bytes || hash != record.sha256 {
                return Err(invalid("Snapshot changed during Save."));
            }
            total += size;
        }
        let mut output = writer.finish().map_err(io_error)?;
        output.flush().map_err(io_error)?;
        output.sync_all().map_err(io_error)?;
        drop(output);
        check_archive(&temporary, None)?;
        before_replace()?;
        security::directory(parent)?;
        if target.exists() {
            security::regular_file(target)?;
        }
        replace(&temporary, target)
    })();
    if outcome.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    outcome
}

pub fn open(archive: &Path, destination: &Path) -> Outcome<Value> {
    security::directory(destination)?;
    if fs::read_dir(destination)
        .map_err(io_error)?
        .next()
        .is_some()
    {
        return Err(invalid(
            "Project extraction requires a new empty private directory.",
        ));
    }
    // Only this empty private directory can receive extracted files. The core activates it after native validation.
    let destination = fs::canonicalize(destination).map_err(io_error)?;
    check_archive(archive, Some(&destination))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use sha2::{Digest, Sha256};
    use std::{
        fs,
        io::{Read, Write},
        path::PathBuf,
    };
    use tempfile::TempDir;
    use zip::{write::SimpleFileOptions, CompressionMethod, ZipArchive, ZipWriter};

    fn digest(bytes: &[u8]) -> String {
        format!("{:x}", Sha256::digest(bytes))
    }
    fn draft() -> Value {
        json!({"desktop_version":1,"box_dimensions_mm":[20,30,40],
            "clearance_mm":{"pair":1,"wall":1},"orientation":{"mode":"cube"},
            "pitch_mm":1,"budget_seconds":2,"seed":"18446744073709551615"})
    }
    fn fixture(result: bool) -> (TempDir, PathBuf, Vec<(String, Vec<u8>)>) {
        let temporary = tempfile::tempdir().unwrap();
        let root = temporary.path().join("snapshot");
        fs::create_dir(&root).unwrap();
        fs::create_dir(root.join("assets")).unwrap();
        // Transport fixtures exercise byte identity; the core integration uses real native geometry.
        let source = b"transport STL bytes".to_vec();
        let solid = b"transport float64 PLY bytes".to_vec();
        let source_path = format!("assets/{}.stl", digest(&source));
        let solid_path = format!("assets/{}.ply", digest(&solid));
        let report = json!({"schema_version":1,"role":"object","state":"accepted",
            "source":{"path":source_path,"sha256":digest(&source)},
            "accepted_solid":{"path":solid_path,"sha256":digest(&solid)},"repair_record":null});
        let mut files = vec![
            (source_path, source),
            (solid_path, solid),
            (
                "object.report.json".into(),
                serde_json::to_vec(&report).unwrap(),
            ),
        ];
        if result {
            let settings = json!({"settings_version":1,"search":{"seed":"17"}});
            let result = json!({"schema_version":1,"assets":{"object":report},
                "search":{"resolved_settings":settings},"placements":[],"count":0});
            files.push((
                "settings.json".into(),
                serde_json::to_vec(&settings).unwrap(),
            ));
            files.push(("result.json".into(), serde_json::to_vec(&result).unwrap()));
        }
        for (name, bytes) in &files {
            fs::write(root.join(name), bytes).unwrap();
        }
        (temporary, root, files)
    }
    fn entries(path: &Path) -> Vec<(String, Vec<u8>)> {
        let mut archive = ZipArchive::new(fs::File::open(path).unwrap()).unwrap();
        (0..archive.len())
            .map(|index| {
                let mut entry = archive.by_index(index).unwrap();
                let name = entry.name().to_owned();
                let mut bytes = Vec::new();
                entry.read_to_end(&mut bytes).unwrap();
                (name, bytes)
            })
            .collect()
    }
    fn write_zip(path: &Path, entries: &[(String, Vec<u8>)]) {
        let mut writer = ZipWriter::new(fs::File::create(path).unwrap());
        for (name, bytes) in entries {
            writer
                .start_file(
                    name,
                    SimpleFileOptions::default().compression_method(CompressionMethod::Deflated),
                )
                .unwrap();
            writer.write_all(bytes).unwrap();
        }
        writer.finish().unwrap();
    }
    fn manifest(files: &[(String, Vec<u8>)], result: bool) -> Value {
        json!({"project_version":1,"format":"spectrapack-project","saved_at":"2026-09-30T00:00:00Z",
            "object_report":"object.report.json","resolved_settings":if result {json!("settings.json")} else {Value::Null},
            "best_result":if result {json!("result.json")} else {Value::Null},"draft_settings":draft(),
            "files":files.iter().map(|(path,bytes)|json!({"path":path,"sha256":digest(bytes),"size_bytes":bytes.len()})).collect::<Vec<_>>()})
    }
    fn archive_fixture(path: &Path, files: &[(String, Vec<u8>)], manifest: &Value) {
        let mut contents = files.to_vec();
        contents.push(("project.json".into(), serde_json::to_vec(manifest).unwrap()));
        write_zip(path, &contents);
    }
    #[test]
    fn data02_save_before_first_result_moves_and_preserves_all_bytes() {
        let (temporary, root, files) = fixture(false);
        let saved = temporary.path().join("saved.spectrapack");
        save(&root, &saved, &draft(), false).unwrap();
        let moved = temporary.path().join("renamed.spectrapack");
        fs::rename(&saved, &moved).unwrap();
        let before = fs::read(&moved).unwrap();
        let extracted = temporary.path().join("extracted");
        fs::create_dir(&extracted).unwrap();
        let manifest = open(&moved, &extracted).unwrap();
        assert!(manifest["best_result"].is_null());
        assert!(manifest["resolved_settings"].is_null());
        assert_eq!(manifest["draft_settings"], draft());
        for (name, expected) in &files {
            assert_eq!(&fs::read(extracted.join(name)).unwrap(), expected);
        }
        let contents = entries(&moved);
        assert_eq!(contents.len(), files.len() + 1);
        for record in manifest["files"].as_array().unwrap() {
            let bytes = &contents
                .iter()
                .find(|(name, _)| Some(name.as_str()) == record["path"].as_str())
                .unwrap()
                .1;
            assert_eq!(record["sha256"], digest(bytes));
            assert_eq!(record["size_bytes"], bytes.len());
        }
        assert_eq!(fs::read(&moved).unwrap(), before);
    }
    #[test]
    fn data02_pending_draft_does_not_relabel_resolved_result() {
        let (temporary, root, files) = fixture(true);
        let saved = temporary.path().join("result.spectrapack");
        let mut pending = draft();
        pending["clearance_mm"]["pair"] = json!(7);
        save(&root, &saved, &pending, true).unwrap();
        let output = temporary.path().join("opened");
        fs::create_dir(&output).unwrap();
        let manifest = open(&saved, &output).unwrap();
        assert_eq!(manifest["draft_settings"], pending);
        for (name, expected) in files {
            assert_eq!(fs::read(output.join(name)).unwrap(), expected);
        }
    }
    #[test]
    fn data02_bad_save_retains_previous_complete_archive() {
        let (temporary, root, _) = fixture(false);
        let saved = temporary.path().join("previous.spectrapack");
        fs::write(&saved, b"previous complete archive").unwrap();
        let mut bad = draft();
        bad["pitch_mm"] = json!(0);
        assert!(save(&root, &saved, &bad, false).is_err());
        assert_eq!(fs::read(saved).unwrap(), b"previous complete archive");
    }
    #[test]
    fn data02_independent_ordinary_zip_is_accepted() {
        let (temporary, _, files) = fixture(false);
        let path = temporary.path().join("independent.zip");
        archive_fixture(&path, &files, &manifest(&files, false));
        let output = temporary.path().join("opened");
        fs::create_dir(&output).unwrap();
        open(&path, &output).unwrap();
    }
    fn rejected(path: &Path, base: &Path) -> crate::model::DesktopError {
        let output = base.join(format!("bad-{}", uuid::Uuid::new_v4().simple()));
        fs::create_dir(&output).unwrap();
        open(path, &output).unwrap_err()
    }
    #[test]
    fn data02_fault_before_replace_preserves_archive_and_cleans_owned_temporary() {
        let (temporary, root, _) = fixture(false);
        let path = temporary.path().join("previous.spectrapack");
        save(&root, &path, &draft(), false).unwrap();
        let before = fs::read(&path).unwrap();
        let result = save_internal(&root, &path, &draft(), false, || {
            Err(error("INJECTED_FAILURE", "Before replacement"))
        });
        assert_eq!(result.unwrap_err().code, "INJECTED_FAILURE");
        assert_eq!(fs::read(&path).unwrap(), before);
        assert_eq!(fs::read_dir(temporary.path()).unwrap().count(), 2);
    }
    #[cfg(windows)]
    #[test]
    fn data02_windows_failed_atomic_replace_retains_previous_bytes() {
        use std::os::windows::fs::OpenOptionsExt;
        let (temporary, root, _) = fixture(false);
        let path = temporary.path().join("previous.spectrapack");
        save(&root, &path, &draft(), false).unwrap();
        let before = fs::read(&path).unwrap();
        // Deny FILE_SHARE_DELETE: the real Windows replacement call must fail without deleting the old file.
        let locked = fs::OpenOptions::new()
            .read(true)
            .share_mode(1)
            .open(&path)
            .unwrap();
        assert_eq!(
            save(&root, &path, &draft(), false).unwrap_err().code,
            "OUTPUT_WRITE_FAILED"
        );
        assert_eq!(fs::read(&path).unwrap(), before);
        drop(locked);
        assert_eq!(fs::read_dir(temporary.path()).unwrap().count(), 2);
    }
    #[test]
    fn security_archive_target_cannot_alias_source_by_hardlink() {
        let (temporary, root, files) = fixture(false);
        let path = temporary.path().join("alias.spectrapack");
        fs::hard_link(root.join(&files[0].0), &path).unwrap();
        assert!(save(&root, &path, &draft(), false).is_err());
        assert_eq!(fs::read(root.join(&files[0].0)).unwrap(), files[0].1);
    }
    #[test]
    fn security_paths_reject_windows_aliases_traversal_and_absolute_names() {
        let (temporary, _, files) = fixture(false);
        let path = temporary.path().join("bad.zip");
        let cases = [
            "../escape",
            "/absolute",
            "C:/drive",
            "//server/share",
            "\\\\server\\share",
            "assets\\escape",
            "assets/x:stream",
            "assets/./x",
            "assets//x",
            "assets/x/",
            "assets/CON",
            "assets/aux.txt",
            "assets/LPT1.txt",
            "assets/COM¹.dat",
            "assets/NUL .txt",
            "assets/trailing.",
            "assets/trailing ",
            "assets/q?x",
            "assets/CONIN$",
        ];
        for name in cases {
            let mut entries = files.clone();
            entries.push((name.into(), b"unexpected".to_vec()));
            archive_fixture(&path, &entries, &manifest(&entries, false));
            assert!(
                rejected(&path, temporary.path()).code != "",
                "Accepted {name}"
            );
        }
        assert!(!temporary.path().join("escape").exists());
    }
    #[test]
    fn security_tampered_hash_size_unlisted_and_unreferenced_entries_reject() {
        let (temporary, _, files) = fixture(false);
        let path = temporary.path().join("bad.zip");
        let mut tampered = files.clone();
        tampered[0].1[0] ^= 1;
        archive_fixture(&path, &tampered, &manifest(&files, false));
        rejected(&path, temporary.path());
        let mut wrong_size = manifest(&files, false);
        wrong_size["files"][0]["size_bytes"] = json!(0);
        archive_fixture(&path, &files, &wrong_size);
        rejected(&path, temporary.path());
        let mut extra = files.clone();
        extra.push(("surprise.txt".into(), b"unlisted".to_vec()));
        archive_fixture(&path, &extra, &manifest(&files, false));
        rejected(&path, temporary.path());
        let bytes = b"unreferenced".to_vec();
        let mut extra = files.clone();
        extra.push((format!("assets/{}.ply", digest(&bytes)), bytes));
        archive_fixture(&path, &extra, &manifest(&extra, false));
        rejected(&path, temporary.path());
        let mut absent = files.clone();
        absent.remove(0);
        archive_fixture(&path, &absent, &manifest(&absent, false));
        rejected(&path, temporary.path());
    }
    #[test]
    fn security_manifest_version_shape_timestamp_and_nullable_pairing_reject() {
        let (temporary, _, files) = fixture(false);
        let path = temporary.path().join("bad.zip");
        let original = manifest(&files, false);
        for bad in [
            {
                let mut m = original.clone();
                m["project_version"] = json!(2);
                m
            },
            {
                let mut m = original.clone();
                m["unknown"] = json!(true);
                m
            },
            {
                let mut m = original.clone();
                m["saved_at"] = json!("2026-02-30T00:00:00Z");
                m
            },
            {
                let mut m = original.clone();
                m["resolved_settings"] = json!("settings.json");
                m
            },
            {
                let mut m = original.clone();
                m.as_object_mut().unwrap().remove("best_result");
                m
            },
            {
                let mut m = original.clone();
                m["draft_settings"]["seed"] = json!("18446744073709551616");
                m
            },
            {
                let mut m = original.clone();
                m["files"][0]["unknown"] = json!(true);
                m
            },
            {
                let mut m = original.clone();
                let duplicate = m["files"][0].clone();
                m["files"].as_array_mut().unwrap().push(duplicate);
                m
            },
        ] {
            archive_fixture(&path, &files, &bad);
            rejected(&path, temporary.path());
        }
        let text = serde_json::to_string(&original).unwrap();
        let text = format!("{{\"project_version\":1,{}", &text[1..]);
        let mut entries = files.clone();
        entries.push(("project.json".into(), text.into_bytes()));
        write_zip(&path, &entries);
        rejected(&path, temporary.path());
    }
    #[test]
    fn data02_result_settings_and_object_association_reject_mismatch() {
        let (temporary, _, files) = fixture(true);
        let path = temporary.path().join("bad.zip");
        for field in ["settings.json", "result.json"] {
            let mut changed = files.clone();
            let item = changed.iter_mut().find(|(name, _)| name == field).unwrap();
            let mut value: Value = serde_json::from_slice(&item.1).unwrap();
            if field == "settings.json" {
                value["search"]["seed"] = json!("18");
            } else {
                value["assets"]["object"]["role"] = json!("container");
            }
            item.1 = serde_json::to_vec(&value).unwrap();
            archive_fixture(&path, &changed, &manifest(&changed, true));
            assert_eq!(rejected(&path, temporary.path()).code, "ASSET_MISMATCH");
        }
    }
    #[test]
    fn data02_repair_record_preserves_transitive_meshes_once() {
        let (temporary, root, mut files) = fixture(false);
        let mesh = b"repair before float64 PLY".to_vec();
        let mesh_name = format!("assets/{}.ply", digest(&mesh));
        let after_hash = digest(&files[1].1);
        let repair = json!({"schema_version":1,"before":{"mesh_sha256":digest(&mesh)},"after":{"mesh_sha256":after_hash}});
        let repair_bytes = serde_json::to_vec(&repair).unwrap();
        let repair_name = format!("assets/{}.repair.json", digest(&repair_bytes));
        let report = files
            .iter_mut()
            .find(|(name, _)| name == "object.report.json")
            .unwrap();
        let mut value: Value = serde_json::from_slice(&report.1).unwrap();
        value["repair_record"] =
            json!({"path":repair_name,"sha256":digest(&repair_bytes),"accepted_by_user":true});
        report.1 = serde_json::to_vec(&value).unwrap();
        files.push((mesh_name, mesh));
        files.push((repair_name, repair_bytes));
        for (name, bytes) in &files {
            fs::write(root.join(name), bytes).unwrap();
        }
        let path = temporary.path().join("repair.spectrapack");
        save(&root, &path, &draft(), false).unwrap();
        let output = temporary.path().join("opened");
        fs::create_dir(&output).unwrap();
        open(&path, &output).unwrap();
        assert_eq!(entries(&path).len(), files.len() + 1);
        for (name, bytes) in files {
            assert_eq!(fs::read(output.join(name)).unwrap(), bytes);
        }
    }
    // Mutate a raw central/local header independently of admission code. ZipArchive locates records.
    fn mutate_header(
        path: &Path,
        index: usize,
        central_offset: usize,
        local_offset: Option<usize>,
        replacement: &[u8],
    ) {
        let mut zip = ZipArchive::new(fs::File::open(path).unwrap()).unwrap();
        let entry = zip.by_index(index).unwrap();
        let central = entry.central_header_start() as usize;
        let local = entry.header_start() as usize;
        drop(entry);
        drop(zip);
        let mut bytes = fs::read(path).unwrap();
        bytes[central + central_offset..central + central_offset + replacement.len()]
            .copy_from_slice(replacement);
        if let Some(offset) = local_offset {
            bytes[local + offset..local + offset + replacement.len()].copy_from_slice(replacement);
        }
        fs::write(path, bytes).unwrap();
    }
    #[test]
    fn security_raw_zip_links_encryption_compression_header_mismatch_and_truncation_reject() {
        let (temporary, _, files) = fixture(false);
        let path = temporary.path().join("bad.zip");
        for (offset, local, bytes) in [
            (38, None, ((0o120777_u32) << 16).to_le_bytes().to_vec()),
            (38, None, ((0o060600_u32) << 16).to_le_bytes().to_vec()),
            (38, None, 0x400_u32.to_le_bytes().to_vec()),
            (8, Some(6), 1_u16.to_le_bytes().to_vec()),
            (10, Some(8), 99_u16.to_le_bytes().to_vec()),
            (6, Some(4), 45_u16.to_le_bytes().to_vec()),
            (34, None, 1_u16.to_le_bytes().to_vec()),
            (16, None, 0_u32.to_le_bytes().to_vec()),
        ] {
            archive_fixture(&path, &files, &manifest(&files, false));
            mutate_header(&path, 0, offset, local, &bytes);
            rejected(&path, temporary.path());
        }
        archive_fixture(&path, &files, &manifest(&files, false));
        let original = fs::read(&path).unwrap();
        for keep in [
            0,
            4,
            original.len() / 2,
            original.len() - 1,
            original.len() - 22,
        ] {
            fs::write(&path, &original[..keep]).unwrap();
            rejected(&path, temporary.path());
        }
    }
    #[test]
    fn security_duplicate_and_case_collision_names_reject_before_zip_library_deduplication() {
        let (temporary, _, files) = fixture(false);
        let path = temporary.path().join("bad.zip");
        let mut collision = files.clone();
        collision.push(("OBJECT.REPORT.JSON".into(), files[2].1.clone()));
        archive_fixture(&path, &collision, &manifest(&collision, false));
        rejected(&path, temporary.path());
        let mut identical = files.clone();
        let mut name = files[0].0.clone();
        name.replace_range(7..8, "0");
        if name == files[0].0 {
            name.replace_range(7..8, "1");
        }
        identical.push((name, files[0].1.clone()));
        archive_fixture(&path, &identical, &manifest(&identical, false));
        // ZIP writer disallows duplicates; patch a same-length second name into an actual duplicate.
        mutate_header(&path, 3, 46, Some(30), files[0].0.as_bytes());
        assert_eq!(
            ZipArchive::new(fs::File::open(&path).unwrap())
                .unwrap()
                .len(),
            identical.len()
        );
        rejected(&path, temporary.path());
    }
    #[test]
    fn security_manifest_entry_archive_and_streamed_size_limits_reject() {
        let (temporary, _, files) = fixture(false);
        let path = temporary.path().join("bad.zip");
        let mut entries = files.clone();
        entries.push((
            "project.json".into(),
            vec![b' '; MANIFEST_LIMIT as usize + 1],
        ));
        write_zip(&path, &entries);
        assert_eq!(rejected(&path, temporary.path()).code, "MEMORY_LIMIT");
        let entries: Vec<_> = (0..ENTRY_LIMIT + 1)
            .map(|i| (format!("item{i}"), Vec::new()))
            .collect();
        write_zip(&path, &entries);
        assert_eq!(rejected(&path, temporary.path()).code, "MEMORY_LIMIT");
        let sparse = fs::File::create(&path).unwrap();
        sparse.set_len(TOTAL_LIMIT + 1).unwrap();
        drop(sparse);
        assert_eq!(rejected(&path, temporary.path()).code, "MEMORY_LIMIT");
        // Declared total expanded bytes cannot bypass the global limit.
        archive_fixture(&path, &files, &manifest(&files, false));
        mutate_header(&path, 0, 24, Some(22), &(TOTAL_LIMIT as u32).to_le_bytes());
        assert_eq!(rejected(&path, temporary.path()).code, "MEMORY_LIMIT");
        // The reader must count actual streamed bytes even when a ZIP size declaration is false.
        let mut lying = manifest(&files, false);
        lying["files"][0]["size_bytes"] = json!(1);
        archive_fixture(&path, &files, &lying);
        mutate_header(&path, 0, 24, Some(22), &1_u32.to_le_bytes());
        assert_eq!(rejected(&path, temporary.path()).code, "MEMORY_LIMIT");
    }
    #[test]
    fn security_extraction_refuses_existing_destination_contents() {
        let (temporary, _, files) = fixture(false);
        let path = temporary.path().join("normal.zip");
        archive_fixture(&path, &files, &manifest(&files, false));
        let output = temporary.path().join("opened");
        fs::create_dir(&output).unwrap();
        fs::write(output.join("retained"), b"previous active state").unwrap();
        assert!(open(&path, &output).is_err());
        assert_eq!(
            fs::read(output.join("retained")).unwrap(),
            b"previous active state"
        );
    }
    #[cfg(windows)]
    #[test]
    fn security_actual_windows_junction_inputs_and_destination_are_rejected() {
        use std::os::windows::process::CommandExt;
        let (temporary, root, files) = fixture(false);
        let path = temporary.path().join("normal.zip");
        archive_fixture(&path, &files, &manifest(&files, false));
        let actual = temporary.path().join("actual");
        fs::create_dir(&actual).unwrap();
        let junction = temporary.path().join("redirected");
        let output = std::process::Command::new("cmd.exe")
            .args(["/C", "mklink", "/J"])
            .arg(&junction)
            .arg(&actual)
            .creation_flags(0x08000000)
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "junction creation failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(open(&path, &junction).is_err());
        assert!(save(&root, &junction.join("outside.zip"), &draft(), false).is_err());
        fs::copy(&path, actual.join("input.zip")).unwrap();
        let extraction = temporary.path().join("opened");
        fs::create_dir(&extraction).unwrap();
        assert!(open(&junction.join("input.zip"), &extraction).is_err());
        assert_eq!(fs::read_dir(&actual).unwrap().count(), 1);
        fs::remove_dir(&junction).unwrap();
    }
}
