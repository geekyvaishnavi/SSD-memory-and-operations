#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ftlsim {

class Nand;

enum class GcPolicy { Greedy, CostBenefit };

std::string to_string(GcPolicy policy);
/// Parse "greedy" / "cost-benefit"; returns false on an unknown name.
bool gc_policy_from_string(const std::string& name, GcPolicy& out);

/// One garbage collection run, recorded for the visualization trace.
struct GcEvent {
    std::uint64_t timestep = 0;
    int victim_block = -1;
    int copied_pages = 0;      // valid pages relocated before the erase
    int reclaimed_pages = 0;   // invalid pages freed by the erase
    std::uint64_t erase_count = 0;  // erase count of the victim after erasing
};

/// Victim-selection strategy. Policies sit behind this interface so a new one
/// slots in without touching the GC loop in Ftl.
class VictimSelector {
public:
    virtual ~VictimSelector() = default;
    virtual const char* name() const = 0;
    /// Pick a block to reclaim, skipping `open_block` and clean blocks.
    /// Returns -1 when no candidate exists.
    virtual int select(const Nand& nand, int open_block, std::uint64_t now) const = 0;
};

/// Reclaims the block holding the most invalid pages: cheapest copy-out now,
/// blind to how recently the block was written.
class GreedySelector : public VictimSelector {
public:
    const char* name() const override { return "greedy"; }
    int select(const Nand& nand, int open_block, std::uint64_t now) const override;
};

/// Ranks blocks by (1 - u) * age / (2u), where u is the valid-page fraction and
/// age is time since last modification: prefers cold blocks whose remaining
/// valid data is unlikely to be invalidated on its own.
class CostBenefitSelector : public VictimSelector {
public:
    const char* name() const override { return "cost-benefit"; }
    int select(const Nand& nand, int open_block, std::uint64_t now) const override;
};

std::unique_ptr<VictimSelector> make_victim_selector(GcPolicy policy);

}  // namespace ftlsim
