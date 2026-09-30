use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DesktopError {
    pub code: String,
    pub message: String,
    pub details: Value,
    pub recoverable: bool,
}

pub fn error(code: &str, message: impl Into<String>) -> DesktopError {
    DesktopError {
        code: code.into(),
        message: message.into(),
        details: json!({}),
        recoverable: true,
    }
}

impl std::fmt::Display for DesktopError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.code, self.message)
    }
}
impl std::error::Error for DesktopError {}

pub type Outcome<T> = Result<T, DesktopError>;

pub fn now() -> String {
    time::OffsetDateTime::now_utc()
        .format(&time::format_description::well_known::Rfc3339)
        .unwrap()
}

pub fn id(prefix: &str) -> String {
    format!("{prefix}-{}", uuid::Uuid::new_v4().simple())
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct State {
    pub desktop_version: u32,
    pub session_id: String,
    pub revision: u64,
    pub object: Option<Value>,
    pub draft_settings: Option<Value>,
    pub result: Option<Value>,
    pub operation: Option<Operation>,
    pub last_error: Option<DesktopError>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Operation {
    pub id: String,
    pub kind: String,
    pub phase: String,
    pub started_at: String,
    pub finished_at: Option<String>,
    pub result_id: Option<String>,
    pub detail: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Receipt {
    pub operation_id: String,
}
#[derive(Debug, Clone, Serialize)]
pub struct StopResponse {
    pub operation_id: String,
    pub accepted: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ImportRequest {
    pub units: String,
    pub scale_mm: Option<f64>,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct StartRequest {
    pub settings: Value,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct StopRequest {
    pub operation_id: String,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ExportRequest {
    pub format: String,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PreviewRequest {
    pub preview_id: String,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SaveRequest {
    pub settings: Value,
}

pub fn validate_import(request: &ImportRequest) -> Outcome<()> {
    match request.units.as_str() {
        "mm" | "inch" if request.scale_mm.is_none() => Ok(()),
        "custom" if request.scale_mm.is_some_and(|x| x.is_finite() && x > 0.0) => Ok(()),
        _ => Err(error(
            "INVALID_REQUEST",
            "Units require mm, inch, or custom with a positive scale.",
        )),
    }
}

pub fn validate_settings(value: &Value) -> Outcome<()> {
    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    struct Settings {
        desktop_version: u32,
        box_dimensions_mm: [f64; 3],
        clearance_mm: Clearance,
        orientation: Orientation,
        pitch_mm: f64,
        budget_seconds: f64,
        seed: String,
    }
    #[derive(Deserialize)]
    #[serde(deny_unknown_fields)]
    struct Clearance {
        pair: f64,
        wall: f64,
    }
    #[derive(Deserialize)]
    #[serde(tag = "mode", deny_unknown_fields)]
    enum Orientation {
        #[serde(rename = "fixed")]
        Fixed { quaternion_xyzw: [f64; 4] },
        #[serde(rename = "cube")]
        Cube,
    }
    let settings: Settings = serde_json::from_value(value.clone())
        .map_err(|e| error("INVALID_SETTINGS", e.to_string()))?;
    let positive = |x: f64| x.is_finite() && x > 0.0;
    let seed_ok = settings.seed.parse::<u64>().is_ok()
        && (settings.seed == "0"
            || (!settings.seed.starts_with('0')
                && settings.seed.bytes().all(|x| x.is_ascii_digit())));
    if settings.desktop_version != 1
        || !settings.box_dimensions_mm.into_iter().all(positive)
        || !positive(settings.pitch_mm)
        || !positive(settings.budget_seconds)
        || !settings.clearance_mm.pair.is_finite()
        || settings.clearance_mm.pair < 0.0
        || !settings.clearance_mm.wall.is_finite()
        || settings.clearance_mm.wall < 0.0
        || !seed_ok
    {
        return Err(error(
            "INVALID_SETTINGS",
            "Version, dimensions, clearances, pitch, budget or uint64 seed is invalid.",
        ));
    }
    if let Orientation::Fixed { quaternion_xyzw } = settings.orientation {
        let norm = quaternion_xyzw.iter().map(|x| x * x).sum::<f64>().sqrt();
        if quaternion_xyzw.iter().any(|x| !x.is_finite()) || (norm - 1.0).abs() > 1e-9 {
            return Err(error(
                "INVALID_SETTINGS",
                "Fixed quaternion must be normalized.",
            ));
        }
    }
    Ok(())
}
