cmake_minimum_required(VERSION 3.24)

project(GlassLight VERSION 0.2.0 LANGUAGES C CXX)

include(FetchContent)
include(CTest)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
endif()

find_package(Vulkan 1.2 REQUIRED COMPONENTS glslangValidator)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.12
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.3.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(vma)

add_library(glasslight-imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(glasslight-imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(glasslight-imgui PUBLIC SDL3::SDL3-static)

set(GLASSLIGHT_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(GLASSLIGHT_COMPILED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(GLASSLIGHT_SHADER_SOURCES
    ${GLASSLIGHT_SHADER_DIR}/first_light.comp
    ${GLASSLIGHT_SHADER_DIR}/glass_preview.comp
    ${GLASSLIGHT_SHADER_DIR}/resolve.comp
)
set(GLASSLIGHT_SHADER_BINARIES)
foreach(shader IN LISTS GLASSLIGHT_SHADER_SOURCES)
    get_filename_component(shader_name "${shader}" NAME)
    set(shader_binary "${GLASSLIGHT_COMPILED_SHADER_DIR}/${shader_name}.spv")
    add_custom_command(
        OUTPUT "${shader_binary}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GLASSLIGHT_COMPILED_SHADER_DIR}"
        COMMAND Vulkan::glslangValidator -V --target-env vulkan1.2
                "-I${GLASSLIGHT_SHADER_DIR}" "${shader}" -o "${shader_binary}"
        DEPENDS "${shader}" "${GLASSLIGHT_SHADER_DIR}/geometry.glsl"
        VERBATIM
    )
    list(APPEND GLASSLIGHT_SHADER_BINARIES "${shader_binary}")
endforeach()
foreach(shader_name IN ITEMS first_light.comp glass_preview.comp)
    set(shader "${GLASSLIGHT_SHADER_DIR}/${shader_name}")
    set(shader_binary "${GLASSLIGHT_COMPILED_SHADER_DIR}/${shader_name}.fracture.spv")
    add_custom_command(
        OUTPUT "${shader_binary}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GLASSLIGHT_COMPILED_SHADER_DIR}"
        COMMAND Vulkan::glslangValidator -V --target-env vulkan1.2
                -DGLASSLIGHT_FRACTURE_SHADER=1
                "-I${GLASSLIGHT_SHADER_DIR}" "${shader}" -o "${shader_binary}"
        DEPENDS "${shader}" "${GLASSLIGHT_SHADER_DIR}/geometry.glsl"
        VERBATIM
    )
    list(APPEND GLASSLIGHT_SHADER_BINARIES "${shader_binary}")
endforeach()
add_custom_target(glasslight-shaders DEPENDS ${GLASSLIGHT_SHADER_BINARIES})

add_library(glasslight-platform STATIC
    src/platform/platform.cpp
)
target_include_directories(glasslight-platform PUBLIC src)
if(WIN32)
    target_link_libraries(glasslight-platform PRIVATE ole32 shell32 uuid)
endif()

add_library(glasslight-core STATIC
    src/core/composition.cpp
    src/core/fracture_layout.cpp
    src/core/persistence.cpp
)
target_include_directories(glasslight-core PUBLIC src)
target_link_libraries(glasslight-core PUBLIC
    glasslight-platform
    nlohmann_json::nlohmann_json
)

add_library(glasslight-render STATIC
    src/render/vulkan_renderer.cpp
)
add_dependencies(glasslight-render glasslight-shaders)
target_include_directories(glasslight-render PUBLIC src)
target_compile_definitions(glasslight-render PRIVATE
    GLASSLIGHT_SHADER_DIR="${GLASSLIGHT_COMPILED_SHADER_DIR}"
)
target_link_libraries(glasslight-render PUBLIC
    glasslight-core
    glasslight-platform
    Vulkan::Vulkan
    SDL3::SDL3-static
)

add_library(glasslight-media STATIC
    src/media/png_metadata.cpp
    src/media/video_export.cpp
)
target_include_directories(glasslight-media PUBLIC src)
target_link_libraries(glasslight-media PUBLIC
    glasslight-platform
    SDL3::SDL3-static
)

add_library(glasslight-studio STATIC
    src/studio/studio_ui.cpp
)
target_include_directories(glasslight-studio PUBLIC src)
target_link_libraries(glasslight-studio PUBLIC glasslight-core glasslight-imgui)

if(WIN32)
    configure_file(packaging/windows/glasslight.manifest.in
        ${CMAKE_CURRENT_BINARY_DIR}/glasslight.manifest @ONLY)
    configure_file(packaging/windows/glasslight.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/glasslight.rc @ONLY)
    add_executable(glasslight WIN32
        src/app/main.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/glasslight.rc
    )
    set_target_properties(glasslight PROPERTIES OUTPUT_NAME GlassLight)
else()
    add_executable(glasslight src/app/main.cpp)
endif()
target_link_libraries(glasslight PRIVATE
    glasslight-core
    glasslight-render
    glasslight-media
    glasslight-studio
    glasslight-imgui
    nlohmann_json::nlohmann_json
)
target_compile_definitions(glasslight PRIVATE GLASSLIGHT_VERSION="${PROJECT_VERSION}")
target_compile_definitions(glasslight-render PRIVATE
    GLASSLIGHT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    GLASSLIGHT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    GLASSLIGHT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

if(MSVC)
    target_compile_options(glasslight PRIVATE /W4 /permissive-)
    target_compile_options(glasslight-platform PRIVATE /W4 /permissive-)
    target_compile_options(glasslight-core PRIVATE /W4 /permissive-)
    target_compile_options(glasslight-render PRIVATE /W4 /permissive-)
    target_compile_options(glasslight-media PRIVATE /W4 /permissive-)
    target_compile_options(glasslight-studio PRIVATE /W4 /permissive-)
else()
    target_compile_options(glasslight PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(glasslight-core PRIVATE -Wall -Wextra -Wpedantic)
    # Vulkan's canonical C aggregate initialization sets sType first and fills
    # the remaining fields immediately afterward; GCC otherwise emits one
    # warning for every untouched reserved member.
    target_compile_options(glasslight-render PRIVATE
        -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    target_compile_options(glasslight-media PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(glasslight-studio PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(glasslight-platform PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(BUILD_TESTING)
    add_executable(glasslight-composition-tests tests/composition_tests.cpp)
    target_link_libraries(glasslight-composition-tests PRIVATE glasslight-core)
    add_test(NAME composition COMMAND glasslight-composition-tests)

    add_executable(glasslight-png-tests tests/png_metadata_tests.cpp)
    target_link_libraries(glasslight-png-tests PRIVATE glasslight-media glasslight-core)
    add_test(NAME png-metadata COMMAND glasslight-png-tests)

    add_executable(glasslight-video-tests tests/video_export_tests.cpp)
    target_link_libraries(glasslight-video-tests PRIVATE glasslight-media)
    add_test(NAME video-export COMMAND glasslight-video-tests)

    add_executable(glasslight-studio-state-tests tests/studio_state_tests.cpp)
    target_link_libraries(glasslight-studio-state-tests PRIVATE
        glasslight-studio glasslight-core)
    add_test(NAME studio-state COMMAND glasslight-studio-state-tests)

    add_executable(glasslight-platform-tests tests/platform_tests.cpp)
    target_link_libraries(glasslight-platform-tests PRIVATE glasslight-platform)
    add_test(NAME platform COMMAND glasslight-platform-tests)

    # GPU-dependent structural and gallery validation remains opt-in rather
    # than part of CTest so headless/non-Vulkan build machines stay supported.
    add_executable(glasslight-render-validation tests/render_validation.cpp)
    target_link_libraries(glasslight-render-validation PRIVATE
        glasslight-render glasslight-media glasslight-core)
endif()

if(WIN32)
    install(TARGETS glasslight RUNTIME DESTINATION .)
    install(FILES ${GLASSLIGHT_SHADER_BINARIES} DESTINATION shaders)
    install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
else()
    install(TARGETS glasslight RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES ${GLASSLIGHT_SHADER_BINARIES}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glasslight/shaders)
    if(UNIX AND NOT APPLE)
        install(FILES packaging/linux/is.magnusp.glasslight.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES assets/glasslight.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
            RENAME is.magnusp.glasslight.svg)
        install(FILES packaging/linux/is.magnusp.glasslight.appdata.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    endif()
    install(FILES LICENSE THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/glasslight)
endif()

set(CPACK_PACKAGE_NAME "glasslight")
set(CPACK_PACKAGE_VENDOR "Magnus Petursson")
set(CPACK_PACKAGE_CONTACT "GlassLight maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Seeded Vulkan caustic generative-art studio")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_FILE_NAME "GlassLight-${PROJECT_VERSION}-windows-x64")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
else()
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "ffmpeg")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
endif()
include(CPack)
