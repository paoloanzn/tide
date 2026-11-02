cmake_minimum_required(VERSION 3.25)
project(tide VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings for Tide's own code. Missing field initializers stay allowed: C++20
# designated initializers deliberately omit members that have defaults.
set(TIDE_WARNINGS
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-qual
  -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2
  -Wimplicit-fallthrough -Wextra-semi -Wredundant-decls -Wmissing-declarations
  -Wzero-as-null-pointer-constant -Wno-missing-field-initializers
  "$<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond;-Wduplicated-branches;-Wlogical-op;-Wuseless-cast>")
option(TIDE_WERROR "Treat compiler warnings as errors" OFF)
if(TIDE_WERROR)
  list(APPEND TIDE_WARNINGS -Werror)
endif()

# Runs clang-tidy (.clang-tidy) on every Tide source as it compiles.
option(TIDE_CLANG_TIDY "Lint with clang-tidy during the build" OFF)
if(TIDE_CLANG_TIDY)
  find_program(TIDE_CLANG_TIDY_EXE NAMES clang-tidy-18 clang-tidy REQUIRED)
  # clang-tidy parses GCC's command line, which carries GCC-only warnings.
  set(CMAKE_CXX_CLANG_TIDY ${TIDE_CLANG_TIDY_EXE} --extra-arg=-Wno-unknown-warning-option)
  if(TIDE_WERROR)
    list(APPEND CMAKE_CXX_CLANG_TIDY --warnings-as-errors=*)
  endif()
endif()

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
target_compile_options(tide_core PRIVATE ${TIDE_WARNINGS})
# lexy's DSL relies on `a >> b + c` precedence by design.
set_source_files_properties(src/parser.cpp PROPERTIES COMPILE_OPTIONS -Wno-parentheses)

add_library(tide_solana_backend STATIC src/solana_backend.cpp)
target_link_libraries(tide_solana_backend PUBLIC tide_core cpr::cpr)
target_compile_options(tide_solana_backend PRIVATE ${TIDE_WARNINGS})

add_executable(tide src/main.cpp)
target_link_libraries(tide PRIVATE tide_core tide_solana_backend)
# project(VERSION) above is the single source of the version; the release
# workflow refuses to publish a tag that does not match it.
target_compile_definitions(tide PRIVATE TIDE_VERSION="${PROJECT_VERSION}")
target_compile_options(tide PRIVATE ${TIDE_WARNINGS})

option(TIDE_BUILD_TESTS "Build the Tide test suite" ON)
if(TIDE_BUILD_TESTS)
  find_package(Catch2 3 CONFIG REQUIRED)
  enable_testing()
  file(GLOB TIDE_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(tide_tests ${TIDE_TEST_SOURCES})
  target_link_libraries(tide_tests PRIVATE tide_core tide_solana_backend Catch2::Catch2WithMain)
  target_compile_options(tide_tests PRIVATE ${TIDE_WARNINGS})
  target_compile_definitions(tide_tests PRIVATE
    TIDE_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/examples")
  include(Catch)
  catch_discover_tests(tide_tests)
endif()
