#pragma once
#include "simple_mesh.h"
#include <cstdint>
#include <memory>

namespace DarkRecomp::Native {
// Snapshot the completed original bitmap-font definition. The render consumer
// reads no guest memory; only its single primary atlas is supported.
void registerSubtitleFont(uint8_t* base, uint32_t font) noexcept;
// A completed font load can precede its lazy atlas upload. Verified subtitle
// producers may retry against their current font and captured owned generation.
void ensureSubtitleFontRegistered(uint8_t* base, uint32_t font) noexcept;
// Size controls geometry; this factor controls only vector rasterization.
unsigned subtitleFontRasterScale(unsigned renderHeight, unsigned subtitlePercent) noexcept;
uint32_t subtitleFontCodepoint(uint16_t guestCode, bool russian) noexcept;
// A missing/unsupported definition or failed rasterization keeps the original.
// Registration binds to the captured owned image generation and rejects ID reuse.
std::shared_ptr<const ColorImage> subtitleFontReplacementFor(uint32_t textureId,
    const std::shared_ptr<const ColorImage>& source, unsigned factor) noexcept;
std::shared_ptr<const ColorImage> subtitleFontReplacementFor(uint32_t textureId,
    const std::shared_ptr<const AlphaImage>& source, unsigned factor) noexcept;
}
