#include "ftlsim/ftl/ftl.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ftlsim {
namespace {

std::int64_t compute_logical_pages(const FtlConfig& config, const Nand& nand) {
    const double usable = static_cast<double>(nand.total_pages()) * (1.0 - config.overprovisioning);
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(usable));
}

}  // namespace

Ftl::Ftl(const FtlConfig& config)
    : config_(config),
      nand_(config.blocks, config.pages_per_block),
      mapping_(compute_logical_pages(config, nand_)),
      selector_(make_victim_selector(config.gc_policy)),
      wear_leveler_(config.wear_leveling, config.static_wl_interval, config.static_wl_threshold) {
    if (config_.overprovisioning < 0.0 || config_.overprovisioning >= 1.0) {
        throw std::invalid_argument("overprovisioning must be in [0, 1)");
    }
    // The reserve has to be at least two blocks: one so the host write can open
    // a fresh block, and one so a GC copy-out always has somewhere to land.
    config_.gc_free_block_threshold = std::max(2, config_.gc_free_block_threshold);
    if (config_.gc_free_block_threshold >= config_.blocks) {
        throw std::invalid_argument("too few blocks for the requested GC reserve");
    }

    free_blocks_.reserve(static_cast<std::size_t>(config_.blocks));
    for (int i = 0; i < config_.blocks; ++i) {
        free_blocks_.push_back(i);
    }
}

void Ftl::write(std::int64_t lpn) {
    if (lpn < 0 || lpn >= mapping_.logical_pages()) {
        throw std::out_of_range("write to logical page outside the exported capacity");
    }

    ++timestep_;
    ensure_free_space();

    const std::int64_t previous = mapping_.lookup(lpn);
    const std::int64_t ppn = allocate_page(lpn);
    // Out-of-place update: the superseded copy becomes garbage for GC to find.
    invalidate_ppn(previous);
    mapping_.map(lpn, ppn);
    ++metrics_.host_writes;

    maybe_static_wear_level();
}

std::int64_t Ftl::read(std::int64_t lpn) {
    ++metrics_.host_reads;
    const std::int64_t ppn = mapping_.lookup(lpn);
    if (ppn == kInvalidPpn) {
        ++metrics_.read_misses;
    }
    return ppn;
}

std::vector<GcEvent> Ftl::take_gc_events() {
    return std::exchange(gc_events_, {});
}

std::vector<WearLevelEvent> Ftl::take_wear_level_events() {
    return std::exchange(wl_events_, {});
}

void Ftl::ensure_free_space() {
    while (static_cast<int>(free_blocks_.size()) < config_.gc_free_block_threshold) {
        if (!collect_garbage()) {
            break;
        }
    }
}

bool Ftl::collect_garbage() {
    const int victim = selector_->select(nand_, open_block_, timestep_);
    if (victim < 0) {
        return false;
    }

    Block& block = nand_.block(victim);
    const int reclaimed = block.invalid_pages();
    // Reclaiming an all-valid block costs a full copy and frees nothing; give
    // up rather than spin.
    if (reclaimed == 0) {
        return false;
    }

    relocating_ = true;
    int copied = 0;
    for (int i = 0; i < block.pages_per_block(); ++i) {
        if (!block.page(i).is_valid()) {
            continue;
        }
        const std::int64_t lpn = block.page(i).lpn();
        const std::int64_t new_ppn = allocate_page(lpn);
        mapping_.map(lpn, new_ppn);
        nand_.block(victim).invalidate(i, timestep_);
        ++metrics_.gc_writes;
        ++copied;
    }
    relocating_ = false;

    nand_.erase_block(victim, timestep_);
    free_blocks_.push_back(victim);
    ++metrics_.erases;
    ++metrics_.gc_runs;

    GcEvent event;
    event.timestep = timestep_;
    event.victim_block = victim;
    event.copied_pages = copied;
    event.reclaimed_pages = reclaimed;
    event.erase_count = nand_.block(victim).erase_count();
    gc_events_.push_back(event);
    return true;
}

void Ftl::maybe_static_wear_level() {
    if (!wear_leveler_.should_migrate(nand_, metrics_.host_writes)) {
        return;
    }

    ensure_free_space();
    const int source = wear_leveler_.select_cold_block(nand_, open_block_);
    if (source < 0) {
        return;
    }

    const EraseStats stats = compute_erase_stats(nand_);
    // Only worth doing if this block really is one of the cold ones.
    if (nand_.block(source).erase_count() >= stats.max) {
        return;
    }

    relocating_ = true;
    int copied = 0;
    for (int i = 0; i < nand_.block(source).pages_per_block(); ++i) {
        if (!nand_.block(source).page(i).is_valid()) {
            continue;
        }
        const std::int64_t lpn = nand_.block(source).page(i).lpn();
        const std::int64_t new_ppn = allocate_page(lpn);
        mapping_.map(lpn, new_ppn);
        nand_.block(source).invalidate(i, timestep_);
        ++metrics_.wl_writes;
        ++copied;
    }
    relocating_ = false;

    WearLevelEvent event;
    event.timestep = timestep_;
    event.source_block = source;
    event.copied_pages = copied;
    event.source_erase_count = nand_.block(source).erase_count();
    event.max_erase_count = stats.max;

    // Cold data now lives elsewhere, so this block can re-enter the allocation
    // pool and start taking its share of erases.
    nand_.erase_block(source, timestep_);
    free_blocks_.push_back(source);
    ++metrics_.erases;
    ++metrics_.wl_migrations;
    wl_events_.push_back(event);
}

std::int64_t Ftl::allocate_page(std::int64_t lpn) {
    if (open_block_ < 0 || nand_.block(open_block_).is_full()) {
        open_new_block();
    }
    const std::int64_t ppn = nand_.program(open_block_, lpn, timestep_);
    if (ppn == kInvalidPpn) {
        throw std::runtime_error("open block rejected a program");
    }
    return ppn;
}

void Ftl::open_new_block() {
    if (free_blocks_.empty()) {
        if (relocating_) {
            // The GC reserve is sized so a copy-out always has a landing block.
            throw std::runtime_error("out of free blocks during relocation");
        }
        if (!collect_garbage()) {
            throw std::runtime_error("device is full: no reclaimable block");
        }
    }

    const std::size_t pick = wear_leveler_.choose_free_block(nand_, free_blocks_);
    open_block_ = free_blocks_[pick];
    free_blocks_.erase(free_blocks_.begin() + static_cast<std::ptrdiff_t>(pick));
}

void Ftl::invalidate_ppn(std::int64_t ppn) {
    if (ppn == kInvalidPpn) {
        return;
    }
    nand_.invalidate(ppn, timestep_);
}

}  // namespace ftlsim
