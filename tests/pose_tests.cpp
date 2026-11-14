// Unit + network tests for pose transport. Exit code 0 = all passed.
//
// The fixture is a real 19-bone skier pose captured from the game (ghost frame
// produced by make_ghost_frame for the local player), with its body-part tags.
#include <winsock2.h>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "../src/net.h"
#include "../src/pose_math.h"

using posemath::kBoneFloats;

static int g_failures = 0, g_checks = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        ++g_checks;                                                   \
        if (!(cond)) {                                                \
            ++g_failures;                                             \
            printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            printf(__VA_ARGS__);                                      \
            printf("\n");                                             \
        }                                                             \
    } while (0)

static const float kFixture[19][9] = {
    {558.818f, 351.936f, 86.462f, 562.367f, 352.914f, 86.830f, -0.089f, -0.051f, 0.995f},
    {558.940f, 351.501f, 86.374f, 562.489f, 352.479f, 86.742f, -0.102f, -0.003f, 0.995f},
    {559.848f, 352.808f, 85.970f, 559.612f, 352.743f, 87.955f, -0.982f, -0.144f, -0.122f},
    {560.252f, 351.272f, 85.880f, 560.051f, 351.216f, 87.869f, -0.911f, -0.398f, -0.103f},
    {560.592f, 352.425f, 86.646f, 560.305f, 352.346f, 86.616f, -0.089f, -0.051f, 0.995f},
    {560.714f, 351.990f, 86.558f, 560.426f, 351.910f, 86.528f, -0.102f, -0.003f, 0.995f},
    {560.455f, 352.153f, 88.235f, 560.512f, 352.168f, 87.638f, -0.959f, -0.264f, -0.099f},
    {560.397f, 352.137f, 88.833f, 560.455f, 352.153f, 88.235f, -0.959f, -0.264f, -0.099f},
    {560.397f, 352.137f, 88.833f, 560.359f, 352.126f, 89.231f, -0.995f, -0.021f, -0.096f},
    {560.592f, 352.425f, 86.646f, 559.994f, 352.247f, 87.061f, -0.540f, -0.128f, -0.832f},
    {560.714f, 351.990f, 86.558f, 560.148f, 351.847f, 87.029f, -0.601f, -0.183f, -0.778f},
    {559.994f, 352.247f, 87.061f, 560.459f, 352.361f, 87.638f, -0.746f, -0.187f, 0.639f},
    {560.148f, 351.847f, 87.029f, 560.565f, 351.976f, 87.638f, -0.778f, -0.236f, 0.582f},
    {560.342f, 352.869f, 88.306f, 559.701f, 352.758f, 87.998f, -0.192f, -0.726f, 0.660f},
    {560.701f, 351.487f, 88.289f, 560.130f, 351.249f, 87.920f, -0.608f, 0.489f, 0.625f},
    {560.331f, 352.378f, 88.833f, 560.342f, 352.869f, 88.306f, -0.911f, -0.293f, -0.292f},
    {560.464f, 351.896f, 88.833f, 560.701f, 351.487f, 88.289f, -0.943f, -0.161f, -0.291f},
    {559.701f, 352.758f, 87.998f, 559.523f, 352.728f, 87.912f, -0.192f, -0.726f, 0.660f},
    {560.130f, 351.249f, 87.920f, 559.972f, 351.183f, 87.818f, -0.608f, 0.489f, 0.625f},
};
static const uint32_t kFixtureTags[19] = {1, 1, 8, 8, 7, 7, 6, 5, 11, 9, 9, 10, 10, 12, 12, 13, 13, 4, 4};

static void LoadFixture(float (*b)[kBoneFloats]) {
    for (int i = 0; i < 19; ++i) {
        memcpy(b[i], kFixture[i], 9 * sizeof(float));
        float* d = &b[i][6];
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        d[0] /= l, d[1] /= l, d[2] /= l;
        memcpy(&b[i][9], &kFixtureTags[i], 4);
    }
}

