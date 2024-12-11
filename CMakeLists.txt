# Required for Apple Silicon support.
cmake_minimum_required(VERSION 3.19)

project(
        LaunchDarklyCPPClientPersistent
        VERSION 0.1
        DESCRIPTION "LaunchDarkly Hello CPP Client-side SDK with persistent caching"
        LANGUAGES CXX
)

# set these env variables if using macos/homebrew. replace with the appropriate version
#set(BOOST_ROOT "/opt/homebrew/opt/boost@1.85")
#set(BOOST_LIBRARYDIR "/opt/homebrew/opt/boost@1.85/lib")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
add_subdirectory(vendor/launchdarkly)

# add sqlite
find_package(SQLite3 REQUIRED)

# support for clang/macos which doesn't support asan
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-fsanitize=address" COMPILER_SUPPORTS_ASAN)
if(NOT COMPILER_SUPPORTS_ASAN)
    add_link_options(-fsanitize=address)
endif()

add_executable(hello-cpp-client-persistent main.cpp)
target_link_libraries(hello-cpp-client-persistent PRIVATE launchdarkly::client Threads::Threads sqlite3)
