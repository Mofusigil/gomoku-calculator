cmake_minimum_required(VERSION 3.20)

project(gomoku_analyzer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GOMOKU_BUILD_TESTS "Build the test suite" ON)

add_library(gomoku_engine
    src/engine/board.cpp
    src/engine/rules.cpp
    src/engine/evaluator.cpp
    src/engine/search.cpp
)
target_include_directories(gomoku_engine PUBLIC src)
target_compile_options(gomoku_engine PRIVATE -Wall -Wextra -Wpedantic)

add_library(gomoku_service
    src/service/analysis_jobs.cpp
)
target_include_directories(gomoku_service PUBLIC src)
target_compile_options(gomoku_service PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(gomoku_service PUBLIC gomoku_engine)

add_library(gomoku_server_lib
    src/server/json.cpp
    src/server/http_server.cpp
)
target_include_directories(gomoku_server_lib PUBLIC src)
target_compile_options(gomoku_server_lib PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(gomoku_server_lib PUBLIC pthread)

add_executable(gomoku_server src/main.cpp)
target_link_libraries(gomoku_server PRIVATE gomoku_service gomoku_server_lib)
target_compile_definitions(gomoku_server PRIVATE
    GOMOKU_DEFAULT_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/web"
)
target_compile_options(gomoku_server PRIVATE -Wall -Wextra -Wpedantic)

if(GOMOKU_BUILD_TESTS)
    enable_testing()
    add_executable(gomoku_tests
        tests/test_main.cpp
    )
    target_link_libraries(gomoku_tests PRIVATE gomoku_service gomoku_server_lib)
    target_compile_options(gomoku_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gomoku_tests COMMAND gomoku_tests)
endif()
