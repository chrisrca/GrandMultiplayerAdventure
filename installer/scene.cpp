// The installer's backdrop, rendered the way Grand Mountain Adventure draws its
// piste map: a smooth snowy heightfield seen from above at a tilt, lit by a
// warm low sun and a blue sky (the colours of the game's day lighting,
// assets/envs_light/day.yaml), with soft terrain and tree shadows, ambient
// occlusion, faceted low-poly pines, a chairlift, fine snow grain, a
// tilt-shift blur towards the top and bottom, and a vignette.
//
// Pipeline: build the heightfield and its maps (normals, shadow, occlusion)
// on a grid -> raymarch the terrain per pixel (2x2 supersampled) -> rasterise
// the trees and draw the lift against its depth -> downsample -> blur,
// vignette, grain.
#include "scene.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace {

struct V3 {
    float x, y, z;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator*(V3 a, V3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 Norm(V3 a) { return a * (1.f / std::sqrt(Dot(a, a))); }
inline V3 Mix(V3 a, V3 b, float t) { return a + (b - a) * t; }
inline float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline float Smooth(float a, float b, float v) {
    float t = Clamp01((v - a) / (b - a));
    return t * t * (3 - 2 * t);
}

// Runs f(i) for i in [0, n) on every core.
template <class F>
void Parallel(int n, F f) {
    unsigned k = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < k; ++t)
        pool.emplace_back([&] {
            for (int i; (i = next++) < n;) f(i);
        });
    for (auto& t : pool) t.join();
}

// ---- noise --------------------------------------------------------------------
inline uint32_t Hash(int x, int y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float Hash01(int x, int y, uint32_t seed) { return (Hash(x, y, seed) & 0xffffff) / 16777216.f; }

float Noise(float x, float y, uint32_t seed) {  // gradient noise, about [-0.7, 0.7]
    static const float g[16][2] = {{1, 0},     {.92f, .38f},  {.71f, .71f},  {.38f, .92f},  {0, 1},  {-.38f, .92f},
                                   {-.71f, .71f}, {-.92f, .38f}, {-1, 0},     {-.92f, -.38f}, {-.71f, -.71f},
                                   {-.38f, -.92f}, {0, -1},      {.38f, -.92f}, {.71f, -.71f}, {.92f, -.38f}};
    int ix = (int)std::floor(x), iy = (int)std::floor(y);
    float fx = x - ix, fy = y - iy;
    auto dot = [&](int cx, int cy) {
        const float* v = g[Hash(ix + cx, iy + cy, seed) & 15];
        return v[0] * (fx - cx) + v[1] * (fy - cy);
    };
    float u = fx * fx * fx * (fx * (fx * 6 - 15) + 10), w = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    float a = dot(0, 0) + (dot(1, 0) - dot(0, 0)) * u;
    float b = dot(0, 1) + (dot(1, 1) - dot(0, 1)) * u;
    return a + (b - a) * w;
}
float Fbm(float x, float y, int octaves, uint32_t seed, float gain = 0.5f) {
    float sum = 0, amp = 0.5f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * Noise(x, y, seed + i * 101);
        float nx = x * 1.62f - y * 1.18f, ny = x * 1.18f + y * 1.62f;  // rotate + scale 2x
        x = nx, y = ny;
        amp *= gain;
    }
    return sum;
}

// ---- the world ------------------------------------------------------------------
// A kWorld x kWorld square of terrain sampled on an N x N grid.
constexpr int N = 640;
constexpr float kWorld = 2.f;
constexpr float kCell = kWorld / (N - 1);

float TerrainHeight(float x, float z) {
    // warped so that the ridges wander
    float wx = Fbm(x * 1.1f + 3.1f, z * 1.1f + 7.7f, 3, 11), wz = Fbm(x * 1.1f - 4.2f, z * 1.1f + 1.9f, 3, 23);
    float px = x + 0.45f * wx, pz = z + 0.45f * wz;
    float ridge = 1 - std::fabs(Noise(px * 0.9f + 0.3f, pz * 0.9f - 1.7f, 37) * 1.4f);
    ridge = ridge * ridge * ridge;
    float hills = Fbm(px * 3.2f, pz * 3.2f, 5, 41, 0.45f);
    float bumps = Fbm(x * 9.f, z * 9.f, 3, 53, 0.5f);
    return 0.12f * ridge + 0.1f * hills + 0.006f * bumps;
}

// The icon's mountain: one peak in the middle of the world with ridges
// running off it.
float PeakHeight(float x, float z) {
    float wx = Fbm(x * 2, z * 2, 3, 11) * 0.12f, wz = Fbm(x * 2 + 5, z * 2, 3, 23) * 0.12f;
    float dx = x - 1 + wx, dz = z - 1 + wz, r = std::sqrt(dx * dx + dz * dz);
    float peak = 0.75f * std::exp(-std::pow(r / 0.3f, 0.95f));
    float ridges = 1 - std::fabs(Noise(x * 5, z * 5, 37) * 1.4f);
    return peak * (0.82f + 0.28f * ridges * ridges) + 0.03f * Fbm(x * 4, z * 4, 4, 41);
}

struct Tree {
    float x, z, y, h, r, rot, snow;
};

struct World {
    std::vector<float> height, occluder, shadow, ao, forest;
    std::vector<V3> normal;
    std::vector<Tree> trees;
    float maxHeight = 0;
    V3 sun{};
    bool peak = false;  // the icon's single peak (else the backdrop's slopes)

