

cmake_minimum_required(VERSION 3.30)

project(oscilloscope_visualizer VERSION 0.1 LANGUAGES C CXX)


if(EXISTS "./_builds/_deps/imgui-build" AND IS_DIRECTORY "./_builds/_deps/imgui-build")
    message(STATUS "imgui found!")
else()
    include(FetchContent)

    message(STATUS "Téléchargement de Dear ImGui depuis GitHub...")

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        docking 
    )
    FetchContent_MakeAvailable(imgui)
endif()


set(imgui_SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/_deps/imgui-src )

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    # Backends requis pour faire le pont avec SDL3 et SDL_Renderer
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    imfilebrowser.h
)


if (MSVC)
    set(CMAKE_CXX_STANDARD 23)
endif()



if (LINUX)
        set(CMAKE_CXX_STANDARD 26)
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)

#if(MSVC)
#    add_compile_options("/fp:fast")
#  else()
#    add_compile_options("-ffast-math" "-fno-finite-math-only" "-fsigned-zeros" "-fno-associative-math")
#endif()



set(PROJECT_SOURCES

        main.cpp
        miniaudio.c
        dr_libs/dr_flac.h 
        dr_libs/dr_mp3.h 
        dr_libs/dr_wav.h
)

#find_package(OpenGL REQUIRED)


add_executable(${PROJECT_NAME} main.cpp ${IMGUI_SOURCES} )

#target_link_libraries(${PROJECT_NAME} PRIVATE OpenGL::GL)

if (MSVC)

    add_compile_options( /arch:AVX2 /arch:SSE2)

    #target_link_options(pitch_control_daw_qt PRIVATE "/NODEFAULTLIB:library") 
    #target_link_directories(pitch_control_daw_qt PRIVATE ${CMAKE_SOURCE_DIR}/SDL3/lib)
    target_link_libraries(${PROJECT_NAME} PRIVATE  ${CMAKE_SOURCE_DIR}/SDL3.lib) # SDL3

endif()

if(LINUX)

        add_compile_options(-msse2 -lpthread -lm -march=native -mfma  )
        target_include_directories(${PROJECT_NAME} PUBLIC  /usr/local/include ./build/_deps/imgui-src ./build/_deps/imgui-src/backends )  
        target_link_directories(${PROJECT_NAME} PUBLIC . )
        target_link_libraries( ${PROJECT_NAME} PRIVATE  ${CMAKE_SOURCE_DIR}/libSDL3.a ) # SDL3

endif()


