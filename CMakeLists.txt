cmake_minimum_required(VERSION 3.16)
project(imtrakker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-2.30.11
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL2)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_DIR ${imgui_SOURCE_DIR})

add_executable(imtrakker
    src/main.cpp
    src/hsq.cpp
    src/midi.cpp
    src/mod/module.cpp
    src/mod/load_mmd.cpp
    src/mod/load_sfx.cpp
    src/mod/load_extended.cpp
    src/mod/player.cpp
    src/mod/save_mod.cpp
    src/mod/editor.cpp
    src/mod/sample_io.cpp
    src/mod/sample_edit.cpp
    src/smus/smus_load.cpp
    src/smus/smus_engine.cpp
    src/smus/smus_save.cpp
    src/smus/smus_convert.cpp
    src/modarchive.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
)

target_include_directories(imtrakker PRIVATE
    src
    external
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(imtrakker PRIVATE
    SDL2::SDL2main
    SDL2::SDL2
)

if(WIN32)
    # portable-file-dialogs (IFileDialog / GetOpenFileName)
    target_link_libraries(imtrakker PRIVATE comdlg32 ole32 uuid shell32 winmm winhttp)

    # Emit SDL2*.dll into the same config folder as imtrakker.exe (Debug/Release/…).
    # Avoids a post-build copy race where F5 launches before the DLL lands beside the exe
    # (Windows error 0xc0000135 STATUS_DLL_NOT_FOUND).
    set_target_properties(SDL2 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
    )

    # Belt-and-suspenders: still copy after link (covers odd multi-config layouts).
    add_custom_command(TARGET imtrakker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SDL2::SDL2>"
            "$<TARGET_FILE_DIR:imtrakker>"
        COMMENT "Ensure SDL2 DLL is next to imtrakker"
        VERBATIM
    )
endif()

# Relative fallback when resolving CLI module paths / IMTRAKKER_DATA is unset.
target_compile_definitions(imtrakker PRIVATE
    IMTRAKKER_DEFAULT_DATA_DIR="../data"
)
