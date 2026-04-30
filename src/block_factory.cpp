#include "block_factory.hpp"

#include <stdexcept>

namespace line_scanner {

BlockFactory& BlockFactory::instance() {
    static BlockFactory inst;
    return inst;
}

void BlockFactory::registerBlock(const std::string& name, Creator creator) {
    registry_.emplace(name, std::move(creator));
}

std::unique_ptr<Block> BlockFactory::create(const std::string& name) const {
    const auto it = registry_.find(name);
    if (it == registry_.end()) {
        throw std::runtime_error("BlockFactory: unknown block name '" + name + "'");
    }
    return it->second();
}

bool BlockFactory::isRegistered(const std::string& name) const {
    return registry_.find(name) != registry_.end();
}

}  // namespace line_scanner