    static float Sample(const std::vector<float>& m, float x, float z) {
        float gx = std::min(std::max(x / kCell, 0.f), N - 1.001f), gz = std::min(std::max(z / kCell, 0.f), N - 1.001f);
        int ix = (int)gx, iz = (int)gz;
        float fx = gx - ix, fz = gz - iz;
        const float* r0 = &m[iz * N + ix];
        const float* r1 = r0 + N;
        float a = r0[0] + (r0[1] - r0[0]) * fx, b = r1[0] + (r1[1] - r1[0]) * fx;
        return a + (b - a) * fz;
    }
    V3 Normal(float x, float z) const {
        float gx = std::min(std::max(x / kCell, 0.f), N - 1.001f), gz = std::min(std::max(z / kCell, 0.f), N - 1.001f);
        int ix = (int)gx, iz = (int)gz;
        float fx = gx - ix, fz = gz - iz;
        const V3* r0 = &normal[iz * N + ix];
        const V3* r1 = r0 + N;
        return Norm(Mix(Mix(r0[0], r0[1], fx), Mix(r1[0], r1[1], fx), fz));
    }
    float H(float x, float z) const { return Sample(height, x, z); }

    // Soft visibility of the sun from (x, y, z) over a height map.
    float SunVisibility(const std::vector<float>& m, float x, float y, float z) const {
        float hx = sun.x, hz = sun.z, hl = std::sqrt(hx * hx + hz * hz);
        hx /= hl, hz /= hl;
        float slope = sun.y / hl, vis = 1;
        for (float s = kCell; s < 1.5f; s += kCell * (s < 0.2f ? 1.f : 2.f)) {
            float qx = x + hx * s, qz = z + hz * s, ry = y + s * slope;
            if (qx < 0 || qz < 0 || qx > kWorld || qz > kWorld || ry > maxHeight) break;
            vis = std::min(vis, 9.f * (ry - Sample(m, qx, qz)) / s);
            if (vis <= 0) return 0;
        }
        return Smooth(0, 1, vis);
    }

