cmake_minimum_required(VERSION 3.16)
project(gdal_GeoPBF CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(GDAL REQUIRED)

add_library(gdal_GeoPBF MODULE ogrgeopbf.cpp)
target_include_directories(gdal_GeoPBF PRIVATE ${GDAL_INCLUDE_DIRS})
target_link_libraries(gdal_GeoPBF PRIVATE GDAL::GDAL)

# strip lib prefix so GDAL finds it as "gdal_GeoPBF.so"
set_target_properties(gdal_GeoPBF PROPERTIES PREFIX "")

# install target: gdal-config --plugindir
execute_process(COMMAND gdal-config --plugindir
    OUTPUT_VARIABLE GDAL_PLUGIN_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT GDAL_PLUGIN_DIR)
    set(GDAL_PLUGIN_DIR "/usr/local/lib/gdalplugins")
endif()

install(TARGETS gdal_GeoPBF DESTINATION ${GDAL_PLUGIN_DIR})
message(STATUS "Plugin will install to: ${GDAL_PLUGIN_DIR}")
