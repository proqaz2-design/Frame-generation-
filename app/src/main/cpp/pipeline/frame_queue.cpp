/**
 * Frame Queue — instantiation and helpers
 */

#include "frame_queue.h"

// Explicit template instantiation for the default queue
namespace framegen {
    template class FrameQueue<8>;
    template class FrameQueue<16>;
}
