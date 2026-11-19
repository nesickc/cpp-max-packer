// The stdio service: newline-delimited protocol records in, protocol records out.
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <variant>

#include "fuzz_support.hpp"
#include "spectrapack/service/service.hpp"

namespace io = spectrapack::io;
namespace service = spectrapack::service;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static io::ContractValidator validator(io::ContractKind::protocol);
    const std::string input(reinterpret_cast<const char*>(data), size);
    // The shipped limits, and small ones so that a short input reaches the caps.
    const service::ServiceLimits cases[] = { {}, { 512, 4, 8192 } };
    for (const auto& limits : cases) {
        std::istringstream requests(input);
        std::ostringstream responses, diagnostics;
        const int code = service::run_stdio(requests, responses, diagnostics, { "fuzz", "fuzz" }, limits);
        // 0 end of input, 2 bad framing, 3 a limit; 4 is an internal failure.
        FUZZ_REQUIRE(code == 0 || code == 2 || code == 3);
        std::istringstream lines(responses.str());
        for (std::string line; std::getline(lines, line);) {
            const auto record = io::Json::parse(line);
            FUZZ_REQUIRE(std::holds_alternative<io::ValidatedDocument>(
                validator.validate(io::ContractKind::protocol, record)));
            FUZZ_REQUIRE(record.at("ok") == true || record.at("error").at("code") != "INTERNAL_ERROR");
        }
    }
    return 0;
}
