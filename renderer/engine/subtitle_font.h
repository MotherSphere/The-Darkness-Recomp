#pragma once
#include "simple_mesh.h"
#include <cstdint>
#include <memory>

namespace DarkRecomp::Native {
// Only the ordinary TEXT resource may use replacement glyphs. Read its original
// CStr before the font loader consumes it; decorative/unknown fonts stay native.
bool subtitleFontResourceAllowsReplacement(uint8_t* base, uint32_t resource) noexcept;
// Snapshot the completed definition, including loads before the lazy upload.
// A rejected load also invalidates an earlier definition at the reused address.
void registerSubtitleFont(uint8_t* base, uint32_t font, bool replaceable) noexcept;
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
