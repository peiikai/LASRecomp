# oead's Python module (py/) for Android, without its FindPython (it can't find a Python for another platform): the
# module built against the Android Python's headers and libpython (android/deps/build_gen_deps.sh passes OEAD, PYINC,
# PYLIB), named as Python looks for it.
cmake_minimum_required(VERSION 3.15)
project(oead_android CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(PYBIND11_NOPYTHON ON)
add_subdirectory(${OEAD}/lib/pybind11 pybind11)
add_subdirectory(${OEAD} oead)
file(GLOB PYSRC ${OEAD}/py/*.cpp)
add_library(oead_py MODULE ${PYSRC})
target_include_directories(oead_py PRIVATE ${PYINC})
target_link_libraries(oead_py PRIVATE oead pybind11::module ${PYLIB})
set_target_properties(oead_py PROPERTIES PREFIX "" OUTPUT_NAME oead SUFFIX ".cpython-314-aarch64-linux-android.so"
                      CXX_VISIBILITY_PRESET hidden)
