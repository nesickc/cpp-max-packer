#include <spectrapack/io/contracts.hpp>
#include <spectrapack/core/asset_id.hpp>

#include "embedded_schemas.hpp"

#include <nlohmann/json-schema.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace spectrapack::io {
namespace {
constexpr std::size_t kMaxIssues = 32;
enum class ParseProblem { duplicate_key, nesting_too_deep };
struct ParseFailure final { ParseProblem problem; };

std::string name(ContractKind kind) {
  switch (kind) {
    case ContractKind::settings: return "settings";
    case ContractKind::assets: return "assets";
    case ContractKind::results: return "results";
    case ContractKind::protocol: return "protocol";
    case ContractKind::benchmark_summary: return "benchmark_summary";
  }
  throw std::logic_error("unknown contract kind");
}

std::string id_for(const std::string& key) {
  return "https://spectrapack.invalid/schemas/v1/" +
      (key == "benchmark_summary" ? "benchmark-summary" : key) + ".schema.json";
}

class Issues final : public nlohmann::json_schema::error_handler {
 public:
  void error(const Json::json_pointer& pointer, const Json&, const std::string& message) override {
    if (items.size() < kMaxIssues) items.push_back({ValidationStage::schema, pointer.to_string(), "SCHEMA_INVALID", message});
  }
  std::vector<ValidationIssue> items;
};

ContractFailure parse_failure(ContractKind kind, std::string code, std::string message) {
  return ContractFailure{kind, {{ValidationStage::parse, "", std::move(code), std::move(message)}}};
}

bool contains_nul(std::string_view text) {
  return text.find('\0') != std::string_view::npos;
}

bool valid_utf8(std::string_view text) {
  for (std::size_t index = 0; index < text.size();) {
    const unsigned char first = static_cast<unsigned char>(text[index++]);
    if (first < 0x80) continue;
    const int continuation_count = first >= 0xf0 && first <= 0xf4 ? 3 :
        first >= 0xe0 && first <= 0xef ? 2 : first >= 0xc2 && first <= 0xdf ? 1 : -1;
    if (continuation_count < 0 || index + static_cast<std::size_t>(continuation_count) > text.size()) return false;
    const unsigned char second = static_cast<unsigned char>(text[index]);
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second > 0x9f) ||
        (first == 0xf0 && second < 0x90) || (first == 0xf4 && second > 0x8f)) return false;
    for (int count = 0; count < continuation_count; ++count) {
      if ((static_cast<unsigned char>(text[index++]) & 0xc0) != 0x80) return false;
    }
  }
  return true;
}

bool has_invalid_utf8(const Json& value) {
  if (value.is_string()) return !valid_utf8(value.get_ref<const std::string&>());
  if (value.is_array()) {
    for (const auto& item : value) if (has_invalid_utf8(item)) return true;
  } else if (value.is_object()) {
    for (const auto& item : value.items()) {
      if (!valid_utf8(item.key()) || has_invalid_utf8(item.value())) return true;
    }
  }
  return false;
}

bool identity_key(std::string_view key) {
  return key == "path" || key == "source_path" || key == "request_id" || key == "method" ||
      key == "job_id" || key == "copy_id" || key == "asset_id" || key == "object_asset_id" ||
      (key.size() >= 3 && key.substr(key.size() - 3) == "_id");
}

bool has_nested_identity_nul(const Json& value) {
  if (value.is_array()) {
    for (const auto& item : value) if (has_nested_identity_nul(item)) return true;
  } else if (value.is_object()) {
    for (const auto& item : value.items()) {
      if (item.key() == "details") continue;
      if (identity_key(item.key()) && item.value().is_string() && contains_nul(item.value().get_ref<const std::string&>())) return true;
      if (has_nested_identity_nul(item.value())) return true;
    }
  }
  return false;
}

bool has_identity_nul(ContractKind kind, const Json& value) {
  if (kind != ContractKind::protocol) return has_nested_identity_nul(value);
  if (!value.is_object()) return false;
  static constexpr std::string_view envelope_fields[] = {"request_id", "method", "job_id", "type"};
  for (const auto field : envelope_fields) {
    const auto found = value.find(field);
    if (found != value.end() && found->is_string() && contains_nul(found->get_ref<const std::string&>())) return true;
  }
  return false;
}

bool finite_json(const Json& value) {
  if (value.is_number_float()) return std::isfinite(value.get<double>());
  if (value.is_array()) for (const auto& child : value) if (!finite_json(child)) return false;
  if (value.is_object()) for (const auto& item : value.items()) if (!finite_json(item.value())) return false;
  return true;
}

void semantic_issue(std::vector<ValidationIssue>& issues, std::string path, std::string code, std::string message) {
  if (issues.size() < kMaxIssues) issues.push_back({ValidationStage::semantic, std::move(path), std::move(code), std::move(message)});
}

bool seed_in_range(const std::string& value) {
  constexpr std::string_view max = "18446744073709551615";
  return value.size() < max.size() || (value.size() == max.size() && value <= max);
}

bool unit_quaternion(const Json& value, bool canonical) {
  if (!value.is_array() || value.size() != 4) return false;
  double norm2 = 0;
  for (const auto& e : value) { if (!e.is_number() || !std::isfinite(e.get<double>())) return false; norm2 += e.get<double>() * e.get<double>(); }
  if (std::abs(std::sqrt(norm2) - 1.0) > 1e-12) return false;
  if (!canonical) return true;
  const double x=value[0], y=value[1], z=value[2], w=value[3];
  return w > 0 || (w == 0 && (x > 0 || (x == 0 && (y > 0 || (y == 0 && z >= 0)))));
}

bool duplicate_quaternions(const Json& quaternions) {
  for (std::size_t first = 0; first < quaternions.size(); ++first) {
    for (std::size_t second = first + 1; second < quaternions.size(); ++second) {
      bool equal = true;
      for (std::size_t component = 0; component < 4; ++component) {
        if (quaternions.at(first).at(component).get<double>() !=
            quaternions.at(second).at(component).get<double>()) {
          equal = false;
          break;
        }
      }
      if (equal) return true;
    }
  }
  return false;
}