static float WrapDeg(float d) {
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

// Lean of the body: vector from the first bone's start (feet) to the head top.
static void Lean(const float (*b)[kBoneFloats], int n, float& tilt, float& az) {
    float head[3] = {0, 0, -1e9f};
    for (int i = 0; i < n; ++i)
        if (posemath::Tag(b[i]) == 11)
            for (int e = 0; e < 2; ++e)
                if (b[i][e * 3 + 2] > head[2]) memcpy(head, &b[i][e * 3], 12);
    float lx = head[0] - b[0][0], ly = head[1] - b[0][1], lz = head[2] - b[0][2];
    tilt = atan2f(sqrtf(lx * lx + ly * ly), lz) * 57.29578f;
    az = atan2f(ly, lx) * 57.29578f;
}

struct Err {
    float pos = 0, dir = 0;
    bool tags = true;
};
static Err Compare(const float (*a)[kBoneFloats], const float (*b)[kBoneFloats], int n) {
    Err e;
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < 6; ++k) e.pos = fmaxf(e.pos, fabsf(a[i][k] - b[i][k]));
        for (int k = 6; k < 9; ++k) e.dir = fmaxf(e.dir, fabsf(a[i][k] - b[i][k]));
        if (memcmp(&a[i][9], &b[i][9], 4) != 0) e.tags = false;
    }
    return e;
}

static void TestPackRoundTrip() {
    printf("pack/unpack round trip (real game pose)\n");
    float src[mp::kMaxBones][kBoneFloats] = {}, dst[mp::kMaxBones][kBoneFloats] = {};
    LoadFixture(src);
    mp::PlayerState ps{};
    mp::PackPose(ps, 19, src);
    int n = mp::UnpackPose(ps, dst);
    CHECK(n == 19, "count %d", n);
    Err e = Compare(src, dst, 19);
    CHECK(e.pos <= 0.0006f, "max position error %.5f m", e.pos);
    CHECK(e.dir <= 0.0001f, "max direction error %.6f", e.dir);
    CHECK(e.tags, "body-part tags must be bit-exact");

    printf("pack/unpack round trip (2000 random poses, bones within 30 m of the root)\n");
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> pos(-30.f, 30.f), dir(-1.f, 1.f), base(-5000.f, 5000.f);
    float worstPos = 0, worstDir = 0;
    bool tagsOk = true;
    for (int it = 0; it < 2000; ++it) {
        int count = 1 + it % mp::kMaxBones;
        float root[3] = {base(rng), base(rng), base(rng) * 0.2f};
        for (int i = 0; i < count; ++i) {
            for (int k = 0; k < 3; ++k) {
                src[i][k] = (i == 0 ? root[k] : root[k] + pos(rng));
                src[i][3 + k] = root[k] + pos(rng);
            }
            float d[3] = {dir(rng), dir(rng), dir(rng)};
            float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
            for (int k = 0; k < 3; ++k) src[i][6 + k] = d[k] / l;
            uint32_t tag = (uint32_t)(rng() % 64);
            memcpy(&src[i][9], &tag, 4);
        }
        mp::PlayerState p{};
        mp::PackPose(p, count, src);
        int m = mp::UnpackPose(p, dst);
        if (m != count) tagsOk = false;
        Err er = Compare(src, dst, count);
        worstPos = fmaxf(worstPos, er.pos);
        worstDir = fmaxf(worstDir, er.dir);
        tagsOk = tagsOk && er.tags;
    }
    // positions are absolute floats around +-5000 m, so float rounding adds ~0.5 mm
    CHECK(worstPos <= 0.0012f, "worst position error %.5f m", worstPos);
    CHECK(worstDir <= 0.0001f, "worst direction error %.6f", worstDir);
    CHECK(tagsOk, "counts/tags preserved");
}

