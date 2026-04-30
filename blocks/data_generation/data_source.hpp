#pragma once

#include "pipeline_config.hpp"

#include <optional>

namespace line_scanner {

// Strategy interface for the source of pixel data. Returning nullopt
// signals end-of-stream (e.g. CSV file exhausted). RNG never returns nullopt.
class DataSource {
   public:
    virtual ~DataSource()                      = default;
    virtual std::optional<PixelPair> getNext() = 0;
};

}  // namespace line_scanner
