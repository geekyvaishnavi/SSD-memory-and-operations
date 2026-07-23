#pragma once

#include <cstdint>
#include <string>

namespace ftlsim {

class Nand;

/// Counters accumulated over a run. Physical writes are split by origin so the
/// write amplification factor can be attributed to GC vs wear leveling.
struct Metrics {
    std::uint64_t host_writes = 0;    // page programs requested by the host
    std::uint64_t host_reads = 0;
    std::uint64_t read_misses = 0;    // reads of never-written logical pages
    std::uint64_t gc_writes = 0;      // valid-page copy-outs during GC
    std::uint64_t wl_writes = 0;      // copy-outs during static wear leveling
    std::uint64_t erases = 0;
    std::uint64_t gc_runs = 0;
    std::uint64_t wl_migrations = 0;

    std::uint64_t physical_writes() const { return host_writes + gc_writes + wl_writes; }

    /// WAF = physical page programs / host page programs. 1.0 is ideal.
    double write_amplification() const {
        return host_writes == 0 ? 0.0
                                : static_cast<double>(physical_writes()) /
                                      static_cast<double>(host_writes);
    }
};

/// Distribution of erase counts across blocks -- the wear-leveling scorecard.
struct EraseStats {
    std::uint64_t min = 0;
    std::uint64_t max = 0;
    std::uint64_t total = 0;
    double mean = 0.0;
    double stddev = 0.0;

    std::uint64_t spread() const { return max - min; }
};

EraseStats compute_erase_stats(const Nand& nand);

/// Human-readable histogram of erase counts, one bucket per line.
std::string format_erase_histogram(const Nand& nand, int buckets = 10);

}  // namespace ftlsim
