#include "XPlaneUDP.hpp"

int main () {
    auto xp = XPlaneUdp();
    std::vector<float> n1(2);
    std::string dataref1{"sim/flightmodel/position/latitude"};
    std::string dataref2{"sim/flightmodel/engine/ENGN_N1_"};
    std::string dataref3{"sim/cockpit/radios/com1_freq_hz"};
    xp.addDataref(dataref1); // 读取dataref数据
    xp.addDatarefArray(dataref2, 16); // 读取dataref数组
    bool rev{}; // 写入dataref数据
    xp.addBasicInfo(2); // 机模基本信息
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cout << "-------" << std::endl;

        std::cout << xp.getDataref(xp.datarefName2Id(dataref1)) << std::endl;
        xp.getDatarefArray(dataref2, n1);
        std::cout << n1[0] << ' ' << n1[1] << std::endl;
        if (rev)
            xp.setDataref(dataref3, 12640);
        else
            xp.setDataref(dataref3, 12665);
        rev = !rev;
        std::cout << xp.getBasicInfo().lon << std::endl;
    }
    return 0;
}
