// Test peers for one-PC testing (protocol v4). All modes use the host's REAL
// skeletal pose (as sent by the game running the mod), so what you see is the
// game's own skier.
//
//   fakeclient.exe [host] [port] [seconds] [radius]
//       circle: skis in circles around the host's player, turning to face along
//       the circle (pose rotated to the direction of travel).
//
//   fakeclient.exe echo [host] [port] [seconds] [dx] [dy] [yawDeg]
//       echo: copies the host's live pose every tick, rotated by yawDeg about
//       the host's feet and moved by (dx, dy). With yaw 0 the bot is an exact
//       twin standing beside you; it must turn/lean/crouch/jump exactly as you
//       do. Used to verify rotation and pose fidelity in game.
//
//   fakeclient.exe host <port> <scene> <seconds>
//       fake host that claims to be on <scene> and circles the client.
//
// Environment: GMA_FAKE_STEAMID=<SteamID64> makes the bot announce that Steam
// account (its avatar is then shown); GMA_FAKE_NAME overrides the bot's name;
// GMA_FAKE_ID=<number> sets the player id (to run several bots at once);
// GMA_FAKE_SCENE=<scene> makes a client bot claim to be on that mountain;
// GMA_FAKE_STAY=1 makes a client bot stay on the first mountain it sees the
// host on (it keeps circling the host's last spot there after the host leaves).
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../src/net.h"
#include "../src/pose_math.h"

static double Now() {
    static LARGE_INTEGER f, t0;
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        init = true;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
}

// dst = src's pose rotated by `yaw` about src's feet and placed so the feet land at `at`.
static void PlacePose(const mp::PlayerState& src, mp::PlayerState& dst, const float at[3], float yaw) {
    float in[mp::kMaxBones][mp::kBoneFloats], out[mp::kMaxBones][mp::kBoneFloats];
    int n = mp::UnpackPose(src, in);
    posemath::Transform(in, out, n, src.root, at, yaw);
    mp::PackPose(dst, n, out);
    float c = cosf(yaw), s = sinf(yaw);
    for (int i = 0; i < 2; ++i) {
        const float* v = i == 0 ? src.pos : src.head;
        float* o = i == 0 ? dst.pos : dst.head;
        float x = v[0] - src.root[0], y = v[1] - src.root[1];
        o[0] = at[0] + c * x - s * y;
        o[1] = at[1] + s * x + c * y;
        o[2] = at[2] + (v[2] - src.root[2]);
    }
}

static void Send(SOCKET s, const mp::PlayerState& ps, const sockaddr* to, int toLen) {
    uint8_t out[sizeof(mp::PacketHeader) + sizeof(mp::PlayerState)];
    mp::PacketHeader h{mp::kMagic, mp::kProtoVersion, mp::kPktState, 1};
    memcpy(out, &h, sizeof h);
    memcpy(out + sizeof h, &ps, sizeof ps);
    sendto(s, (char*)out, sizeof out, 0, to, toLen);
}

// Challenge results the bot pretends to have: the host's own, slightly changed
// (GMA_FAKE_SCORE_FACTOR, default 0.97 = 3% better), so they land on the same
// challenges' lists.
static mp::Records g_hostScores, g_myScores;
static uint32_t g_hostScoreVersion = 0;
static float g_scoreFactor = 0.97f;

static void OnHostScores(const uint8_t* buf, int n) {
    if (n < (int)(sizeof(mp::PacketHeader) + sizeof(mp::ScoreChunkHeader))) return;
    mp::ScoreChunkHeader ch;
    memcpy(&ch, buf + sizeof(mp::PacketHeader), sizeof ch);
    if (ch.version != g_hostScoreVersion) g_hostScoreVersion = ch.version, g_hostScores.clear();
    size_t off = sizeof(mp::PacketHeader) + sizeof ch;
    mp::DecodeRecords(std::string((const char*)buf + off, n - off), g_hostScores);
    g_myScores.clear();
    for (auto& kv : g_hostScores)
        for (const mp::Result& r : kv.second)
            g_myScores[kv.first].push_back({r.value * g_scoreFactor, (uint8_t)(r.sport % 4 + 1)});
}

