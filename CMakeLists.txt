cmake_minimum_required(VERSION 3.24)

project(DiscordCpp
    VERSION 0.1.0
    DESCRIPTION "Independent C++ Discord library foundation"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)

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

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

set(discordcpp_cmake_install_dir "${CMAKE_INSTALL_LIBDIR}/cmake/discordcpp")
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

install(TARGETS discordcpp
    EXPORT DiscordCppTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
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


