#include "XPlaneUDP.hpp"

#ifdef _WIN32
constexpr bool isWindows = true;
#else
constexpr bool isWindows = false;
#endif

using namespace std;

constexpr int headerLength{5}; // 指令头部长度 4字母+1空
const static string datarefHead{'R', 'R', 'E', 'F', '\x00'};
const static string beconHead{'B', 'E', 'C', 'N', '\x00'};


XPlaneUdp::XPlaneUdp (Asio::io_context &io_context): localSocket(io_context), strand_(io_context.get_executor()) {
    autoUdpFind();
    Ip::udp::endpoint local(Ip::udp::v4(), 0);
    localSocket.open(local.protocol());
    localSocket.bind(local);
    addDataref("sim/network/misc/network_time_sec"); // 维护定时器
    startReceive();
}

XPlaneUdp::~XPlaneUdp () {}

/**
 * @brief 寻找电脑上运行的 XPlane 实例
 */
void XPlaneUdp::autoUdpFind () {
    // 创建 UDP 并允许端口复用
    Asio::io_context ioContext;
    Ip::udp::socket multicastSocket(ioContext);
    multicastSocket.open(Ip::udp::v4());
    Asio::socket_base::reuse_address option(true);
    multicastSocket.set_option(option);
    // 绑定至多播
    Ip::udp::endpoint multicastEndpoint;
    if (isWindows)
        multicastEndpoint = Ip::udp::endpoint(Ip::udp::v4(), multiCastPort);
    else
        multicastEndpoint = Ip::udp::endpoint(Ip::make_address(multiCastGroup), multiCastPort);
    multicastSocket.bind(multicastEndpoint);
    Ip::address_v4 multicast_address = Ip::make_address_v4(multiCastGroup);
    multicastSocket.set_option(Ip::multicast::join_group(multicast_address));
    // 接收数据
    udpBuffer_t buffer{};
    Ip::udp::endpoint senderEndpoint;
    size_t bytesReceived;
    try {
        bytesReceived = receiveData(multicastSocket, buffer, senderEndpoint, 3000);
    } catch (const XPlaneTimeout &e) {
        throw XPlaneIpNotFound();
    }

    cout << "XPlane Beacon: ";
    for (size_t i = 0; i < bytesReceived; i++)
        cout << hex << setw(2) << setfill('0') << (static_cast<unsigned int>(buffer[i]) & 0xff);
    cout << endl;
    // 解析数据 12.2.0-rc1
    /* 头部 char8[5] (BECN\x00)
     * 主版本 uchar8 (1 不明)
     * 副版本 uchar8 (2 不明)
     * 程序 int32 (1XP 2PlaneMaker)
     * 版本号 int32 (122015)
     * 规则 uint32 (1主机 2外部 3IOS)
     * 端口 ushort16 (49000)
     * 计算机名称 char8[N] (LAPTOP-NO0CK753)
     */
    if ((bytesReceived < 5 + 16) || (string_view{buffer.data(), 5} != beconHead)) { // 非xp数据
        cerr << "Unknown packet from " << senderEndpoint << endl;
        cout << buffer.size() << " bytes" << endl;
        for (const auto &byte : buffer)
            cout << hex << setw(2) << setfill('0') << (static_cast<unsigned int>(byte) & 0xff);
        cout << endl;
    } else { // xp数据
        string_view data{buffer.data() + 5, 16};
        uint8_t mainVer, minorVer;
        int32_t software, xpVer;
        uint32_t role;
        uint16_t port;
        unpack(data, 0, mainVer, minorVer, software, xpVer, role, port);
        if ((mainVer == 1) && (minorVer <= 2) && (software == 1)) {
            cout << "XPlane Beacon Version: " << static_cast<int>(mainVer) << '.' << static_cast<int>(minorVer) << '.'
                    << software << endl;
            cout << dec << "IP: " << senderEndpoint.address() << ", Port: " << port << ", hostname: " << string_view{
                buffer.data() + 21, bytesReceived - 24
            } << ", XPlaneVersion: " << xpVer << ", role: " << role << endl;
            this->remoteEndpoint = Ip::udp::endpoint(Ip::make_address(senderEndpoint.address().to_string()), port);
        } else {
            cerr << "XPlane Beacon Version not supported: " << mainVer << '.' << minorVer << '.' << software << endl;
            throw XPlaneVersionNotSupported();
        }
    }
}

/**
 * @brief 开始接收异步接收
 */
void XPlaneUdp::startReceive () {
    Ip::udp::endpoint temp{};
    localSocket.async_receive_from(Asio::buffer(udpReceive),temp ,
                                   bind_executor(strand_, [this](auto && PH1, auto && PH2) { handleReceive(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2)); }));
}

/**
 * @brief 异步接收回调函数
 * @param error 错误
 * @param length 接收长度
 */
void XPlaneUdp::handleReceive (const Sys::error_code &error, const size_t length) {
    if (!error) {
        if (equal(datarefHead.begin(), datarefHead.begin() + 4, udpReceive.begin())) { // dataref解析
            cout << "receive length: " << length << endl;
        }
        startReceive();
    } else if (error == Asio::error::operation_aborted)
        cerr << "Receive aborted" << endl;
}

/**
 * @brief 新增监听目标
 * @param dataRef dataref 名称
 * @param freq 频率, 0时停止
 * @param index 目标为数组时的索引
 */
void XPlaneUdp::addDataref (const string &dataRef, const int32_t freq, const int index) {
    string combineName{(index != -1) ? (dataRef + '[' + to_string(index) + ']') : dataRef};
    if (freq == 0) {
        dataref.right.erase(combineName);
        if (dataref.size() == 1) // 始终保留一个dataref维持udp通信
            return;
    } else
        dataref.insert({datarefIndex, combineName});
    array<char, 413> buffer{};
    pack(buffer, 0, datarefHead, freq, datarefIndex, combineName);
    datarefIndex++;
    sendUdpData(buffer);
}

/**
 * @brief 获取某个 dataref 最新值
 * @param dataRef dataref 名称
 * @param defaultValue 不存在时的默认值
 * @param index 目标为数组时的索引
 * @return 最新值
 */
float XPlaneUdp::getDataref (const std::string &dataRef, float defaultValue, int index) {
    int32_t datarefIndex = datarefName2Id(dataRef, index);
    if (datarefIndex == -1)
        return defaultValue;
    return getDataref(datarefIndex, defaultValue);
}

/**
 * @brief 获取某个 dataref 最新值
 * @param id dataref 索引
 * @param defaultValue 不存在时的默认值
 * @return 最新值
 */
float XPlaneUdp::getDataref (const int32_t id, const float defaultValue) {
    const auto it = latestDataref.find(id);
    if (it == latestDataref.end())
        return defaultValue;
    return it->second;
}

/**
 * @brief 通过 dataref 名称索引唯一 id
 * @param dataRef dataref 名称
 * @param index 目标为数组时的索引
 * @return 唯一 id, 未找到返回 -1
 */
int32_t XPlaneUdp::datarefName2Id (const std::string &dataRef, int index) {
    const string combineName{(index != -1) ? (dataRef + '[' + to_string(index) + ']') : dataRef};
    const auto it = dataref.right.find(combineName);
    if (it == dataref.right.end())
        return -1;
    return it->get_left();
}
