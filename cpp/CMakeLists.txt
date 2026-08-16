# dsh-desk — DeepSeek Harness 桌面壳（C++/Qt）
#
# 工具链（本机实测，见 notes/02 D8）：
#   CMake 3.30 + Ninja 1.12 + MSVC 2022 (14.44) + Qt 6.11.1 msvc2022_64
# 构建：
#   cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
#   cmake --build build
cmake_minimum_required(VERSION 3.21)

project(dsh_desk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.11 REQUIRED COMPONENTS Widgets WebEngineWidgets Network)

qt_add_executable(dsh-desk WIN32
    src/main.cpp
    src/shellwindow.h
    src/shellwindow.cpp
    src/dshprocess.h
    src/dshprocess.cpp
    src/selftest.h
    src/selftest.cpp
    src/app.rc
)

# 窗口图标资源。显式用 qt_add_resources：VS 生成器下 AUTORCC 对 .qrc
# 未生效（真机诊断：icons.qrc 被当作 <None> 未编译，资源 file_exists=0）。
# 资源名：/dsh.png（窗口图标，Qt 内置格式）、/dsh.ico（备用）。
qt_add_resources(dsh-desk "icons"
    PREFIX "/"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
    FILES assets/dsh.png assets/dsh.ico
)

target_link_libraries(dsh-desk PRIVATE
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::Network
)

# MSVC 警告基线（与 Qt 代码共存时的合理级别）
if(MSVC)
    target_compile_options(dsh-desk PRIVATE /W4 /permissive- /utf-8)
endif()
