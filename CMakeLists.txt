cmake_minimum_required(VERSION 3.15)
project(MonCtrl LANGUAGES CXX RC)

add_executable(MonCtrl WIN32 main.cpp app.rc)
add_executable(poke poke.cpp)
add_executable(installer WIN32 installer.cpp)

set_target_properties(MonCtrl poke installer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

target_compile_definitions(MonCtrl PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
target_compile_definitions(poke PRIVATE
    WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)

target_link_libraries(MonCtrl PRIVATE
    user32 gdi32 comctl32 shell32 dxva2 advapi32)
target_link_libraries(poke PRIVATE
    user32 dxva2)
target_link_libraries(installer PRIVATE
    user32 ole32 shell32 uuid)
