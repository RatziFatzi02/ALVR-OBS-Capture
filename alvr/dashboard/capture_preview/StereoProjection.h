#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stereo {
// Tangent-space bounds, in texture order (left, right, top, bottom).
struct Bounds { float left = -1, right = 1, top = -1, bottom = 1; };
struct Projection {
    Bounds eyes[2];
    Bounds output;
    bool valid = false;
    float widthScale = 1;
};
inline bool Valid(const Bounds& b) {
    return std::isfinite(b.left) && std::isfinite(b.right) && std::isfinite(b.top)
        && std::isfinite(b.bottom) && b.right - b.left > 0.001f && b.bottom - b.top > 0.001f;
}
inline Projection MakeProjection(Bounds left, Bounds right) {
    Projection p;
    if (!Valid(left) || !Valid(right)) return p;
    p.eyes[0] = left; p.eyes[1] = right;
    // Keep a rectangular area covered vertically by both eyes.
    p.output = {std::min(left.left, right.left), std::max(left.right, right.right),
        std::max(left.top, right.top), std::min(left.bottom, right.bottom)};
    if (!Valid(p.output) || std::max(left.left, right.left) >= std::min(left.right, right.right)) return Projection{};
    p.widthScale = (p.output.right - p.output.left) / (p.output.bottom - p.output.top)
        * (left.bottom - left.top) / (left.right - left.left);
    p.valid = std::isfinite(p.widthScale) && p.widthScale > 0 && p.widthScale < 4;
    return p;
}
// xy multiply, zw add: output UV -> eye UV.
inline void EyeMap(const Projection& p, int eye, float* map) {
    const Bounds& b = p.eyes[eye];
    map[0] = (p.output.right - p.output.left) / (b.right - b.left);
    map[1] = (p.output.bottom - p.output.top) / (b.bottom - b.top);
    map[2] = (p.output.left - b.left) / (b.right - b.left);
    map[3] = (p.output.top - b.top) / (b.bottom - b.top);
}
}
