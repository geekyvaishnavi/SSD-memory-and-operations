#include "ftlsim/metrics/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

#include "ftlsim/nand/nand.hpp"

namespace ftlsim {

EraseStats compute_erase_stats(const Nand& nand) {
    EraseStats stats;
    const int blocks = nand.block_count();
    if (blocks == 0) {
        return stats;
    }

    stats.min = nand.block(0).erase_count();
    stats.max = stats.min;
    for (int i = 0; i < blocks; ++i) {
        const std::uint64_t count = nand.block(i).erase_count();
        stats.min = std::min(stats.min, count);
        stats.max = std::max(stats.max, count);
        stats.total += count;
    }
    stats.mean = static_cast<double>(stats.total) / blocks;

    double variance = 0.0;
    for (int i = 0; i < blocks; ++i) {
        const double delta = static_cast<double>(nand.block(i).erase_count()) - stats.mean;
        variance += delta * delta;
    }
    stats.stddev = std::sqrt(variance / blocks);
    return stats;
}

std::string format_erase_histogram(const Nand& nand, int buckets) {
    const EraseStats stats = compute_erase_stats(nand);
    if (buckets <= 0) {
        return {};
    }

    const std::uint64_t span = stats.max - stats.min;
    const double width = span == 0 ? 1.0 : static_cast<double>(span + 1) / buckets;

    std::vector<int> counts(static_cast<std::size_t>(buckets), 0);
    for (int i = 0; i < nand.block_count(); ++i) {
        const double offset = static_cast<double>(nand.block(i).erase_count() - stats.min);
        int bucket = static_cast<int>(offset / width);
        bucket = std::min(bucket, buckets - 1);
        ++counts[static_cast<std::size_t>(bucket)];
    }

    const int peak = *std::max_element(counts.begin(), counts.end());
    std::ostringstream out;
    for (int b = 0; b < buckets; ++b) {
        const std::uint64_t lo = stats.min + static_cast<std::uint64_t>(b * width);
        const std::uint64_t hi = stats.min + static_cast<std::uint64_t>((b + 1) * width) - 1;
        const int bar = peak == 0 ? 0 : counts[static_cast<std::size_t>(b)] * 40 / peak;
        out << "  [" << lo << ".." << std::max(lo, hi) << "] "
            << std::string(static_cast<std::size_t>(bar), '#') << " "
            << counts[static_cast<std::size_t>(b)] << "\n";
    }
    return out.str();
}

}  // namespace ftlsim
