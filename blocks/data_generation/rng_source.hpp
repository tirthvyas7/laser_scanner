#pragma once

#include "data_source.hpp"

#include <random>

namespace line_scanner {

// Uniform random pixel pair generator. Mersenne Twister engine seeded once
// from std::random_device. Never returns nullopt (infinite stream); the
// pipeline terminates by external trigger (timer / SIGINT).
class RngSource : public DataSource {
   public:
    RngSource();

    std::optional<PixelPair> getNext() override;

   private:
    std::mt19937                                engine_;
    std::uniform_int_distribution<unsigned int> dist_;
};

}  // namespace line_scanner