void append_prefixed_issues(
    std::vector<ValidationIssue>& issues,
    std::string_view prefix,
    const std::vector<ValidationIssue>& nested) {
  for (const auto& issue : nested) {
    semantic_issue(issues, std::string(prefix) + issue.path, issue.code, issue.message);
  }
}

bool content_ref_matches_asset(const Json& reference, const Json& asset) {
  return reference.at("source_sha256") == asset.at("source").at("sha256") &&
      reference.at("accepted_solid_sha256") == asset.at("accepted_solid").at("sha256");
}

bool derived_equal(double actual, double expected) {
  return std::abs(actual - expected) <=
      1e-9 + 8 * std::numeric_limits<double>::epsilon() * std::max(std::abs(actual), std::abs(expected));
}

bool portable_path_valid(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.back() == '/' || path.find('\\') != std::string_view::npos) return false;
  std::size_t start = 0;
  while (start < path.size()) {
    const auto end = path.find('/', start);
    const auto component = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
    if (component.empty() || component == "." || component == "..") return false;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return true;
}

bool has_invalid_portable_path(const Json& value) {
  if (value.is_array()) {
    for (const auto& item : value) if (has_invalid_portable_path(item)) return true;
  } else if (value.is_object()) {
    for (const auto& item : value.items()) {
      if (item.key() == "path" && item.value().is_string() && !portable_path_valid(item.value().get_ref<const std::string&>())) return true;
      if (has_invalid_portable_path(item.value())) return true;
    }
  }
  return false;
}

bool approximately_equal(double actual, double expected) {
  return std::abs(actual - expected) <= 1e-12;
}

bool translation_equal(double actual, double expected) {
  return std::abs(actual - expected) <=
      1e-9 + 8 * std::numeric_limits<double>::epsilon() * std::max(std::abs(actual), std::abs(expected));
}

bool transform_matches(const Json& actual, const Json& expected) {
  for (std::size_t row = 0; row < 4; ++row) {
    for (std::size_t column = 0; column < 4; ++column) {
      const double left = actual.at(row).at(column).get<double>();
      const double right = expected.at(row).at(column).get<double>();
      if (row < 3 && column == 3 ? !translation_equal(left, right) : !approximately_equal(left, right))
        return false;
    }
  }
  return true;
}

bool source_frame_valid(const Json& asset) {
  const auto& source = asset.at("source");
  const auto& bounds = asset.at("frame").at("source_bounds");
  const auto& minimum = bounds.at("min");
  const auto& maximum = bounds.at("max");
  const double scale = source.at("unit_scale_mm").get<double>();
  const auto& units = source.at("units");
  if ((units == "inch" && scale != 25.4) ||
      (units == "mm" && scale != 1.0)) return false;
  const auto& dimensions = asset.at("dimensions_mm");
  const auto& matrix = asset.at("frame").at("source_to_local");
  for (std::size_t axis = 0; axis != 3; ++axis) {
    const double low = minimum.at(axis).get<double>();
    const double high = maximum.at(axis).get<double>();
    const bool needs_positive_extent = asset.at("diagnostics").at("status") == "valid" || asset.at("state") == "accepted";
    const double scaled_low = scale * low;
    const double scaled_high = scale * high;
    const double expected_dimension = scaled_high - scaled_low;
    const double origin = asset.at("role") == "object"
        ? scaled_low / 2.0 + scaled_high / 2.0
        : scaled_low;
    if ((needs_positive_extent && !(high > low)) || high < low ||
        !std::isfinite(scaled_low) || !std::isfinite(scaled_high) ||
        !std::isfinite(expected_dimension) || !std::isfinite(origin) ||
        (needs_positive_extent && (!(expected_dimension > 0.0) ||
                                   !(dimensions.at(axis).get<double>() > 0.0))) ||
        !translation_equal(dimensions.at(axis).get<double>(), expected_dimension)) return false;
    for (std::size_t column = 0; column != 4; ++column) {
      const double expected = column == axis ? scale : (column == 3 ? -origin : 0.0);
      const double actual = matrix.at(axis).at(column).get<double>();
      if (column == 3 ? !translation_equal(actual, expected) :
          (column == axis ? actual != expected : actual != 0.0)) return false;
    }
  }
  return matrix.at(3).at(0).get<double>() == 0.0 &&
      matrix.at(3).at(1).get<double>() == 0.0 &&
      matrix.at(3).at(2).get<double>() == 0.0 &&
      matrix.at(3).at(3).get<double>() == 1.0;
}

bool utc_calendar_valid(std::string_view value) {
  if (value.size() < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') return false;
  if (value.size() == 20) {
    if (value[19] != 'Z') return false;
  } else {
    if (value.size() < 22 || value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index)
      if (value[index] < '0' || value[index] > '9') return false;
  }
  auto number = [&value](std::size_t first, std::size_t size) { int out = 0; for (std::size_t i=first;i<first+size;++i) { if (value[i] < '0' || value[i] > '9') return -1; out = out*10 + value[i]-'0'; } return out; };
  const int year=number(0,4), month=number(5,2), day=number(8,2), hour=number(11,2), minute=number(14,2), second=number(17,2);
  if (year < 0 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59) return false;
  static constexpr int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const int limit = days[month - 1] + (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0));
  return day <= limit;
}

bool placement_matrix_matches(const Json& placement) {
  const auto& q = placement.at("quaternion_xyzw"); const auto& t = placement.at("translation_mm"); const auto& m = placement.at("local_to_world");
  const double x=q[0], y=q[1], z=q[2], w=q[3];
  const double expected[3][3] = {{1-2*y*y-2*z*z,2*x*y-2*z*w,2*x*z+2*y*w},{2*x*y+2*z*w,1-2*x*x-2*z*z,2*y*z-2*x*w},{2*x*z-2*y*w,2*y*z+2*x*w,1-2*x*x-2*y*y}};
  for (std::size_t r=0;r<3;++r) for (std::size_t c=0;c<3;++c) if (!approximately_equal(m[r][c].get<double>(), expected[r][c])) return false;
  for (std::size_t r=0;r<3;++r) if (std::abs(m[r][3].get<double>()-t[r].get<double>()) > 1e-9 + 8*std::numeric_limits<double>::epsilon()*std::max(std::abs(m[r][3].get<double>()), std::abs(t[r].get<double>()))) return false;
  return approximately_equal(m[3][0].get<double>(),0)&&approximately_equal(m[3][1].get<double>(),0)&&approximately_equal(m[3][2].get<double>(),0)&&approximately_equal(m[3][3].get<double>(),1);
}

