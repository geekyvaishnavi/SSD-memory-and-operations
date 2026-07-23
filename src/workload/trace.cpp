#include "ftlsim/workload/trace.hpp"

#include <algorithm>
#include <sstream>

#include "ftlsim/ftl/ftl.hpp"
#include "ftlsim/metrics/metrics.hpp"

namespace ftlsim {
namespace {

std::string json_string(const std::string& value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

}  // namespace

TraceReader::TraceReader(const std::string& path) : stream_(path) {}

bool TraceReader::next(Request& out) {
    std::string line;
    while (std::getline(stream_, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream parser(line);
        std::string first;
        parser >> first;
        if (first.empty()) {
            continue;
        }

        // Either "<op> <lpn>" or a bare LPN, which defaults to a write.
        RequestOp op = RequestOp::Write;
        std::int64_t lpn = 0;
        if (first == "W" || first == "w") {
            if (!(parser >> lpn)) continue;
        } else if (first == "R" || first == "r") {
            op = RequestOp::Read;
            if (!(parser >> lpn)) continue;
        } else {
            std::istringstream number(first);
            if (!(number >> lpn)) continue;
        }
        if (lpn < 0) {
            continue;
        }

        out.op = op;
        out.lpn = lpn;
        max_lpn_ = std::max(max_lpn_, lpn);
        return true;
    }
    return false;
}

StateRecorder::StateRecorder(const std::string& path, int snapshot_interval)
    : stream_(path), snapshot_interval_(std::max(1, snapshot_interval)) {}

void StateRecorder::write_config(const Ftl& ftl, const std::string& workload) {
    if (!is_open()) {
        return;
    }
    const FtlConfig& config = ftl.config();
    stream_ << "{\"type\":\"config\""
            << ",\"blocks\":" << config.blocks
            << ",\"pages_per_block\":" << config.pages_per_block
            << ",\"logical_pages\":" << ftl.logical_pages()
            << ",\"overprovisioning\":" << config.overprovisioning
            << ",\"gc_policy\":" << json_string(to_string(config.gc_policy))
            << ",\"wear_leveling\":" << json_string(to_string(config.wear_leveling))
            << ",\"workload\":" << json_string(workload)
            << ",\"snapshot_interval\":" << snapshot_interval_
            << "}\n";
}

void StateRecorder::record_gc_events(const std::vector<GcEvent>& events) {
    pending_gc_.insert(pending_gc_.end(), events.begin(), events.end());
}

void StateRecorder::record_wear_level_events(const std::vector<WearLevelEvent>& events) {
    pending_wl_.insert(pending_wl_.end(), events.begin(), events.end());
}

void StateRecorder::write_snapshot(const Ftl& ftl) {
    if (!is_open()) {
        return;
    }
    const Nand& nand = ftl.nand();
    const Metrics& metrics = ftl.metrics();
    const EraseStats erase = compute_erase_stats(nand);

    stream_ << "{\"type\":\"snapshot\",\"t\":" << ftl.timestep()
            << ",\"open_block\":" << ftl.open_block()
            << ",\"free_blocks\":" << ftl.free_blocks()
            << ",\"blocks\":[";
    for (int i = 0; i < nand.block_count(); ++i) {
        if (i > 0) {
            stream_ << ',';
        }
        // "e" = erase count, "p" = one char per page (f/v/i).
        stream_ << "{\"e\":" << nand.block(i).erase_count()
                << ",\"p\":\"" << nand.state_string(i) << "\"}";
    }
    stream_ << "],\"metrics\":{\"host_writes\":" << metrics.host_writes
            << ",\"gc_writes\":" << metrics.gc_writes
            << ",\"wl_writes\":" << metrics.wl_writes
            << ",\"erases\":" << metrics.erases
            << ",\"gc_runs\":" << metrics.gc_runs
            << ",\"wl_migrations\":" << metrics.wl_migrations
            << ",\"waf\":" << metrics.write_amplification()
            << ",\"erase_min\":" << erase.min
            << ",\"erase_max\":" << erase.max
            << ",\"erase_stddev\":" << erase.stddev
            << "},\"gc\":[";
    for (std::size_t i = 0; i < pending_gc_.size(); ++i) {
        const GcEvent& event = pending_gc_[i];
        if (i > 0) {
            stream_ << ',';
        }
        stream_ << "{\"t\":" << event.timestep
                << ",\"block\":" << event.victim_block
                << ",\"copied\":" << event.copied_pages
                << ",\"reclaimed\":" << event.reclaimed_pages
                << ",\"erase_count\":" << event.erase_count << "}";
    }
    stream_ << "],\"wl\":[";
    for (std::size_t i = 0; i < pending_wl_.size(); ++i) {
        const WearLevelEvent& event = pending_wl_[i];
        if (i > 0) {
            stream_ << ',';
        }
        stream_ << "{\"t\":" << event.timestep
                << ",\"block\":" << event.source_block
                << ",\"copied\":" << event.copied_pages
                << ",\"erase_count\":" << event.source_erase_count
                << ",\"max_erase_count\":" << event.max_erase_count << "}";
    }
    stream_ << "]}\n";

    pending_gc_.clear();
    pending_wl_.clear();
}

void StateRecorder::write_summary(const Ftl& ftl) {
    if (!is_open()) {
        return;
    }
    const Metrics& metrics = ftl.metrics();
    const EraseStats erase = compute_erase_stats(ftl.nand());
    stream_ << "{\"type\":\"summary\""
            << ",\"host_writes\":" << metrics.host_writes
            << ",\"physical_writes\":" << metrics.physical_writes()
            << ",\"gc_writes\":" << metrics.gc_writes
            << ",\"wl_writes\":" << metrics.wl_writes
            << ",\"erases\":" << metrics.erases
            << ",\"gc_runs\":" << metrics.gc_runs
            << ",\"wl_migrations\":" << metrics.wl_migrations
            << ",\"waf\":" << metrics.write_amplification()
            << ",\"erase_min\":" << erase.min
            << ",\"erase_max\":" << erase.max
            << ",\"erase_mean\":" << erase.mean
            << ",\"erase_stddev\":" << erase.stddev
            << "}\n";
}

}  // namespace ftlsim
