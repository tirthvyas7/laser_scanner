#pragma once

#include "block.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace line_scanner {

// Plugin registry. Each block .cpp self-registers at program startup via a
// static initializer (see LS_REGISTER_BLOCK macro). The orchestrator looks
// up block names from pipeline.yaml and asks the factory for instances.
//
// Adding a new block requires zero changes here — the new block file just
// invokes LS_REGISTER_BLOCK and the registry picks it up automatically.
class BlockFactory {
   public:
    using Creator = std::function<std::unique_ptr<Block>()>;

    static BlockFactory& instance();

    void registerBlock(const std::string& name, Creator creator);

    std::unique_ptr<Block> create(const std::string& name) const;

    [[nodiscard]] bool isRegistered(const std::string& name) const;

   private:
    BlockFactory() = default;
    std::unordered_map<std::string, Creator> registry_;
};

}  // namespace line_scanner

// Helper macro used by each block's .cpp. Use AFTER defining the block class,
// from the global scope. NAME is the YAML string; TYPE is the fully-qualified
// C++ type. TAG is a short identifier unique within the file (used to name
// the static registrar variable).
//
// Example:
//   LS_REGISTER_BLOCK(datagen, "DataGeneration", line_scanner::DataGenerationBlock)
#define LS_REGISTER_BLOCK(TAG, NAME, TYPE)                      \
    namespace {                                                 \
    const bool _ls_reg_##TAG = [] {                             \
        ::line_scanner::BlockFactory::instance().registerBlock( \
            NAME, [] { return std::make_unique<TYPE>(); });     \
        return true;                                            \
    }();                                                        \
    }
