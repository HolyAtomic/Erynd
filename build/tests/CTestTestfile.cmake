# CMake generated Testfile for 
# Source directory: C:/Users/dd/OneDrive/Desktop/discordcpp/tests
# Build directory: C:/Users/dd/OneDrive/Desktop/discordcpp/build/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test("discordcpp.unit" "C:/Users/dd/OneDrive/Desktop/discordcpp/build/tests/Debug/discordcpp_tests.exe")
  set_tests_properties("discordcpp.unit" PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/dd/OneDrive/Desktop/discordcpp/tests/CMakeLists.txt;6;add_test;C:/Users/dd/OneDrive/Desktop/discordcpp/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test("discordcpp.unit" "C:/Users/dd/OneDrive/Desktop/discordcpp/build/tests/Release/discordcpp_tests.exe")
  set_tests_properties("discordcpp.unit" PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/dd/OneDrive/Desktop/discordcpp/tests/CMakeLists.txt;6;add_test;C:/Users/dd/OneDrive/Desktop/discordcpp/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test("discordcpp.unit" "C:/Users/dd/OneDrive/Desktop/discordcpp/build/tests/MinSizeRel/discordcpp_tests.exe")
  set_tests_properties("discordcpp.unit" PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/dd/OneDrive/Desktop/discordcpp/tests/CMakeLists.txt;6;add_test;C:/Users/dd/OneDrive/Desktop/discordcpp/tests/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test("discordcpp.unit" "C:/Users/dd/OneDrive/Desktop/discordcpp/build/tests/RelWithDebInfo/discordcpp_tests.exe")
  set_tests_properties("discordcpp.unit" PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/dd/OneDrive/Desktop/discordcpp/tests/CMakeLists.txt;6;add_test;C:/Users/dd/OneDrive/Desktop/discordcpp/tests/CMakeLists.txt;0;")
else()
  add_test("discordcpp.unit" NOT_AVAILABLE)
endif()
