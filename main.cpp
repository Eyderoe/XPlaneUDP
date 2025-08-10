#include "XPlaneUDP.hpp"

int main () {
    auto xp = XPlaneUdp();
    const std::string dataref1{"sim/flightmodel/position/latitude"};
    const std::string dataref2{"sim/flightmodel/engine/ENGN_N1_"};
    const std::string dataref3{"sim/cockpit/radios/com1_freq_hz"};
    xp.addDataref(dataref1); // 读取dataref数据
    xp.addDatarefArray(dataref2, 16); // 读取dataref数组
    bool rev{}; // 写入dataref数据
    xp.addBasicInfo(2); // 机模基本信息
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cout << "-------" << std::endl;
        rev = !rev;

        if (auto latId = xp.datarefName2Id(dataref1); latId.has_value())
            if (auto lat = xp.getDataref(latId.value()); lat.has_value())
                std::cout << lat.value() << std::endl;
        if (auto info = xp.getBasicInfo(); info.has_value())
            std::cout << info.value().lon << std::endl;
        if (auto n1 = xp.getDatarefArray(dataref2); n1.has_value())
            std::cout << n1.value()[0] << ' ' << n1.value()[1] << std::endl;
        xp.setDataref(dataref3, rev ? 12640 : 12665);
    }
    return 0;
}
