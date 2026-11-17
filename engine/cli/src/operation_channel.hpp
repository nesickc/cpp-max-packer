#pragma once

#include <spectrapack/io/contracts.hpp>
#include <spectrapack/runtime/operation_control.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>

namespace spectrapack::cli {

// Requests a stop when the marker file appears. A marker that cannot be observed
// (its directory is gone, or it names something other than a regular file) also
// requests a stop and is reported by error().
class StopMarkerWatcher final {
public:
    StopMarkerWatcher(const std::optional<std::filesystem::path>& path, std::shared_ptr<std::stop_source> source)
    {
        if (!path) {
            return;
        }
        const auto observe = [this, file = *path, source] {
            std::error_code error;
            const auto parent = std::filesystem::status(file.parent_path().empty() ? "." : file.parent_path(), error);
            if (!error && !std::filesystem::is_directory(parent)) {
                error = std::make_error_code(std::errc::not_a_directory);
            }
            bool marked = false;
            if (!error) {
                const auto status = std::filesystem::symlink_status(file, error);
                if (error == std::errc::no_such_file_or_directory) {
                    error.clear();
                }
                else if (!error && std::filesystem::exists(status)) {
                    if (!std::filesystem::is_regular_file(status)) {
                        error = std::make_error_code(std::errc::invalid_argument);
                    }
                    else {
                        marked = true;
                    }
                }
            }
            if (error) {
                error_.store(error.value() != 0 ? error.value() : -1);
            }
            if (marked || error) {
                source->request_stop();
            }
            return marked || bool(error);
        };
        if (observe()) {
            return;
        }
        watcher_ = std::jthread([observe, source](std::stop_token done) {
            while (!done.stop_requested() && !source->stop_requested()) {
                if (observe()) {
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    // Nonzero once the marker could not be observed.
    [[nodiscard]] int error() const noexcept { return error_.load(); }
    // Stops watching and returns error().
    [[nodiscard]] int finish()
    {
        watcher_.request_stop();
        if (watcher_.joinable()) {
            watcher_.join();
        }
        return error_.load();
    }

private:
    std::atomic_int error_ {};
    std::jthread watcher_;
};

// Prints `{"progress":{"stage":...,"done":n,"total":m}}` lines on stdout before a
// command's terminal record: a new stage at once, otherwise at most four lines a
// second. done and total appear only for a stage with a known size.
class ProgressPrinter final {
public:
    void attach(runtime::OperationControl& control) noexcept
    {
        control.progress_sink = &ProgressPrinter::print;
        control.progress_context = this;
    }

private:
    static void print(void* raw, const char* stage, std::uint64_t done, std::uint64_t total) noexcept
    {
        auto& self = *static_cast<ProgressPrinter*>(raw);
        try {
            const std::lock_guard lock(self.mutex_);
            const auto now = std::chrono::steady_clock::now();
            if (self.stage_ == stage && now - self.printed_ < std::chrono::milliseconds(250)) {
                return;
            }
            self.stage_ = stage;
            self.printed_ = now;
            io::Json progress { { "stage", stage } };
            if (total != 0) {
                progress["done"] = done;
                progress["total"] = total;
            }
            std::cout << io::Json { { "progress", std::move(progress) } }.dump() << '\n' << std::flush;
        }
        catch (...) {
        }
    }

    std::mutex mutex_;
    std::string stage_;
    std::chrono::steady_clock::time_point printed_ {};
};

}  // namespace spectrapack::cli
