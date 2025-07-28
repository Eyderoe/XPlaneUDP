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
        explicit XPlaneUdp (Asio::io_context &io_context);
        ~XPlaneUdp ();
        void addDataRef (const std::string &dataRef, int32_t freq = 1, int index = -1);
        float getDataRef (const std::string &dataRef, int index = -1);
    private:
        int32_t datarefIndex{0}; // dataref 索引
        std::map<int, float> latestDataref; // 最新 dataref 数据
        boost::bimap<int32_t, std::string> dataref; // 双映射 dataref <索引,名称>
        Asio::io_context &io_context;
        Ip::udp::socket socket;
        Ip::udp::endpoint listenEndpoint;
        std::atomic<bool> running{false}; // 线程运行状态

        void autoUdpFind ();
        void receiveUdpData ();
        template <typename T>
        void sendUdpData (T &buffer);
};
/**
 * @brief 通过 UDP 发送数据
 * @param buffer 缓冲区 array<char, N>
 */
template <typename T>
void XPlaneUdp::sendUdpData (T &buffer) {
    socket.send_to(Asio::buffer(buffer), listenEndpoint); // 线程安全 不加锁
}

/**
 * @brief 定时阻塞接收 UDP 数据
 * @param socket 套接字
 * @param buffer 接收缓冲区
 * @param endpoint 网络端子
 * @param timeout 超时时间(ms), 接收超时或无接收数据则抛出异常
 * @return 接收到的字符数量
 */
template <typename T>
size_t receiveData (Ip::udp::socket &socket, T &buffer, Ip::udp::endpoint &endpoint, int timeout) {
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
        timer.cancel(); // 接收到数据，取消定时器
        ec = error;
        bytesReceived = bytes;
        finished = true;
    });
    // 运行 io_context，直到一个操作完成
    io_context.restart();
    // 循环执行，直到有操作完成
    while (!finished && !io_context.stopped()) {
        io_context.run_one();
    }
    // 错误处理
    if (!finished) { // 如果 finished 为 false，说明 io_context 停止了但接收操作的回调没有被触发
        throw XPlaneTimeout();
    }
    if (ec) {
        if (ec == Asio::error::operation_aborted) {
            throw XPlaneTimeout();
        }
        throw Sys::system_error(ec);
    }
    return bytesReceived;
}
template size_t receiveData<std::array<char, 1472>> (Ip::udp::socket &,
                                                     std::array<char, 1472> &, Ip::udp::endpoint &, int);

/**
 * @brief 解包一串字符数据
 * @tparam T1 支持.data()方法的容器
 * @tparam First,Rests 基本数据类型
 */
template <typename T1, typename First, typename... Rests>
void unpack (const T1 &container, size_t offset, First &first, Rests &... rest) {
    memcpy(&first, container.data() + offset, sizeof(First));
    if constexpr (sizeof...(rest) > 0) // 编译期确定哪些函数特化, 不用写终止函数
        unpack(container, offset + sizeof(First), rest...);
}

/**
 * @brief 打包为字符数据
 * @tparam T1 支持.data()方法的容器
 * @tparam First,Rests 基本数据类型, array<char, 413>
 */
template <typename T1, typename First, typename... Rests>
void pack (T1 &container, size_t offset, const First &first, const Rests &... rest) {
    memcpy(container.data() + offset, &first, sizeof(First));
    if constexpr (sizeof...(rest) > 0)
        pack(container, offset + sizeof(First), rest...);
}
template <typename T1, typename... Rests>
void pack (T1 &container, size_t offset, const std::array<char, 413> &first, const Rests &... rest) {
    memcpy(container.data() + offset, first.data(), first.size());
    if constexpr (sizeof...(rest) > 0)
        pack(container, offset + first.size(), rest...);
}


#endif //XPLANEUDP_HPP
