cmake_minimum_required(VERSION 3.10)
project(BatteryCalibrate)

# 使用 C++11 (或 C++14/17)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译生成可执行文件 (注意：在 Android 环境中执行无需后缀)
add_executable(battery_calibrate main.cpp)

# 如果你的安卓环境对动态链接的 libc++ 支持不好，可以开启静态编译
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -static-libstdc++ -static")