    void Build() {
        height.resize(N * N);
        Parallel(N, [&](int j) {
            for (int i = 0; i < N; ++i) height[j * N + i] = peak ? PeakHeight(i * kCell, j * kCell) : TerrainHeight(i * kCell, j * kCell);
        });
        maxHeight = *std::max_element(height.begin(), height.end()) + 0.08f;
        normal.resize(N * N);
        Parallel(N, [&](int j) {
            for (int i = 0; i < N; ++i) {
                int i0 = std::max(i - 1, 0), i1 = std::min(i + 1, N - 1), j0 = std::max(j - 1, 0), j1 = std::min(j + 1, N - 1);
                float dx = (height[j * N + i1] - height[j * N + i0]) / ((i1 - i0) * kCell);
                float dz = (height[j1 * N + i] - height[j0 * N + i]) / ((j1 - j0) * kCell);
                normal[j * N + i] = Norm({-dx, 1, -dz});
            }
        });

        // pines: in patches of forest, on gentle slopes below the tree line
        constexpr float kSpacing = 0.024f;
        for (int j = 0; j * kSpacing < kWorld; ++j)
            for (int i = 0; i * kSpacing < kWorld; ++i) {
                float x = (i + 0.15f + 0.7f * Hash01(i, j, 71)) * kSpacing, z = (j + 0.15f + 0.7f * Hash01(i, j, 72)) * kSpacing;
                float density = Smooth(0.06f, 0.26f, Fbm(x * 3.2f, z * 3.2f, 3, 61)) * 0.75f +
                                Smooth(0.25f, 0.4f, Fbm(x * 9.f, z * 9.f, 2, 67)) * 0.25f;
                V3 n = Normal(x, z);
                density *= Smooth(0.8f, 0.9f, n.y) * (peak ? Smooth(0.2f, 0.14f, H(x, z)) : Smooth(0.2f, 0.15f, H(x, z)));
                if (Hash01(i, j, 73) >= density) continue;
                float h = 0.034f + 0.02f * Hash01(i, j, 74);
                trees.push_back({x, z, H(x, z) - 0.002f, h, h * (0.27f + 0.06f * Hash01(i, j, 75)),
                                 Hash01(i, j, 76) * 6.283f, 0.1f + 0.3f * Hash01(i, j, 77)});
            }

        // the trees cast shadows (a cone each) and darken the snow around them
        occluder = height;
        forest.assign(N * N, 0.f);
        for (const Tree& t : trees) {
            int ci = (int)(t.x / kCell), cj = (int)(t.z / kCell), rad = (int)(t.r * 2.2f / kCell) + 1;
            for (int j = std::max(cj - rad, 0); j <= std::min(cj + rad, N - 1); ++j)
                for (int i = std::max(ci - rad, 0); i <= std::min(ci + rad, N - 1); ++i) {
                    float dx = i * kCell - t.x, dz = j * kCell - t.z, d = std::sqrt(dx * dx + dz * dz);
                    if (d < t.r) {
                        float top = t.y + t.h * (0.12f + 0.88f * (1 - d / t.r));
                        float& o = occluder[j * N + i];
                        o = std::max(o, top);
                    }
                    float k = d / (t.r * 1.5f);
                    forest[j * N + i] += std::exp(-k * k);
                }
        }
        shadow.resize(N * N);
        Parallel(N, [&](int j) {
            for (int i = 0; i < N; ++i)
                shadow[j * N + i] = SunVisibility(occluder, i * kCell, height[j * N + i] + 0.0015f, j * kCell);
        });
        // ambient occlusion: how far the terrain around rises above each point
        ao.resize(N * N);
        Parallel(N, [&](int j) {
            for (int i = 0; i < N; ++i) {
                float h0 = height[j * N + i], occ = 0;
                int samples = 0;
                for (int r : {3, 8, 18, 36})
                    for (int a = 0; a < 8; ++a) {
                        float ang = a * 0.785f + r * 0.37f;
                        int si = i + (int)std::lround(std::cos(ang) * r), sj = j + (int)std::lround(std::sin(ang) * r);
                        if (si < 0 || sj < 0 || si >= N || sj >= N) continue;
                        occ += std::max(0.f, std::atan((height[sj * N + si] - h0) / (r * kCell))) / 1.5708f;
                        ++samples;
                    }
                float trees = 1 - 0.45f * Clamp01(forest[j * N + i]);
                ao[j * N + i] = Clamp01(1 - 1.6f * occ / std::max(samples, 1)) * trees;
            }
        });
    }
};

// ---- lighting (linear) ------------------------------------------------------------
const V3 kSunColor{1.05f, 0.98f, 0.86f};
const V3 kSkyColor{0.25f, 0.32f, 0.47f};
const V3 kSnow{0.94f, 0.96f, 1.0f};
const V3 kRock{0.26f, 0.16f, 0.13f};
const V3 kPine{0.12f, 0.2f, 0.15f};
const V3 kForestFloor{0.55f, 0.66f, 0.6f};
const V3 kHaze{0.6f, 0.66f, 0.78f};
const V3 kSkyTop{0.015f, 0.06f, 0.3f}, kSkyHorizon{0.16f, 0.3f, 0.62f};  // the icon's sky

V3 Light(V3 n, V3 sun, float sunVis, float ao, float fill = 1) {
    float d = std::max(0.f, Dot(n, sun));
    float sky = 0.62f + 0.38f * n.y;
    return kSunColor * (d * sunVis * 1.25f) + kSkyColor * (fill * sky * (0.35f + 0.65f * ao)) + V3{0.05f, 0.05f, 0.06f} * ao;
}
inline float ToneChannel(float c) { return std::pow(1 - std::exp(-c * 1.35f), 1 / 2.1f); }  // filmic-ish, gamma 2.1
inline V3 Tone(V3 c) { return {ToneChannel(c.x), ToneChannel(c.y), ToneChannel(c.z)}; }

struct Camera {
    V3 pos, fwd, right, up;
    float tanHalf, aspect;
};

struct Frame {
    int w, h;
    std::vector<V3> color;    // tone mapped
    std::vector<float> depth;  // along the view axis
};

// The first hit of the ray o + t d (t >= t0) with the terrain.
bool March(const World& wd, V3 o, V3 d, float& t, float t0 = 0.05f) {
    t = t0;
    float prevT = t, prevGap = 1e9f;
    for (int s = 0; s < 600 && t < 6; ++s) {
        V3 p = o + d * t;
        if (p.y > wd.maxHeight && d.y >= 0) return false;
        float gap = p.y - wd.H(p.x, p.z);
        if (gap < 0) {
            if (prevGap < 1e8f) t = prevT + (t - prevT) * prevGap / (prevGap - gap);
            return true;
        }
        prevT = t, prevGap = gap;
        t += std::max(gap * 0.55f, 0.0004f + t * 0.0004f);
    }
    return false;
}
// The view ray through (sx, sy), in 0..1 across the frame.
V3 Ray(const Camera& cam, float sx, float sy) {
    return Norm(cam.fwd + cam.right * ((sx * 2 - 1) * cam.tanHalf * cam.aspect) + cam.up * ((1 - sy * 2) * cam.tanHalf));
}
// A world point on a w x h frame: pixel position and view depth.
bool Project(const Camera& cam, int w, int h, V3 p, float& sx, float& sy, float& z) {
    V3 d = p - cam.pos;
    z = Dot(d, cam.fwd);
    if (z < 0.02f) return false;
    sx = (Dot(d, cam.right) / (z * cam.tanHalf * cam.aspect) * 0.5f + 0.5f) * w;
    sy = (0.5f - Dot(d, cam.up) / (z * cam.tanHalf) * 0.5f) * h;
    return true;
}

void RenderTerrain(const World& wd, const Camera& cam, Frame& f) {
    Parallel(f.h, [&](int y) {
        for (int x = 0; x < f.w; ++x) {
            V3 d = Ray(cam, (x + 0.5f) / f.w, (y + 0.5f) / f.h);
            float t;
            int idx = y * f.w + x;
            if (!March(wd, cam.pos, d, t)) {
                f.color[idx] = Tone(wd.peak ? Mix(kSkyHorizon, kSkyTop, Smooth(-0.05f, 0.22f, d.y)) : kHaze);
                f.depth[idx] = 1e9f;
                continue;
            }
            V3 p = cam.pos + d * t;
            V3 n = wd.Normal(p.x, p.z);
            // snow grain: fine wind-blown ripples and sparkle-scale noise
            float gx = Noise(p.x * 140, p.z * 55, 91) * 0.03f + Noise(p.x * 700, p.z * 700, 93) * 0.05f;
            float gz = Noise(p.x * 140 + 9, p.z * 55 + 4, 92) * 0.03f + Noise(p.x * 700 + 5, p.z * 700 + 3, 94) * 0.05f;
            V3 ns = Norm({n.x + gx, n.y, n.z + gz});
            // exposed rock on the steepest faces
            float rock = Smooth(0.76f, 0.62f, n.y) * Smooth(-0.1f, 0.15f, Noise(p.x * 14, p.z * 14, 95) + 0.25f);
            float forest = Clamp01(World::Sample(wd.forest, p.x, p.z) * 0.6f);
            V3 albedo = Mix(Mix(kSnow, kSnow * kForestFloor, forest * 0.35f), kRock, rock * (wd.peak ? 0.45f : 1.f));
            float sunVis = World::Sample(wd.shadow, p.x, p.z), ao = World::Sample(wd.ao, p.x, p.z);
            V3 c = albedo * Light(ns, wd.sun, sunVis, ao, wd.peak ? 0.6f : 1.f);
            // a little haze with distance
            if (wd.peak) c = Mix(c, kSkyHorizon * 1.5f, Clamp01((t - 1.2f) * 0.3f));
            else c = Mix(c, kHaze, Clamp01((t - 0.9f) * 0.25f));
            f.color[idx] = Tone(c);
            f.depth[idx] = t * Dot(d, cam.fwd);
        }
    });
}

void RenderTrees(const World& wd, const Camera& cam, Frame& f) {
    struct P {
        float x, y, z;  // screen x, y; view depth
    };
    auto project = [&](V3 w, P& o) { return Project(cam, f.w, f.h, w, o.x, o.y, o.z); };
    // facets in parallel would race on the depth buffer; trees are cheap, so
    // split the screen into bands instead
    const int bands = 32;
    Parallel(bands, [&](int band) {
        int y0 = f.h * band / bands, y1 = f.h * (band + 1) / bands;
        for (const Tree& t : wd.trees) {
            P base;
            if (!project({t.x, t.y, t.z}, base)) continue;
            float reach = t.h / base.z / cam.tanHalf * f.h;  // rough size on screen
            if (base.x < -reach || base.x > f.w + reach || base.y < y0 - reach * 0.2f || base.y - reach * 1.2f > y1) continue;
            float sunVis = std::min(World::Sample(wd.shadow, t.x, t.z) + 0.25f, 1.f) *
                           wd.SunVisibility(wd.height, t.x, t.y + t.h * 0.5f, t.z);
            float ao = World::Sample(wd.ao, t.x, t.z);
            // two tiers of a five-sided cone
            struct Tier {
                float bottom, radius, top, turn;
            } tiers[2] = {{0.1f, 1.f, 0.7f, 0}, {0.42f, 0.66f, 1.f, 0.628f}};
            for (const Tier& tier : tiers) {
                const int sides = 5;
                V3 tip{t.x, t.y + t.h * tier.top, t.z};
                for (int s = 0; s < sides; ++s) {
                    float a0 = t.rot + tier.turn + s * 6.2832f / sides, a1 = a0 + 6.2832f / sides;
                    float r = t.r * tier.radius, yb = t.y + t.h * tier.bottom;
                    V3 v0{t.x + std::cos(a0) * r, yb, t.z + std::sin(a0) * r};
                    V3 v1{t.x + std::cos(a1) * r, yb, t.z + std::sin(a1) * r};
                    V3 n = Norm(Cross(v1 - tip, v0 - tip));
                    V3 mid = (tip + v0 + v1) * (1 / 3.f);
                    if (Dot(n, mid - V3{t.x, mid.y, t.z}) < 0) n = n * -1;
                    if (Dot(n, mid - cam.pos) > 0) continue;  // facing away
                    V3 albedo = Mix(kPine, kSnow * 0.8f, t.snow * Clamp01(n.y * 1.6f));
                    V3 lit = albedo * Light(n, wd.sun, sunVis, ao, wd.peak ? 0.6f : 1.f);
                    P p[3];
                    if (!project(tip, p[0]) || !project(v0, p[1]) || !project(v1, p[2])) continue;
                    float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x);
                    if (std::fabs(area) < 1e-6f) continue;
                    int minX = std::max(0, (int)std::floor(std::min({p[0].x, p[1].x, p[2].x})));
                    int maxX = std::min(f.w - 1, (int)std::ceil(std::max({p[0].x, p[1].x, p[2].x})));
                    int minY = std::max(y0, (int)std::floor(std::min({p[0].y, p[1].y, p[2].y})));
                    int maxY = std::min(y1 - 1, (int)std::ceil(std::max({p[0].y, p[1].y, p[2].y})));
                    for (int y = minY; y <= maxY; ++y)
                        for (int x = minX; x <= maxX; ++x) {
                            float px = x + 0.5f, py = y + 0.5f;
                            float w0 = ((p[1].x - px) * (p[2].y - py) - (p[1].y - py) * (p[2].x - px)) / area;
                            float w1 = ((p[2].x - px) * (p[0].y - py) - (p[2].y - py) * (p[0].x - px)) / area;
                            float w2 = 1 - w0 - w1;
                            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                            float zi = w0 / p[0].z + w1 / p[1].z + w2 / p[2].z, z = 1 / zi;
                            int idx = y * f.w + x;
                            if (z >= f.depth[idx]) continue;
                            // darker towards the bottom of each tier (the branches above)
                            float up = (w0 / p[0].z) / zi;
                            f.depth[idx] = z;
                            f.color[idx] = Tone(lit * (0.65f + 0.35f * up));
                        }
                }
            }
        }
    });
}