static void SendScores(SOCKET s, uint32_t id, const sockaddr* to, int toLen) {
    static size_t next = 0;
    if (g_myScores.empty()) return;
    uint32_t version = 0;
    std::vector<std::string> chunks = mp::EncodeRecords(g_myScores, version);
    size_t i = next++ % chunks.size();
    uint8_t out[sizeof(mp::PacketHeader) + sizeof(mp::ScoreChunkHeader) + mp::kScorePayloadMax];
    mp::PacketHeader h{mp::kMagic, mp::kProtoVersion, mp::kPktScores, 0};
    mp::ScoreChunkHeader ch{id, version, (uint16_t)i, (uint16_t)chunks.size()};
    memcpy(out, &h, sizeof h);
    memcpy(out + sizeof h, &ch, sizeof ch);
    memcpy(out + sizeof h + sizeof ch, chunks[i].data(), chunks[i].size());
    sendto(s, (char*)out, (int)(sizeof h + sizeof ch + chunks[i].size()), 0, to, toLen);
}

static bool Recv(SOCKET s, mp::PlayerState& ps, sockaddr_in* from) {
    uint8_t buf[2048];
    int fl = sizeof(sockaddr_in);
    for (;;) {
        int n = recvfrom(s, (char*)buf, sizeof buf, 0, (sockaddr*)from, &fl);
        if (n < (int)sizeof(mp::PacketHeader)) return false;
        mp::PacketHeader h;
        memcpy(&h, buf, sizeof h);
        if (h.magic != mp::kMagic || h.version != mp::kProtoVersion) continue;
        if (h.type == mp::kPktScores) {
            mp::ScoreChunkHeader ch;
            if (n >= (int)(sizeof h + sizeof ch)) memcpy(&ch, buf + sizeof h, sizeof ch);
            // only the host's own results (not other bots' relayed ones)
            // (bot ids start with 0xB0)
            if (n >= (int)(sizeof h + sizeof ch) && (ch.sender >> 24) != 0xB0) OnHostScores(buf, n);
            continue;
        }
        if (n < (int)(sizeof h + sizeof(mp::PlayerState)) || h.type != mp::kPktState) continue;
        break;
    }
    mp::PacketHeader h;
    memcpy(&h, buf, sizeof h);
    memcpy(&ps, buf + sizeof h, sizeof ps);
    return true;
}

