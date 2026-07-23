#include "orchestrator.hpp"

#include "block_factory.hpp"
#include "metrics.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <yaml-cpp/yaml.h>

namespace line_scanner {

namespace {

constexpr size_t kMaxChannelCapacityPairs = 16;

size_t computeChannelCapacityPairs(size_t columns) {
    // Cap at 16 pairs to keep the ring buffer L1-resident even for huge m.
    // Floor at 1 pair for degenerate small m.
    const size_t base = std::max<size_t>(1, columns / 2);
    return std::min(base, kMaxChannelCapacityPairs);
}

// Map a block's declared output type to a concrete RingBuffer<T>.
std::shared_ptr<ChannelBase> makeChannel(ChannelType type, size_t cap) {
    switch (type) {
        case ChannelType::PixelPair:
            return std::make_shared<RingBuffer<PixelPair>>(cap);
        case ChannelType::LabelledElement:
            return std::make_shared<RingBuffer<LabelledElement>>(cap);
        case ChannelType::DefectRecord:
            return std::make_shared<RingBuffer<DefectRecord>>(cap);
        case ChannelType::None:
            return nullptr;
        default:
            throw std::runtime_error("orchestrator: channel type not yet wired");
    }
}

}  // namespace

Orchestrator::Orchestrator(PipelineConfig cfg) : cfg_(std::move(cfg)) {}

PipelineConfig Orchestrator::loadConfigFromFile(const std::string& yaml_path) {
    YAML::Node       root = YAML::LoadFile(yaml_path);
    const YAML::Node pn   = root["pipeline"];
    if (!pn)
        throw std::runtime_error("pipeline.yaml: missing 'pipeline' node");

    PipelineConfig cfg;
    cfg.columns       = pn["columns"].as<size_t>();
    cfg.threshold     = static_cast<uint8_t>(pn["threshold"].as<unsigned int>());
    cfg.cycle_time_ns = pn["cycle_time_ns"].as<uint64_t>();

    const std::string mode = pn["mode"].as<std::string>("csv");
    cfg.mode               = (mode == "rng") ? SourceMode::Rng : SourceMode::Csv;

    if (pn["csv_path"])
        cfg.csv_path = pn["csv_path"].as<std::string>();
    if (pn["output_path"])
        cfg.output_path = pn["output_path"].as<std::string>();
    if (pn["run_duration_ms"])
        cfg.run_duration_ms = pn["run_duration_ms"].as<uint64_t>();

    const YAML::Node blocks = pn["blocks"];
    if (!blocks || !blocks.IsSequence() || blocks.size() == 0) {
        throw std::runtime_error("pipeline.yaml: 'blocks' must be a non-empty sequence");
    }
    for (const auto& b : blocks) {
        BlockSpec spec;
        spec.name = b["name"].as<std::string>();
        if (b["skip_logging"])
            spec.skip_logging = b["skip_logging"].as<bool>();
        cfg.blocks.push_back(std::move(spec));
    }
    return cfg;
}

void Orchestrator::requestStop() {
    stop_requested_.store(true, std::memory_order_release);
}

void Orchestrator::waitForTermination() {
    using namespace std::chrono;
    const auto t0        = steady_clock::now();
    const bool has_timer = (cfg_.run_duration_ms > 0);
    while (!stop_requested_.load(std::memory_order_acquire)) {
        if (has_timer) {
            const auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
            if (static_cast<uint64_t>(elapsed_ms) >= cfg_.run_duration_ms)
                break;
        }

        // CSV mode: DataGen self-stops on EOF and closes its channel; that
        // propagates to downstream blocks which exit on their own. We
        // detect overall completion by checking if every block thread
        // finished (joinable but not yet joined).
        bool all_done = true;
        for (size_t i = 0; i < threads_.size(); ++i) {
            if (threads_[i].joinable()) {
                // Probe non-blocking: try_join is not in the std, so we
                // approximate by checking the first channel's state.
                all_done = false;
                break;
            }
        }
        if (all_done && !threads_.empty())
            break;

        // Also exit if the first channel has been closed and drained AND
        // the last block has stopped producing — heuristic via occupancy.
        if (!channels_.empty()) {
            const auto& first = channels_.front();
            if (first->isClosed() && first->isEmpty()) {
                // Give downstream a moment to finish current pair then exit.
                std::this_thread::sleep_for(milliseconds(20));
                break;
            }
        }

        std::this_thread::sleep_for(milliseconds(20));
    }
}

void Orchestrator::run() {
    // ---------------------------------------------------------------------
    // Loop 1: Create + Configure each block in a single pass.
    //   - Factory lookups are O(1); configure() reads only the fields each
    //     block cares about. Insertion order = pipeline topology order.
    // ---------------------------------------------------------------------
    for (const auto& spec : cfg_.blocks) {
        if (!BlockFactory::instance().isRegistered(spec.name)) {
            throw std::runtime_error("Unknown block in pipeline.yaml: " + spec.name);
        }
        auto blk = BlockFactory::instance().create(spec.name);
        blk->configure(cfg_);
        blocks_.push_back(std::move(blk));
    }

    // ---------------------------------------------------------------------
    // Loop 2: Wire ring-buffer channels between adjacent block pairs.
    //   - Must come AFTER all blocks exist (needs block[i+1]).
    //   - Same shared_ptr wired into block[i] output AND block[i+1] input
    //     so the two ends share one ring buffer.
    // ---------------------------------------------------------------------
    const size_t cap_pairs = computeChannelCapacityPairs(cfg_.columns);
    for (size_t i = 0; i + 1 < blocks_.size(); ++i) {
        auto ch = makeChannel(blocks_[i]->outputType(), cap_pairs);
        channels_.push_back(ch);
        blocks_[i]->setOutputChannel(ch);
        blocks_[i + 1]->setInputChannel(ch);
    }

    // ---------------------------------------------------------------------
    // Loop 3: Spawn one std::thread per block.
    //   - Must come AFTER all wiring is complete; otherwise an early thread
    //     could call write() before its output channel has been set.
    // ---------------------------------------------------------------------
    threads_.reserve(blocks_.size());
    for (auto& blk : blocks_) {
        Block* raw = blk.get();
        threads_.emplace_back([raw] { raw->run(); });
    }

    // Spawn the metrics logger thread separately. Its CPU and memory cost
    // are intentionally NOT counted against the pipeline budget (per spec).
    std::vector<Block*> raw_blocks;
    raw_blocks.reserve(blocks_.size());
    for (size_t i = 0; i < blocks_.size(); ++i)
        if (!cfg_.blocks[i].skip_logging)  // pipeline.yaml opt-out (e.g. terminal sink)
            raw_blocks.push_back(blocks_[i].get());

    MetricsLogger logger(raw_blocks, "metrics_log.csv", 50);
    logger.start();

    // Supervisor: wait for CSV EOF / timer / external SIGINT.
    waitForTermination();

    // ---------------------------------------------------------------------
    // Loop 4: Teardown — stop each block, join its thread, collect final
    // metrics, all in a single ordered pass per block.
    //   - stop() sets the block's atomic; run() exits on next loop check.
    //   - Stopping in pipeline order lets the "drain-then-stop" pattern
    //     propagate: DataGen closes its channel, FilterThreshold sees
    //     closed-and-empty, exits naturally.
    // ---------------------------------------------------------------------
    std::vector<NamedMetrics> finals;
    finals.reserve(blocks_.size());
    for (size_t i = 0; i < blocks_.size(); ++i) {
        blocks_[i]->stop();
    }
    for (size_t i = 0; i < threads_.size(); ++i) {
        if (threads_[i].joinable())
            threads_[i].join();
    }
    for (size_t i = 0; i < blocks_.size(); ++i) {
        if (cfg_.blocks[i].skip_logging)
            continue;  // opted out of metrics (pipeline.yaml)
        finals.push_back({blocks_[i]->name(), blocks_[i]->publishedMetrics()});
    }

    logger.stop();

    const size_t memory_budget_bytes = cfg_.columns;
    MetricsLogger::writeFinalReport(finals, "metrics_report.txt", cfg_.cycle_time_ns,
                                    memory_budget_bytes, 100.0);
}

}  // namespace line_scanner
