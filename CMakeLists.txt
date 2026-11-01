cmake_minimum_required(VERSION 3.25)
project(tide VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(lexy CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Frontend + interpreter + Solana primitives. The test-validator backend is
# linked separately so the core never depends on RPC or process management.
add_library(tide_core STATIC
  src/parser.cpp
  src/sema.cpp
  src/interp.cpp
  src/solana.cpp
)
target_include_directories(tide_core PUBLIC src)
target_link_libraries(tide_core
  PUBLIC  foonathan::lexy unofficial-sodium::sodium OpenSSL::Crypto nlohmann_json::nlohmann_json)
target_compile_options(tide_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
# lexy's DSL relies on `a >> b + c` precedence by design.
set_source_files_properties(src/parser.cpp PROPERTIES COMPILE_OPTIONS -Wno-parentheses)

add_library(tide_solana_backend STATIC src/solana_backend.cpp)
target_link_libraries(tide_solana_backend PUBLIC tide_core cpr::cpr)
target_compile_options(tide_solana_backend PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(tide src/main.cpp)
target_link_libraries(tide PRIVATE tide_core tide_solana_backend)
target_compile_options(tide PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

option(TIDE_BUILD_TESTS "Build the Tide test suite" ON)
if(TIDE_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)
  enable_testing()
  file(GLOB TIDE_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(tide_tests ${TIDE_TEST_SOURCES})
  target_link_libraries(tide_tests PRIVATE tide_core tide_solana_backend Catch2::Catch2WithMain)
  target_compile_definitions(tide_tests PRIVATE
    TIDE_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/examples")
  include(Catch)
  catch_discover_tests(tide_tests)
endif()
