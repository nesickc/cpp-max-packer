// JSON contract readers: every document kind the engine parses from a file or a pipe.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "fuzz_support.hpp"
#include "spectrapack/io/contracts.hpp"

namespace io = spectrapack::io;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static io::ContractValidator validator;
    static constexpr io::ContractKind kinds[] = { io::ContractKind::settings,          io::ContractKind::assets,
                                                  io::ContractKind::results,           io::ContractKind::protocol,
                                                  io::ContractKind::benchmark_summary, io::ContractKind::desktop,
                                                  io::ContractKind::desktop_runtime };
    // The command-line readers bound their diagnostics like this.
    static constexpr io::ContractDiagnosticLimits limits { 16, 256 };
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    for (const auto kind : kinds) {
        const auto outcome = validator.parse(kind, text, limits);
        if (const auto* document = std::get_if<io::ValidatedDocument>(&outcome)) {
            // An accepted document stays accepted once it is written and read back.
            const auto again = validator.parse(kind, document->value().dump(), limits);
            FUZZ_REQUIRE(std::holds_alternative<io::ValidatedDocument>(again));
            continue;
        }
        const auto& failure = std::get<io::ContractFailure>(outcome);
        FUZZ_REQUIRE(!failure.issues.empty() && failure.issues.size() <= limits.max_issues);
        for (const auto& issue : failure.issues) {
            FUZZ_REQUIRE(issue.path.size() <= limits.max_string_bytes &&
                         issue.message.size() <= limits.max_string_bytes);
        }
        // A rejection is always reportable.
        (void)io::error_json(io::contract_error(failure)).dump();
    }
    return 0;
}
