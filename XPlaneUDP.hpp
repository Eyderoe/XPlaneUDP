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
    using UdpReceiveCallback = std::function<void  (const udpBuffer_t &data, const Ip::udp::endpoint &sender_endpoint)>;
    public:
        // 默认方法
        explicit XPlaneUdp (Asio::io_context &io_context);
        ~XPlaneUdp ();
        // dataref
        void addDataref (const std::string &dataRef, int32_t freq = 1, int index = -1);
        float getDataref (const std::string &dataRef, float defaultValue = 0, int index = -1);
        float getDataref (int32_t id, float defaultValue = 0);
        int32_t datarefName2Id (const std::string &dataRef, int index = -1);
    private:
        // dataref
        int32_t datarefIndex{0}; // dataref 索引
        std::map<int, float> latestDataref; // 最新 dataref 数据
        boost::bimap<int32_t, std::string> dataref; // 双映射 dataref <索引,名称>
        // socket
        Ip::udp::socket localSocket; // 绑定了本地地址的 socket
        Ip::udp::endpoint remoteEndpoint; // xp 地址
        Asio::io_context &io_context; // 上下文

        // 网络通信
        void autoUdpFind ();
        void receiveDataref (const udpBuffer_t &receiveBuffer, size_t length);
        // 模板
        template <typename T>
        void sendUdpData (T buffer);
};
/**
 * @brief 通过 UDP 异步发送数据
 * @param buffer 缓冲区 array<char, N>
 */
template <typename T>
void XPlaneUdp::sendUdpData (T buffer) {
    auto shared = std::make_shared<T>(std::move(buffer)); // 注意发送缓冲区生命周期 !
    localSocket.async_send_to(Asio::buffer(*shared), remoteEndpoint,
                              [shared](Sys::error_code ec, std::size_t bytesSent) {});
    io_context.run();
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
