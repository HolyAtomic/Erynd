#----------------------------------------------------------------
# Generated CMake target import file for configuration "RelWithDebInfo".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "discordcpp::discordcpp" for configuration "RelWithDebInfo"
set_property(TARGET discordcpp::discordcpp APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)
set_target_properties(discordcpp::discordcpp PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELWITHDEBINFO "CXX"
  IMPORTED_LOCATION_RELWITHDEBINFO "${_IMPORT_PREFIX}/lib/discordcpp.lib"
  )

list(APPEND _cmake_import_check_targets discordcpp::discordcpp )
list(APPEND _cmake_import_check_files_for_discordcpp::discordcpp "${_IMPORT_PREFIX}/lib/discordcpp.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
