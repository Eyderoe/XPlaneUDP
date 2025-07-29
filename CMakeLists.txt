cmake_minimum_required(VERSION 3.29)
project(XPlaneUDP)

set(CMAKE_CXX_STANDARD 17)
include_directories(D:\\boost_1_78_0)

find_package(Boost)
add_executable(XPlaneUDP main.cpp
        XPlaneUDP.cpp
        XPlaneUDP.hpp
        temp.cpp)

target_link_libraries(XPlaneUDP ${Boost_LIBRARIES} ws2_32)