#include "XPlaneUDP.hpp"
#include <ctime>

int main () {
    Asio::io_context io_context;
    auto xp = XPlaneUdp(io_context);
    xp.addDataref("sim/flightmodel/position/latitude"); // 读取基本数据
    xp.addDataref("sim/flightmodel/engine/POINT_thrust", 1, 0); // 读取数组
    while (1) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << xp.getDataref("sim/flightmodel/position/latitude") << std::endl;
        std::cout << xp.getDataref("sim/flightmodel/engine/POINT_thrust", 0, 0) << std::endl;
    }
    return 0;
    // equal 没写
    // socket 锁没写
}
