// tools -- owns the editing tools (transform, brush, mesh, texturing, ...) as pure logic over the document.
//
// Threads: UI thread.
// Spec: 07_EDITOR_PORT.md group B; tasks M4, M11
//
// This header only identifies the module (used by the skeleton test that checks every module library links).
#pragma once

namespace native::tools {

const char* moduleName();

} // namespace native::tools
