#pragma once

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace spectrapack::io {

using Json = nlohmann::json;

enum class ContractKind { settings, assets, results, protocol, benchmark_summary };
enum class ValidationStage { parse, schema, semantic };

struct ValidationIssue {
  ValidationStage stage;
  std::string path;
  std::string code;
  std::string message;
};

struct ContractFailure {
  ContractKind kind;
  std::vector<ValidationIssue> issues;
};

class ValidatedDocument {
 public:
  ContractKind kind() const noexcept;
  const Json& value() const noexcept;

 private:
  friend class ContractValidator;
  ValidatedDocument(ContractKind kind, Json value);
  ContractKind kind_;
  Json value_;
};

using DecodeOutcome = std::variant<ValidatedDocument, ContractFailure>;

class ContractValidator {
 public:
  ContractValidator();
  ~ContractValidator();
  ContractValidator(ContractValidator&&) noexcept;
  ContractValidator& operator=(ContractValidator&&) noexcept;
  ContractValidator(const ContractValidator&) = delete;
  ContractValidator& operator=(const ContractValidator&) = delete;

  DecodeOutcome parse(ContractKind kind, std::string_view utf8);
  DecodeOutcome validate(ContractKind kind, const Json& value);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

struct Error {
  std::string code;
  std::string message;
  Json details;
  bool recoverable;
};

Json error_json(const Error& error);
Error contract_error(const ContractFailure& failure);

}  // namespace spectrapack::io
