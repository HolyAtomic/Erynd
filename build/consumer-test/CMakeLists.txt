cmake_minimum_required(VERSION 3.24)
project(DiscordCppPackageConsumer LANGUAGES CXX)

find_package(DiscordCpp 0.1 CONFIG REQUIRED)
if(NOT TARGET discordcpp::discordcpp)
    message(FATAL_ERROR "Installed package did not define discordcpp::discordcpp")
endif()

get_target_property(_target_type discordcpp::discordcpp TYPE)
if(NOT _target_type STREQUAL "STATIC_LIBRARY")
    message(FATAL_ERROR "Expected STATIC_LIBRARY, got: ${_target_type}")
endif()
get_target_property(_debug_library discordcpp::discordcpp IMPORTED_LOCATION_DEBUG)
if(NOT _debug_library OR NOT EXISTS "${_debug_library}")
    message(FATAL_ERROR "Debug library location is missing or does not exist: ${_debug_library}")
endif()

add_executable(package_consumer main.cpp)
target_link_libraries(package_consumer PRIVATE discordcpp::discordcpp)
