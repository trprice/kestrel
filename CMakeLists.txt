cmake_minimum_required(VERSION 3.25)
project(Kestrel VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(KESTREL_BUILD_APP "Build the kestrel_app executable" ON)
option(KESTREL_BUILD_TESTS "Build the kestrel_tests target" ON)

# std::expected is mandatory (GCC 12+, Clang 16+, MSVC 17.3+).
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
  #include <expected>
  int main() { std::expected<int, int> e = 1; return *e - 1; }
" KESTREL_HAS_STD_EXPECTED)
if(NOT KESTREL_HAS_STD_EXPECTED)
  message(FATAL_ERROR
    "Kestrel needs a compiler with std::expected (GCC 12+, Clang 16+, MSVC 17.3+).")
endif()

include(cmake/KestrelDependencies.cmake)

add_library(kestrel
  src/types/decimal.cpp
  src/types/error.cpp
  src/types/time.cpp
  src/types/symbol.cpp
  src/types/timeframe.cpp
  src/types/market.cpp
  src/util/hmac.cpp
  src/util/json.cpp
  src/util/rate_limiter.cpp
  src/util/retry.cpp
  src/net/cpr_http_client.cpp
  src/data/symbols.cpp
  src/data/kraken_data_source.cpp
  src/data/kraken_quote_stream.cpp
  src/data/schwab_data_source.cpp
  src/data/binanceus_data_source.cpp
  src/data/binanceus_quote_stream.cpp
  src/trading/order_rules.cpp
  src/trading/paper_trading_venue.cpp
  src/trading/kraken_trading_venue.cpp
  src/trading/schwab_trading_venue.cpp
  src/trading/binanceus_trading_venue.cpp
  src/config/configuration.cpp
  src/config/schwab_token_store.cpp
  src/factory.cpp
)
add_library(kestrel::kestrel ALIAS kestrel)
target_include_directories(kestrel PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(kestrel PUBLIC
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  tomlplusplus::tomlplusplus
  cpr::cpr
  ixwebsocket::ixwebsocket
  OpenSSL::Crypto
  OpenSSL::SSL)
target_compile_options(kestrel PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>)

if(KESTREL_BUILD_APP)
  add_executable(kestrel_app app/main.cpp)
  target_link_libraries(kestrel_app PRIVATE kestrel::kestrel)
endif()

if(KESTREL_BUILD_TESTS)
  enable_testing()
  add_executable(kestrel_tests
    tests/decimal_test.cpp
    tests/symbol_test.cpp
    tests/timeframe_test.cpp
    tests/util_test.cpp
    tests/kraken_data_source_test.cpp
    tests/binanceus_data_source_test.cpp
    tests/paper_trading_venue_test.cpp
    tests/config_test.cpp
  )
  target_link_libraries(kestrel_tests PRIVATE kestrel::kestrel GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(kestrel_tests DISCOVERY_MODE PRE_TEST)
endif()
