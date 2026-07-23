#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace line_scanner {

using PixelPair = std::pair<uint8_t, uint8_t>;

enum class SourceMode { Csv, Rng };

struct BlockSpec {
    std::string name;
    bool        skip_logging = false;  // exclude from metrics logger + final report (still runs)
};

struct PipelineConfig {
    size_t                 columns       = 0;
    uint8_t                threshold     = 128;
    uint64_t               cycle_time_ns = 1000;
    SourceMode             mode          = SourceMode::Csv;
    std::string            csv_path;
    std::string            output_path;      // terminal-block CSV dump (empty => <block name>.csv)
    uint64_t               run_duration_ms = 0;
    std::vector<BlockSpec> blocks;
};

}  // namespace line_scanner
