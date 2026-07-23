#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "ftlsim/ftl/ftl.hpp"
#include "ftlsim/metrics/metrics.hpp"
#include "ftlsim/workload/generator.hpp"
#include "ftlsim/workload/trace.hpp"

namespace {

using namespace ftlsim;

struct Options {
    FtlConfig ftl;
    WorkloadConfig workload;
    std::string trace_path;       // input workload trace
    std::string dump_path;        // output JSONL visualization trace
    int dump_interval = 100;
    bool prefill = false;
    bool histogram = false;
    bool quiet = false;
};

void print_usage() {
    std::cout <<
        "ftlsim -- Flash Translation Layer simulator\n"
        "\n"
        "Usage: ftlsim [options]\n"
        "\n"
        "Device geometry:\n"
        "  --blocks N              NAND blocks (default 128)\n"
        "  --pages-per-block N     pages per block (default 64)\n"
        "  --overprovisioning F    fraction hidden from the host (default 0.10)\n"
        "\n"
        "FTL policy:\n"
        "  --gc-policy P           greedy | cost-benefit (default greedy)\n"
        "  --wear-leveling M       none | dynamic | static (default dynamic)\n"
        "  --gc-threshold N        clean blocks kept in reserve (default 3)\n"
        "  --static-wl-interval N  host writes between static WL checks (default 1000)\n"
        "  --static-wl-threshold N erase-count spread that triggers static WL (default 8)\n"
        "\n"
        "Workload:\n"
        "  --workload W            sequential | random | hotspot (default random)\n"
        "  --pages N               number of host requests to issue (default 10000)\n"
        "  --seed N                RNG seed (default 42)\n"
        "  --hot-fraction F        share of the address space that is hot (default 0.10)\n"
        "  --hot-ratio F           share of accesses aimed at the hot region (default 0.90)\n"
        "  --read-ratio F          share of requests issued as reads (default 0.0)\n"
        "  --trace FILE            replay a workload trace instead of generating one\n"
        "  --prefill               write every logical page once before measuring,\n"
        "                          so the device starts full and holds cold data\n"
        "\n"
        "Output:\n"
        "  --dump FILE             write the JSONL visualization trace\n"
        "  --dump-interval N       host writes between snapshots (default 100)\n"
        "  --histogram             print the erase-count histogram\n"
        "  --quiet                 print only the summary lines\n"
        "  -h, --help              show this help\n";
}

/// Fetches the value following `--flag`, or reports the missing argument.
bool take_value(int argc, char** argv, int& i, const std::string& flag, std::string& out) {
    if (i + 1 >= argc) {
        std::cerr << "error: " << flag << " requires a value\n";
        return false;
    }
    out = argv[++i];
    return true;
}

bool parse_args(int argc, char** argv, Options& options, bool& should_exit) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;

        if (arg == "-h" || arg == "--help") {
            print_usage();
            should_exit = true;
            return true;
        } else if (arg == "--blocks") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.ftl.blocks = std::stoi(value);
        } else if (arg == "--pages-per-block") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.ftl.pages_per_block = std::stoi(value);
        } else if (arg == "--overprovisioning" || arg == "--op") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.ftl.overprovisioning = std::stod(value);
        } else if (arg == "--gc-policy") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            if (!gc_policy_from_string(value, options.ftl.gc_policy)) {
                std::cerr << "error: unknown gc policy '" << value << "'\n";
                return false;
            }
        } else if (arg == "--wear-leveling" || arg == "--wl") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            if (!wear_leveling_from_string(value, options.ftl.wear_leveling)) {
                std::cerr << "error: unknown wear-leveling mode '" << value << "'\n";
                return false;
            }
        } else if (arg == "--gc-threshold") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.ftl.gc_free_block_threshold = std::stoi(value);
        } else if (arg == "--static-wl-interval") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.ftl.static_wl_interval = std::stoi(value);
        } else if (arg == "--static-wl-threshold") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.ftl.static_wl_threshold = std::stoull(value);
        } else if (arg == "--workload") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            if (!workload_type_from_string(value, options.workload.type)) {
                std::cerr << "error: unknown workload '" << value << "'\n";
                return false;
            }
        } else if (arg == "--pages" || arg == "--ops" || arg == "--requests") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.workload.requests = std::stoull(value);
        } else if (arg == "--seed") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.workload.seed = static_cast<unsigned>(std::stoul(value));
        } else if (arg == "--hot-fraction") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.workload.hot_fraction = std::stod(value);
        } else if (arg == "--hot-ratio") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.workload.hot_ratio = std::stod(value);
        } else if (arg == "--read-ratio") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.workload.read_ratio = std::stod(value);
        } else if (arg == "--trace") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.trace_path = value;
        } else if (arg == "--dump") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.dump_path = value;
        } else if (arg == "--dump-interval") {
            if (!take_value(argc, argv, i, arg, value)) return false;
            options.dump_interval = std::stoi(value);
        } else if (arg == "--prefill") {
            options.prefill = true;
        } else if (arg == "--histogram") {
            options.histogram = true;
        } else if (arg == "--quiet") {
            options.quiet = true;
        } else {
            std::cerr << "error: unknown option '" << arg << "' (try --help)\n";
            return false;
        }
    }
    return true;
}

