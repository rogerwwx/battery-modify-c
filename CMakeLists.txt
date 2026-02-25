cmake_minimum_required(VERSION 3.10)
project(BatteryCalibrate)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

# 优化与剔除未用代码
add_compile_options(-O2 -fdata-sections -ffunction-sections -fvisibility=hidden -fno-exceptions -fno-rtti)
add_link_options(-Wl,--gc-sections -Wl,-s)

# 可选：更激进的体积优化（可能影响兼容性）
add_compile_options(-Os -flto)
add_link_options(-flto)

add_executable(battery_calibrate main.cpp)