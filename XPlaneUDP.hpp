#ifndef XPLANEUDP_HPP
#define XPLANEUDP_HPP

#include <iostream>
#include <iomanip>
#include <string>
#include <exception>
#include <boost/asio/steady_timer.hpp>
#include <boost/system.hpp>
#include <boost/asio.hpp>
#include <boost/bimap.hpp>
#include <map>
#include <thread>
#include <mutex>
#include <shared_mutex>


namespace sys = boost::system;
namespace asio = boost::asio;
namespace ip = asio::ip;

using UdpBuffer = std::array<char, 1472>;
using Strand = asio::strand<asio::io_context::executor_type>;

class XPlaneIpNotFound;
class XPlaneTimeout;
class XPlaneVersionNotSupported;
class XPlaneUdp;

template <typename T1, typename First, typename... Rests>
void unpack (const T1 &container, size_t offset, First &first, Rests &... rest);
template <typename T1, typename... Rests>
size_t pack (T1 &container, size_t offset, const std::string &first, const Rests &... rest);
template <typename T1, typename First, typename... Rests>
size_t pack (T1 &container, size_t offset, const First &first, const Rests &... rest);


struct PlaneInfo { // double8字节下无填充
    double lon, lat; // 经纬度
    double alt; // 高度
    float agl; // 离地高
    float pitch, track, roll; // 俯仰 真航向
    float vX, vY, vZ; // 三轴速度
    float rollRate, pitchRate, yawRate; // 横滚 俯仰 偏航
};

class XPlaneIpNotFound final : public std::exception {
    public:
        [[nodiscard]] const char* what () const noexcept override {
            return "Could not find any running XPlane instance in network.";
        }
};

class XPlaneTimeout final : public std::exception {
    public:
        [[nodiscard]] const char* what () const noexcept override {
            return "XPlane timeout.";
        }
};

class XPlaneVersionNotSupported final : public std::exception {
    public:
        [[nodiscard]] const char* what () const noexcept override {
            return "XPlane version not supported.";
        }
};

class XPlaneUdp {
    inline const static std::string MULTI_CAST_GROUP{"239.255.1.1"};
    static constexpr unsigned short MULTI_CAST_PORT{49707};
    public:
        // 默认
        XPlaneUdp ();
        ~XPlaneUdp ();
        // 状态
        void close ();
        bool getState ();
        // dataref
        void addDataref (const std::string &dataRef, int32_t freq = 1, int index = -1);
        std::optional<float> getDataref (const std::string &dataRef, int index = -1);
        std::optional<float> getDataref (int32_t id);
        void setDataref (const std::string &dataRef, float value, int index = -1);
        std::optional<int32_t> datarefName2Id (const std::string &dataRef, int index = -1);
        void addDatarefArray (const std::string &dataRef, int length, int32_t freq = 1);
        std::optional<std::vector<float>> getDatarefArray (const std::string &dataRef);
        std::optional<std::vector<float>> getDatarefArray (int32_t id);
        void setDatarefArray (const std::string &dataRef, const std::vector<float> &container);
        std::optional<int32_t> datarefArrayName2Id (const std::string &dataRef);
        // 基本信息
        void addBasicInfo (int32_t freq = 1);
        std::optional<PlaneInfo> getBasicInfo ();
    private:
        // dataref
        std::atomic<int32_t> datarefIndex{0}; // dataref 索引
        std::map<int, float> latestDataref; // 最新 dataref 数据
        boost::bimap<int32_t, std::string> dataref; // 双映射 dataref <索引,名称>
        std::unordered_map<std::string, int32_t> arrayLength; // 数组长度
        // 基本信息
        PlaneInfo latestBasicInfo{};
        std::atomic<bool> receivedInfo{false};
        // 网络
        asio::io_context io_context{}; // 上下文
        ip::udp::socket localSocket; // 绑定了本地地址的 socket
        ip::udp::endpoint remoteEndpoint; // xp 地址
        std::atomic<bool> timeout{false};
        // 多线程
        std::thread ioThread;
        std::atomic<bool> runThread{true}; // 线程终止循环
        Strand strand_; // udp协调
        std::mutex latestDatarefMutex; // 锁
        std::shared_mutex datarefMutex; // 读写锁
        std::mutex latestBasicInfoMutex; // 锁
        std::shared_mutex arrayLengthMutex; // 读写锁
        std::mutex datarefIndexMutex; // 锁

