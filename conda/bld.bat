@echo on
cmake -B build -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_INSTALL_PREFIX="%LIBRARY_PREFIX%" ^
    -DGDAL_PLUGIN_DIR="%LIBRARY_LIB%\gdalplugins" ^
    "%SRC_DIR%"
if errorlevel 1 exit 1

cmake --build build --parallel %CPU_COUNT%
if errorlevel 1 exit 1

cmake --install build
if errorlevel 1 exit 1
