#include "XPlaneUDP.hpp"

int main () {
    auto xp = XPlaneUdp();
    std::string dataref1{"sim/flightmodel/position/latitude"};
    std::string dataref2{"sim/flightmodel/engine/POINT_thrust"};
    std::string dataref3{"sim/cockpit/radios/com1_freq_hz"};
    xp.addDataref(dataref1); // 读取dataref数据
    xp.addDataref(dataref2, 100, 0); // 读取dataref数组
    bool rev{}; // 写入dataref数据
    xp.addBasicInfo(2); // 机模基本信息
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cout << "-------" << std::endl;

        std::cout << xp.getDataref(xp.datarefName2Id(dataref1)) << std::endl;
        std::cout << xp.getDataref("sim/flightmodel/engine/POINT_thrust", 0, 0) << std::endl;
        if (rev)
            xp.setDataref(dataref3, 12640);
        else
            xp.setDataref(dataref3, 12665);
        rev = !rev;
        std::cout << xp.getBasicInfo().lon << std::endl;
    }
    return 0;
}