// Thin dark marks drawn over the frame with depth testing: a coverage mask
// that is then blended in.
struct Marks {
    const Camera& cam;
    Frame& f;
    std::vector<float> mask;
    Marks(const Camera& c, Frame& fr) : cam(c), f(fr), mask((size_t)fr.w * fr.h, 0.f) {}
    // A soft dot of radius r pixels at world point p. onSurface: it lies on
    // whatever is visible there (a track, a shadow); else it must be in front.
    void Dot(V3 p, float r, bool onSurface) {
        float sx, sy, z;
        if (!Project(cam, f.w, f.h, p, sx, sy, z)) return;
        int x0 = std::max(0, (int)(sx - r - 1)), x1 = std::min(f.w - 1, (int)(sx + r + 1));
        int y0 = std::max(0, (int)(sy - r - 1)), y1 = std::min(f.h - 1, (int)(sy + r + 1));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                int i = y * f.w + x;
                float dz = z - f.depth[i];
                if (onSurface ? std::fabs(dz) > 0.004f * z : dz > 0) continue;
                float dx = x + 0.5f - sx, dy = y + 0.5f - sy;
                float a = Clamp01(r + 0.5f - std::sqrt(dx * dx + dy * dy));
                mask[i] = std::max(mask[i], a);
            }
    }
    // Dots every half pixel from a to b.
    void Line(V3 a, V3 b, float r, bool onSurface) {
        float ax, ay, az, bx, by, bz;
        int n = 2;
        if (Project(cam, f.w, f.h, a, ax, ay, az) && Project(cam, f.w, f.h, b, bx, by, bz))
            n = std::max(2, (int)(std::hypot(bx - ax, by - ay) * 2));
        for (int i = 0; i <= n; ++i) Dot(a + (b - a) * ((float)i / n), r, onSurface);
    }
    void Apply(V3 color, float strength, bool multiply) {
        for (size_t i = 0; i < mask.size(); ++i)
            if (mask[i] > 0)
                f.color[i] = multiply ? f.color[i] * (1 - strength * mask[i]) : Mix(f.color[i], color, strength * mask[i]);
    }
};