int main(int argc, char** argv) {
    enum { kCircle, kEcho, kHost } mode = kCircle;
    int a = 1;
    if (argc > 1 && strcmp(argv[1], "host") == 0) mode = kHost, a = 2;
    else if (argc > 1 && strcmp(argv[1], "echo") == 0) mode = kEcho, a = 2;
    auto arg = [&](int i, const char* def) { return argc > a + i ? argv[a + i] : def; };

    const char* host = mode == kHost ? "0.0.0.0" : arg(0, "127.0.0.1");
    int port = mode == kHost ? atoi(arg(0, "47801")) : atoi(arg(1, "47800"));
    const char* hostScene = mode == kHost ? arg(1, "waldtal") : "";
    double seconds = atof(mode == kHost ? arg(2, "120") : arg(2, "60"));
    float radius = mode == kCircle ? (float)atof(arg(3, "10")) : 0.f;
    float echoDx = mode == kEcho ? (float)atof(arg(3, "0")) : 0.f;
    float echoDy = mode == kEcho ? (float)atof(arg(4, "4")) : 0.f;
    float echoYaw = mode == kEcho ? (float)atof(arg(5, "0")) * 3.14159265f / 180.f : 0.f;

    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in peer{};
    bool havePeer = false;
    if (mode == kHost) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons((u_short)port);
        bind(s, (sockaddr*)&local, sizeof local);
    } else {
        peer.sin_family = AF_INET;
        peer.sin_port = htons((u_short)port);
        inet_pton(AF_INET, host, &peer.sin_addr);
        havePeer = true;
    }

    mp::PlayerState me{}, other{};
    me.id = mode == kHost ? 0xF0057u : 0xB07B07u;
    snprintf(me.name, sizeof me.name, "%s", mode == kHost ? "FakeHost" : (mode == kEcho ? "EchoBot" : "TestBot"));
    if (const wchar_t* wn = _wgetenv(L"GMA_FAKE_NAME"))  // read as Unicode, send as UTF-8
        WideCharToMultiByte(CP_UTF8, 0, wn, -1, me.name, sizeof me.name - 1, nullptr, nullptr);
    if (const char* sid = getenv("GMA_FAKE_STEAMID")) me.steamId = strtoull(sid, nullptr, 10);
    if (const char* id = getenv("GMA_FAKE_ID")) me.id = (uint32_t)strtoul(id, nullptr, 0);
    const char* sceneOverride = getenv("GMA_FAKE_SCENE");
    if (const char* f = getenv("GMA_FAKE_SCORE_FACTOR")) g_scoreFactor = (float)atof(f);
    const bool stay = getenv("GMA_FAKE_STAY") != nullptr;
    char stayScene[mp::kMaxScene] = {};
    me.color = (uint8_t)(me.id % 8);
    me.hue = mode == kHost ? -0.6f : 0.45f;
    bool haveOther = false;
    double lastPrint = 0;
    for (;;) {
        double now = Now();
        if (now > seconds) break;
        mp::PlayerState in;
        sockaddr_in from{};
        while (Recv(s, in, &from)) {
            if (in.id == me.id) continue;
            if (mode != kHost && !(in.flags & mp::kFlagHost)) continue;  // follow the host only
            if (stay) {
                bool skiing = in.boneCount > 0 && (in.flags & mp::kFlagInWorld);
                if (!stayScene[0] && skiing) {
                    memcpy(stayScene, in.scene, sizeof stayScene);
                    stayScene[mp::kMaxScene - 1] = 0;
                    printf("staying on %s\n", stayScene);
                }
                // host loading or elsewhere: keep circling its last spot here
                if (stayScene[0] && (!skiing || strncmp(in.scene, stayScene, mp::kMaxScene) != 0)) continue;
            }
            other = in;
            haveOther = true;
            if (mode == kHost) {
                peer = from;
                havePeer = true;
            }
        }
        if (haveOther && other.boneCount > 0) {
            float at[3];
            float yaw;
            if (mode == kEcho) {
                at[0] = other.root[0] + echoDx;
                at[1] = other.root[1] + echoDy;
                at[2] = other.root[2];
                yaw = echoYaw;
            } else {
                float t = (float)now * 0.5f, r = mode == kHost ? 12.f : radius;
                at[0] = other.root[0] + cosf(t) * r;
                at[1] = other.root[1] + sinf(t) * r;
                at[2] = other.root[2] + 0.3f;
                // face along the circle: rotate the host pose from its heading to the tangent
                float in[mp::kMaxBones][mp::kBoneFloats];
                mp::UnpackPose(other, in);
                yaw = (t + 1.5708f) - posemath::Heading(in);
            }
            PlacePose(other, me, at, yaw);
            snprintf(me.scene, sizeof me.scene, "%s",
                     mode == kHost ? hostScene : (sceneOverride ? sceneOverride : other.scene));
            me.flags = mp::kFlagInWorld | (mode == kHost ? mp::kFlagHost : 0);
            if (other.flags & mp::kFlagHeading) {
                // the facing the real player's game reports, turned with the pose
                float d[2], r[2];
                mp::UnpackHeading(other.heading, d);
                r[0] = d[0] * cosf(yaw) - d[1] * sinf(yaw);
                r[1] = d[0] * sinf(yaw) + d[1] * cosf(yaw);
                me.heading = mp::PackHeading(r);
                me.flags |= mp::kFlagHeading;
            }
        } else if (mode == kHost) {
            snprintf(me.scene, sizeof me.scene, "%s", hostScene);
            me.flags = mp::kFlagHost;  // not in world yet, but announce the scene
        }
        me.seq++;
        if (havePeer) Send(s, me, (sockaddr*)&peer, sizeof peer);
        static double lastScores = 0;
        if (havePeer && mode != kHost && now - lastScores > 0.3) {
            lastScores = now;
            SendScores(s, me.id, (sockaddr*)&peer, sizeof peer);
        }
        if (now - lastPrint > 2) {
            lastPrint = now;
            printf("t=%.0fs other=%s scene=%s bones=%d root=(%.1f,%.1f,%.1f)\n", now, haveOther ? other.name : "-",
                   other.scene, other.boneCount, other.root[0], other.root[1], other.root[2]);
            fflush(stdout);
        }
        Sleep(mode == kEcho ? 16 : 50);  // echo at ~60 Hz to keep latency low
    }
    closesocket(s);
    return 0;
}
