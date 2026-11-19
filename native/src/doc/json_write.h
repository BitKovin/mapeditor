// doc/json_write -- typed node -> JSON, patched into the node's previous raw JSON.
//
// Owns: writeBrush / writeEntity / writeInstance: given the typed value a tool produced and the JSON the node had before, return the
// JSON to save. Only the keys whose typed value changed are written (compared with what the old raw parses to), so keys the editor
// does not model, key formatting of untouched values and the order of untouched array items stay exactly as they were; a node whose
// typed value did not change comes back equal to its old raw. With no old raw (a new node) the JSON is built from the typed value and
// omits what is default. Number format is the Shell's (JSON.stringify): an integral number is written as an integer.
// Key names and value formats are the ones core/src/document.cpp reads and shell/src/model/document.ts types.
// Thread: any (pure functions).
// Spec: 03_DOCUMENT.md §2, 08_MILESTONES.md T2.3.
#pragma once

#include <core/document.h>

#include <nlohmann/json.hpp>

namespace native::doc {

// `oldRaw` may be null / not an object / unparsable: the node is then written from scratch.
nlohmann::json writeBrush(const core::Brush& brush, const nlohmann::json& oldRaw);
// The entity's own keys; its brushes are separate nodes (typed.brushes is ignored and no "brushes" key is written or touched).
nlohmann::json writeEntity(const core::Entity& entity, const nlohmann::json& oldRaw);
nlohmann::json writeInstance(const core::PrefabInstance& instance, const nlohmann::json& oldRaw);

// A number as the Shell writes it: an integral value as an integer (2, not 2.0), anything else as a double.
nlohmann::json numberJson(double value);

} // namespace native::doc
