#pragma once

namespace portal {
// Runs the ScreenCast portal handshake. Returns the PipeWire node id of
// the monitor the user picked, or -1 on failure/cancel.
long screencast_node(bool verbose);
} // namespace portal