constexpr double kOrientationPermissionToleranceRadians = 1e-7;

double quaternion_angular_distance(const Json& left, const Json& right) {
  const double lx = left.at(0).get<double>(), ly = left.at(1).get<double>();
  const double lz = left.at(2).get<double>(), lw = left.at(3).get<double>();
  const double rx = right.at(0).get<double>(), ry = right.at(1).get<double>();
  const double rz = right.at(2).get<double>(), rw = right.at(3).get<double>();
  const double relative_x = lw * rx - lx * rw - ly * rz + lz * ry;
  const double relative_y = lw * ry + lx * rz - ly * rw - lz * rx;
  const double relative_z = lw * rz - lx * ry + ly * rx - lz * rw;
  const double relative_w = lw * rw + lx * rx + ly * ry + lz * rz;
  return 2.0 * std::atan2(std::hypot(relative_x, relative_y, relative_z), std::abs(relative_w));
}

bool quaternion_matches(const Json& left, const Json& right) {
  return quaternion_angular_distance(left, right) <= kOrientationPermissionToleranceRadians;
}

bool preserves_positive_z(const Json& quaternion) {
  const double x = quaternion.at(0).get<double>();
  const double y = quaternion.at(1).get<double>();
  const double z = quaternion.at(2).get<double>();
  const double w = quaternion.at(3).get<double>();
  const double rotated_x = 2 * x * z + 2 * y * w;
  const double rotated_y = 2 * y * z - 2 * x * w;
  const double rotated_z = 1 - 2 * x * x - 2 * y * y;
  return std::atan2(std::hypot(rotated_x, rotated_y), rotated_z) <=
      kOrientationPermissionToleranceRadians;
}

bool is_signed_axis_rotation(const Json& quaternion) {
  const double x = quaternion.at(0).get<double>();
  const double y = quaternion.at(1).get<double>();
  const double z = quaternion.at(2).get<double>();
  const double w = quaternion.at(3).get<double>();
  const double rotation[3][3] = {
      {1 - 2 * y * y - 2 * z * z, 2 * x * y - 2 * z * w, 2 * x * z + 2 * y * w},
      {2 * x * y + 2 * z * w, 1 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * x * w},
      {2 * x * z - 2 * y * w, 2 * y * z + 2 * x * w, 1 - 2 * x * x - 2 * y * y}};
  static constexpr int permutations[6][3] = {
      {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  for (const auto& permutation : permutations) {
    int inversions = 0;
    for (int first = 0; first < 3; ++first)
      for (int second = first + 1; second < 3; ++second)
        if (permutation[first] > permutation[second]) ++inversions;
    const int permutation_sign = inversions % 2 == 0 ? 1 : -1;
    for (const int sign_x : {-1, 1}) {
      for (const int sign_y : {-1, 1}) {
        for (const int sign_z : {-1, 1}) {
          const int signs[3] = {sign_x, sign_y, sign_z};
          if (permutation_sign * sign_x * sign_y * sign_z != 1) continue;
          double relative[3][3] = {};
          for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
              for (int axis = 0; axis < 3; ++axis) {
                const double cube = axis == permutation[row] ? signs[row] : 0.0;
                relative[row][column] += cube * rotation[axis][column];
              }
            }
          }
          const double cosine = std::clamp(
              (relative[0][0] + relative[1][1] + relative[2][2] - 1.0) / 2.0, -1.0, 1.0);
          const double skew_x = relative[2][1] - relative[1][2];
          const double skew_y = relative[0][2] - relative[2][0];
          const double skew_z = relative[1][0] - relative[0][1];
          const double sine = 0.5 * std::hypot(skew_x, skew_y, skew_z);
          if (std::atan2(sine, cosine) <= kOrientationPermissionToleranceRadians) return true;
        }
      }
    }
  }
  return false;
}

bool orientation_allows(const Json& policy, const Json& quaternion) {
  const auto mode = policy.at("mode").get<std::string>();
  if (mode == "free") return true;
  if (mode == "fixed") return quaternion_matches(quaternion, policy.at("quaternion_xyzw"));
  if (mode == "upright") return preserves_positive_z(quaternion);
  if (mode == "cube") return is_signed_axis_rotation(quaternion);
  for (const auto& allowed : policy.at("quaternions_xyzw")) {
    if (quaternion_matches(quaternion, allowed)) return true;
  }
  return false;
}

