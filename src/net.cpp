#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ini_template.h"
#include "log.h"

namespace mp {

static std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}


Config LoadConfig(const std::wstring& iniPath) {
    Config c;
    FILE* f = _wfopen(iniPath.c_str(), L"rb");
    if (!f) {
        f = _wfopen(iniPath.c_str(), L"wb");
        if (f) {
            fputs(DefaultIni().c_str(), f);
            fclose(f);
        }
        return c;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        std::string l = Trim(line);
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(l.substr(0, eq)), v = Trim(l.substr(eq + 1));
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        std::string lv = v;
        std::transform(lv.begin(), lv.end(), lv.begin(), ::tolower);
        if (k == "mode") c.host = (lv == "host");
        else if (k == "host_address") c.hostAddr = v;  // "ip", "name" or either with ":port"
        else if (k == "port") c.port = (uint16_t)atoi(v.c_str());
        else if (k == "name") c.name = v.substr(0, kMaxName - 1);
        else if (k == "send_rate_hz") c.sendRateHz = std::min(60.f, std::max(1.f, (float)atof(v.c_str())));
        else if (k == "show_names") c.showNames = atoi(v.c_str()) != 0;
        else if (k == "show_panel") c.showPanel = atoi(v.c_str()) != 0;
        else if (k == "debug_pose_log") c.debugPoseLog = atoi(v.c_str()) != 0;
        else if (k == "toggle_key") {
            if (lv.size() >= 2 && lv[0] == 'f') {
                int n = atoi(lv.c_str() + 1);
                if (n >= 1 && n <= 12) c.toggleKey = VK_F1 + n - 1;
            }
        }
    }
    fclose(f);
    if (c.name.empty()) c.name = "Skier";
    return c;
}

static SOCKET S(uintptr_t s) { return (SOCKET)s; }

static int16_t Q(float v, float scale) {
    float q = v * scale;
    if (!(q == q)) q = 0;  // NaN
    q = q < -32767.f ? -32767.f : (q > 32767.f ? 32767.f : q);
    return (int16_t)lrintf(q);
}

uint8_t PackHeading(const float dir[2]) {
    float a = atan2f(dir[1], dir[0]) * (128.f / 3.14159265f);  // -128..128
    return (uint8_t)(lrintf(a) & 0xff);
}

void UnpackHeading(uint8_t h, float dir[2]) {
    float a = (float)h * (3.14159265f / 128.f);
    dir[0] = cosf(a);
    dir[1] = sinf(a);
}

void PackPose(PlayerState& ps, int count, const float (*bones)[kBoneFloats]) {
    if (count < 0 || count > kMaxBones) count = 0;
    ps.boneCount = (uint8_t)count;
    if (count == 0) return;
    for (int k = 0; k < 3; ++k) ps.root[k] = bones[0][k];
    for (int b = 0; b < count; ++b) {
        for (int k = 0; k < 3; ++k) {
            ps.bones[b][k] = Q(bones[b][k] - ps.root[k], 1000.f);
            ps.bones[b][3 + k] = Q(bones[b][3 + k] - ps.root[k], 1000.f);
            ps.bones[b][6 + k] = Q(bones[b][6 + k], 32767.f);
        }
        memcpy(&ps.boneTag[b], &bones[b][9], 4);
    }
}

int UnpackPose(const PlayerState& ps, float (*bones)[kBoneFloats]) {
    int count = ps.boneCount > kMaxBones ? 0 : ps.boneCount;
    for (int b = 0; b < count; ++b) {
        for (int k = 0; k < 3; ++k) {
            bones[b][k] = ps.root[k] + ps.bones[b][k] / 1000.f;
            bones[b][3 + k] = ps.root[k] + ps.bones[b][3 + k] / 1000.f;
            bones[b][6 + k] = ps.bones[b][6 + k] / 32767.f;
        }
        memcpy(&bones[b][9], &ps.boneTag[b], 4);
    }
    return count;
}

bool Net::Start(const Config& cfg, uint32_t localId) {
    cfg_ = cfg;
    localId_ = localId;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log("WSAStartup failed");
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        Log("socket() failed: %d", WSAGetLastError());
        return false;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    // Windows reports ICMP "port unreachable" as WSAECONNRESET on the next
    // recvfrom of a UDP socket; disable that so a client that left doesn't
    // stall the host's receive loop.
    DWORD off = FALSE, ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
    int rcvbuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof rcvbuf);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = cfg_.host ? htons(cfg_.port) : 0;
    if (bind(s, (sockaddr*)&local, sizeof local) != 0) {
        Log("bind() failed on port %u: %d", cfg_.host ? cfg_.port : 0, WSAGetLastError());
        closesocket(s);
        return false;
    }
    sock_ = (uintptr_t)s;
    Log("Net started as %s (port %u, name '%s', id %08X, state %u bytes)", cfg_.host ? "HOST" : "CLIENT", cfg_.port,
        cfg_.name.c_str(), localId_, (unsigned)sizeof(PlayerState));
    if (!cfg_.host) Log("Will connect to host %s", cfg_.hostAddr.c_str());
    return true;
}

void Net::Stop() {
    if (sock_ == ~uintptr_t(0)) return;
    closesocket(S(sock_));
    sock_ = ~uintptr_t(0);
    WSACleanup();
}

void Net::SendState(const PlayerState& ps, const void* addr, int addrLen) {
    uint8_t buf[sizeof(PacketHeader) + sizeof(PlayerState)];
    PacketHeader h{kMagic, kProtoVersion, kPktState, 1};
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, &ps, sizeof ps);
    sendto(S(sock_), (char*)buf, sizeof buf, 0, (const sockaddr*)addr, addrLen);
}

