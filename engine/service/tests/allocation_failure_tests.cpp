#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <new>
#include <sstream>
#include <streambuf>
#include <string_view>

#include <spectrapack/service/service.hpp>

namespace {
std::atomic_bool fail_allocations{false};

class AllocationFailureScope final {
 public:
  AllocationFailureScope() { fail_allocations.store(true); }
  ~AllocationFailureScope() { fail_allocations.store(false); }
  AllocationFailureScope(const AllocationFailureScope&) = delete;
  AllocationFailureScope& operator=(const AllocationFailureScope&) = delete;
};

class FixedBuffer final : public std::streambuf {
 public:
  std::string_view view() const { return {storage_, used_}; }
 protected:
  std::streamsize xsputn(const char* text, std::streamsize count) override {
    const auto available = static_cast<std::streamsize>(sizeof(storage_) - used_);
    const auto written = count < available ? count : available;
    for (std::streamsize index = 0; index < written; ++index) storage_[used_++] = text[index];
    return written;
  }
  int overflow(int character) override {
    if (character == traits_type::eof() || used_ == sizeof(storage_)) return traits_type::eof();
    storage_[used_++] = static_cast<char>(character);
    return character;
  }
 private:
  char storage_[512]{};
  std::size_t used_ = 0;
};
}

void* operator new(std::size_t size) {
  if (fail_allocations.load()) throw std::bad_alloc();
  if (void* value = std::malloc(size)) return value;
  throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { std::free(value); }

TEST_CASE("sustained allocation failure emits allocation-free emergency record", "[DATA-01][service]") {
  std::istringstream input("{\"protocol_version\":1}\n");
  FixedBuffer buffer;
  std::ostream output(&buffer);
  std::ostringstream diagnostics;
  const spectrapack::service::BuildInfo build{"test", "commit"};
  int code = 0;
  {
    const AllocationFailureScope failure;
    code = spectrapack::service::run_stdio(input, output, diagnostics, build);
  }
  REQUIRE(code == 3);
  REQUIRE(buffer.view().find("MEMORY_LIMIT") != std::string_view::npos);
}
