// render -- owns the viewport renderer, render modes, picking, gizmos and overlays.
//
// Threads: render thread; picking may run on the UI thread against a snapshot.
// Spec: 05_RENDERER.md; tasks T1.3, M3
//
// This header only identifies the module (used by the skeleton test that checks every module library links).
#pragma once

namespace native::render {

const char* moduleName();

} // namespace native::render
