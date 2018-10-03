#pragma once

#include <core/vector_types.h>
#include <core/array_types.h>
#include <mol/molecule_utils.h>

enum class RamachandranConformationClassification {
    None,
    BetaHigh,
    BetaMid,
    BetaLow,
    AlphaHigh,
    AlphaMid,
    AlphaLow,
    LeftAlphaHigh,
    LeftAlphaMid,
    LeftAlphaLow,
    PMid,
    PLow
};

namespace ramachandran {
void initialize();
void shutdown();

void clear_accumulation_texture();
// Radius is given as percentage of normalized texture space coordinates (1.0 = 1% of texture width and height)
void compute_accumulation_texture(Array<const BackboneAngles> angles, vec4 color, float radius = 1.f, float outline = 0.f);
uint32 get_accumulation_texture();
uint32 get_segmentation_texture();
}  // namespace ramachandran