static void TestRotation() {
    printf("rotation: heading, lean and bone lengths (yaw sweep -180..180, through the network codec)\n");
    float src[mp::kMaxBones][kBoneFloats] = {}, rot[mp::kMaxBones][kBoneFloats] = {},
          rx[mp::kMaxBones][kBoneFloats] = {};
    LoadFixture(src);
    float h0 = posemath::Heading(src) * 57.29578f, tilt0, az0;
    Lean(src, 19, tilt0, az0);
    const float to[3] = {1000.f, -250.f, 300.f};
    float worstH = 0, worstLen = 0, worstTilt = 0, worstAz = 0, worstRelDir = 0;
    bool tags = true;
    for (int deg = -180; deg <= 180; deg += 15) {
        float yaw = deg * 3.14159265f / 180.f;
        posemath::Transform(src, rot, 19, src[0], to, yaw);
        mp::PlayerState ps{};
        mp::PackPose(ps, 19, rot);  // what actually goes over the wire
        mp::UnpackPose(ps, rx);
        float h = posemath::Heading(rx) * 57.29578f;
        worstH = fmaxf(worstH, fabsf(WrapDeg(h - h0 - deg)));
        for (int i = 0; i < 19; ++i) {
            worstLen = fmaxf(worstLen, fabsf(posemath::BoneLength(rx[i]) - posemath::BoneLength(src[i])));
            // each bone's direction vector rotates by exactly yaw too
            float ds = atan2f(src[i][7], src[i][6]) * 57.29578f, dr = atan2f(rx[i][7], rx[i][6]) * 57.29578f;
            float horiz = sqrtf(src[i][6] * src[i][6] + src[i][7] * src[i][7]);
            if (horiz > 0.05f) worstRelDir = fmaxf(worstRelDir, fabsf(WrapDeg(dr - ds - deg)));
            if (fabsf(rx[i][8] - src[i][8]) > 1e-4f) worstRelDir = 999;
            tags = tags && posemath::Tag(rx[i]) == kFixtureTags[i];
        }
        float tilt, az;
        Lean(rx, 19, tilt, az);
        worstTilt = fmaxf(worstTilt, fabsf(tilt - tilt0));
        worstAz = fmaxf(worstAz, fabsf(WrapDeg(az - az0 - deg)));
        CHECK(fabsf(rx[0][0] - to[0]) < 0.001f && fabsf(rx[0][1] - to[1]) < 0.001f, "feet placed at target (yaw %d)",
              deg);
    }
    CHECK(worstH <= 0.05f, "heading follows yaw: worst error %.4f deg", worstH);
    CHECK(worstLen <= 0.002f, "bone lengths preserved: worst error %.5f m", worstLen);
    CHECK(worstTilt <= 0.05f, "body tilt preserved: worst error %.4f deg", worstTilt);
    CHECK(worstAz <= 0.5f, "lean direction follows yaw: worst error %.4f deg", worstAz);
    CHECK(worstRelDir <= 0.05f, "bone direction vectors rotate with yaw: worst error %.4f deg", worstRelDir);
    CHECK(tags, "tags survive rotation + codec");
}

static void TestSmoothing() {
    printf("smoothing converges to the received pose\n");
    float target[mp::kMaxBones][kBoneFloats] = {}, cur[mp::kMaxBones][kBoneFloats] = {};
    LoadFixture(target);
    const float to[3] = {target[0][0] + 3, target[0][1] - 2, target[0][2]};
    posemath::Transform(target, cur, 19, target[0], to, 1.2f);  // start rotated and displaced
    for (int i = 0; i < 200; ++i) posemath::EaseToward(cur, target, 19, 0.1f);
    Err e = Compare(target, cur, 19);
    CHECK(e.pos <= 0.0005f, "position error after easing %.5f m", e.pos);
    CHECK(e.dir <= 0.0005f, "direction error after easing %.5f", e.dir);
    CHECK(e.tags, "tags exact while easing");
    float worstUnit = 0;
    for (int i = 0; i < 19; ++i)
        worstUnit = fmaxf(worstUnit, fabsf(sqrtf(cur[i][6] * cur[i][6] + cur[i][7] * cur[i][7] + cur[i][8] * cur[i][8]) - 1));
    CHECK(worstUnit <= 1e-4f, "direction vectors stay unit length (%.6f)", worstUnit);
}

