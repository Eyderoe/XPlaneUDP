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


namespace Sys = boost::system;
namespace Asio = boost::asio;
namespace Ip = Asio::ip;

using udpBuffer_t = std::array<char, 1472>;
using strand_t = Asio::strand<Asio::io_context::executor_type>;

class XPlaneIpNotFound;
class XPlaneTimeout;
class XPlaneVersionNotSupported;
class XPlaneUdp;

template <typename T1, typename First, typename... Rests>
void unpack (const T1 &container, size_t offset, First &first, Rests &... rest);
template <typename T1, typename... Rests>
void pack (T1 &container, size_t offset, const std::string &first, const Rests &... rest);
template <typename T1, typename First, typename... Rests>
void pack (T1 &container, size_t offset, const First &first, const Rests &... rest);


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
    inline const static std::string multiCastGroup{"239.255.1.1"};
    static constexpr unsigned short multiCastPort{49707};
    public:
        // 默认
        XPlaneUdp ();
        ~XPlaneUdp ();
        // dataref
        void addDataref (const std::string &dataRef, int32_t freq = 1, int index = -1);
        int32_t datarefName2Id (const std::string &dataRef, int index = -1);
        float getDataref (const std::string &dataRef, float defaultValue = 0, int index = -1);
        float getDataref (int32_t id, float defaultValue = 0);
        void setDataref (const std::string &dataRef, float value, int index = -1);
        // 基本信息
        void needBasicInfo (int32_t freq = 1);
        PlaneInfo getBasicInfo ();
        // 控制
        void close ();
    private:
        // dataref
        int32_t datarefIndex{0}; // dataref 索引
        std::map<int, float> latestDataref; // 最新 dataref 数据
        boost::bimap<int32_t, std::string> dataref; // 双映射 dataref <索引,名称>
        // 基本信息
        PlaneInfo latestBasicInfo{};
        // 网络
        Asio::io_context io_context{}; // 上下文
        Ip::udp::socket localSocket; // 绑定了本地地址的 socket
        Ip::udp::endpoint remoteEndpoint; // xp 地址
        // 多线程
        std::atomic<bool> runThread{true}; // 线程终止循环
        std::atomic<bool> alreadyClose{false}; // udp关闭标志
        strand_t strand_; // udp协调
        std::mutex latestDatarefMutex; // 锁
        std::shared_mutex datarefMutex; // 读写锁
        std::mutex latestBasicInfoMutex; // 锁

        // 网络
        void autoUdpFind ();
        void startReceive ();
        void handleReceive (std::vector<char> received);
        template <typename T>
        void sendUdpData (T buffer);
        template <typename T>
        size_t receiveUdpData (T &buffer, Ip::udp::socket &socket, Ip::udp::endpoint &sender, int timeout);
};
/**
 * @brief 通过 UDP 异步发送数据
 * @param buffer 缓冲区 array<char, N>
 */
template <typename T>
void XPlaneUdp::sendUdpData (T buffer) {
    Asio::post(strand_, [this, copyBuffer=std::move(buffer)] () {
        localSocket.async_send_to(Asio::buffer(copyBuffer), remoteEndpoint, Asio::bind_executor(strand_,
                                      [](const Sys::error_code &, std::size_t)-> void {}));
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
size_t XPlaneUdp::receiveUdpData (T &buffer, Ip::udp::socket &socket, Ip::udp::endpoint &sender, const int timeout) {
    // 初始化
    bool finished{false}, alreadyTimeout{false};
    size_t receivedBytes{0};
    Asio::steady_timer timer(io_context);
    Sys::error_code ec;
    timer.expires_after(std::chrono::milliseconds(timeout));
    // 接收
    timer.async_wait([&](const Sys::error_code &error) {
        if (!error) { // 定时器触发
            alreadyTimeout = true;
            finished = true;
            socket.cancel();
        }
    });
    Asio::post(strand_, [&] () {
        socket.async_receive_from(Asio::buffer(buffer), sender, [&](const Sys::error_code &error, const size_t bytes) {
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
    memcpy(&first, container.data() + offset, sizeof(First));
    if constexpr (sizeof...(rest) > 0) // 编译期确定哪些函数特化, 不用写终止函数(空包)
        unpack(container, offset + sizeof(First), rest...);
}

/**
 * @brief 打包为字符数据
 * @tparam T1 支持.data()方法的char容器
 * @tparam First,Rests 基本数据类型,string
 *
 */
template <typename T1, typename First, typename... Rests>
void pack (T1 &container, size_t offset, const First &first, const Rests &... rest) {
    memcpy(container.data() + offset, &first, sizeof(First));
    if constexpr (sizeof...(rest) > 0)
        pack(container, offset + sizeof(First), rest...);
}
/**
 * @brief 打包为字符数据
 * @tparam T1 支持.data()方法的char容器
 * @tparam Rests 基本数据类型,string
 */
template <typename T1, typename... Rests>
void pack (T1 &container, size_t offset, const std::string &first, const Rests &... rest) {
    memcpy(container.data() + offset, first.data(), first.size());
    if constexpr (sizeof...(rest) > 0)
        pack(container, offset + first.size(), rest...);
}

#endif //XPLANEUDP_HPP
