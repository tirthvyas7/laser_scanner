#include "block_factory.hpp"
#include "output_logger_block.hpp"
#include "tracing_block.hpp"  // DefectRecord (input contract)

namespace line_scanner {

// Concrete sink for Tracing's DefectRecord stream. The template is all the
// logic; this only fixes the block's name (=> defects to "<name>.csv").
struct DefectLogger : OutputLoggerBlock<DefectRecord> {
    DefectLogger() : OutputLoggerBlock<DefectRecord>("OutputLogger") {}
};

}  // namespace line_scanner

LS_REGISTER_BLOCK(deflog, "OutputLogger", line_scanner::DefectLogger)
