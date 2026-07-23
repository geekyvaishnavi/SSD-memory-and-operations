#include "ftlsim/ftl/gc.hpp"

#include <limits>

#include "ftlsim/nand/nand.hpp"

namespace ftlsim {
namespace {

/// A block can be reclaimed if it holds data and is not the block currently
/// being programmed.
bool is_candidate(const Nand& nand, int index, int open_block) {
    return index != open_block && !nand.block(index).is_clean();
}

}  // namespace

std::string to_string(GcPolicy policy) {
    switch (policy) {
        case GcPolicy::Greedy: return "greedy";
        case GcPolicy::CostBenefit: return "cost-benefit";
    }
    return "unknown";
}

bool gc_policy_from_string(const std::string& name, GcPolicy& out) {
    if (name == "greedy") {
        out = GcPolicy::Greedy;
        return true;
    }
    if (name == "cost-benefit" || name == "costbenefit" || name == "cb") {
        out = GcPolicy::CostBenefit;
        return true;
    }
    return false;
}

int GreedySelector::select(const Nand& nand, int open_block, std::uint64_t /*now*/) const {
    int best = -1;
    int best_invalid = -1;
    std::uint64_t best_erases = 0;

    for (int i = 0; i < nand.block_count(); ++i) {
        if (!is_candidate(nand, i, open_block)) {
            continue;
        }
        const Block& block = nand.block(i);
        const int invalid = block.invalid_pages();
        // Tie-break toward the less-worn block so greedy does not repeatedly
        // hammer the same one when several are equally dirty.
        if (invalid > best_invalid ||
            (invalid == best_invalid && block.erase_count() < best_erases)) {
            best = i;
            best_invalid = invalid;
            best_erases = block.erase_count();
        }
    }
    return best;
}

int CostBenefitSelector::select(const Nand& nand, int open_block, std::uint64_t now) const {
    int best = -1;
    double best_score = -1.0;
    std::uint64_t best_erases = 0;

    for (int i = 0; i < nand.block_count(); ++i) {
        if (!is_candidate(nand, i, open_block)) {
            continue;
        }
        const Block& block = nand.block(i);
        const double capacity = static_cast<double>(block.pages_per_block());
        const double u = static_cast<double>(block.valid_pages()) / capacity;
        const double age = static_cast<double>(now - block.last_modified());

        double score;
        if (u <= 0.0) {
            // Nothing to copy out: always the best possible victim.
            score = std::numeric_limits<double>::max();
        } else {
            // Benefit (space freed x age) over cost (one read + one write of
            // every valid page).
            score = (1.0 - u) * age / (2.0 * u);
        }

        // Ties are common -- under a sequential workload many blocks go fully
        // invalid at once and all score identically. Breaking the tie by block
        // index would starve the high-numbered blocks, so break on wear
        // instead, matching the greedy policy.
        if (score > best_score || (score == best_score && block.erase_count() < best_erases)) {
            best = i;
            best_score = score;
            best_erases = block.erase_count();
        }
    }
    return best;
}

std::unique_ptr<VictimSelector> make_victim_selector(GcPolicy policy) {
    switch (policy) {
        case GcPolicy::Greedy:
            return std::make_unique<GreedySelector>();
        case GcPolicy::CostBenefit:
            return std::make_unique<CostBenefitSelector>();
    }
    return std::make_unique<GreedySelector>();
}

}  // namespace ftlsim
