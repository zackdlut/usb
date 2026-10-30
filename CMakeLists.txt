cmake_minimum_required(VERSION 3.16)
project(usb_tutorial CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

# miniusb 只有头文件
add_library(miniusb INTERFACE)
target_include_directories(miniusb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 每个例子一个可执行文件，名字就是目录名，例如 build/examples/02-signal
function(add_example dir)
  file(GLOB srcs ${CMAKE_CURRENT_SOURCE_DIR}/examples/${dir}/*.cpp)
  add_executable(${dir} ${srcs})
  target_link_libraries(${dir} PRIVATE miniusb)
  set_target_properties(${dir} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)
endfunction()

add_example(02-signal)
add_example(03-packet)
add_example(04-transaction)
add_example(05-transfer-types)
add_example(06-descriptors)
add_example(07-enumeration)
add_example(08-hid-keyboard)
add_example(09-cdc-serial)
add_example(12-troubleshoot)

# 第 10 章：真实的 USB 主机程序，需要 libusb（sudo apt install libusb-1.0-0-dev）
find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0)
find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0)
if(LIBUSB_INCLUDE_DIR AND LIBUSB_LIBRARY)
  message(STATUS "libusb: ${LIBUSB_LIBRARY}")
  foreach(prog lsusb_lite vendor_client)
    add_executable(${prog} examples/10-libusb/${prog}.cpp)
    target_link_libraries(${prog} PRIVATE miniusb ${LIBUSB_LIBRARY})
    target_include_directories(${prog} PRIVATE ${LIBUSB_INCLUDE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/examples/11-stm32-vendor)
    set_target_properties(${prog} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)
  endforeach()
else()
  message(STATUS "没找到 libusb-1.0，跳过第 10 章的例子。安装：sudo apt install libusb-1.0-0-dev")
endif()

add_executable(test_miniusb tests/test_miniusb.cpp)
target_link_libraries(test_miniusb PRIVATE miniusb)
set_target_properties(test_miniusb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
enable_testing()
add_test(NAME miniusb COMMAND test_miniusb)
