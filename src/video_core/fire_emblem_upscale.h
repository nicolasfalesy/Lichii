// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>
#include "common/common_types.h"
#include "video_core/textures/texture.h"

namespace VideoCommon::FireEmblemUpscale {

constexpr u64 ProgramId = 0x04007EF01F8AE000ULL;
// NativeProgramKey of the captured DLSS final output pass, not its lowered shader hash.
constexpr u64 OutputProgramKey = 0x14b4222bbe23abeeULL;
constexpr u64 PrepassProgramKey = 0x4cbf6eff32525aecULL;
constexpr u64 StarFoxProgramId = 0x0400E88025B1A000ULL;
constexpr u64 StarFoxPrepassProgramKey = 0xd686ca0b9fad9e6dULL;
constexpr u64 StarFoxOutputProgramKey = 0x7d3764ff101bec27ULL;
constexpr u32 PrepassColorOffset = 0x278;
constexpr u32 JitterOffset = 0x1b0;
constexpr u32 OutputColorInputOffset = 0x328;
constexpr u32 OutputColorOffset = 0x348;

struct Passes {
    u64 prepass;
    u64 output;
    bool spatial_only;
};

constexpr std::optional<Passes> CapturedPasses(u64 program_id) {
    if (program_id == ProgramId) {
        return Passes{PrepassProgramKey, OutputProgramKey, false};
    }
    if (program_id == StarFoxProgramId) {
        return Passes{StarFoxPrepassProgramKey, StarFoxOutputProgramKey, true};
    }
    return std::nullopt;
}

struct JitterSample {
    GPUVAddr color{};
    u32 width{}, height{};
    std::array<f32, 2> offset{};
};

class FrameJitter {
public:
    void Capture(GPUVAddr color, u32 width, u32 height, std::array<f32, 2> offset) {
        sample.reset();
        if (color == 0 || width == 0 || height == 0 ||
            !std::isfinite(offset[0]) || !std::isfinite(offset[1]) ||
            std::abs(offset[0]) > 0.5f || std::abs(offset[1]) > 0.5f) {
            return;
        }
        sample = JitterSample{color, width, height, offset};
    }

