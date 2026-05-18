#ifndef XPLANEUDP_HPP
#define XPLANEUDP_HPP

#include <boost/system.hpp>
#include <boost/asio.hpp>
#include <boost/dynamic_bitset.hpp>
#include <format>
#include <iostream>
#include <ranges>
#include <memory>
#include <array>
#include <shared_mutex>
#include <boost/pool/pool_alloc.hpp>


template <typename T>
concept Container = std::ranges::random_access_range<T> &&
        std::ranges::sized_range<T> &&
        std::is_assignable_v<std::ranges::range_reference_t<T>, float> && requires(T contain) {
            contain.size();
        } ;
template <typename T>
concept CharArray = requires(T contain) {
    requires std::is_same_v<typename T::value_type, char> ||
    std::is_same_v<typename T::value_type, std::byte> ||
    std::is_same_v<typename T::value_type, uint8_t>;
};

constexpr int HEADER_LENGTH{5}; // 指令头部长度 4字母+1空
static const std::string DATAREF_GET_HEAD{'R', 'R', 'E', 'F', '\x00'};
static const std::string DATAREF_SET_HEAD{'D', 'R', 'E', 'F', '\x00'};
static const std::string BASIC_INFO_HEAD{'R', 'P', 'O', 'S', '\x00'};
static const std::string BECON_HEAD{'B', 'E', 'C', 'N', '\x00'};

namespace sys = boost::system;
namespace asio = boost::asio;
namespace ip = asio::ip;

class XPlaneUdp;
class BufferPool;

template <typename T, typename... Rests>
    requires (std::same_as<std::string, T> || std::is_fundamental_v<T>)
size_t packSize (size_t offset, const T &first, const Rests &... rest);
template <CharArray T1, typename T2, typename... Rests>
    requires (std::same_as<std::string, T2> || std::is_fundamental_v<T2>)
size_t pack (T1 &container, size_t offset, const T2 &first, const Rests &... rest);
template <CharArray CharList, typename First, typename... Rests>
void unpack (const CharList &container, size_t offset, First &first, Rests &... rest);


class BufferPool {
    struct BufferPro {
        std::array<char, 1472> data{};
        size_t length;
        BufferPro () : length(0) { std::memset(data.data(), 0x00, data.size()); }
    };
    public:
        static std::shared_ptr<std::array<char, 1472>> getBuffer (size_t length);
    private:
        static void recycleBuffer (BufferPro *buffer);
};

class XPlaneUdp {
    public:
        struct DatarefIndex {
            DatarefIndex () : idx(0) {}
            explicit DatarefIndex (const size_t index) : idx(index) {}
            DatarefIndex (const DatarefIndex &) = default;
            DatarefIndex (DatarefIndex &&) = default;
            DatarefIndex& operator= (const DatarefIndex &) = default;
            DatarefIndex& operator= (DatarefIndex &&) = default;
            [[nodiscard]] size_t getIdx () const { return idx; }
            private:
                size_t idx;
        };
        struct PlaneInfo {
            double lon, lat, alt; // 经纬度 高度
            float agl, pitch, track, roll; // 离地高 / 俯仰 真航向 滚转
            float vX, vY, vZ, rollRate, pitchRate, yawRate; // 三轴速度 / 横滚 俯仰 偏航
        };

        explicit XPlaneUdp (bool autoReConnect = true);
        ~XPlaneUdp ();
        XPlaneUdp (const XPlaneUdp &) = delete;
        XPlaneUdp& operator= (const XPlaneUdp &) = delete;
        XPlaneUdp (XPlaneUdp &&) = delete;
        XPlaneUdp& operator= (XPlaneUdp &&) = delete;

        void setCallback (const std::function<void  (bool)> &callbackFunc);
        void reconnect (bool del = false);
        void stop ();
        void close ();

        DatarefIndex addDataref (const std::string &dataref, int32_t freq = 1, int index = -1);
        DatarefIndex addDatarefArray (const std::string &dataref, int length, int32_t freq = 1);
        bool getDataref (const DatarefIndex &dataref, float &value, float defaultValue = 0) const;
        template <Container T>
        bool getDataref (const DatarefIndex &dataref, T &container, float defaultValue = 0);
        void changeDatarefFreq (const DatarefIndex &dataref, int32_t freq);
        void setDataref (const std::string &dataref, float value, int index = -1);
        template <Container T>
        void setDataref (const std::string &dataref, const T &value);

        void addPlaneInfo (int freq = 1);
        void getPlaneInfo (PlaneInfo &infoDst) const;
    private:
        struct DatarefInfo {
            std::string name; // dataref 长度
            int start, end; // values中索引,[start,end]
            int32_t freq; // 频率
            bool available; // 是否可用
            bool isArray; // 是否是数组
        };

