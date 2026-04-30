#pragma once

#include "data_source.hpp"

#include <fstream>
#include <string>
#include <vector>

namespace line_scanner {

// Reads a CSV file as an m-column 2D array. Emits pixels two-at-a-time,
// row by row, advancing two columns per call. When the last pixel is
// emitted, returns nullopt on the subsequent call.
class CsvSource : public DataSource {
   public:
    CsvSource(std::string path, size_t columns);

    std::optional<PixelPair> getNext() override;

   private:
    bool loadNextRow();

    std::string          path_;
    std::ifstream        file_;
    std::string          line_buf_;
    std::vector<uint8_t> row_;
    size_t               columns_;
    size_t               col_idx_ = 0;
    bool                 eof_     = false;
};

}  // namespace line_scanner
