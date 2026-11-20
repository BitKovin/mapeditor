// tools/properties -- the typed property editors of the Details panel, Qt-free (port of the pure parts of shell/src/ui/properties.ts): which widget a key of the
// entity definitions gets (checkbox, number, colour, list, entity picker, ...), colour conversions, clamping to the .ent range, vector parsing, defaults.
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.4.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "core/entdef.h"
#include "core/document.h"

namespace native::tools {

// How a key is edited. `Ref` is the entity picker (needs the document, so the panel builds it).
enum class FieldKind { Ref, Asset, Boolean, List, Color01, Color255, Integer, Real, Vec3, Text };

bool isRefType(const std::string& type);   // target, target_destination, killtarget
FieldKind fieldKind(const std::string& type, bool hasItems);
inline FieldKind fieldKind(const core::EntKey& k) { return fieldKind(k.type, !k.items.empty()); }

// The keys of a class edited as typed properties: every defined key in .ent order, except `hidden` (keys the form edits elsewhere).
std::vector<const core::EntKey*> propertyKeys(const core::EntClass* def, const std::vector<std::string>& hidden);

// "r g b" (each 0..1) -> numbers; nullopt when it is not three numbers.
std::optional<std::array<double, 3>> parseColor1(const std::string& v);
std::string colorToHex(const std::array<double, 3>& c);                       // "#rrggbb"
std::optional<std::array<double, 3>> hexToColor(const std::string& hex);
std::string formatColor(const std::array<double, 3>& c);                      // three numbers, at most 4 decimals, no trailing zeros
std::string formatColor255(const std::array<double, 3>& c);                   // 0..255 integers

// A typed number inside the .ent's range (integers are rounded); nullopt for junk (NaN, infinity).
std::optional<double> clampValue(const core::EntKey& k, double raw);

// "x y z" -> three numbers; a missing or bad component is nullopt.
std::array<std::optional<double>, 3> parseVec3(const std::string& v);

// The shortest text of a number (at most 6 decimals, no trailing zeros): what keyvalues store.
std::string formatNumber(double v);

// The value a key shows: the entity's keyvalue, else the .ent default, else "".
std::string currentValue(const core::Entity& e, const core::EntKey& k);
bool isDefault(const core::Entity& e, const core::EntKey& k);   // unset, or equal to the default
bool isChecked(const std::string& v);                            // "0" / "" / "false" are off

// `modelscale_vec` "x y z" -> three numbers (1 1 1 when missing, malformed or with a 0).
std::array<double, 3> parseScaleVec(const std::string& kv);

} // namespace native::tools
