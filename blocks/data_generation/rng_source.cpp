#include "rng_source.hpp"

namespace line_scanner {

RngSource::RngSource() : engine_(std::random_device{}()), dist_(0, 255) {}

std::optional<PixelPair> RngSource::getNext() {
    return PixelPair{static_cast<uint8_t>(dist_(engine_)), static_cast<uint8_t>(dist_(engine_))};
}

}  // namespace line_scanner
