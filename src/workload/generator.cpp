#include "ftlsim/workload/generator.hpp"

#include <algorithm>
#include <stdexcept>

namespace ftlsim {

std::string to_string(WorkloadType type) {
    switch (type) {
        case WorkloadType::Sequential: return "sequential";
        case WorkloadType::Random: return "random";
        case WorkloadType::Hotspot: return "hotspot";
    }
    return "unknown";
}

bool workload_type_from_string(const std::string& name, WorkloadType& out) {
    if (name == "sequential" || name == "seq") {
        out = WorkloadType::Sequential;
        return true;
    }
    if (name == "random" || name == "rand") {
        out = WorkloadType::Random;
        return true;
    }
    if (name == "hotspot" || name == "hot") {
        out = WorkloadType::Hotspot;
        return true;
    }
    return false;
}

WorkloadGenerator::WorkloadGenerator(const WorkloadConfig& config)
    : config_(config), rng_(config.seed) {
    if (config_.logical_pages <= 0) {
        throw std::invalid_argument("workload needs a positive address space");
    }
    hot_pages_ = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(static_cast<double>(config_.logical_pages) *
                                     config_.hot_fraction));
    hot_pages_ = std::min(hot_pages_, config_.logical_pages);
}

bool WorkloadGenerator::next(Request& out) {
    if (issued_ >= config_.requests) {
        return false;
    }
    ++issued_;

    out.lpn = next_lpn();
    out.op = RequestOp::Write;
    if (config_.read_ratio > 0.0) {
        std::uniform_real_distribution<double> coin(0.0, 1.0);
        if (coin(rng_) < config_.read_ratio) {
            out.op = RequestOp::Read;
        }
    }
    return true;
}

std::int64_t WorkloadGenerator::next_lpn() {
    switch (config_.type) {
        case WorkloadType::Sequential: {
            const std::int64_t lpn = cursor_;
            cursor_ = (cursor_ + 1) % config_.logical_pages;
            return lpn;
        }
        case WorkloadType::Random: {
            std::uniform_int_distribution<std::int64_t> pick(0, config_.logical_pages - 1);
            return pick(rng_);
        }
        case WorkloadType::Hotspot: {
            std::uniform_real_distribution<double> coin(0.0, 1.0);
            const bool hot = coin(rng_) < config_.hot_ratio || hot_pages_ >= config_.logical_pages;
            if (hot) {
                std::uniform_int_distribution<std::int64_t> pick(0, hot_pages_ - 1);
                return pick(rng_);
            }
            std::uniform_int_distribution<std::int64_t> pick(hot_pages_, config_.logical_pages - 1);
            return pick(rng_);
        }
    }
    return 0;
}

}  // namespace ftlsim
