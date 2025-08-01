#include "XPlaneUDP.hpp"
#include <ctime>

int main () {
    auto xp = XPlaneUdp();
    xp.addDataref("sim/flightmodel/position/latitude"); // 读取基本数据
    xp.addDataref("sim/flightmodel/position/longitude");
    xp.addDataref("sim/flightmodel/engine/POINT_thrust", 10, 0); // 读取数组
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << xp.getDataref("sim/flightmodel/position/latitude") << std::endl;
        int index = xp.datarefName2Id("sim/flightmodel/position/longitude");
        std::cout << xp.getDataref(index) << std::endl;
        std::cout << xp.getDataref("sim/flightmodel/engine/POINT_thrust", 0, 0) << std::endl;
        std::cout << "-------" << std::endl;
    }
    return 0;
}