        bool closed{false};
        // 数据
        std::vector<DatarefInfo> dataRefs;
        std::vector<float> values;
        boost::dynamic_bitset<> space;
        std::unordered_map<std::string, size_t> exist;
        PlaneInfo info{.track = -999};
        mutable std::shared_mutex dataMutex;
        // 网络
        bool autoReconnect; // 自动重连
        asio::io_context io_context{}; // 上下文
        asio::executor_work_guard<asio::io_context::executor_type> workGuard;
        ip::udp::socket multicastSocket{io_context}; // 监听多播
        ip::udp::socket xpSocket{io_context}; // xp通信
        ip::udp::endpoint xpEndpoint; // xp端口
        std::thread worker; // io_content驱动
        int planeInfoFreq{}; // 基本信息频率
        // 回调
        bool state{false}; // xp状态
        std::function<void  (bool)> callback{nullptr}; // 回调

        void setState (bool newState);
        size_t findSpace (size_t length);
        void detectBeacon ();
        asio::awaitable<void> detect ();
        void sendData (const std::shared_ptr<std::array<char, 1472>> &data, size_t size);
        asio::awaitable<void> send (std::shared_ptr<std::array<char, 1472>> data, size_t size);
        void receiveData ();
        asio::awaitable<void> receive ();
        void receiveDataProcess (const std::shared_ptr<std::array<char, 1472>> &data, size_t size,
                                 const ip::udp::endpoint &sender);
};

/**
 * @brief 解包一串字符数据
 * @param container 容器
 * @param offset 偏移字节
 */
template <CharArray CharList, typename First, typename... Rests>
void unpack (const CharList &container, size_t offset, First &first, Rests &... rest) {
    assert(container.size() >= offset + sizeof(First) && "not enough to unpack !");
    memcpy(&first, container.data() + offset, sizeof(First));
    if constexpr (sizeof...(rest) > 0)
        unpack(container, offset + sizeof(First), rest...);
}


/**
 * @brief 打包字符数量
 * @param offset 偏移量
 * @param first string,基本类型
 * @return 打包数据量
 */
template <typename T, typename... Rests>
    requires (std::same_as<std::string, T> || std::is_fundamental_v<T>)
size_t packSize (const size_t offset, const T &first, const Rests &... rest) {
    if constexpr (std::same_as<std::string, T>) { // string
        if constexpr (sizeof...(rest) > 0)
            return packSize(offset + first.size(), rest...);
        else
            return offset + first.size();
    } else { // 基本类型
        if constexpr (sizeof...(rest) > 0)
            return packSize(offset + sizeof(T), rest...);
        else
            return offset + sizeof(T);
    }
}

/**
 * @brief 打包为字符数组
 * @param container 容器
 * @param offset 偏移量
 * @param first string,基本类型
 * @return 打包数据量
 */
template <CharArray T1, typename T2, typename... Rests>
    requires (std::same_as<std::string, T2> || std::is_fundamental_v<T2>)
size_t pack (T1 &container, const size_t offset, const T2 &first, const Rests &... rest) {
    if constexpr (std::same_as<std::string, T2>) { // string
        assert(container.size() >= offset + first.size() && "not enough to pack !");
        memcpy(container.data() + offset, first.data(), first.size());
        if constexpr (sizeof...(rest) > 0)
            return pack(container, offset + first.size(), rest...);
        else
            return offset + first.size();
    } else { // 基本类型
        assert(container.size() >= offset + sizeof(T2) && "not enough to pack !");
        memcpy(container.data() + offset, &first, sizeof(T2));
        if constexpr (sizeof...(rest) > 0)
            return pack(container, offset + sizeof(T2), rest...);
        else
            return offset + sizeof(T2);
    }
}

/**
 * @brief 获取 dataref 最新值
 * @param dataref 标识
 * @param container 容器
 * @param defaultValue 默认值
 * @return 值可用
 */
template <Container T>
bool XPlaneUdp::getDataref (const DatarefIndex &dataref, T &container, float defaultValue) {
    std::shared_lock lock(dataMutex);
    const auto &ref = dataRefs.at(dataref.getIdx());
    const size_t size = ref.end - ref.start + 1;
    if (!ref.available) {
        std::ranges::fill(container | std::views::take(size), defaultValue);
        return false;
    }
    size_t containerCapacity; // 容器能塞多少元素
    if constexpr (requires { container.capacity(); }) { // vector等
        containerCapacity = container.capacity();
    } else if constexpr (requires { container.size(); }) { // array等
        containerCapacity = container.size();
    }
    auto source = values | std::views::drop(ref.start) | std::views::take(std::min(size, containerCapacity));
    std::ranges::copy(source, container.begin());
    return true;
}

/**
 * @brief 设置某组 dataref 值
 * @param dataref dataref 名称
 * @param value 容器
 */
template <Container T>
void XPlaneUdp::setDataref (const std::string &dataref, const T &value) {
    for (int i = 0; i < value.size(); ++i) {
        const size_t bufferSize = packSize(0, DATAREF_SET_HEAD, value.at(i), std::format("{}[{}]", dataref, i), '\x00');
        const auto buffer = BufferPool::getBuffer(bufferSize);
        pack(*buffer, 0, DATAREF_SET_HEAD, value.at(i), std::format("{}[{}]", dataref, i), '\x00');
        sendData(buffer, 509);
    }
}

#endif
