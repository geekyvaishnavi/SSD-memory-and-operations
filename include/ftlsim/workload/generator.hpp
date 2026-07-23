#pragma once

#include <cstdint>
#include <random>
#include <string>

namespace ftlsim {

enum class RequestOp { Read, Write };

struct Request {
    RequestOp op = RequestOp::Write;
    std::int64_t lpn = 0;
};

/// Sequential -- ascending LPNs, wrapping at the end of the address space.
/// Random     -- uniform over the address space.
/// Hotspot    -- `hot_ratio` of accesses land in `hot_fraction` of the space.
enum class WorkloadType { Sequential, Random, Hotspot };

std::string to_string(WorkloadType type);
bool workload_type_from_string(const std::string& name, WorkloadType& out);

struct WorkloadConfig {
    WorkloadType type = WorkloadType::Random;
    std::int64_t logical_pages = 0;
    std::uint64_t requests = 0;
    unsigned seed = 42;
    double hot_fraction = 0.10;  // share of the address space that is hot
    double hot_ratio = 0.90;     // share of accesses aimed at the hot region
    double read_ratio = 0.0;     // share of requests issued as reads
};

/// Synthetic workload source. Deterministic for a given seed.
class WorkloadGenerator {
public:
    explicit WorkloadGenerator(const WorkloadConfig& config);

    /// Fills `out` and returns true, or returns false once exhausted.
    bool next(Request& out);

    std::uint64_t issued() const { return issued_; }

private:
    std::int64_t next_lpn();

    WorkloadConfig config_;
    std::mt19937_64 rng_;
    std::uint64_t issued_ = 0;
    std::int64_t cursor_ = 0;
    std::int64_t hot_pages_ = 0;
};

}  // namespace ftlsim
