#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "ftlsim/ftl/gc.hpp"
#include "ftlsim/ftl/wearlevel.hpp"
#include "ftlsim/workload/generator.hpp"

namespace ftlsim {

class Ftl;

/// Reads a workload trace file. One request per line:
///   "W 1234", "R 1234", or a bare "1234" (treated as a write).
/// Blank lines and lines starting with '#' are ignored.
class TraceReader {
public:
    explicit TraceReader(const std::string& path);

    bool is_open() const { return stream_.is_open(); }
    bool next(Request& out);

    /// Highest LPN seen so far -- used to size the device when replaying.
    std::int64_t max_lpn() const { return max_lpn_; }

private:
    std::ifstream stream_;
    std::int64_t max_lpn_ = -1;
};

/// Writes the JSONL visualization trace consumed by web/index.html:
///   line 1  -- {"type":"config", ...}
///   line n  -- {"type":"snapshot","t":...,"blocks":[...],"metrics":{...},"events":[...]}
///   last    -- {"type":"summary", ...}
/// Snapshots carry a per-block page-state string, so the frontend can replay
/// the array without re-running the simulator.
class StateRecorder {
public:
    StateRecorder(const std::string& path, int snapshot_interval);

    bool is_open() const { return stream_.is_open(); }
    int snapshot_interval() const { return snapshot_interval_; }

    void write_config(const Ftl& ftl, const std::string& workload);
    void record_gc_events(const std::vector<GcEvent>& events);
    void record_wear_level_events(const std::vector<WearLevelEvent>& events);
    /// Emit a snapshot plus any events buffered since the previous one.
    void write_snapshot(const Ftl& ftl);
    void write_summary(const Ftl& ftl);

private:
    std::ofstream stream_;
    int snapshot_interval_;
    std::vector<GcEvent> pending_gc_;
    std::vector<WearLevelEvent> pending_wl_;
};

}  // namespace ftlsim