static void TestNetworkRelay() {
    printf("network: host + 2 clients over UDP, pose relayed client A -> host -> client B\n");
    const uint16_t port = (uint16_t)(47900 + GetCurrentProcessId() % 50);
    mp::Config hc, cc;
    hc.host = true;
    hc.port = port;
    hc.name = "Host";
    cc.host = false;
    cc.hostAddr = "127.0.0.1";
    cc.port = port;
    mp::Net host, a, b;
    CHECK(host.Start(hc, 0x1111), "host start");
    cc.name = "A";
    CHECK(a.Start(cc, 0x2222), "client A start");
    cc.name = "B";
    CHECK(b.Start(cc, 0x3333), "client B start");

    float pose[mp::kMaxBones][kBoneFloats] = {}, rotated[mp::kMaxBones][kBoneFloats] = {};
    LoadFixture(pose);
    const float to[3] = {pose[0][0] + 5, pose[0][1], pose[0][2]};
    posemath::Transform(pose, rotated, 19, pose[0], to, 0.7f);

    auto state = [](const char* name, const char* scene, const float (*bones)[kBoneFloats]) {
        mp::PlayerState s{};
        snprintf(s.name, sizeof s.name, "%s", name);
        snprintf(s.scene, sizeof s.scene, "%s", scene);
        memcpy(s.pos, bones[0], 12);
        s.flags = mp::kFlagInWorld;
        s.hue = 0.25f;
        mp::PackPose(s, 19, bones);
        return s;
    };
    // A has a non-Latin Steam name; every player carries a SteamID64.
    const char* aName = "ã¹ã­ã¼ A";  // "スキー A"
    mp::PlayerState hs = state("Host", "waldtal", pose), as = state(aName, "waldtal", rotated),
                    bs = state("B", "waldtal", pose);
    hs.steamId = 76561197960287930ULL;
    as.steamId = 76561198000000001ULL;
    double t0 = GetTickCount64() / 1000.0;
    for (int i = 0; i < 300; ++i) {
        double now = GetTickCount64() / 1000.0 - t0;
        host.Tick(now, hs);
        a.Tick(now, as);
        b.Tick(now, bs);
        Sleep(5);
    }
    auto find = [](const std::vector<mp::RemotePlayer>& v, uint32_t id) -> const mp::RemotePlayer* {
        for (auto& r : v)
            if (r.state.id == id) return &r;
        return nullptr;
    };
    auto rb = b.Remotes();
    bool joinEvent = false;
    for (auto& e : b.TakeEvents())
        if (e.joined && e.steamId == 76561198000000001ULL && e.name == aName) joinEvent = true;
    CHECK(joinEvent, "B gets a join event for A with A's Steam name + ID");
    const mp::RemotePlayer* aAtB = find(rb, 0x2222);
    CHECK(aAtB != nullptr, "client B sees client A");
    CHECK(find(rb, 0x1111) != nullptr, "client B sees the host");
    CHECK(b.HostScene() == "waldtal", "client B knows the host's scene ('%s')", b.HostScene().c_str());
    CHECK(find(host.Remotes(), 0x2222) && find(host.Remotes(), 0x3333), "host sees both clients");
    if (aAtB) {
        float rx[mp::kMaxBones][kBoneFloats] = {};
        int n = mp::UnpackPose(aAtB->state, rx);
        CHECK(n == 19, "bone count %d", n);
        Err e = Compare(rotated, rx, 19);
        CHECK(e.pos <= 0.0006f && e.dir <= 0.0001f && e.tags, "A's pose arrives intact at B (pos %.5f dir %.6f)",
              e.pos, e.dir);
        float hd = WrapDeg((posemath::Heading(rx) - posemath::Heading(pose)) * 57.29578f);
        CHECK(fabsf(hd - 0.7f * 57.29578f) < 0.1f, "A's rotation (40.1 deg) is preserved: %.2f deg", hd);
        CHECK(strcmp(aAtB->state.name, aName) == 0 && fabsf(aAtB->state.hue - 0.25f) < 1e-6f,
              "UTF-8 name/hue preserved");
        CHECK(aAtB->state.steamId == 76561198000000001ULL, "A's SteamID is relayed to B");
        const mp::RemotePlayer* hAtB = find(rb, 0x1111);
        CHECK(hAtB && hAtB->state.steamId == 76561197960287930ULL, "host's SteamID reaches B");
    }
    host.Stop();
    a.Stop();
    b.Stop();
}

int main() {
    TestPackRoundTrip();
    TestRotation();
    TestSmoothing();
    TestNetworkRelay();
    printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
