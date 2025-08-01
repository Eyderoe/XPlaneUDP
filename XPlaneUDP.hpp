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

class XPlaneIpNotFound;
class XPlaneTimeout;
class XPlaneVersionNotSupported;
class XPlaneUdp;

template <typename T>
size_t receiveData (Ip::udp::socket &socket, T &buffer, Ip::udp::endpoint &endpoint, int timeout);
template size_t receiveData<udpBuffer_t> (Ip::udp::socket &, udpBuffer_t &, Ip::udp::endpoint &, int);
template <typename T1, typename First, typename... Rests>
void unpack (const T1 &container, size_t offset, First &first, Rests &... rest);
template <typename T1, typename... Rests>
void pack (T1 &container, size_t offset, const std::string &first, const Rests &... rest);
template <typename T1, typename First, typename... Rests>
void pack (T1 &container, size_t offset, const First &first, const Rests &... rest);


class XPlaneIpNotFound : public std::exception {
    public:
        [[nodiscard]] const char* what () const noexcept override {
            return "Could not find any running XPlane instance in network.";
        }
};

class XPlaneTimeout : public std::exception {
    public:
        [[nodiscard]] const char* what () const noexcept override {
            return "XPlane timeout.";
        }
};

class XPlaneVersionNotSupported : public std::exception {
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
        float getDataref (const std::string &dataRef, float defaultValue = 0, int index = -1);
        float getDataref (int32_t id, float defaultValue = 0);
        int32_t datarefName2Id (const std::string &dataRef, int index = -1);
        // 控制
        void close ();
    private:
        // dataref
        int32_t datarefIndex{0}; // dataref 索引
        std::map<int, float> latestDataref; // 最新 dataref 数据
        boost::bimap<int32_t, std::string> dataref; // 双映射 dataref <索引,名称>
        // 网络
        Asio::io_context io_context{};
        Ip::udp::socket localSocket; // 绑定了本地地址的 socket
        Ip::udp::endpoint remoteEndpoint; // xp 地址
        // 多线程
        std::atomic<bool> runThread{true};
        std::thread ioThread; // 接收线程
        Asio::strand<Asio::io_context::executor_type> strand_; // udp协调
        std::mutex latestDatarefMutex; // 锁
        std::shared_mutex datarefMutex; // 读写锁

        // 网络
        void autoUdpFind ();
        template <typename T>
        void sendUdpData (T buffer);
        void startReceive ();
        void handleReceive (std::vector<char> received);
};
/**
 * @brief 通过 UDP 异步发送数据
 * @param buffer 缓冲区 array<char, N>
 */
template <typename T>
void XPlaneUdp::sendUdpData (T buffer) {
    Asio::post(strand_, [this, copyBuffer=std::move(buffer)] () {
        localSocket.async_send_to(Asio::buffer(copyBuffer), this->remoteEndpoint,
                                  Asio::bind_executor(strand_, [](const Sys::error_code &, std::size_t)-> void {}));
    });
}

/**
 * @brief 定时阻塞接收 UDP 数据
 * @param socket 套接字
 * @param buffer 接收缓冲区
 * @param endpoint 发送段网络端子
 * @param timeout 超时时间(ms), 接收超时或无接收数据则抛出XPlaneTimeout异常
 * @return 接收到的字符数量
 */
template <typename T>
size_t receiveData (Ip::udp::socket &socket, T &buffer, Ip::udp::endpoint &endpoint, const int timeout) {
    size_t bytesReceived = 0;
    Sys::error_code ec;
    bool finished = false;
    auto executor = socket.get_executor();
    Asio::io_context &io_context = static_cast<Asio::io_context&>(executor.context());
    // 定时器
    Asio::steady_timer timer(io_context);
    timer.expires_from_now(std::chrono::milliseconds(timeout));
    timer.async_wait([&](const Sys::error_code &timer_ec) {
        if (!timer_ec) {
            socket.cancel();
        }
    });
    // 异步接收数据
    socket.async_receive_from(Asio::buffer(buffer), endpoint, [&](const Sys::error_code &error, size_t bytes) {
        timer.cancel();
        ec = error;
        bytesReceived = bytes;
        finished = true;
    });
    io_context.restart();
    while (!finished && !io_context.stopped()) {
        io_context.run_one();
    }
    // 错误处理

    return bytesReceived;
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
