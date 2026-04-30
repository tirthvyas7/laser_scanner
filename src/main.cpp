#include "orchestrator.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

std::atomic<bool>           g_shutdown{false};
line_scanner::Orchestrator* g_orch = nullptr;

void handleSigint(int) {
    g_shutdown.store(true, std::memory_order_release);
    if (g_orch)
        g_orch->requestStop();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string yaml_path = (argc > 1) ? argv[1] : "pipeline.yaml";

    try {
        auto                       cfg = line_scanner::Orchestrator::loadConfigFromFile(yaml_path);
        line_scanner::Orchestrator orch(std::move(cfg));
        g_orch = &orch;

        std::signal(SIGINT, handleSigint);

        std::cout << "Starting line-scanner pipeline (config: " << yaml_path << ")\n";
        orch.run();
        std::cout << "Pipeline terminated cleanly.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FATAL: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