// A chairlift up the slope between two points on screen: pylons and a sagging
// cable, with their shadows on the snow.
void RenderLift(const World& wd, const Camera& cam, Frame& f, float sx0, float sy0, float sx1, float sy1) {
    float t0, t1;
    V3 r0 = Ray(cam, sx0, sy0), r1 = Ray(cam, sx1, sy1);
    if (!March(wd, cam.pos, r0, t0) || !March(wd, cam.pos, r1, t1)) return;
    V3 g0 = cam.pos + r0 * t0, g1 = cam.pos + r1 * t1;
    const int pylons = 7;
    const float tall = 0.05f, px = f.h / 420.f;  // px: frame pixels per window pixel at 100%
    std::vector<V3> foot, top;
    for (int i = 0; i < pylons; ++i) {
        V3 g = Mix(g0, g1, (float)i / (pylons - 1));
        g.y = wd.H(g.x, g.z);
        foot.push_back(g);
        top.push_back(g + V3{0, tall, 0});
    }
    Marks lines(cam, f), shade(cam, f);
    auto shadowOf = [&](V3 p, V3& out) {
        float t;
        if (!March(wd, p, wd.sun * -1, t, 0.0005f)) return false;
        out = p - wd.sun * t;
        return true;
    };
    for (int i = 0; i < pylons; ++i) {
        lines.Line(foot[i], top[i], 1.1f * px, false);
        lines.Line(top[i] + cam.right * -0.004f, top[i] + cam.right * 0.004f, 0.9f * px, false);  // cross arm
        V3 a, b;
        if (shadowOf(foot[i] + V3{0, 0.001f, 0}, a) && shadowOf(top[i], b)) shade.Line(a, b, 1.3f * px, true);
    }
    for (int i = 0; i + 1 < pylons; ++i)
        for (int k = 0; k < 160; ++k) {
            float s0 = k / 160.f, s1 = (k + 1) / 160.f;
            auto at = [&](float s) { return Mix(top[i], top[i + 1], s) - V3{0, 0.012f * 4 * s * (1 - s), 0}; };
            lines.Line(at(s0), at(s1), 0.75f * px, false);
            V3 a, b;
            if (shadowOf(at(s0), a) && shadowOf(at(s1), b)) shade.Line(a, b, 0.9f * px, true);
        }
    shade.Apply({}, 0.32f, true);
    lines.Apply(Tone({0.012f, 0.013f, 0.016f}), 0.95f, false);
}

