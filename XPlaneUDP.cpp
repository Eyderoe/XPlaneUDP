#include "XPlaneUDP.hpp"

using namespace std;

#ifdef _WIN32
constexpr bool isWindows = true;
#else
constexpr bool isWindows = false;
#endif

XPlaneUdp::XPlaneUdp (Asio::io_context &io_context): io_context(io_context), socket(io_context), listenEndpoint() {
    autoUdpFind();
    socket.open(listenEndpoint.protocol());
    socket.bind(listenEndpoint);
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
    Ip::udp::socket socket(ioContext);
    socket.open(Ip::udp::v4());
    Asio::socket_base::reuse_address option(true);
    socket.set_option(option);
    // 绑定至多播
    Ip::udp::endpoint listenEndpoint;
    if (isWindows)
        listenEndpoint = Ip::udp::endpoint(Ip::udp::v4(), multiCastPort);
    else
        listenEndpoint = Ip::udp::endpoint(Ip::make_address(multiCastGroup), multiCastPort);
    socket.bind(listenEndpoint);
    Ip::address_v4 multicast_address = Ip::make_address_v4(multiCastGroup);
    socket.set_option(Asio::ip::multicast::join_group(multicast_address));
    // 接收数据
    array<char, 1472> buffer{};
    Ip::udp::endpoint senderEndpoint;
    size_t bytesReceived;
    try {
        bytesReceived = receiveData(socket, buffer, senderEndpoint, 3000);
    } catch (const XPlaneTimeout &e) {
        socket.close();
        throw e;
    }
    socket.close();
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
    if ((bytesReceived < 5 + 16) || (string_view{buffer.data(), 5} != string{'B', 'E', 'C', 'N', '\x00'})) { // 非xp数据
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
        port=49001;
        if ((mainVer == 1) && (minorVer <= 2) && (software == 1)) {
            cout << "XPlane Beacon Version: " << static_cast<int>(mainVer) << '.' << static_cast<int>(minorVer) << '.'
                    << software << endl;
            cout << dec << "IP: " << senderEndpoint.address() << ", Port: " << port << ", hostname: " << string_view{
                buffer.data() + 21, bytesReceived - 24
            } << ", XPlaneVersion: " << xpVer << ", role: " << role << endl;
            this->listenEndpoint = Ip::udp::endpoint(Ip::make_address(senderEndpoint.address().to_string()), port);
        } else {
            cerr << "XPlane Beacon Version not supported: " << mainVer << '.' << minorVer << '.' << software << endl;
            throw XPlaneVersionNotSupported();
        }
    }
}


void XPlaneUdp::receiveUdpData () {
    array<char, 1472> recvBuffer{};
    while (running.load()) {
        if (dataref.empty())
            continue;
        Sys::error_code ec;
        size_t bytesReceived = 0;
        cout<<listenEndpoint.port()<<endl;
        bytesReceived = socket.receive_from(Asio::buffer(recvBuffer),listenEndpoint , 0, ec);
        if (bytesReceived > 0) {
            cout<<bytesReceived<<endl;
        }
    }
}

/**
 * @brief 新增监听目标
 * @param dataRef dataref 名称
 * @param freq 频率, 0时停止
 * @param index 目标为数组时的索引
 */
void XPlaneUdp::addDataRef (const string &dataRef, const int32_t freq, const int index) {
    const static string cmd{'R', 'R', 'E', 'F', '\x00'};
    if (freq == 0)
        dataref.right.erase(dataRef);
    string strIndex{};
    if (index != -1) // 如果对应dataref是数组, 则为索引
        strIndex = '[' + to_string(index) + ']';
    dataref.insert({datarefIndex, dataRef + strIndex});
    array<char, 413> buffer{};
    pack(buffer, 0, cmd, freq, datarefIndex, dataRef + strIndex);
    datarefIndex++;
    sendUdpData(buffer);
}


/**
 * @brief 获取某个 dataref 最新值
 * @param dataRef dataref 名称
 * @param index 目标为数组时的索引
 * @return 最新值
 */
float XPlaneUdp::getDataRef (const std::string &dataRef, const int index) {}