std::vector<ValidationIssue> semantics(ContractKind kind, const Json& value) {
  std::vector<ValidationIssue> issues;
  if (has_identity_nul(kind, value)) semantic_issue(issues, "", "IDENTITY_NUL", "Identity strings must not contain NUL.");
  if (kind == ContractKind::settings) {
    if (value.contains("object_asset_id") && !core::AssetId::parse(value.at("object_asset_id").get_ref<const std::string&>()))
      semantic_issue(issues, "/object_asset_id", "ASSET_ID_INVALID", "Requested object asset ID is invalid.");
    if (value.contains("object_asset_id") && value.at("container").at("kind") == "stl_volume" &&
        !core::AssetId::parse(value.at("container").at("asset_id").get_ref<const std::string&>()))
      semantic_issue(issues, "/container/asset_id", "ASSET_ID_INVALID", "Requested container asset ID is invalid.");
    const auto& search = value.at("search");
    const auto seed = search.at("seed").get<std::string>();
    if (!seed_in_range(seed)) semantic_issue(issues, "/search/seed", "SEED_OUT_OF_RANGE", "Seed exceeds uint64 maximum.");
    const bool deterministic = search.at("deterministic").get<bool>();
    const bool has_seconds = search.contains("budget_seconds");
    const bool has_work = search.contains("work_budget");
    if ((deterministic && (!has_work || has_seconds)) || (!deterministic && (!has_seconds || has_work)))
      semantic_issue(issues, "/search", "SEARCH_BUDGET_MODE", "Search budget does not match deterministic mode.");
    const auto compute_backend = value.at("compute").at("backend").get<std::string>();
    if (deterministic && compute_backend == "vulkan")
      semantic_issue(issues, "/compute/backend", "DETERMINISTIC_BACKEND_INVALID",
          "Deterministic work-budget settings must use CPU or resolve auto to CPU.");
    const auto& orientation = value.at("orientation");
    if (orientation.contains("quaternion_xyzw") && !unit_quaternion(orientation.at("quaternion_xyzw"), false))
      semantic_issue(issues, "/orientation/quaternion_xyzw", "QUATERNION_NOT_NORMALIZED", "Requested quaternion must be unit length.");
    if (orientation.contains("quaternions_xyzw")) for (std::size_t i=0;i<orientation.at("quaternions_xyzw").size();++i)
      if (!unit_quaternion(orientation.at("quaternions_xyzw")[i], false)) semantic_issue(issues,"/orientation/quaternions_xyzw/"+std::to_string(i),"QUATERNION_NOT_NORMALIZED","Requested quaternion must be unit length.");
    if (value.contains("resolved")) {
      const auto resolved_backend = value.at("resolved").at("backend").get<std::string>();
      if (compute_backend != "auto" && compute_backend != resolved_backend)
        semantic_issue(issues, "/resolved/backend", "BACKEND_RESOLUTION_MISMATCH",
            "An explicit compute backend must equal the initial resolved backend.");
      if (deterministic && resolved_backend != "cpu")
        semantic_issue(issues, "/resolved/backend", "DETERMINISTIC_BACKEND_INVALID",
            "Deterministic work-budget settings must resolve to CPU.");
      if (orientation.contains("quaternion_xyzw") && !unit_quaternion(orientation.at("quaternion_xyzw"), true))
        semantic_issue(issues, "/orientation/quaternion_xyzw", "QUATERNION_NOT_CANONICAL", "Resolved quaternion must be normalized and canonical.");
      if (orientation.contains("quaternions_xyzw")) {
        const auto& quaternions = orientation.at("quaternions_xyzw");
        for (std::size_t i = 0; i < quaternions.size(); ++i) {
          if (!unit_quaternion(quaternions[i], true))
            semantic_issue(issues, "/orientation/quaternions_xyzw/" + std::to_string(i), "QUATERNION_NOT_CANONICAL", "Resolved quaternion must be normalized and canonical.");
        }
        if (duplicate_quaternions(quaternions))
          semantic_issue(issues, "/orientation/quaternions_xyzw", "QUATERNION_DUPLICATE", "Resolved orientation catalog must not contain duplicates.");
      }
    }
  }
  if (kind == ContractKind::assets) {
    if (has_invalid_portable_path(value))
      semantic_issue(issues, "/source/path", "PORTABLE_PATH_INVALID", "Portable paths must use nonempty relative components.");
    if (!source_frame_valid(value))
      semantic_issue(issues, "/frame/source_to_local", "SOURCE_FRAME_MISMATCH", "Source frame must preserve the required unit and origin mapping.");
  }
  if (kind == ContractKind::results) {
    const auto& assets = value.at("assets");
    const auto& object_asset = assets.at("object");
    const auto& container = value.at("container");
    const auto& constraints = value.at("constraints");
    const auto& search = value.at("search");
    const auto& run_segments = search.at("run_segments");

    append_prefixed_issues(issues, "/assets/object", semantics(ContractKind::assets, object_asset));
    append_prefixed_issues(issues, "/search/resolved_settings",
        semantics(ContractKind::settings, search.at("resolved_settings")));
    for (std::size_t i = 0; i < run_segments.size(); ++i) {
      append_prefixed_issues(issues, "/search/run_segments/" + std::to_string(i) + "/resolved_settings",
          semantics(ContractKind::settings, run_segments.at(i).at("resolved_settings")));
    }

    if (value.at("count").get<std::uint64_t>() != value.at("placements").size())
      semantic_issue(issues, "/count", "COUNT_MISMATCH", "Count must equal placements length.");
    for (std::size_t i = 0; i < value.at("placements").size(); ++i) if (!unit_quaternion(value.at("placements")[i].at("quaternion_xyzw"), true))
      semantic_issue(issues, "/placements/" + std::to_string(i) + "/quaternion_xyzw", "QUATERNION_NOT_CANONICAL", "Persisted quaternion must be normalized and canonical.");
    std::set<std::string> copy_ids;
    for (std::size_t i=0;i<value.at("placements").size();++i) {
      const auto& placement=value.at("placements")[i];
      if (!copy_ids.insert(placement.at("copy_id").get<std::string>()).second) semantic_issue(issues,"/placements/"+std::to_string(i)+"/copy_id","COPY_ID_DUPLICATE","Copy IDs must be unique.");
      if (!placement_matrix_matches(placement)) semantic_issue(issues,"/placements/"+std::to_string(i)+"/local_to_world","POSE_MATRIX_MISMATCH","Placement matrix must match quaternion and translation.");
      if (!orientation_allows(constraints.at("orientation"), placement.at("quaternion_xyzw")))
        semantic_issue(issues, "/placements/" + std::to_string(i) + "/quaternion_xyzw",
            "PLACEMENT_ORIENTATION_DISALLOWED", "Placement quaternion is outside the recorded orientation permission.");
    }

    if (object_asset.at("role") != "object")
      semantic_issue(issues, "/assets/object/role", "ASSET_ROLE_MISMATCH", "The result object asset must have object role.");

    const bool stl_container = container.at("kind") == "stl_volume";
    const bool has_container_asset = assets.contains("container");
    const Json* accepted_container = nullptr;
    if (stl_container) {
      if (!has_container_asset) {
        semantic_issue(issues, "/assets/container", "CONTAINER_ASSET_MISMATCH",
            "An STL result requires its accepted container asset.");
      } else {
        accepted_container = &assets.at("container");
        append_prefixed_issues(issues, "/assets/container",
            semantics(ContractKind::assets, *accepted_container));
        if (accepted_container->at("role") != "container")
          semantic_issue(issues, "/assets/container/role", "ASSET_ROLE_MISMATCH",
              "The STL container asset must have container role.");
        if (!content_ref_matches_asset(container.at("asset"), *accepted_container))
          semantic_issue(issues, "/container/asset", "ASSET_REFERENCE_MISMATCH",
              "The result container reference must match its accepted asset.");
        if (!transform_matches(container.at("source_to_world"),
                accepted_container->at("frame").at("source_to_local")))
          semantic_issue(issues, "/container/source_to_world", "CONTAINER_FRAME_MISMATCH",
              "The STL source-to-world transform must match the accepted container frame.");
      }
    } else if (has_container_asset) {
      semantic_issue(issues, "/assets/container", "CONTAINER_ASSET_MISMATCH",
          "A box result must not carry an accepted container asset.");
    }

    auto check_resolved_settings = [&](const Json& settings, const std::string& base) {
      if (!content_ref_matches_asset(settings.at("object_asset"), object_asset))
        semantic_issue(issues, base + "/object_asset", "ASSET_REFERENCE_MISMATCH",
            "Resolved object references must match the accepted object asset.");

      bool physical_match = settings.at("clearance_mm") == constraints.at("clearance_mm") &&
          settings.at("orientation") == constraints.at("orientation") &&
          settings.at("resolved").at("orientation_catalog_sha256") ==
              constraints.at("orientation_catalog_sha256");
      if (stl_container) {
        const auto& settings_container = settings.at("container");
        const bool settings_stl = settings_container.at("kind") == "stl_volume";
        physical_match = physical_match && settings_stl;
        if (settings_stl) {
          physical_match = physical_match && settings_container.at("asset") == container.at("asset");
          if (accepted_container != nullptr &&
              !content_ref_matches_asset(settings_container.at("asset"), *accepted_container))
            semantic_issue(issues, base + "/container/asset", "ASSET_REFERENCE_MISMATCH",
                "Resolved container references must match the accepted container asset.");
        }
      } else {
        physical_match = physical_match && settings.at("container").at("kind") == "box" &&
            settings.at("container").at("dimensions_mm") == container.at("dimensions_mm");
      }
      if (!physical_match)
        semantic_issue(issues, base, "PHYSICAL_SETTINGS_MISMATCH",
            "Container, clearance and orientation permissions must remain unchanged across segments.");
    };
    check_resolved_settings(search.at("resolved_settings"), "/search/resolved_settings");
    for (std::size_t i = 0; i < run_segments.size(); ++i)
      check_resolved_settings(run_segments.at(i).at("resolved_settings"),
          "/search/run_segments/" + std::to_string(i) + "/resolved_settings");

    std::multiset<std::string> expected_hashes = {
        object_asset.at("accepted_solid").at("sha256").get<std::string>()};
    if (accepted_container != nullptr)
      expected_hashes.insert(accepted_container->at("accepted_solid").at("sha256").get<std::string>());
    std::multiset<std::string> recorded_hashes;
    for (const auto& hash : value.at("validation").at("authoritative_geometry_sha256"))
      recorded_hashes.insert(hash.get<std::string>());
    if (recorded_hashes != expected_hashes)
      semantic_issue(issues, "/validation/authoritative_geometry_sha256", "AUTHORITATIVE_HASH_MISMATCH",
          "Validation hashes must exactly identify the accepted object and STL container solids.");

    if (value.contains("artifacts")) {
      const auto& artifacts = value.at("artifacts");
      for (std::size_t i = 0; i < artifacts.size(); ++i) {
        if (!portable_path_valid(artifacts.at(i).at("path").get_ref<const std::string&>()))
          semantic_issue(issues, "/artifacts/" + std::to_string(i) + "/path", "PORTABLE_PATH_INVALID",
              "Artifact paths must use nonempty relative components.");
      }
    }

    if (!utc_calendar_valid(value.at("created_at").get<std::string>()))
      semantic_issue(issues, "/created_at", "UTC_CALENDAR_INVALID", "Timestamp must name a real UTC date.");

    const auto& metrics = value.at("metrics");
    const bool solid_null = metrics.at("solid_volume_mm3").is_null();
    const bool container_null = metrics.at("container_volume_mm3").is_null();
    const bool utilization_null = metrics.at("utilization").is_null();
    const bool utilization_available = !solid_null && !container_null;
    if (utilization_null == utilization_available) {
      semantic_issue(issues, "/metrics/utilization", "UTILIZATION_AVAILABILITY_MISMATCH",
          "Utilization must be null exactly when either input volume is unavailable.");
    } else if (utilization_available) {
      const double solid_volume = metrics.at("solid_volume_mm3").get<double>();
      const double container_volume = metrics.at("container_volume_mm3").get<double>();
      const double expected_utilization = value.at("count").get<double>() * solid_volume / container_volume;
      if (!std::isfinite(expected_utilization)) {
        semantic_issue(issues, "/metrics/utilization", "DERIVED_ARITHMETIC_INVALID",
            "Derived metric arithmetic must remain finite.");
      } else if (!derived_equal(metrics.at("utilization").get<double>(), expected_utilization)) {
        semantic_issue(issues, "/metrics/utilization", "UTILIZATION_MISMATCH",
            "Utilization must equal count times solid volume divided by container volume.");
      }

    }
    if (!stl_container && !container_null) {
      const double container_volume = metrics.at("container_volume_mm3").get<double>();
      double expected_container_volume = 1.0;
      for (const auto& dimension : container.at("dimensions_mm"))
        expected_container_volume *= dimension.get<double>();
      if (!std::isfinite(expected_container_volume)) {
        semantic_issue(issues, "/metrics/container_volume_mm3", "DERIVED_ARITHMETIC_INVALID",
            "Derived metric arithmetic must remain finite.");
      } else if (!derived_equal(container_volume, expected_container_volume)) {
        semantic_issue(issues, "/metrics/container_volume_mm3", "CONTAINER_VOLUME_MISMATCH",
            "Box container volume must equal the product of its dimensions.");
      }
    }
    if (metrics.at("time_to_best_seconds").get<double>() > search.at("elapsed_seconds").get<double>())
      semantic_issue(issues, "/metrics/time_to_best_seconds", "TIME_TO_BEST_INVALID",
          "Time to best must not exceed total search elapsed time.");

    const auto solution_revision = value.at("solution_revision").get<std::uint64_t>();
    double elapsed = 0;
    std::uint64_t candidates = 0;
    std::uint64_t passes = 0;
    bool totals_overflow = false;
    std::set<std::string> segment_ids;
    struct BackendState {
      std::string current;
      double segment_elapsed;
      bool deterministic;
      double last_transition_elapsed = 0;
      bool has_transition = false;
    };
    std::map<std::string, BackendState> backend_states;
    for (std::size_t i = 0; i < run_segments.size(); ++i) {
      const auto& segment = run_segments.at(i);
      const std::string base = "/search/run_segments/" + std::to_string(i);
      const auto segment_id = segment.at("segment_id").get<std::string>();
      if (!segment_ids.insert(segment_id).second) {
        semantic_issue(issues, base + "/segment_id", "SEGMENT_ID_DUPLICATE", "Run segment IDs must be unique.");
      } else {
        backend_states.emplace(segment_id, BackendState{
            segment.at("backend").get<std::string>(), segment.at("elapsed_seconds").get<double>(),
            segment.at("resolved_settings").at("search").at("deterministic").get<bool>()});
      }

      if (segment.at("backend") != segment.at("resolved_settings").at("resolved").at("backend"))
        semantic_issue(issues, base + "/backend", "SEGMENT_BACKEND_MISMATCH",
            "Segment backend must match its original resolved dispatch backend.");

      const auto& parent = segment.at("parent_solution_revision");
      if ((i == 0 && !parent.is_null()) ||
          (i != 0 && (parent.is_null() || parent.get<std::uint64_t>() > solution_revision)))
        semantic_issue(issues, base + "/parent_solution_revision", "SEGMENT_PARENT_INVALID",
            "The first segment parent must be null; later parents must reference an available result revision.");

      const auto& segment_search = segment.at("resolved_settings").at("search");
      if (segment.at("seed") != segment_search.at("seed"))
        semantic_issue(issues, base + "/seed", "SEGMENT_SEED_MISMATCH",
            "Each segment seed must match its own resolved settings.");

      const auto segment_candidates = segment.at("work_counts").at("candidate_evaluations").get<std::uint64_t>();
      const auto segment_passes = segment.at("work_counts").at("search_passes").get<std::uint64_t>();
      if (segment_search.contains("work_budget")) {
        const auto& budget = segment_search.at("work_budget");
        if (segment_candidates > budget.at("max_candidate_evaluations").get<std::uint64_t>() ||
            segment_passes > budget.at("max_search_passes").get<std::uint64_t>())
          semantic_issue(issues, base + "/work_counts", "SEGMENT_WORK_BUDGET_EXCEEDED",
              "Segment work counts must not exceed that segment's resolved work budget.");
      }

      elapsed += segment.at("elapsed_seconds").get<double>();
      if (segment_candidates > std::numeric_limits<std::uint64_t>::max() - candidates ||
          segment_passes > std::numeric_limits<std::uint64_t>::max() - passes) {
        totals_overflow = true;
      } else {
        candidates += segment_candidates;
        passes += segment_passes;
      }
    }

    for (std::size_t i = 0; i < search.at("backend_transitions").size(); ++i) {
      const auto& transition = search.at("backend_transitions").at(i);
      const std::string base = "/search/backend_transitions/" + std::to_string(i);
      const auto found = backend_states.find(transition.at("segment_id").get<std::string>());
      if (found == backend_states.end()) {
        semantic_issue(issues, base + "/segment_id", "TRANSITION_SEGMENT_UNKNOWN",
            "Backend transitions must reference a recorded run segment.");
        continue;
      }
      auto& state = found->second;
      const double transition_elapsed = transition.at("elapsed_seconds").get<double>();
      if (transition_elapsed > state.segment_elapsed ||
          (state.has_transition && transition_elapsed < state.last_transition_elapsed))
        semantic_issue(issues, base + "/elapsed_seconds", "TRANSITION_TIME_INVALID",
            "Transition times must be nondecreasing and within their segment elapsed time.");
      if (transition.at("from") == transition.at("to") ||
          transition.at("from").get<std::string>() != state.current)
        semantic_issue(issues, base, "TRANSITION_CHAIN_INVALID",
            "Backend transitions must change and continue from the segment's current backend.");
      if (state.deterministic &&
          (transition.at("from") == "vulkan" || transition.at("to") == "vulkan"))
        semantic_issue(issues, base, "DETERMINISTIC_BACKEND_TRANSITION",
            "A deterministic CPU segment cannot transition to or from Vulkan.");
      state.current = transition.at("to").get<std::string>();
      state.last_transition_elapsed = transition_elapsed;
      state.has_transition = true;
    }

    const auto& latest = run_segments.back();
    if (search.at("seed") != latest.at("seed"))
      semantic_issue(issues, "/search/seed", "LATEST_SEGMENT_MISMATCH",
          "Result seed must match the latest run segment.");
    if (search.at("resolved_settings") != latest.at("resolved_settings"))
      semantic_issue(issues, "/search/resolved_settings", "LATEST_SEGMENT_MISMATCH",
          "Result resolved settings must match the latest run segment.");

    if (totals_overflow || !std::isfinite(elapsed))
      semantic_issue(issues, "/search", "DERIVED_ARITHMETIC_INVALID",
          "Derived search arithmetic must remain finite and representable.");
    if (totals_overflow || !std::isfinite(elapsed) ||
        !approximately_equal(elapsed, search.at("elapsed_seconds").get<double>()) ||
        candidates != search.at("work_counts").at("candidate_evaluations").get<std::uint64_t>() ||
        passes != search.at("work_counts").at("search_passes").get<std::uint64_t>())
      semantic_issue(issues, "/search", "SEARCH_TOTAL_MISMATCH", "Search totals must equal run segment totals.");
  }
  if (kind == ContractKind::benchmark_summary) {
    append_prefixed_issues(issues, "/configuration/settings",
        semantics(ContractKind::settings, value.at("configuration").at("settings")));
    if (!utc_calendar_valid(value.at("created_at").get<std::string>()))
      semantic_issue(issues, "/created_at", "UTC_CALENDAR_INVALID",
          "Timestamp must name a real UTC date with an optional fractional second component.");

    std::uint64_t successful_runs = 0;
    std::uint64_t failed_runs = 0;
    std::vector<std::uint64_t> completed_counts;
    const auto& runs = value.at("runs");
    for (std::size_t run_index = 0; run_index < runs.size(); ++run_index) {
      const auto& run = runs.at(run_index);
      const std::string base = "/runs/" + std::to_string(run_index);
      const auto seed = run.at("seed").get<std::string>();
      if (!seed_in_range(seed))
        semantic_issue(issues, base + "/seed", "SEED_OUT_OF_RANGE", "Seed exceeds uint64 maximum.");

      const bool has_settings = !run.at("settings").is_null();
      const bool has_count = !run.at("count").is_null();
      const bool has_result = !run.at("result").is_null();
      if (has_count != has_result || ((has_count || has_result) && !has_settings))
        semantic_issue(issues, base, "BENCHMARK_RESULT_AVAILABILITY_MISMATCH",
            "A recorded count and result must appear together with resolved settings.");

      if (has_settings) {
        const auto& settings = run.at("settings");
        append_prefixed_issues(issues, base + "/settings", semantics(ContractKind::settings, settings));
        if (run.at("seed") != settings.at("search").at("seed"))
          semantic_issue(issues, base + "/seed", "RUN_SEED_MISMATCH",
              "A benchmark run seed must match its resolved settings.");
      }
      if (has_result && has_invalid_portable_path(run.at("result")))
        semantic_issue(issues, base + "/result/path", "PORTABLE_PATH_INVALID",
            "Result paths must use nonempty relative components.");

      if (run.at("status") == "completed") {
        ++successful_runs;
        completed_counts.push_back(run.at("count").get<std::uint64_t>());
      } else {
        ++failed_runs;
      }

      const auto& timings = run.at("timings");
      const double cold_end_to_end = timings.at("cold_end_to_end_seconds").get<double>();
      static constexpr std::string_view phase_names[] = {
          "preprocessing_seconds", "search_seconds", "validation_seconds", "export_seconds"};
      for (const auto phase : phase_names) {
        if (timings.at(phase).get<double>() > cold_end_to_end)
          semantic_issue(issues, base + "/timings/" + std::string(phase), "BENCHMARK_TIMING_INVALID",
              "Each benchmark phase time must not exceed cold end-to-end time.");
      }

      bool first_improvement = true;
      std::uint64_t previous_count = 0;
      double previous_elapsed = 0;
      const double search_seconds = timings.at("search_seconds").get<double>();
      const auto& improvements = run.at("improvements");
      for (std::size_t improvement_index = 0; improvement_index < improvements.size(); ++improvement_index) {
        const auto& improvement = improvements.at(improvement_index);
        const auto count = improvement.at("count").get<std::uint64_t>();
        const double improvement_elapsed = improvement.at("elapsed_seconds").get<double>();
        if ((!first_improvement && (count <= previous_count || improvement_elapsed < previous_elapsed)) ||
            improvement_elapsed > search_seconds ||
            (has_count && count > run.at("count").get<std::uint64_t>()))
          semantic_issue(issues, base + "/improvements/" + std::to_string(improvement_index),
              "IMPROVEMENT_HISTORY_INVALID",
              "Improvement counts must increase strictly without exceeding a recorded final count, and times must be ordered within search duration.");
        first_improvement = false;
        previous_count = count;
        previous_elapsed = improvement_elapsed;
      }

      if (has_settings && run.at("settings").at("search").at("deterministic").get<bool>()) {
        const auto& transitions = run.at("backend_transitions");
        for (std::size_t transition_index = 0; transition_index < transitions.size(); ++transition_index) {
          const auto& transition = transitions.at(transition_index);
          if (transition.at("from") == "vulkan" || transition.at("to") == "vulkan")
            semantic_issue(issues, base + "/backend_transitions/" + std::to_string(transition_index),
                "DETERMINISTIC_BACKEND_TRANSITION",
                "A deterministic CPU benchmark run cannot transition to or from Vulkan.");
        }
      }
    }

    const auto& summary = value.at("summary");
    if (summary.at("successful_runs").get<std::uint64_t>() != successful_runs ||
        summary.at("failed_runs").get<std::uint64_t>() != failed_runs)
      semantic_issue(issues, "/summary", "BENCHMARK_RUN_TOTAL_MISMATCH",
          "Summary success and failure totals must match the recorded runs.");

    const bool best_null = summary.at("best_count").is_null();
    const bool median_null = summary.at("median_count").is_null();
    const bool worst_null = summary.at("worst_count").is_null();
    if (completed_counts.empty()) {
      if (!best_null || !median_null || !worst_null)
        semantic_issue(issues, "/summary", "BENCHMARK_AGGREGATE_MISMATCH",
            "Count aggregates must all be null when no run completed.");
    } else {
      std::sort(completed_counts.begin(), completed_counts.end());
      const auto expected_worst = completed_counts.front();
      const auto expected_best = completed_counts.back();
      const std::size_t middle = completed_counts.size() / 2;
      const double expected_median = completed_counts.size() % 2 == 0
          ? completed_counts.at(middle - 1) / 2.0 + completed_counts.at(middle) / 2.0
          : static_cast<double>(completed_counts.at(middle));
      if (best_null || median_null || worst_null ||
          (!best_null && summary.at("best_count").get<std::uint64_t>() != expected_best) ||
          (!worst_null && summary.at("worst_count").get<std::uint64_t>() != expected_worst) ||
          (!median_null && !derived_equal(summary.at("median_count").get<double>(), expected_median)))
        semantic_issue(issues, "/summary", "BENCHMARK_AGGREGATE_MISMATCH",
            "Best, median and worst counts must aggregate completed runs only.");
    }
  }
  return issues;
}
}  // namespace