void BoxBlur(std::vector<V3>& img, int w, int h, int r) {
    if (r < 1) return;
    std::vector<V3> tmp(img.size());
    float inv = 1.f / (2 * r + 1);
    Parallel(h, [&](int y) {
        const V3* row = &img[y * w];
        V3 acc{0, 0, 0};
        for (int i = -r; i <= r; ++i) acc = acc + row[std::min(std::max(i, 0), w - 1)];
        for (int x = 0; x < w; ++x) {
            tmp[y * w + x] = acc * inv;
            acc = acc + row[std::min(x + r + 1, w - 1)] - row[std::max(x - r, 0)];
        }
    });
    Parallel(w, [&](int x) {
        V3 acc{0, 0, 0};
        for (int i = -r; i <= r; ++i) acc = acc + tmp[std::min(std::max(i, 0), h - 1) * w + x];
        for (int y = 0; y < h; ++y) {
            img[y * w + x] = acc * inv;
            acc = acc + tmp[std::min(y + r + 1, h - 1) * w + x] - tmp[std::max(y - r, 0) * w + x];
        }
    });
}
void Blur(std::vector<V3>& img, int w, int h, float radius) {  // ~gaussian: three box passes
    int r = std::max(1, (int)std::lround(radius * 0.58f));
    for (int i = 0; i < 3; ++i) BoxBlur(img, w, h, r);
}

}  // namespace

