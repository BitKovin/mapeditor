// base -- owns jobs, versions, logging, content hashes and timing helpers; depends on nothing else in native/.
//
// Threads: any.
// Spec: 01_ARCHITECTURE.md §2, §4; tasks T1.1
//
// This header only identifies the module (used by the skeleton test that checks every module library links).
#pragma once

namespace native::base {

const char* moduleName();

} // namespace native::base
