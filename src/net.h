// Minimal UDP relay for GMA multiplayer.
//
// Topology: one player runs as HOST (listens on a UDP port), everyone else is a
// CLIENT pointed at the host. Every peer sends its own PlayerState (including
// the game's skeletal pose of the skier and the mountain it is on) to the host
// ~20x/s. The host forwards each state to every other client and sends its own
// state, flagged kFlagHost. Everyone goes wherever they like; skiers are shown
// to the players on the same mountain. One state per datagram keeps packets
// well under the MTU.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mp {

constexpr uint32_t kMagic = 0x4D414D47;  // "GMAM"
constexpr uint16_t kProtoVersion = 4;
constexpr int kMaxName = 48;  // UTF-8 bytes incl. terminator (Steam names can be non-Latin)
constexpr int kMaxScene = 32;
constexpr int kMaxBones = 26;
constexpr int kBoneFloats = 10;

enum StateFlags : uint8_t {
    kFlagInWorld = 1,  // pose/pos are valid (player is skiing in a scene)
    kFlagHost = 2,     // sender is the host
    kFlagHeading = 4,  // `heading` is set (older builds leave it 0 and don't set this)
};

#pragma pack(push, 1)
struct PlayerState {
    uint32_t id;                // random per session
    uint32_t seq;               // increments per send
    uint64_t steamId;           // SteamID64 of the sender (0 = not on Steam); avatar lookup
    char name[kMaxName];        // display name (Steam persona name, else the ini name)
    char scene[kMaxScene];      // internal scene name, e.g. "waldtal"
    float pos[3];               // feet position (world, Z up) - for UI/distances
    float head[3];              // head position - name tag anchor
    float hue;                  // the player's character_color_hue setting
    uint8_t color;              // UI palette index
    uint8_t flags;              // StateFlags
    uint8_t boneCount;          // pose bones in use
    uint8_t heading;            // facing (the game's GetSkiDirectionXY), 256 steps
                                // per turn from +X toward +Y; see kFlagHeading
    float root[3];              // pose origin; bones are stored relative to it
    // Per bone: start xyz, end xyz (millimetres from root), dir xyz (x32767)
    int16_t bones[kMaxBones][9];
    // Per bone: the 10th word verbatim. It is an integer (body part / material
    // id) stored in a float slot, so it must not be quantised.
    uint32_t boneTag[kMaxBones];
};

struct PacketHeader {
    uint32_t magic;
    uint16_t version;
    uint8_t type;               // PacketType
    uint8_t count;              // number of PlayerState records that follow (1)
};
#pragma pack(pop)

enum PacketType : uint8_t {
    kPktState = 2,   // one PlayerState (client->host, host->clients)
    kPktBye = 3,     // sender is leaving
};

// Facing direction <-> wire byte.
uint8_t PackHeading(const float dir[2]);
void UnpackHeading(uint8_t h, float dir[2]);

// Pose <-> wire conversion.
void PackPose(PlayerState& ps, int count, const float (*bones)[kBoneFloats]);
int UnpackPose(const PlayerState& ps, float (*bones)[kBoneFloats]);

struct RemotePlayer {
    PlayerState state;
    double lastHeard = 0;        // local time (seconds) the state arrived
    // Previous sample, used to estimate velocity for extrapolation.
    float prevPos[3] = {0, 0, 0};
    double prevTime = 0;
    double curTime = 0;
};

struct Config {
    bool host = true;
    std::string hostAddr = "127.0.0.1";  // host to join: "ip", "name", or either with ":port"
    uint16_t port = 47800;
    std::string name = "Skier";
    float sendRateHz = 20.f;
    bool showNames = true;
    bool showPanel = true;
    bool debugPoseLog = false;  // log local + remote pose summaries (tests)
    int toggleKey = 0x77;  // VK_F8
};

Config LoadConfig(const std::wstring& iniPath);

class Net {
public:
    bool Start(const Config& cfg, uint32_t localId);
    void Stop();

    // Called once per frame from the render thread.
    void Tick(double now, const PlayerState& local);

    // Snapshot of remote players (excludes the local player).
    std::vector<RemotePlayer> Remotes() const;

    bool IsHost() const { return cfg_.host; }
    // Client: seconds since the host last answered (inf if never).
    double SinceHostHeard(double now) const { return hostHeard_ > 0 ? now - hostHeard_ : 1e9; }
    int PeerCount() const { return (int)peers_.size(); }

    // Join/leave notifications produced during Tick (drained by caller).
    struct Event {
        std::string name;
        uint64_t steamId;
        bool joined;
    };
    std::vector<Event> TakeEvents() { auto e = std::move(events_); events_.clear(); return e; }

private:
    void Receive(double now);
    bool HandleState(const PlayerState& ps, double now, const void* fromAddr, int fromLen);
    void SendState(const PlayerState& ps, const void* addr, int addrLen);

    Config cfg_;
    uint32_t localId_ = 0;
    uintptr_t sock_ = ~uintptr_t(0);
    double lastSend_ = 0;
    PlayerState localState_{};

    struct Peer {
        uint8_t addr[32];
        int addrLen = 0;
        uint32_t id = 0;
        double lastHeard = 0;
    };
    std::vector<Peer> peers_;            // host only: known client endpoints
    std::vector<RemotePlayer> remotes_;  // everybody: other players
    uint32_t seq_ = 0;
    double hostHeard_ = 0;
    std::vector<Event> events_;
    uint8_t resolvedAddr_[16] = {};
    bool haveResolved_ = false;
    double lastResolve_ = -1e9;
    // All Net methods run on the game's render thread (inside the swap hook),
    // so no locking is needed.
};

}  // namespace mp
