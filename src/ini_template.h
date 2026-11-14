// The gma_mp.ini settings file, written by the mod (defaults on first run, then
// every change made in the game under Settings > Multiplayer).
#pragma once
#include <string>

namespace mp {

struct IniValues {
    bool host = true;
    std::string hostAddress = "127.0.0.1";
    int port = 47800;
    std::string name = "Skier";
    bool followHostScene = true;
    bool showNames = true;
    bool showPanel = true;
    std::string toggleKey = "F8";
    int sendRateHz = 20;
};

inline std::string MakeIni(const IniValues& v) {
    auto b = [](bool x) { return std::string(x ? "1" : "0"); };
    return std::string(
               "; ============================================================\n"
               ";  Grand Multiplayer Adventure - settings\n"
               ";  (also editable in game: Settings > Multiplayer)\n"
               "; ============================================================\n"
               ";\n"
               "; One player hosts, everyone else joins that host.\n"
               ";   mode = host    -> this PC hosts. Forward the UDP port below on your router\n"
               ";                     to this PC, then give friends your public IP.\n"
               ";   mode = client  -> join the host at host_address (IP, hostname or IP:port)\n"
               "mode = ") +
           (v.host ? "host" : "client") + "\nhost_address = " + v.hostAddress + "\nport = " + std::to_string(v.port) +
           "\n"
           "\n"
           "; Your name as shown to other players when Steam is unavailable\n"
           "; (normally your Steam name and avatar are shown)\n"
           "name = " +
           v.name +
           "\n"
           "\n"
           "; Clients automatically travel to whatever mountain the host is on\n"
           "follow_host_scene = " +
           b(v.followHostScene) +
           "\n"
           "\n"
           "; Interface\n"
           "show_names = " +
           b(v.showNames) +
           "\n"
           "; Player list, shown / hidden in game with the toggle key (F8 by default, F1-F12)\n"
           "show_panel = " +
           b(v.showPanel) +
           "\n"
           "toggle_key = " +
           v.toggleKey +
           "\n"
           "\n"
           "; Network updates per second\n"
           "send_rate_hz = " +
           std::to_string(v.sendRateHz) + "\n";
}

inline std::string DefaultIni() { return MakeIni(IniValues{}); }

}  // namespace mp