// Returns true if the state was new (not stale/duplicate).
bool Net::HandleState(const PlayerState& in, double now, const void* fromAddr, int fromLen) {
    if (in.id == localId_) return false;
    PlayerState ps = in;
    ps.name[kMaxName - 1] = 0;
    ps.scene[kMaxScene - 1] = 0;
    if (ps.boneCount > kMaxBones) ps.boneCount = 0;

    auto it = std::find_if(remotes_.begin(), remotes_.end(), [&](const RemotePlayer& r) { return r.state.id == ps.id; });
    if (it == remotes_.end()) {
        RemotePlayer r;
        r.state = ps;
        memcpy(r.prevPos, ps.pos, sizeof r.prevPos);
        r.prevTime = r.curTime = now;
        r.lastHeard = now;
        remotes_.push_back(r);
        Log("Player joined: '%s' (%08X)", ps.name, ps.id);
        events_.push_back({ps.name, ps.steamId, true});
    } else {
        if ((int32_t)(ps.seq - it->state.seq) <= 0) return false;
        memcpy(it->prevPos, it->state.pos, sizeof it->prevPos);
        it->prevTime = it->curTime;
        it->curTime = now;
        it->state = ps;
        it->lastHeard = now;
    }

    if (cfg_.host && fromAddr) {
        auto p = std::find_if(peers_.begin(), peers_.end(), [&](const Peer& q) {
            return q.addrLen == fromLen && memcmp(q.addr, fromAddr, fromLen) == 0;
        });
        if (p == peers_.end()) {
            Peer np;
            memcpy(np.addr, fromAddr, std::min<int>(fromLen, sizeof np.addr));
            np.addrLen = fromLen;
            np.id = ps.id;
            np.lastHeard = now;
            peers_.push_back(np);
        } else {
            p->id = ps.id;
            p->lastHeard = now;
        }
    }
    return true;
}

void Net::Receive(double now) {
    uint8_t buf[2048];
    for (int guard = 0; guard < 512; ++guard) {
        sockaddr_storage from{};
        int fromLen = sizeof from;
        int n = recvfrom(S(sock_), (char*)buf, sizeof buf, 0, (sockaddr*)&from, &fromLen);
        if (n <= 0) break;
        if (n < (int)(sizeof(PacketHeader) + sizeof(PlayerState))) continue;
        PacketHeader h;
        memcpy(&h, buf, sizeof h);
        if (h.magic != kMagic || h.version != kProtoVersion || h.count != 1) continue;
        PlayerState ps;
        memcpy(&ps, buf + sizeof h, sizeof ps);

        if (h.type == kPktBye) {
            auto it = std::find_if(remotes_.begin(), remotes_.end(), [&](const RemotePlayer& r) { return r.state.id == ps.id; });
            if (it != remotes_.end()) {
                events_.push_back({it->state.name, it->state.steamId, false});
                remotes_.erase(it);
            }
            peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const Peer& p) { return p.id == ps.id; }),
                         peers_.end());
            continue;
        }
        if (h.type != kPktState) continue;

        if (cfg_.host) {
            if (HandleState(ps, now, &from, fromLen)) {
                // Relay to every other client.
                for (auto& p : peers_)
                    if (p.id != ps.id) SendState(ps, p.addr, p.addrLen);
            }
        } else {
            hostHeard_ = now;
            HandleState(ps, now, nullptr, 0);
        }
    }
}

void Net::Tick(double now, const PlayerState& local) {
    if (sock_ == ~uintptr_t(0)) return;
    Receive(now);

    const double kTimeout = 5.0;
    for (auto it = remotes_.begin(); it != remotes_.end();) {
        if (now - it->lastHeard > kTimeout) {
            Log("Player timed out: '%s'", it->state.name);
            events_.push_back({it->state.name, it->state.steamId, false});
            it = remotes_.erase(it);
        } else {
            ++it;
        }
    }
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(), [&](const Peer& p) { return now - p.lastHeard > kTimeout; }),
                 peers_.end());

    if (now - lastSend_ < 1.0 / cfg_.sendRateHz) return;
    lastSend_ = now;
    localState_ = local;
    localState_.id = localId_;
    localState_.seq = ++seq_;
    if (cfg_.host) {
        localState_.flags |= kFlagHost;
        for (auto& p : peers_) SendState(localState_, p.addr, p.addrLen);
        return;
    }
    // Client: resolve the host (IP or hostname) once, retrying every 5s on failure.
    if (!haveResolved_ && now - lastResolve_ > 5.0) {
        lastResolve_ = now;
        // host_address may carry its own port ("ip:port"); otherwise the shared port.
        std::string hostName = cfg_.hostAddr;
        uint16_t hostPort = cfg_.port;
        size_t colon = hostName.rfind(':');
        if (colon != std::string::npos && hostName.find(':') == colon) {
            int p = atoi(hostName.c_str() + colon + 1);
            if (p > 0 && p <= 65535) hostPort = (uint16_t)p;
            hostName = hostName.substr(0, colon);
        }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(hostPort);
        if (inet_pton(AF_INET, hostName.c_str(), &to.sin_addr) == 1) {
            haveResolved_ = true;
        } else {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(hostName.c_str(), nullptr, &hints, &res) == 0 && res) {
                to.sin_addr = ((sockaddr_in*)res->ai_addr)->sin_addr;
                freeaddrinfo(res);
                haveResolved_ = true;
            } else {
                Log("Could not resolve host '%s'", cfg_.hostAddr.c_str());
            }
        }
        memcpy(resolvedAddr_, &to, sizeof to);
    }
    if (haveResolved_) SendState(localState_, resolvedAddr_, sizeof(sockaddr_in));
}

std::vector<RemotePlayer> Net::Remotes() const { return remotes_; }

}  // namespace mp
