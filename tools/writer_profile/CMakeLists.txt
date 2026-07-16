cmake_minimum_required(VERSION 3.15)
project(fast_gdb_writer_profile LANGUAGES CXX)

find_package(fast_gdb 0.1 CONFIG REQUIRED)
find_package(GDAL REQUIRED)

add_executable(writer_profile_driver main.cpp)
target_compile_features(writer_profile_driver PRIVATE cxx_std_17)
target_link_libraries(writer_profile_driver PRIVATE fast_gdb::writer)
if(TARGET GDAL::GDAL)
    target_link_libraries(writer_profile_driver PRIVATE GDAL::GDAL)
else()
    target_include_directories(writer_profile_driver PRIVATE
        ${GDAL_INCLUDE_DIRS} ${GDAL_INCLUDE_DIR})
    target_link_libraries(writer_profile_driver PRIVATE
        ${GDAL_LIBRARIES} ${GDAL_LIBRARY})
endif()
