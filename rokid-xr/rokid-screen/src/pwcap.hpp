#pragma once

#include <cstdint>

namespace pwcap {

// Called from pw's data thread with a mapped frame (CPU memory).
typedef void (*FrameCb)(void *ud, const uint8_t *data, uint32_t w, uint32_t h,
			uint32_t stride, uint32_t spa_format);

// Spawns the stream (returns immediately). Frames arrive via cb until
// the global `running` flag (main.cpp) goes false.
long start_stream(uint32_t node, bool verbose, FrameCb cb, void *ud);

// Joins the stream thread and cleans up. Blocks until then.
void stop_stream();

// --portal-test helper: blocking stats loop until Ctrl-C.
long capture_stats(uint32_t node, bool verbose);

} // namespace pwcap