        // 网络
        void autoUdpFind ();
        void startReceive ();
        void handleReceive (std::vector<char> received);
        template <typename T>
        void sendUdpData (T buffer);
        template <typename T>
        size_t receiveUdpData (T &buffer, ip::udp::socket &socket, ip::udp::endpoint &sender, int timeout);
};
/**
 * @brief 通过 UDP 异步发送数据
 * @param buffer 缓冲区 array<char, N>
 */
template <typename T>
void XPlaneUdp::sendUdpData (T buffer) {
    asio::post(strand_, [this, copyBuffer=move(buffer)] () {
        localSocket.async_send_to(asio::buffer(copyBuffer), remoteEndpoint, asio::bind_executor(strand_,
                                      [](const sys::error_code &, std::size_t)-> void {}));
    });
    io_context.poll();
}
/**
 * @brief 定时阻塞接收 UDP 数据
 * @param buffer 接收缓冲区
 * @param socket 套接字
 * @param sender 发送者
 * @param timeout 超时时间(ms), 接收超时或无接收数据则抛出XPlaneTimeout异常
 * @return 接收到的字符数量
 */
template <typename T>
size_t XPlaneUdp::receiveUdpData (T &buffer, ip::udp::socket &socket, ip::udp::endpoint &sender, const int timeout) {
    // 初始化
    bool finished{false}, alreadyTimeout{false};
    size_t receivedBytes{0};
    asio::steady_timer timer(io_context);
    sys::error_code ec;
    timer.expires_after(std::chrono::milliseconds(timeout));
    // 接收
    timer.async_wait([&](const sys::error_code &error) {
        if (!error) { // 定时器触发
            alreadyTimeout = true;
            finished = true;
            socket.cancel();
        }
    });
    asio::post(strand_, [&] () {
        socket.async_receive_from(asio::buffer(buffer), sender, [&](const sys::error_code &error, const size_t bytes) {
            finished = true;
            receivedBytes = bytes;
            timer.cancel();
            if (error) // 报错啦 !
                ec = error;
        });
    });
    io_context.restart();
    while (!finished && !io_context.stopped())
        io_context.run_one();
    // 异常处理
    if (alreadyTimeout)
        throw XPlaneTimeout();
    return receivedBytes;
}

/**
 * @brief 解包一串字符数据
 * @tparam T1 支持.data()方法的char容器
 * @tparam First,Rests 基本数据类型
 * @param offset 偏移字节
 */
template <typename T1, typename First, typename... Rests>
void unpack (const T1 &container, size_t offset, First &first, Rests &... rest) {
    if (offset + sizeof(First) > container.size()) {
        throw std::out_of_range("Buffer overflow in unpack: offset " + std::to_string(offset) + 
                               " + size " + std::to_string(sizeof(First)) + 
                               " > container size " + std::to_string(container.size()));
    }
    memcpy(&first, container.data() + offset, sizeof(First));
    if constexpr (sizeof...(rest) > 0) // 编译期确定哪些函数特化, 不用写终止函数(空包)
        unpack(container, offset + sizeof(First), rest...);
}

/**
 * @brief 打包为字符数据
 * @tparam T1 支持.data()方法的char容器
 * @tparam First,Rests 基本数据类型,string
 * @return 打包数据量
 */
template <typename T1, typename First, typename... Rests>
size_t pack (T1 &container, size_t offset, const First &first, const Rests &... rest) {
    if (offset + sizeof(First) > container.size()) {
        throw std::out_of_range("Buffer overflow in pack: offset " + std::to_string(offset) + 
                               " + size " + std::to_string(sizeof(First)) + 
                               " > container size " + std::to_string(container.size()));
    }
    memcpy(container.data() + offset, &first, sizeof(First));
    if constexpr (sizeof...(rest) > 0)
        return pack(container, offset + sizeof(First), rest...);
    else
        return offset + sizeof(First);
}
/**
 * @brief 打包为字符数据
 * @tparam T1 支持.data()方法的char容器
 * @tparam Rests 基本数据类型,string
 * @return 打包数据量
 */
template <typename T1, typename... Rests>
size_t pack (T1 &container, size_t offset, const std::string &first, const Rests &... rest) {
    if (offset + first.size() > container.size()) {
        throw std::out_of_range("Buffer overflow in pack (string): offset " + std::to_string(offset) + 
                               " + string size " + std::to_string(first.size()) + 
                               " > container size " + std::to_string(container.size()));
    }
    memcpy(container.data() + offset, first.data(), first.size());
    if constexpr (sizeof...(rest) > 0)
        return pack(container, offset + first.size(), rest...);
    else
        return offset + first.size();
}

#endif //XPLANEUDP_HPP
