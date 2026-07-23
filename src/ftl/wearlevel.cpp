#include "ftlsim/ftl/wearlevel.hpp"

#include "ftlsim/metrics/metrics.hpp"
#include "ftlsim/nand/nand.hpp"

namespace ftlsim {

std::string to_string(WearLevelingMode mode) {
    switch (mode) {
        case WearLevelingMode::None: return "none";
        case WearLevelingMode::Dynamic: return "dynamic";
        case WearLevelingMode::Static: return "static";
    }
    return "unknown";
}

bool wear_leveling_from_string(const std::string& name, WearLevelingMode& out) {
    if (name == "none" || name == "off") {
        out = WearLevelingMode::None;
        return true;
    }
    if (name == "dynamic") {
        out = WearLevelingMode::Dynamic;
        return true;
    }
    if (name == "static") {
        out = WearLevelingMode::Static;
        return true;
    }
    return false;
}

WearLeveler::WearLeveler(WearLevelingMode mode, int static_interval,
                         std::uint64_t static_threshold)
    : mode_(mode), static_interval_(static_interval), static_threshold_(static_threshold) {}

std::size_t WearLeveler::choose_free_block(const Nand& nand,
                                           const std::vector<int>& free_blocks) const {
    if (free_blocks.empty()) {
        return 0;
    }
    // Without leveling, recycle blocks in the order they were reclaimed. This
    // is the baseline the dynamic policy is measured against.
    if (mode_ == WearLevelingMode::None) {
        return 0;
    }

    // Dynamic leveling: always open the least-worn free block, so erases spread
    // across the pool instead of concentrating on whatever was freed last.
    std::size_t best = 0;
    std::uint64_t best_erases = nand.block(free_blocks[0]).erase_count();
    for (std::size_t i = 1; i < free_blocks.size(); ++i) {
        const std::uint64_t erases = nand.block(free_blocks[i]).erase_count();
        if (erases < best_erases) {
            best = i;
            best_erases = erases;
        }
    }
    return best;
}

bool WearLeveler::should_migrate(const Nand& nand, std::uint64_t host_writes) const {
    if (mode_ != WearLevelingMode::Static || static_interval_ <= 0) {
        return false;
    }
    if (host_writes - last_check_ < static_cast<std::uint64_t>(static_interval_)) {
        return false;
    }
    last_check_ = host_writes;

    // Only intervene once the pool has actually skewed: a wide erase-count
    // spread means cold data is shielding some blocks from wear.
    const EraseStats stats = compute_erase_stats(nand);
    return stats.spread() > static_threshold_;
}

int WearLeveler::select_cold_block(const Nand& nand, int open_block) const {
    int best = -1;
    std::uint64_t best_erases = 0;

    for (int i = 0; i < nand.block_count(); ++i) {
        if (i == open_block) {
            continue;
        }
        const Block& block = nand.block(i);
        // Needs to hold live data -- migrating an empty or fully invalid block
        // frees nothing that GC would not already reclaim.
        if (block.valid_pages() == 0) {
            continue;
        }
        if (best < 0 || block.erase_count() < best_erases) {
            best = i;
            best_erases = block.erase_count();
        }
    }
    return best;
}

}  // namespace ftlsim
