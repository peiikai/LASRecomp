# quicktex's _quicktex module for Android, the same way as oead's (../oead/CMakeLists.txt): pybind11's headers from
# oead's copy (3.1, which knows Python 3.14), built against the Android Python (QT, OEAD, PYINC, PYLIB).
cmake_minimum_required(VERSION 3.15)
project(quicktex_android CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(PYBIND11_NOPYTHON ON)
add_subdirectory(${OEAD}/lib/pybind11 pybind11)
file(GLOB_RECURSE SRC ${QT}/quicktex/*.cpp)
add_library(quicktex_py MODULE ${SRC})
target_include_directories(quicktex_py PRIVATE ${PYINC} ${QT}/quicktex)
target_compile_definitions(quicktex_py PRIVATE VERSION_INFO=0.2.1)
target_link_libraries(quicktex_py PRIVATE pybind11::module ${PYLIB})
set_target_properties(quicktex_py PROPERTIES PREFIX "" OUTPUT_NAME _quicktex SUFFIX ".cpython-314-aarch64-linux-android.so"
                      CXX_VISIBILITY_PRESET hidden)