class ContractValidator::Impl {
 public:
  Impl() {
    for (const auto& [key, text] : detail::kEmbeddedSchemas) catalog.emplace(id_for(key), Json::parse(text));
    auto loader = [this](const nlohmann::json_uri& uri, Json& target) {
      const auto found = catalog.find(uri.location());
      if (found == catalog.end()) throw std::runtime_error("schema reference is not in embedded catalog");
      target = found->second;
    };
    for (const auto& [id, schema] : catalog) validators.emplace(id, nlohmann::json_schema::json_validator(
        schema, loader, nlohmann::json_schema::default_string_format_check));
  }
  std::map<std::string, Json> catalog;
  std::map<std::string, nlohmann::json_schema::json_validator> validators;
};

ValidatedDocument::ValidatedDocument(ContractKind kind, Json value) : kind_(kind), value_(std::move(value)) {}
ContractKind ValidatedDocument::kind() const noexcept { return kind_; }
const Json& ValidatedDocument::value() const noexcept { return value_; }
ContractValidator::ContractValidator() : impl_(std::make_unique<Impl>()) {}
ContractValidator::~ContractValidator() = default;
ContractValidator::ContractValidator(ContractValidator&&) noexcept = default;
ContractValidator& ContractValidator::operator=(ContractValidator&&) noexcept = default;

