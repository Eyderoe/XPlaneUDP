#include "XPlaneUDP.hpp"
#include <ctime>

int main () {
    Asio::io_context io_context;
    auto xp = XPlaneUdp(io_context);
    xp.addDataRef("sim/flightmodel/position/indicated_airspeed", 1);
    xp.addDataRef("sim/flightmodel/position/latitude");
    while (1) {
    }
    return 0;
}
