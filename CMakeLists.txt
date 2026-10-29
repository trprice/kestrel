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

# Sanitizers: -DKESTREL_SANITIZE=address | undefined | address,undefined |
# thread. Applied to everything built here, dependencies fetched with
# FetchContent included. Use a separate build directory for each setting;
# AddressSanitizer and ThreadSanitizer cannot be combined.
set(KESTREL_SANITIZE "" CACHE STRING
  "Sanitizers to build with: address, undefined, address,undefined or thread")
if(KESTREL_SANITIZE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "KESTREL_SANITIZE needs GCC or Clang.")
  endif()
  string(REPLACE ";" "," _kestrel_sanitize "${KESTREL_SANITIZE}")
  if(_kestrel_sanitize MATCHES "thread" AND _kestrel_sanitize MATCHES "address")
    message(FATAL_ERROR "thread cannot be combined with address.")
  endif()
  if(NOT _kestrel_sanitize MATCHES "^(address|undefined|address,undefined|undefined,address|thread)$")
    message(FATAL_ERROR
      "KESTREL_SANITIZE='${KESTREL_SANITIZE}': use address, undefined, address,undefined or thread.")
  endif()
  message(STATUS "Sanitizers: ${_kestrel_sanitize}")
  add_compile_options(-fsanitize=${_kestrel_sanitize} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${_kestrel_sanitize})
  if(_kestrel_sanitize MATCHES "undefined")
    # Report a finding as a failure instead of printing and carrying on.
    add_compile_options(-fno-sanitize-recover=undefined)
  endif()
endif()

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
  src/net/timed_http_client.cpp
  src/data/symbols.cpp
  src/data/kraken_data_source.cpp
  src/data/kraken_quote_stream.cpp
  src/data/schwab_data_source.cpp
  src/data/binanceus_data_source.cpp
  src/data/binanceus_errors.cpp
  src/data/binanceus_quote_stream.cpp
  src/data/binanceus_bar_stream.cpp
  src/db/database.cpp
  src/db/migrations.cpp
  src/db/bar_store.cpp
  src/db/quote_store.cpp
  src/db/account_fill_store.cpp
  src/db/trade_store.cpp
  src/trading/order_rules.cpp
  src/trading/pnl_tracker.cpp
  src/trading/risk_gate.cpp
  src/trading/paper_trading_venue.cpp
  src/trading/kraken_trading_venue.cpp
  src/trading/schwab_trading_venue.cpp
  src/trading/binanceus_api.cpp
  src/trading/binanceus_trading_venue.cpp
  src/backtest/replay_data_source.cpp
  src/backtest/backtest.cpp
  src/strategy/runner.cpp
  src/strategy/run_recorder.cpp
  src/strategy/sma_crossover.cpp
  src/strategy/market_maker.cpp
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
  OpenSSL::SSL
  PostgreSQL::PostgreSQL)
target_compile_options(kestrel PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>)

if(KESTREL_BUILD_APP)
  add_executable(kestrel_app
    app/main.cpp
    app/common.cpp
    app/commands/init_db.cpp
    app/commands/account.cpp
    app/commands/bars.cpp
    app/commands/order.cpp
    app/commands/stream_quotes.cpp
    app/commands/stream_bars.cpp
    app/commands/run.cpp
    app/commands/backtest.cpp
  )
  target_include_directories(kestrel_app PRIVATE app)
  target_link_libraries(kestrel_app PRIVATE kestrel::kestrel CLI11::CLI11)

  # How the command line refuses bad input: the exit code and the message.
  # 105 (bad value) and 106 (missing argument) are CLI11's own usage-error
  # codes; 2 (bad range) and 1 (failure) are ours.
  if(KESTREL_BUILD_TESTS)
    enable_testing()
    set(_good ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cli_backtest.toml)
    set(_live ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/cli_live_no_risk.toml)
    # name | config | exit code | message | arguments (comma-separated)
    set(_cli_cases
      "no_subcommand|${_good}|106|A subcommand is required|"
      "unknown_timeframe|${_good}|105|unknown timeframe|bars,7x,3"
      "calendar_timeframe|${_good}|105|has no fixed length|bars,1M,3"
      "zero_count|${_good}|105|not in range|bars,1h,0"
      "account_zero_days|${_good}|105|not in range|account,--days,0"
      "account_store_paper|${_good}|2|a paper venue has no history|account,--store"
      "order_needs_test_or_confirm|${_good}|2|exactly one of --test|order,buy,ZEC/USDT,0.01"
      "order_both_flags|${_good}|2|exactly one of --test|order,buy,ZEC/USDT,0.01,--test,--confirm"
      "order_paper|${_good}|2|needs mode|order,buy,ZEC/USDT,0.01,--test"
      "order_bad_side|${_good}|105|not in|order,hold,ZEC/USDT,0.01,--test"
      "cancel_needs_confirm|${_good}|2|add --confirm|cancel,123"
      "cancel_id_or_all|${_good}|2|an order ID or --all|cancel,--confirm"
      "negative_seconds|${_good}|105|not in range|stream,-1"
      "backtest_needs_range|${_good}|106|--from is required|backtest,sma_crossover"
      "backtest_bad_date|${_good}|105|invalid ISO-8601 timestamp|backtest,sma_crossover,--from,soon,--to,2026-01-01"
      "backtest_backwards_range|${_good}|2|must be after its start|backtest,sma_crossover,--from,2026-02-01,--to,2026-01-01"
      "backtest_unknown_strategy|${_good}|1|unknown strategy|backtest,nope,--from,2026-01-01,--to,2026-02-01"
      "run_live_without_risk|${_live}|1|live mode needs a [risk]|run,sma_crossover")
    foreach(_case IN LISTS _cli_cases)
      string(REPLACE "|" ";" _fields "${_case}")
      list(GET _fields 0 _name)
      list(GET _fields 1 _config)
      list(GET _fields 2 _rc)
      list(GET _fields 3 _msg)
      list(GET _fields 4 _args)
      string(REPLACE "," ";" _args "${_args}")
      add_test(NAME cli_${_name}
        COMMAND ${CMAKE_COMMAND}
          -DAPP=$<TARGET_FILE:kestrel_app>
          "-DARGS=--config;${_config};${_args}"
          -DEXPECT_RC=${_rc}
          "-DEXPECT_MSG=${_msg}"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_check.cmake)
    endforeach()
  endif()
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
    tests/binanceus_bar_stream_test.cpp
    tests/bar_store_test.cpp
    tests/quote_store_test.cpp
    tests/account_fill_store_test.cpp
    tests/trade_store_test.cpp
    tests/fixture_data_test.cpp
    tests/paper_trading_venue_test.cpp
    tests/pnl_tracker_test.cpp
    tests/timed_http_client_test.cpp
    tests/binanceus_api_test.cpp
    tests/binanceus_trading_venue_test.cpp
    tests/risk_gate_test.cpp
    tests/runner_test.cpp
    tests/run_recorder_test.cpp
    tests/backtest_test.cpp
    tests/backtest_db_test.cpp
    tests/sma_crossover_test.cpp
    tests/market_maker_test.cpp
    tests/config_test.cpp
  )
  target_link_libraries(kestrel_tests PRIVATE kestrel::kestrel GTest::gtest_main)
  target_include_directories(kestrel_tests PRIVATE tests)
  target_compile_definitions(kestrel_tests PRIVATE
    KESTREL_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  include(GoogleTest)
  gtest_discover_tests(kestrel_tests DISCOVERY_MODE PRE_TEST)
endif()
