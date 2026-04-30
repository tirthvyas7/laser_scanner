#include "csv_source.hpp"

#include <charconv>
#include <stdexcept>
#include <utility>

namespace line_scanner {

CsvSource::CsvSource(std::string path, size_t columns) : path_(std::move(path)), columns_(columns) {
    file_.open(path_, std::ios::in);
    if (!file_.is_open()) {
        throw std::runtime_error("CsvSource: cannot open " + path_);
    }
    row_.reserve(columns_);
}

bool CsvSource::loadNextRow() {
    row_.clear();
    while (std::getline(file_, line_buf_)) {
        // Strip \r for Windows line endings.
        if (!line_buf_.empty() && line_buf_.back() == '\r')
            line_buf_.pop_back();
        if (line_buf_.empty())
            continue;

        const char* p   = line_buf_.data();
        const char* end = p + line_buf_.size();

        while (p < end) {
            // Skip leading whitespace.
            while (p < end && (*p == ' ' || *p == '\t'))
                ++p;

            unsigned int v = 0;
            auto [next, ec] = std::from_chars(p, end, v);

            if (ec != std::errc{}) {
                // Non-numeric cell — skip (handles header rows gracefully).
                while (p < end && *p != ',')
                    ++p;
                if (p < end)
                    ++p;  // consume ','
                row_.clear();
                break;
            }
            if (v > 255) {
                throw std::runtime_error("CsvSource: value out of uint8 range in " + path_);
            }
            row_.push_back(static_cast<uint8_t>(v));

            p = next;
            // Skip trailing whitespace then comma.
            while (p < end && (*p == ' ' || *p == '\t'))
                ++p;
            if (p < end && *p == ',')
                ++p;
        }

        if (row_.empty())
            continue;
        if (row_.size() != columns_) {
            throw std::runtime_error("CsvSource: row width " + std::to_string(row_.size()) +
                                     " != configured columns " + std::to_string(columns_));
        }
        col_idx_ = 0;
        return true;
    }
    eof_ = true;
    return false;
}

std::optional<PixelPair> CsvSource::getNext() {
    if (eof_)
        return std::nullopt;
    if (row_.empty() || col_idx_ >= columns_) {
        if (!loadNextRow())
            return std::nullopt;
    }
    const uint8_t a = row_[col_idx_];
    const uint8_t b = (col_idx_ + 1 < columns_) ? row_[col_idx_ + 1] : 0;
    col_idx_ += 2;
    return PixelPair{a, b};
}

}  // namespace line_scanner
