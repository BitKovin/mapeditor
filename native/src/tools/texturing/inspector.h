// tools/texturing/inspector -- what the Surface Inspector shows for a selection (the reading side of shell/src/ui/texturePanel.ts): the texture, the shift in texels, the stretch
// as a multiple of the natural size, rotation, shear, the projection kind, the face's repeat overrides, the lightmap density and the texel density readout. The values are
// those of the first target face (a selected face; else the first face of the selected brushes); `mixed*` says that the other target faces differ.
// Thread: any (pure). Spec: 07_EDITOR_PORT.md B (texturePanel), 08_MILESTONES.md M11 item 3.
#pragma once

#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/selection.h"
#include "tools/texturing/face_edit.h"

namespace native::tools::tex {

struct SurfaceFields {
    bool valid = false;          // false = nothing to edit (no target faces)
    size_t faceCount = 0;
    std::string texture;
    bool mixedTexture = false;
    V2 shift{0, 0};              // texels
    V2 stretch{1, 1};            // multiple of the natural size
    V2 natural{1, 1};            // natural size, metres
    V2 texels{64, 64};           // texture size, pixels
    double rotate = 0, shear = 0;
    core::texplan::ProjKind kind = core::texplan::ProjKind::Axial;
    std::string repeatMode[2];   // "" = the texture's own, else "repeat" / "mirror"
    std::string edge[2];         // "" = auto, else "wrap" / "clamp"
    double lightmap = 1;
    bool mixedLightmap = false;
    std::string density;         // "" = unknown (the texture is not loaded), else e.g. "as authored"
    bool mixedProjection = false;
};

SurfaceFields surfaceFields(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels, const InfoFn& info);

} // namespace native::tools::tex
