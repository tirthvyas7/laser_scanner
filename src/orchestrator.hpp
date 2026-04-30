#pragma once

#include "block.hpp"
#include "metrics.hpp"
#include "pipeline_config.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace line_scanner {

// Coordinates the full pipeline lifecycle. Reads config, instantiates
// blocks via the factory, wires N-1 ring buffer channels between them,
// spawns one std::thread per block plus a separate logger thread,
// supervises termination, and writes the final report.
class Orchestrator {
   public:
    explicit Orchestrator(PipelineConfig cfg);

    // Loads `pipeline.yaml` from the given path into a PipelineConfig.
    static PipelineConfig loadConfigFromFile(const std::string& yaml_path);

    // External shutdown trigger (e.g. SIGINT handler in main).
    void requestStop();

    // Blocks until the pipeline finishes (CSV EOF / timer / requestStop).
    void run();

   private:
    void waitForTermination();

    PipelineConfig                             cfg_;
    std::vector<std::unique_ptr<Block>>        blocks_;
    std::vector<std::thread>                   threads_;
    std::vector<std::shared_ptr<PixelChannel>> channels_;

    std::atomic<bool> stop_requested_{false};
};

}  // namespace line_scanner
