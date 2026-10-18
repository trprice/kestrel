# Dependency resolution: find_package first, FetchContent as the fallback.
include(FetchContent)

# Several upstream projects still declare ancient cmake_minimum_required values
# that CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)   # needed by IXWebSocket

# --- nlohmann/json ----------------------------------------------------------
find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  message(STATUS "nlohmann_json not found; fetching")
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- spdlog -----------------------------------------------------------------
find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
  message(STATUS "spdlog not found; fetching")
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3 GIT_SHALLOW ON)
  FetchContent_MakeAvailable(spdlog)
endif()

# --- toml++ -----------------------------------------------------------------
find_package(tomlplusplus CONFIG QUIET)
if(NOT tomlplusplus_FOUND)
  message(STATUS "tomlplusplus not found; fetching")
  FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0 GIT_SHALLOW ON)
  FetchContent_MakeAvailable(tomlplusplus)
endif()

# --- cpr (HTTP) -------------------------------------------------------------
find_package(cpr CONFIG QUIET)
if(NOT cpr_FOUND)
  message(STATUS "cpr not found; fetching")
  # Prefer the system libcurl; if there is none, cpr builds curl itself.
  find_package(CURL QUIET)
  if(CURL_FOUND)
    set(CPR_USE_SYSTEM_CURL ON CACHE BOOL "" FORCE)
  else()
    set(CPR_USE_SYSTEM_CURL OFF CACHE BOOL "" FORCE)
  endif()
  set(CPR_ENABLE_SSL ON CACHE BOOL "" FORCE)
  set(CPR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.11.2 GIT_SHALLOW ON)
  FetchContent_MakeAvailable(cpr)
endif()

# --- IXWebSocket (TLS via OpenSSL) -----------------------------------------
find_package(ixwebsocket CONFIG QUIET)
if(NOT ixwebsocket_FOUND)
  message(STATUS "ixwebsocket not found; fetching")
  set(USE_TLS ON CACHE BOOL "" FORCE)
  set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
  set(USE_ZLIB ON CACHE BOOL "" FORCE)
  set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v11.4.5 GIT_SHALLOW ON)
  FetchContent_MakeAvailable(ixwebsocket)
  if(NOT TARGET ixwebsocket::ixwebsocket)
    add_library(ixwebsocket::ixwebsocket ALIAS ixwebsocket)
  endif()
endif()

# --- CLI11 (command line parsing, used by kestrel_app) ----------------------
find_package(CLI11 2.4 CONFIG QUIET)
if(NOT CLI11_FOUND)
  message(STATUS "CLI11 not found; fetching")
  FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.2 GIT_SHALLOW ON)
  FetchContent_MakeAvailable(CLI11)
endif()

# --- GoogleTest -------------------------------------------------------------
if(KESTREL_BUILD_TESTS)
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found; fetching")
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2 GIT_SHALLOW ON)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()
