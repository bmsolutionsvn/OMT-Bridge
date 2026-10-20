#pragma once
// One-time NDI runtime startup (spec 14.3). NDIlib_initialize must run once before any find,
// recv or send instance; a failed start is reported as Offline by each adapter.
namespace ob {

bool ensureNdi();

}  // namespace ob
