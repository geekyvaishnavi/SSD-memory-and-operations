#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ftlsim {

class Nand;

/// None    -- allocate free blocks in FIFO order, no leveling.
/// Dynamic -- bias allocation of free blocks toward the least-worn block.
/// Static  -- dynamic, plus periodic migration of cold data out of low-erase
///            blocks so read-mostly data stops shielding them from wear.
enum class WearLevelingMode { None, Dynamic, Static };

std::string to_string(WearLevelingMode mode);
bool wear_leveling_from_string(const std::string& name, WearLevelingMode& out);

/// Migration performed by static wear leveling, recorded for the trace.
struct WearLevelEvent {
    std::uint64_t timestep = 0;
    int source_block = -1;
    int copied_pages = 0;
    std::uint64_t source_erase_count = 0;
    std::uint64_t max_erase_count = 0;
};

class WearLeveler {
public:
    WearLeveler(WearLevelingMode mode, int static_interval, std::uint64_t static_threshold);

    WearLevelingMode mode() const { return mode_; }

    /// Index into `free_blocks` of the block to open next.
    std::size_t choose_free_block(const Nand& nand, const std::vector<int>& free_blocks) const;

    /// True when static leveling should run now (interval elapsed and the
    /// erase-count spread exceeds the threshold).
    bool should_migrate(const Nand& nand, std::uint64_t host_writes) const;

    /// Least-worn block still holding valid data, or -1 if none qualifies.
    int select_cold_block(const Nand& nand, int open_block) const;

    void note_migration(std::uint64_t host_writes) { last_check_ = host_writes; }

private:
    WearLevelingMode mode_;
    int static_interval_;
    std::uint64_t static_threshold_;
    mutable std::uint64_t last_check_ = 0;
};

}  // namespace ftlsim