void apply(Ftl& ftl, const Request& request, std::int64_t logical_pages) {
    // Trace files may address a larger device than the one configured; fold
    // them into the exported capacity rather than rejecting the run.
    const std::int64_t lpn = request.lpn % logical_pages;
    if (request.op == RequestOp::Read) {
        ftl.read(lpn);
    } else {
        ftl.write(lpn);
    }
}

void print_summary(const Ftl& ftl, const Options& options) {
    const Metrics& metrics = ftl.metrics();
    const EraseStats erase = compute_erase_stats(ftl.nand());

    std::cout << "Total writes: " << metrics.host_writes << "\n";
    std::cout << "Total erases: " << metrics.erases << "\n";
    std::cout << "Write Amplification Factor: " << std::fixed << std::setprecision(2)
              << metrics.write_amplification() << "\n";
    std::cout << "Max erase count: " << erase.max << " | Min erase count: " << erase.min;
    if (options.ftl.wear_leveling != WearLevelingMode::None) {
        std::cout << " (wear leveled)";
    }
    std::cout << "\n";

    if (options.quiet) {
        return;
    }

    std::cout << "\nBreakdown\n"
              << "  Physical writes:   " << metrics.physical_writes()
              << " (host " << metrics.host_writes
              << " + gc " << metrics.gc_writes
              << " + wear leveling " << metrics.wl_writes << ")\n"
              << "  GC runs:           " << metrics.gc_runs << "\n"
              << "  WL migrations:     " << metrics.wl_migrations << "\n"
              << "  Erase spread:      " << erase.spread()
              << " (mean " << std::setprecision(2) << erase.mean
              << ", stddev " << erase.stddev << ")\n";
    if (metrics.host_reads > 0) {
        std::cout << "  Reads:             " << metrics.host_reads
                  << " (" << metrics.read_misses << " unmapped)\n";
    }

    if (options.histogram) {
        std::cout << "\nErase-count distribution\n" << format_erase_histogram(ftl.nand());
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    options.workload.requests = 10000;

    bool should_exit = false;
    if (!parse_args(argc, argv, options, should_exit)) {
        return 1;
    }
    if (should_exit) {
        return 0;
    }

    try {
        Ftl ftl(options.ftl);
        const std::int64_t logical_pages = ftl.logical_pages();
        options.workload.logical_pages = logical_pages;

        if (options.prefill) {
            // Fill the whole address space, then drop the counters: the fill
            // itself is sequential and would flatter the WAF, and what we
            // actually want from it is a device holding cold data that the
            // measured workload never touches.
            for (std::int64_t lpn = 0; lpn < logical_pages; ++lpn) {
                ftl.write(lpn);
            }
            ftl.reset_metrics();
            ftl.take_gc_events();
            ftl.take_wear_level_events();
        }

        std::unique_ptr<StateRecorder> recorder;
        if (!options.dump_path.empty()) {
            recorder = std::make_unique<StateRecorder>(options.dump_path, options.dump_interval);
            if (!recorder->is_open()) {
                std::cerr << "error: cannot write '" << options.dump_path << "'\n";
                return 1;
            }
            const std::string label =
                options.trace_path.empty() ? to_string(options.workload.type) : options.trace_path;
            recorder->write_config(ftl, label);
            recorder->write_snapshot(ftl);
        }

        if (!options.quiet) {
            std::cout << "Device: " << options.ftl.blocks << " blocks x "
                      << options.ftl.pages_per_block << " pages"
                      << " (" << logical_pages << " logical pages, "
                      << std::fixed << std::setprecision(0)
                      << options.ftl.overprovisioning * 100 << "% OP)\n"
                      << "Policy: gc=" << to_string(options.ftl.gc_policy)
                      << " wear-leveling=" << to_string(options.ftl.wear_leveling) << "\n"
                      << "Workload: "
                      << (options.trace_path.empty() ? to_string(options.workload.type)
                                                     : options.trace_path)
                      << "\n\n";
        }

        std::unique_ptr<TraceReader> reader;
        std::unique_ptr<WorkloadGenerator> generator;
        if (!options.trace_path.empty()) {
            reader = std::make_unique<TraceReader>(options.trace_path);
            if (!reader->is_open()) {
                std::cerr << "error: cannot read '" << options.trace_path << "'\n";
                return 1;
            }
        } else {
            generator = std::make_unique<WorkloadGenerator>(options.workload);
        }

        Request request;
        std::uint64_t applied = 0;
        while (reader ? reader->next(request) : generator->next(request)) {
            apply(ftl, request, logical_pages);
            ++applied;

            if (recorder) {
                recorder->record_gc_events(ftl.take_gc_events());
                recorder->record_wear_level_events(ftl.take_wear_level_events());
                if (applied % static_cast<std::uint64_t>(recorder->snapshot_interval()) == 0) {
                    recorder->write_snapshot(ftl);
                }
            }
        }

        if (recorder) {
            recorder->record_gc_events(ftl.take_gc_events());
            recorder->record_wear_level_events(ftl.take_wear_level_events());
            // Skip the closing snapshot when the loop just wrote one on its
            // final iteration, or the replay ends on a duplicate frame.
            const bool already_written =
                applied > 0 &&
                applied % static_cast<std::uint64_t>(recorder->snapshot_interval()) == 0;
            if (!already_written) {
                recorder->write_snapshot(ftl);
            }
            recorder->write_summary(ftl);
        }

        print_summary(ftl, options);
        if (recorder && !options.quiet) {
            std::cout << "\nTrace written to " << options.dump_path
                      << " (open web/index.html to replay)\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
    return 0;
}