// Renders the backdrop, or (icon) the icon's peak, into out.
static void Render(int w, int h, uint32_t* out, bool icon) {
    World wd;
    wd.peak = icon;
    // the backdrop looks down across the slopes; the icon looks up at the peak.
    // The sun is low, back and to the left.
    const float yaw = 0.62f, pitch = icon ? 0.2f : 0.8f, dist = icon ? 1.3f : 1.85f, fov = icon ? 0.72f : 0.5f;
    V3 fwd = Norm({std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)});
    V3 right = Norm(Cross({0, 1, 0}, fwd));
    V3 flat = Norm({fwd.x, 0, fwd.z});
    wd.sun = icon ? Norm(right * -0.8f + flat * -0.45f + V3{0, 0.6f, 0}) : Norm(right * -0.8f + flat * 0.55f + V3{0, 0.52f, 0});
    wd.Build();

    Camera cam;
    V3 target = icon ? V3{1, 0.3f, 1} : V3{0.98f, 0, 0.92f};
    if (!icon) target.y = wd.H(target.x, target.z);
    cam.fwd = fwd;
    cam.right = right;
    cam.up = Cross(fwd, right);
    cam.pos = target - fwd * dist;
    cam.tanHalf = std::tan(fov / 2);
    cam.aspect = (float)w / h;

    const int ss = icon ? 3 : 2;
    Frame f{w * ss, h * ss, {}, {}};
    f.color.resize((size_t)f.w * f.h);
    f.depth.resize((size_t)f.w * f.h);
    RenderTerrain(wd, cam, f);
    RenderTrees(wd, cam, f);
    if (!icon) RenderLift(wd, cam, f, 0.6f, 1.08f, 0.9f, -0.08f);

    std::vector<V3> img((size_t)w * h);
    Parallel(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            V3 acc{0, 0, 0};
            for (int j = 0; j < ss; ++j)
                for (int i = 0; i < ss; ++i) acc = acc + f.color[(y * ss + j) * f.w + x * ss + i];
            img[y * w + x] = acc * (1.f / (ss * ss));
        }
    });

    // tilt-shift (backdrop): sharp across the middle, blurring towards the top
    // and bottom
    float scale = h / 420.f;
    std::vector<V3> soft = img, softer = img;
    if (!icon) {
        Blur(soft, w, h, 2.2f * scale);
        Blur(softer, w, h, 6.f * scale);
    }
    Parallel(h, [&](int y) {
        float fy = (y + 0.5f) / h;
        float b = icon ? 0.f : Clamp01((std::fabs(fy - 0.56f) - 0.1f) / 0.36f);
        b = b * b * (3 - 2 * b);
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            V3 c = b < 0.5f ? Mix(img[i], soft[i], b * 2) : Mix(soft[i], softer[i], b * 2 - 1);
            // a cool, slightly desaturated grade
            float lum = c.x * 0.3f + c.y * 0.59f + c.z * 0.11f;
            c = Mix(V3{lum, lum, lum}, c, icon ? 0.95f : 0.88f);
            c = c * V3{0.98f, 0.99f, 1.02f};
            // vignette
            float vx = ((x + 0.5f) / w - 0.5f) * (icon ? 1.f : 1.25f), vy = fy - 0.5f;
            float v = 1 - (icon ? 0.25f : 0.42f) * Smooth(0.25f, 0.85f, std::sqrt(vx * vx + vy * vy) * 1.25f);
            c = c * v;
            // grain (also dithers the gradients)
            float g = icon ? 0.f : (Hash01(x, y, 99) - 0.5f) * (3.f / 255);
            auto ch = [&](float vch) { return (uint32_t)std::lround(Clamp01(vch + g) * 255); };
            out[i] = 0xff000000u | ch(c.x) << 16 | ch(c.y) << 8 | ch(c.z);
        }
    });
}

void RenderScene(int w, int h, uint32_t* out) { Render(w, h, out, false); }
void RenderIconScene(int size, uint32_t* out) { Render(size, size, out, true); }
