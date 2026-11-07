// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 450

// The blit sampler: unnormalized coordinates (texcoord is in source texels) and bilinear
// filtering, which the taps below rely on.
layout(binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

// Upscale of a square-root encoded source into a linear destination, for Ocarina of Time's
// upscaler stand-in: a Catmull-Rom filter in the encoded space (sharper than bilinear; 9 bilinear
// taps stand for the 16 texels), then each colour channel squared, as the game's network decodes
// its filtered input (x * abs(x)). Alpha passes through.
void main() {
    const vec2 size = vec2(textureSize(tex, 0));
    // Taps stay on texel centres inside the image: the sampler's border is opaque white.
    const vec2 low = vec2(0.5);
    const vec2 high = size - 0.5;
    const vec2 center = floor(texcoord - 0.5) + 0.5;
    const vec2 f = texcoord - center;
    const vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const vec2 w3 = f * f * (-0.5 + 0.5 * f);
    const vec2 w12 = w1 + w2;
    const vec2 p0 = clamp(center - 1.0, low, high);
    const vec2 p12 = clamp(center + w2 / w12, low, high);
    const vec2 p3 = clamp(center + 2.0, low, high);
    vec4 value = textureLod(tex, vec2(p0.x, p0.y), 0) * (w0.x * w0.y);
    value += textureLod(tex, vec2(p12.x, p0.y), 0) * (w12.x * w0.y);
    value += textureLod(tex, vec2(p3.x, p0.y), 0) * (w3.x * w0.y);
    value += textureLod(tex, vec2(p0.x, p12.y), 0) * (w0.x * w12.y);
    value += textureLod(tex, vec2(p12.x, p12.y), 0) * (w12.x * w12.y);
    value += textureLod(tex, vec2(p3.x, p12.y), 0) * (w3.x * w12.y);
    value += textureLod(tex, vec2(p0.x, p3.y), 0) * (w0.x * w3.y);
    value += textureLod(tex, vec2(p12.x, p3.y), 0) * (w12.x * w3.y);
    value += textureLod(tex, vec2(p3.x, p3.y), 0) * (w3.x * w3.y);
    // The filter's negative lobes can undershoot below zero near hard edges: clamp, then square.
    const vec3 encoded = max(value.rgb, vec3(0.0));
    color = vec4(encoded * encoded, value.a);
}