    // Consume on every output attempt, even if its resources cannot be used.
    std::optional<JitterSample> Take() {
        return std::exchange(sample, std::nullopt);
    }

private:
    std::optional<JitterSample> sample;
};

inline bool UseReplacement(u64 program_id, u64 native_key, const char* option) {
    const auto passes = CapturedPasses(program_id);
    return passes && native_key == passes->output &&
           (option == nullptr || (std::string_view{option} == "fsr" || std::string_view{option} == "dlss"));
}

inline bool PlainImage(const Tegra::Texture::TICEntry& image) {
    using namespace Tegra::Texture;
    return (image.texture_type == TextureType::Texture2D ||
            image.texture_type == TextureType::Texture2DNoMipmap) && image.IsBlockLinear() &&
           image.Depth() == 1 && image.BaseLayer() == 0 &&
           image.max_mip_level == 0 && image.Address() != 0;
}

inline bool StarFoxColor(const Tegra::Texture::TICEntry& image,
                         Tegra::Texture::TextureFormat format,
                         Tegra::Texture::ComponentType component, bool srgb) {
    using namespace Tegra::Texture;
    return PlainImage(image) && image.format == format && (image.srgb_conversion != 0) == srgb &&
           image.r_type == component && image.g_type == component &&
           image.b_type == component && image.a_type == component &&
           image.x_source == SwizzleSource::R && image.y_source == SwizzleSource::G &&
           image.z_source == SwizzleSource::B && image.w_source == SwizzleSource::A &&
           image.res_min_mip_level == 0 && image.res_max_mip_level == 0 &&
           image.msaa_mode == MsaaMode::Msaa1x1 && image.block_width == 0 &&
           image.block_depth == 0 && image.is_sparse == 0;
}

inline bool ValidInput(const Tegra::Texture::TICEntry& input, bool spatial_only = false) {
    using namespace Tegra::Texture;
    if (spatial_only) {
        return StarFoxColor(input, TextureFormat::A8B8G8R8, ComponentType::UNORM, true);
    }
    return input.format == TextureFormat::B10G11R11 && input.IsBlockLinear();
}

inline bool ValidPair(const Tegra::Texture::TICEntry& input,
                      const Tegra::Texture::TICEntry& output, bool spatial_only = false) {
    using namespace Tegra::Texture;
    if (spatial_only) {
        return ValidInput(input, true) &&
               StarFoxColor(output, TextureFormat::R16G16B16A16, ComponentType::FLOAT, false) &&
               input.Address() != output.Address() && input.Width() == output.Width() &&
               input.Height() == output.Height();
    }
    return PlainImage(input) && PlainImage(output) &&
           input.format == TextureFormat::B10G11R11 && output.format == TextureFormat::B10G11R11 &&
           input.Address() != output.Address() && output.Width() > input.Width() &&
           output.Height() > input.Height();
}

// A successful substitute dispatch, not a shader rejection. Never cache it as a failure.
struct DispatchReplaced {};

// Ocarina of Time's title movie: an AV1 frame (1080p A8B8G8R8) goes through a 12-dispatch
// tensor network and an output pass that writes the 1440p image. Translated, the network costs
// ~130 ms a frame on an Intel Arc B390 (its programs spill up to 22 KB of scratch at the 32-wide
// subgroup HMMA needs) and tints the movie purple with vertical stripes, so a bilinear upscale of
// the movie frame stands in for the whole chain. Native program keys of build b06d1818.
namespace Ocarina {
constexpr u64 ProgramId = 0x0400486024DEC000ULL;
constexpr std::array<u64, 9> NetworkProgramKeys{
    0x2f5791cea2a85397ULL, 0x313d724f3e214203ULL, 0xdf2c83cf2ad70790ULL,
    0x1fdb95ea2565f49cULL, 0x0be2f024aea21a6cULL, 0x0618cc4a3ac8f33aULL,
    0xe27336d38f1cc8f4ULL, 0xecebb02dec6faa17ULL, 0x2a366b38546a4d5eULL,
};
constexpr u64 OutputProgramKey = 0x6093c9143ba0904dULL;
// The network's last pass samples the movie frame at constant bank 0 + 0x70 (and writes its
// 1080p A2B10G10R10 result, which the output pass dithers into the 1440p A8B8G8R8 image at
// + 0xc8 with a 512x512 noise texture).
constexpr u64 FramePassProgramKey = 0x2a366b38546a4d5eULL;
constexpr u32 FrameOffset = 0x70;
constexpr u32 OutputOffset = 0xc8;

// Gameplay post-process chain (shader hashes, as the frame dump names them): a tile classifier
// reads the scene colour (B10G11R11), two passes build work lists, and a resolve writes the
// result over the colour target the HUD is drawn on. On an Intel Arc B390 the resolve read its
// lists half-written: tiles of stale black flickered around edges, leaves and the HUD boxes.
// A copy of the scene colour stands in for the chain.
constexpr u64 AaClassifyHash = 0xa107e12b6771be48ULL;
constexpr u64 AaListHash = 0xd9ee412f00ce399aULL;
constexpr u64 AaEdgesHash = 0xa2fcaa95c5a162f3ULL;
constexpr u64 AaResolveHash = 0x6bd913e1f34e83ebULL;

// Gameplay upscaler, run when the game renders below 1080p (without the "Max Resolution" add-on,
// at 854x480): one dispatch of a small network (weights in a 12 KiB storage buffer) samples the
// scene (E5B9G9R9 texture at constant bank 0 + 0xc0) and writes the 1920x1080 B10G11R11 image at
// + 0xd0. It takes 11.5 ms of a frame on an Intel Arc B390; a Catmull-Rom upscale stands in. The
// scene is stored square-root encoded: the network squares the filtered scene (x * abs(x)) and
// adds its correction, so the stand-in squares as well (blit_color_square.frag).
constexpr u64 GameUpscaleHash = 0x03bf0ffc7d2d0bedULL;
constexpr u32 GameUpscaleInputOffset = 0xc0;
constexpr u32 GameUpscaleOutputOffset = 0xd0;
// The constants open with the grid (43x24x1), then the last texel of the area the network reads
// (words 6 and 7: 853, 479) and writes (10 and 11: 1919, 1079).
constexpr size_t GameUpscaleConstantWords = 12;

// The stand-in stretches whole images, so it takes over only while both areas cover their whole
// images, in the formats seen: a scene drawn into part of its texture keeps the network.
inline bool ValidGameUpscale(const Tegra::Texture::TICEntry& input,
                             const Tegra::Texture::TICEntry& output,
                             const std::array<u32, GameUpscaleConstantWords>& constants) {
    using namespace Tegra::Texture;
    const auto plain = [](const TICEntry& image, TextureFormat format) {
        return image.format == format && image.Address() != 0 && image.IsBlockLinear() &&
               image.Depth() == 1 && image.max_mip_level == 0 &&
               (image.texture_type == TextureType::Texture2D ||
                image.texture_type == TextureType::Texture2DNoMipmap);
    };
    return plain(input, TextureFormat::E5B9G9R9) && plain(output, TextureFormat::B10G11R11) &&
           input.Address() != output.Address() && output.Width() >= input.Width() &&
           output.Height() >= input.Height() && constants[6] == input.Width() - 1 &&
           constants[7] == input.Height() - 1 && constants[10] == output.Width() - 1 &&
           constants[11] == output.Height() - 1;
}

constexpr bool IsNetworkProgram(u64 native_key) {
    for (const u64 key : NetworkProgramKeys) {
        if (key == native_key) {
            return true;
        }
    }
    return false;
}

// Not PlainImage: the output's descriptor reads base layer 32 although it is a single 2D image
// (texture type 2D, depth 1), and the frame dump finds it at that address as layer 0.
inline bool ValidMovieFrame(const Tegra::Texture::TICEntry& image) {
    using namespace Tegra::Texture;
    return (image.texture_type == TextureType::Texture2D ||
            image.texture_type == TextureType::Texture2DNoMipmap) &&
           image.IsBlockLinear() && image.Depth() == 1 && image.max_mip_level == 0 &&
           image.Address() != 0 && image.format == TextureFormat::A8B8G8R8;
}

inline bool ValidPair(const Tegra::Texture::TICEntry& input, const Tegra::Texture::TICEntry& output) {
    return ValidMovieFrame(input) && ValidMovieFrame(output) &&
           input.Address() != output.Address() && output.Width() >= input.Width() &&
           output.Height() >= input.Height();
}
} // namespace Ocarina

} // namespace VideoCommon::FireEmblemUpscale
