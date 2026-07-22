cmake_minimum_required(VERSION 3.18)
project(LibAio VERSION 1.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Find and configure JNI ---
find_package(JNI REQUIRED)

option(BUILD_SHARED_LIBS "Build shared library" ON)
option(LIBAIO_ENABLE_TESTS "Enable tests" OFF)

# Platform detection
if(APPLE)
    set(PLATFORM_OS "macos")
elseif(ANDROID)
    set(PLATFORM_OS "android")
elseif(UNIX)
    set(PLATFORM_OS "linux")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

add_library(libaio SHARED libaio.cpp)
if(NOT BUILD_SHARED_LIBS)
    add_library(libaio_static STATIC libaio.cpp)
    set_target_properties(libaio_static PROPERTIES OUTPUT_NAME "libaio")
endif()

target_compile_definitions(libaio PRIVATE ${PLATFORM_OS}_PLATFORM)
if(ANDROID)
    target_compile_definitions(libaio PRIVATE LIBAIO_LOG_TAG="libaio_native")
    target_link_libraries(libaio PRIVATE log android)
else()
    target_link_libraries(libaio PRIVATE pthread ${JNI_LIBRARIES})
    if(APPLE)
        # No extra libs for macOS
    endif()
endif()

# Set output name for shared library
set_target_properties(libaio PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libaio"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(APPLE)
    set_target_properties(libaio PROPERTIES SUFFIX ".dylib")
else()
    set_target_properties(libaio PROPERTIES SUFFIX ".so")
endif()

target_include_directories(libaio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
    ${JNI_INCLUDE_DIRS}
)

install(TARGETS libaio
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)
install(FILES libaio.h DESTINATION include)
