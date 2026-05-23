cmake_minimum_required(VERSION 3.16)
project(LowLatencyMatchingEngine VERSION 1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

set(SOURCES
    src/OrderBook.cpp
    src/MatchingEngine.cpp
    src/SlabAllocator.cpp
    src/UDPServer.cpp
)

set(HEADERS
    include/Order.h
    include/OrderBook.h
    include/MatchingEngine.h
    include/LockFreeQueue.h
    include/SlabAllocator.h
    include/UDPServer.h
)

add_library(matching_engine_lib STATIC ${SOURCES} ${HEADERS})
target_include_directories(matching_engine_lib PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(matching_engine_lib Threads::Threads)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(matching_engine_lib PRIVATE -O3 -march=native -mtune=native -funroll-loops -ftree-vectorize)
endif()

add_executable(matching_engine main.cpp)
target_link_libraries(matching_engine matching_engine_lib Threads::Threads)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(matching_engine PRIVATE -O3 -march=native -mtune=native -funroll-loops -ftree-vectorize)
endif()

add_executable(benchmark benchmark/Benchmark.cpp)
target_link_libraries(benchmark matching_engine_lib Threads::Threads)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(benchmark PRIVATE -O3 -march=native -mtune=native -funroll-loops -ftree-vectorize)
endif()

add_executable(UDPClient tools/UDPClient.cpp)
target_link_libraries(UDPClient matching_engine_lib Threads::Threads)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(UDPClient PRIVATE -O3 -march=native -mtune=native -funroll-loops -ftree-vectorize)
endif()

install(TARGETS matching_engine benchmark UDPClient matching_engine_lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES ${HEADERS} DESTINATION include)
