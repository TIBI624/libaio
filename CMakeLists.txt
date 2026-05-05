cmake_minimum_required(VERSION 3.18)
project(LibAio VERSION 1.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(ANDROID)
    add_library(libaio SHARED libaio.cpp)
    target_compile_definitions(libaio PRIVATE 
        __ANDROID__ 
        LIBAIO_LOG_TAG="libaio_native"
    )
    target_link_libraries(libaio PRIVATE log android)
else()
    add_library(libaio SHARED libaio.cpp)
    target_link_libraries(libaio PRIVATE pthread)
endif()

# Force output name to exactly libaio.so (no lib prefix duplication)
set_target_properties(libaio PROPERTIES
    PREFIX ""
    OUTPUT_NAME "libaio"
    SUFFIX ".so"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(libaio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS libaio DESTINATION lib)