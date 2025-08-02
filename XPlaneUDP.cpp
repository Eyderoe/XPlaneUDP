#include "XPlaneUDP.hpp"

#ifdef _WIN32
constexpr bool isWindows = true;
#else
constexpr bool isWindows = false;
#endif

using namespace std;

constexpr int headerLength{5}; // 指令头部长度 4字母+1空
const static string datarefGetHead{'R', 'R', 'E', 'F', '\x00'};
const static string datarefSetHead{'D', 'R', 'E', 'F', '\x00'};
const static string basicInfoHead{'R', 'P', 'O', 'S', '\x00'};
const static string beconHead{'B', 'E', 'C', 'N', '\x00'};


XPlaneUdp::XPlaneUdp (): localSocket(io_context), strand_(io_context.get_executor()) {
    // 绑定xplane
    autoUdpFind();
    Ip::udp::endpoint local(Ip::udp::v4(), 0);
    localSocket.open(local.protocol());
    localSocket.bind(local);
    // 保持udp连接
    addDataref("sim/network/misc/network_time_sec");
    io_context.reset();
    io_context.run();
    // 启动接收
    thread ioThread = thread([this] () { this->startReceive(); });
    ioThread.detach();
}

XPlaneUdp::~XPlaneUdp () {
    close();
}

/**
 * @brief 关闭udp连接
 */
void XPlaneUdp::close () {
    // 关闭线程
    runThread.store(false);
    localSocket.cancel();
    while (!alreadyClose)
        this_thread::sleep_for(std::chrono::milliseconds(10));
    // 停止udp接收
    shared_lock<shared_mutex> lock{datarefMutex};
    vector<string> allDatarefs;
    for (auto &item : dataref.right)
        allDatarefs.emplace_back(item.first);
    lock.unlock();
    needBasicInfo(0);
    addDataref("inop");
    for (auto &item : allDatarefs)
        addDataref(item, 0);
    io_context.poll();
}

/**
 * @brief 开始接收异步接收
 */
void XPlaneUdp::startReceive () {
    Ip::udp::endpoint temp{};
    udpBuffer_t udpReceive{};
    bool closed{false};
    size_t length;
    while (runThread) {
        try {
            length = receiveUdpData(udpReceive, localSocket, temp, 3000);
        } catch (const XPlaneTimeout &e) {
            closed = true;
            break;
        }
        if (length < 5)
            continue;
        handleReceive({udpReceive.begin(), udpReceive.begin() + length});
    }
    alreadyClose.store(true);
    if (closed) {
        close();
        throw XPlaneTimeout();
    }
}

/**
 * @brief 寻找电脑上运行的 XPlane 实例
 */
void XPlaneUdp::autoUdpFind () {
    // 创建 UDP 并允许端口复用
    Ip::udp::socket multicastSocket(io_context);
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
        bytesReceived = receiveUdpData(buffer, multicastSocket, senderEndpoint, 3000);
    } catch (const XPlaneTimeout &e) {
        throw XPlaneIpNotFound();
    }
    cout << "XPlane Beacon: ";
    for (size_t i = 0; i < bytesReceived; i++)
        cout << hex << setw(2) << setfill('0') << (static_cast<unsigned int>(buffer[i]) & 0xff);
    cout << endl;
    // 解析数据
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
 * @brief 处理接收到的udp数据
 * @param received 接收数据
 */
void XPlaneUdp::handleReceive (vector<char> received) {
    if (equal(datarefGetHead.begin(), datarefGetHead.begin() + 4, received.begin())) {
        // dataref,文档有误实际返回 RREF,
        unique_lock<mutex> lock{latestDatarefMutex};
        for (int i = headerLength; i < received.size(); i += 8) {
            int index;
            float value;
            unpack(received, i, index, value);
            latestDataref[index] = value;
        }
    } else if (equal(basicInfoHead.begin(), basicInfoHead.begin() + 4, received.begin())) {
        unique_lock<mutex> lock{latestBasicInfoMutex};
        unpack(received, headerLength, latestBasicInfo);
    }
}

/**
 * @brief 新增监听目标
 * @param dataRef dataref 名称
 * @param freq 频率, 0时停止
 * @param index 目标为数组时的索引
 */
void XPlaneUdp::addDataref (const string &dataRef, const int32_t freq, const int index) {
    string combineName{(index != -1) ? (dataRef + '[' + to_string(index) + ']') : dataRef};
    if (unique_lock<shared_mutex> lock{datarefMutex}; freq == 0) {
        dataref.right.erase(combineName);
        if (dataref.size() == 1) // 始终保留一个dataref维持udp通信
            return;
    } else
        dataref.insert({datarefIndex, combineName});
    array<char, 413> buffer{};
    pack(buffer, 0, datarefGetHead, freq, datarefIndex, combineName);
    datarefIndex++;
    sendUdpData(buffer);
}

/**
 * @brief 设置
 * @param dataRef dataref 名称
 * @param value 值
 * @param index 目标为数组时的索引
 */
void XPlaneUdp::setDataref (const std::string &dataRef, const float value, const int index) {
    array<char, 509> buffer{};
    const string combineName{(index != -1) ? (dataRef + '[' + to_string(index) + ']') : dataRef};
    pack(buffer, 0, datarefSetHead, value, combineName, '\x00');
    sendUdpData(buffer);
}

/**
 * @brief 开始接收基本信息
 * @param freq 接收频率
 */
void XPlaneUdp::needBasicInfo (int32_t freq) {
    const string sentence = basicInfoHead + to_string(freq) + '\x00';
    vector<char> buffer(sentence.size());
    pack(buffer, 0, sentence);
    sendUdpData(move(buffer));
}

/**
 * @brief 获取基本信息最新值
 * @return 基本信息
 */
PlaneInfo XPlaneUdp::getBasicInfo () {
    unique_lock<mutex> lock(latestBasicInfoMutex);
    return latestBasicInfo;
}

/**
 * @brief 获取某个 dataref 最新值
 * @param dataRef dataref 名称
 * @param defaultValue 不存在时的默认值
 * @param index 目标为数组时的索引
 * @return 最新值
 */
float XPlaneUdp::getDataref (const std::string &dataRef, const float defaultValue, int index) {
    const int32_t datarefIndex = datarefName2Id(dataRef, index);
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
    unique_lock<mutex> lock{latestDatarefMutex};
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
    shared_lock<shared_mutex> lock{datarefMutex};
    const auto it = dataref.right.find(combineName);
    if (it == dataref.right.end())
        return -1;
    return it->get_left();
}
