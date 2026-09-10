#pragma once

#include <cstddef>
#include <istream>
#include <ostream>
#include <string>

#include <spectrapack/io/contracts.hpp>

namespace spectrapack::service {

struct ServiceLimits {
  std::size_t max_record_bytes = 1048576;
  std::size_t max_request_ids = 1024;
  std::size_t max_cache_bytes = 8 * 1048576;
};

struct BuildInfo { std::string engine_version; std::string engine_commit; };

io::Json capabilities(const BuildInfo& build);
int run_stdio(std::istream& input, std::ostream& protocol_output, std::ostream& diagnostics,
              const BuildInfo& build, ServiceLimits limits = {});

}  // namespace spectrapack::service