DecodeOutcome ContractValidator::parse(ContractKind kind, std::string_view utf8) {
  if (contains_nul(utf8)) return parse_failure(kind, "RAW_NUL", "Raw NUL bytes are not permitted.");
  if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xef &&
      static_cast<unsigned char>(utf8[1]) == 0xbb && static_cast<unsigned char>(utf8[2]) == 0xbf)
    return parse_failure(kind, "BOM_FORBIDDEN", "UTF-8 BOM is not permitted.");
  try {
    std::vector<std::set<std::string>> object_keys;
    Json::parser_callback_t callback = [&object_keys](int depth, Json::parse_event_t event, Json& parsed) {
      if ((event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) && depth >= 64) {
        throw ParseFailure{ParseProblem::nesting_too_deep};
      }
      if (event == Json::parse_event_t::object_start) {
        object_keys.emplace_back();
      } else if (event == Json::parse_event_t::object_end) {
        object_keys.pop_back();
      } else if (event == Json::parse_event_t::key && !object_keys.back().insert(parsed.get<std::string>()).second) {
        throw ParseFailure{ParseProblem::duplicate_key};
      }
      return true;
    };
    Json parsed = Json::parse(utf8.begin(), utf8.end(), callback, true, false);
    return validate(kind, parsed);
  }
  catch (const ParseFailure& failure) {
    return failure.problem == ParseProblem::duplicate_key
        ? parse_failure(kind, "DUPLICATE_KEY", "Object keys must be unique.")
        : parse_failure(kind, "NESTING_TOO_DEEP", "JSON nesting must not exceed 64.");
  }
  catch (const nlohmann::json::parse_error& error) {
    const std::string message = error.what();
    if (message.find("UTF-8") != std::string::npos || message.find("utf-8") != std::string::npos)
      return parse_failure(kind, "INVALID_UTF8", "Input must be well-formed UTF-8.");
    return parse_failure(kind, "INVALID_JSON", "Input is not valid JSON.");
  }
  catch (const nlohmann::json::out_of_range&) {
    return parse_failure(kind, "INVALID_JSON", "Numeric value is outside the supported JSON range.");
  }
}
DecodeOutcome ContractValidator::validate(ContractKind kind, const Json& value) {
  if (has_invalid_utf8(value))
    return ContractFailure{kind, {{ValidationStage::semantic, "", "INVALID_UTF8", "JSON strings must be well-formed UTF-8."}}};
  if (has_identity_nul(kind, value))
    return ContractFailure{kind, {{ValidationStage::semantic, "", "IDENTITY_NUL", "Identity strings must not contain NUL."}}};
  if (!finite_json(value))
    return ContractFailure{kind, {{ValidationStage::semantic, "", "NONFINITE_NUMBER", "JSON numbers must be finite."}}};
  const char* version_key = kind == ContractKind::settings ? "settings_version" :
      kind == ContractKind::protocol ? "protocol_version" : "schema_version";
  if (value.is_object() && value.contains(version_key) && value.at(version_key).is_number_integer() &&
      value.at(version_key) != 1) {
    return ContractFailure{kind, {{ValidationStage::schema, "/" + std::string(version_key),
        "SCHEMA_UNSUPPORTED", "Document version is unsupported."}}};
  }
  Issues handler;
  impl_->validators.at(id_for(name(kind))).validate(value, handler);
  if (!handler.items.empty()) return ContractFailure{kind, std::move(handler.items)};
  auto semantic = semantics(kind, value);
  if (!semantic.empty()) return ContractFailure{kind, std::move(semantic)};
  return ValidatedDocument(kind, value);
}

Json error_json(const Error& error) { return Json{{"code", error.code}, {"message", error.message}, {"details", error.details.is_object() ? error.details : Json::object()}, {"recoverable", error.recoverable}}; }
Error contract_error(const ContractFailure& failure) {
  Json issues = Json::array(); for (const auto& issue : failure.issues) issues.push_back({{"path", issue.path}, {"code", issue.code}, {"message", issue.message}});
  return {failure.kind == ContractKind::settings ? "INVALID_SETTINGS" : "INVALID_DOCUMENT", "Contract validation failed.", {{"issues", std::move(issues)}}, true};
}
}  // namespace spectrapack::io
