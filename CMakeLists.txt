cmake_minimum_required(VERSION 3.24)

project(DiscordCpp
    VERSION 0.1.0
    DESCRIPTION "Independent C++ Discord library foundation"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)

option(DISCORDCPP_WITH_CURL "Build the optional libcurl HTTP transport" OFF)

add_library(discordcpp STATIC
    src/discordcpp.cpp
    src/rate_limit.cpp
    src/rest_client.cpp
)
add_library(discordcpp::discordcpp ALIAS discordcpp)

set_target_properties(discordcpp PROPERTIES
    CXX_EXTENSIONS OFF
    EXPORT_NAME discordcpp
)
target_compile_features(discordcpp PUBLIC cxx_std_20)
target_include_directories(discordcpp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

function(discordcpp_enable_private_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
discordcpp_enable_private_warnings(discordcpp)

if(DISCORDCPP_WITH_CURL)
    find_package(CURL 7.84.0 REQUIRED)

    add_library(discordcpp_curl_transport STATIC src/curl_transport.cpp)
    add_library(discordcpp::curl_transport ALIAS discordcpp_curl_transport)
    set_target_properties(discordcpp_curl_transport PROPERTIES
        CXX_EXTENSIONS OFF
        EXPORT_NAME curl_transport
    )
    target_compile_features(discordcpp_curl_transport PUBLIC cxx_std_20)
    target_include_directories(discordcpp_curl_transport PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_link_libraries(discordcpp_curl_transport PUBLIC discordcpp::discordcpp PRIVATE CURL::libcurl)
    discordcpp_enable_private_warnings(discordcpp_curl_transport)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

set(discordcpp_cmake_install_dir "${CMAKE_INSTALL_LIBDIR}/cmake/discordcpp")
set(DISCORDCPP_WITH_CURL_PACKAGE ${DISCORDCPP_WITH_CURL})
configure_package_config_file(
    cmake/DiscordCppConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/DiscordCppConfig.cmake"
    INSTALL_DESTINATION "${discordcpp_cmake_install_dir}"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/DiscordCppConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

set(discordcpp_install_targets discordcpp)
if(DISCORDCPP_WITH_CURL)
    list(APPEND discordcpp_install_targets discordcpp_curl_transport)
endif()
install(TARGETS ${discordcpp_install_targets}
    EXPORT DiscordCppTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
if(DISCORDCPP_WITH_CURL)
    install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
else()
    install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" PATTERN "curl_transport.hpp" EXCLUDE)
endif()
install(EXPORT DiscordCppTargets
    FILE DiscordCppTargets.cmake
    NAMESPACE discordcpp::
    DESTINATION "${discordcpp_cmake_install_dir}"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/DiscordCppConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/DiscordCppConfigVersion.cmake"
    DESTINATION "${discordcpp_cmake_install_dir}"
)


