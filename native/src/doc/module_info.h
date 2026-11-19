// doc -- owns the in-memory document, JSON I/O, commands, undo, selection and prefab expansion.
//
// Threads: UI thread mutates; immutable DocumentVersions may be read from any thread.
// Spec: 03_DOCUMENT.md; tasks M2
//
// This header only identifies the module (used by the skeleton test that checks every module library links).
#pragma once

namespace native::doc {

const char* moduleName();

} // namespace native::doc
