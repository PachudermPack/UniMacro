cmake_minimum_required(VERSION 3.12.0)
project(UniMacro VERSION 1.0.3 LANGUAGES CXX)

add_executable(UniMacro main.cpp)

target_compile_features(UniMacro PRIVATE cxx_std_17)

if(MSVC)
    target_compile_definitions(UniMacro PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_link_libraries(UniMacro PRIVATE winmm)

install(TARGETS UniMacro
    RUNTIME DESTINATION .
)

install(FILES UniMacro.ini
    DESTINATION .
)
