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
    running.store(true);
    thread([this] () { receiveUdpData(); }).detach();
}

XPlaneUdp::~XPlaneUdp () {
    running.store(false);
}

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
 * @brief 接收 UDP 数据
 */
void XPlaneUdp::receiveUdpData () {
    udpBuffer_t recvBuffer{};
    while (running.load()) {
        {
            if (shared_lock<shared_mutex> lock(datarefMapMutex); dataref.empty()) {
                this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }
        Ip::udp::endpoint sender{};
        size_t bytesReceived{};
        try {
            bytesReceived = receiveData(localSocket, recvBuffer, sender, 2000);
        } catch (const XPlaneTimeout &e) {
            if (shared_lock<shared_mutex> lock(datarefMapMutex); dataref.empty())
                continue;
            throw e;
        }
        if (bytesReceived > headerLength) {
            if (equal(datarefHead.begin(), datarefHead.begin() + 4, recvBuffer.begin())) // 文档有误 xp12实际返回 RREF,{...},...
                receiveDataref(recvBuffer, bytesReceived);
        }
    }
}

/**
 * 处理接收到的 dataref 数据
 * @param receiveBuffer UDP 接收缓冲区
 * @param length 接收到的数据长度
 */
void XPlaneUdp::receiveDataref (const udpBuffer_t &receiveBuffer, const size_t length) {
    lock_guard<mutex> guard(latestMutex);
    for (int i = headerLength; i < length; i += 8) {
        int index;
        float value;
        unpack(receiveBuffer, i, index, value);
        latestDataref[index] = value;
    }
}

/**
 * @brief 新增监听目标
 * @param dataRef dataref 名称
 * @param freq 频率, 0时停止
 * @param index 目标为数组时的索引
 */
void XPlaneUdp::addDataref (const string &dataRef, const int32_t freq, const int index) {
    if (unique_lock<shared_mutex> lock(datarefMapMutex); freq == 0)
        dataref.right.erase(dataRef);
    string strIndex{};
    if (index != -1) // 如果对应dataref是数组, 则为索引
        strIndex = '[' + to_string(index) + ']';
    {
        unique_lock<shared_mutex> lock(datarefMapMutex);
        dataref.insert({datarefIndex, dataRef + strIndex});
    }
    array<char, 413> buffer{};
    strIndex = dataRef + strIndex;
    pack(buffer, 0, datarefHead, freq, datarefIndex, strIndex);
    datarefIndex++;
    sendUdpData(move(buffer));
}

/**
 * @brief 获取某个 dataref 最新值
 * @param dataRef dataref 名称
 * @param defaultValue 不存在时的默认值
 * @param index 目标为数组时的索引
 * @return 最新值
 */
float XPlaneUdp::getDataref (const std::string &dataRef, float defaultValue, int index) {
    string combine{};
    if (index != -1)
        combine = dataRef + '[' + to_string(index) + ']';
    else
        combine = dataRef;
    int datarefIndex{};
    {
        shared_lock<shared_mutex> lock(datarefMapMutex);
        const auto it = dataref.right.find(combine);
        if (it == dataref.right.end())
            return defaultValue;
        datarefIndex = it->get_left();
    }
    {
        lock_guard<mutex> guard(latestMutex);
        const auto it = latestDataref.find(datarefIndex);
        if (it == latestDataref.end())
            return defaultValue;
        return it->second;
    }
}
float XPlaneUdp::getDataref (int32_t id, float defaultValue) {}
float XPlaneUdp::datarefName2Id (const std::string &dataRef, int index) {}
