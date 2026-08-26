cmake_minimum_required(VERSION 3.16)
project(ogr_GeoPBF CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(GDAL REQUIRED)

# ベクタドライバのプラグイン名は ogr_* が GDAL の慣習（ラスタは gdal_*）。GDAL はどちらの接頭辞も読む。
add_library(ogr_GeoPBF MODULE ogrgeopbf.cpp ogrgeopbfwrite.cpp)
target_include_directories(ogr_GeoPBF PRIVATE ${GDAL_INCLUDE_DIRS})
target_link_libraries(ogr_GeoPBF PRIVATE GDAL::GDAL)

# strip lib prefix so GDAL finds it as "ogr_GeoPBF.so"
set_target_properties(ogr_GeoPBF PROPERTIES PREFIX "")

# install target: -DGDAL_PLUGIN_DIR=... で上書き可（パッケージャ用）。既定は gdal-config に聞く。
if(NOT GDAL_PLUGIN_DIR)
    execute_process(COMMAND gdal-config --plugindir
        OUTPUT_VARIABLE GDAL_PLUGIN_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(NOT GDAL_PLUGIN_DIR)
    set(GDAL_PLUGIN_DIR "${CMAKE_INSTALL_PREFIX}/lib/gdalplugins")
endif()

install(TARGETS ogr_GeoPBF DESTINATION ${GDAL_PLUGIN_DIR})
message(STATUS "Plugin will install to: ${GDAL_PLUGIN_DIR}")
