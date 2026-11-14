// Pose helpers shared by the mod, the test bot and the unit tests.
// A pose is the game's ghost frame: up to 26 bones, each
// {start xyz, end xyz, dir xyz, tag} in world space (Z up). The tag is an
// integer body-part id stored in a float slot and must be copied bit-exact.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace posemath {

constexpr int kBoneFloats = 10;

inline uint32_t Tag(const float* bone) {
    uint32_t t;
    memcpy(&t, &bone[9], 4);
    return t;
}

// Heading (radians, atan2 in the XY plane) of the first bone, which for skis
// and boards runs tail -> tip.
inline float Heading(const float (*bones)[kBoneFloats]) {
    return atan2f(bones[0][4] - bones[0][1], bones[0][3] - bones[0][0]);
}

inline float BoneLength(const float* b) {
    float dx = b[3] - b[0], dy = b[4] - b[1], dz = b[5] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// out = pose rotated by `yaw` about the vertical axis through `pivot`, then
// moved so that `pivot` lands on `to`. Tags are copied bit-exact.
inline void Transform(const float (*in)[kBoneFloats], float (*out)[kBoneFloats], int count, const float pivot[3],
                      const float to[3], float yaw) {
    const float c = cosf(yaw), s = sinf(yaw);
    for (int b = 0; b < count; ++b) {
        for (int p = 0; p < 2; ++p) {
            const float* v = &in[b][p * 3];
            float x = v[0] - pivot[0], y = v[1] - pivot[1], z = v[2] - pivot[2];
            out[b][p * 3 + 0] = to[0] + c * x - s * y;
            out[b][p * 3 + 1] = to[1] + s * x + c * y;
            out[b][p * 3 + 2] = to[2] + z;
        }
        const float* d = &in[b][6];
        out[b][6] = c * d[0] - s * d[1];
        out[b][7] = s * d[0] + c * d[1];
        out[b][8] = d[2];
        memcpy(&out[b][9], &in[b][9], 4);
    }
}

// Move `cur` a fraction k (0..1) of the way toward `target`; direction vectors
// are re-normalised and tags copied bit-exact from the target.
inline void EaseToward(float (*cur)[kBoneFloats], const float (*target)[kBoneFloats], int count, float k) {
    for (int b = 0; b < count; ++b) {
        for (int i = 0; i < 9; ++i) cur[b][i] += (target[b][i] - cur[b][i]) * k;
        memcpy(&cur[b][9], &target[b][9], 4);
        float* d = &cur[b][6];
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 1e-4f) d[0] /= l, d[1] /= l, d[2] /= l;
    }
}

}  // namespace posemath
