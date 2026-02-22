cmake_minimum_required(VERSION 3.10)
project(MessageBoard LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SQLite3 REQUIRED)

find_library(MHD_LIBRARY NAMES microhttpd libmicrohttpd REQUIRED)
find_path(MHD_INCLUDE_DIR NAMES microhttpd.h REQUIRED)

add_executable(
    message_board
    src/main.c
    src/http.c
    src/db.c
    src/db_tags.c
    src/render.c
    src/util.c
    src/logging.c
)

target_include_directories(message_board PRIVATE "${MHD_INCLUDE_DIR}")
target_link_libraries(message_board PRIVATE "${MHD_LIBRARY}" SQLite::SQLite3)

if(EXISTS "${CMAKE_SOURCE_DIR}/messages.db")
    configure_file("${CMAKE_SOURCE_DIR}/messages.db" "${CMAKE_BINARY_DIR}/messages.db" COPYONLY)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/assets")
    file(COPY "${CMAKE_SOURCE_DIR}/assets" DESTINATION "${CMAKE_BINARY_DIR}")
endif()
