// scene -- owns the scene builder, per-object derived caches and immutable SceneSnapshots.
//
// Threads: job pool builds; render and bake threads read snapshots.
// Spec: 04_SCENE_AND_DERIVED.md; tasks M3
//
// This header only identifies the module (used by the skeleton test that checks every module library links).
#pragma once

namespace native::scene {

const char* moduleName();

} // namespace native::scene
