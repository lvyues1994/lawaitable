# QNX SDP 7.1 交叉编译公共部分（qcc / q++，GCC 8.3.0，默认标准库 LLVM libc++）。
# 由 qnx710-aarch64le.cmake / qnx710-x86_64.cmake 在设置 AW_QNX_CPU / AW_QNX_VARIANT 后 include。
# 使用前先在当前 shell 里 source <SDP>/qnxsdp-env.sh，它导出 QNX_HOST / QNX_TARGET。
# 仓库里不写任何机器上的绝对路径。
if(NOT DEFINED ENV{QNX_HOST} OR NOT DEFINED ENV{QNX_TARGET})
    message(FATAL_ERROR "QNX_HOST / QNX_TARGET 未设置：先 source <QNX SDP 7.1>/qnxsdp-env.sh")
endif()

# qcc 的许可证从 $QNX_CONFIGURATION/license 读取。qnxsdp-env.sh 把它设成 ~/.qnx；SDP 自带许可证
# 目录的（如 <SDP>/.qnx/license），需要在 source 之后把 QNX_CONFIGURATION 和
# QNX_CONFIGURATION_EXCLUSIVE 指过去（构建期间 ninja 调 qcc 时也要在环境里）。
if(NOT EXISTS "$ENV{QNX_CONFIGURATION}/license/licenses")
    message(WARNING "QNX_CONFIGURATION=$ENV{QNX_CONFIGURATION} 下没有 license/licenses，qcc 可能报 "
                    "\"license check failed\"。SDP 自带许可证时：export QNX_CONFIGURATION=<SDP>/.qnx "
                    "QNX_CONFIGURATION_EXCLUSIVE=<SDP>/.qnx")
endif()

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 7.1.0)
set(CMAKE_SYSTEM_PROCESSOR ${AW_QNX_CPU})

set(CMAKE_C_COMPILER "$ENV{QNX_HOST}/usr/bin/qcc")
set(CMAKE_C_COMPILER_TARGET "gcc_nto${AW_QNX_VARIANT}")
set(CMAKE_CXX_COMPILER "$ENV{QNX_HOST}/usr/bin/q++")
set(CMAKE_CXX_COMPILER_TARGET "gcc_nto${AW_QNX_VARIANT}")

# libc++ 的 <thread>/<mutex> 用到 PTHREAD_MUTEX_RECURSIVE 等 POSIX 扩展；严格的 -std=c++NN
# （CXX_EXTENSIONS OFF）下 QNX 头文件会把它们藏起来，需要 _QNX_SOURCE 打开。
set(CMAKE_CXX_FLAGS_INIT "-D_QNX_SOURCE")
set(CMAKE_C_FLAGS_INIT "-D_QNX_SOURCE")

set(CMAKE_FIND_ROOT_PATH "$ENV{QNX_TARGET}" "$ENV{QNX_TARGET}/${AW_QNX_VARIANT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
