cmake_minimum_required(VERSION 3.30)

set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
set(CMAKE_CXX_MODULE_STD ON)

project(imgui_sdl3_vulkan_example)

find_package(SDL3 REQUIRED)
find_package(Vulkan REQUIRED)

set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cpm_cache")

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake")
    file(DOWNLOAD https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/CPM.cmake
         ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)
endif()
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

CPMAddPackage(
  NAME imgui
  GITHUB_REPOSITORY ocornut/imgui
  GIT_TAG v1.91.8 # Consider using the latest release or a specific commit
  DOWNLOAD_ONLY YES # imgui is added as source files directly
)

CPMAddPackage(
    NAME tinygltf
    GITHUB_REPOSITORY syoyo/tinygltf
    VERSION 2.9.6 # Consider using the latest release or a specific commit    
)

CPMAddPackage(
    NAME bsThreadPool
    GITHUB_REPOSITORY bshoshany/thread-pool 
    VERSION 5.0.0
    PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/bsThread_pool.patch
)

CPMAddPackage(
    NAME freeType
    GITHUB_REPOSITORY freetype/freetype
    GIT_TAG 1529bc6
)

if (TARGET freetype AND NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

CPMAddPackage(
    NAME msdfgen
    GITHUB_REPOSITORY Chlumsky/msdfgen
    GIT_TAG 0388956
    OPTIONS
    "MSDFGEN_CORE_ONLY OFF"
        "MSDFGEN_BUILD_STANDALONE OFF" # Explicitly disable CLI build
        "MSDFGEN_USE_VCPKG OFF"       # Explicitly disable Vcpkg
        "MSDFGEN_DISABLE_SVG ON"      # Explicitly disable SVG (and thus tinyxml2)
        "MSDFGEN_DISABLE_PNG ON"      # Explicitly disable PNG if not needed
        "MSDFGEN_USE_SKIA OFF"        # Explicitly disable Skia if not needed
)

CPMAddPackage(
    NAME msdfAtlasGen
    GITHUB_REPOSITORY Chlumsky/msdf-atlas-gen
    GIT_TAG 30b6f4f
    OPTIONS
    "MSDFGEN_CORE_ONLY OFF"
        "MSDFGEN_BUILD_STANDALONE OFF" # Explicitly disable CLI build
        "MSDFGEN_USE_VCPKG OFF"       # Explicitly disable Vcpkg
        "MSDFGEN_DISABLE_SVG ON"      # Explicitly disable SVG (and thus tinyxml2)
        "MSDFGEN_DISABLE_PNG ON"      # Explicitly disable PNG if not needed
        "MSDFGEN_USE_SKIA OFF"        # Explicitly disable Skia if not needed
        "MSDF_ATLAS_BUILD_STANDALONE OFF"
        "MSDF_ATLAS_USE_VCPKG OFF"
        "MSDF_ATLAS_MSDFGEN_EXTERNAL ON"       
        "MSDF_ATLAS_USE_SKIA OFF"
        "MSDF_ATLAS_NO_ARTERY_FONT OFF"
)

CPMAddPackage(
    NAME xxHash
    GITHUB_REPOSITORY Cyan4973/xxHash
    GIT_TAG b5694e436dc9fc1c5a0389ac46b446167075490a
)

CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG 5c205738c191bcb0abc65c4febfa9bd25ff35234 # This is a specific commit for stb_image.h etc.
    # For header-only libraries like stb, DOWNLOAD_ONLY might be an option
    # if you manually manage include directories. However, if it has a CMakeLists.txt
    # that defines a target, it's better to let CPM handle it.
    # Given stb is typically used as individual headers, often DOWNLOAD_ONLY is fine
    # if you then add stb_SOURCE_DIR to include_directories.
    # For simplicity if it doesn't have a good CMake target, DOWNLOAD_ONLY YES
    # and then target_include_directories(AppModule PRIVATE ${stb_SOURCE_DIR})
    # Let's assume for now it's fine as is, or it provides a target.
)

CPMAddPackage(
  NAME VulkanMemoryAllocatorHpp
  GITHUB_REPOSITORY YaaZ/VulkanMemoryAllocator-Hpp
  GIT_TAG v3.2.1
  PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/patches/vk_mem_alloc.patch"
)

add_library(AppModule)
target_compile_features(AppModule PRIVATE cxx_std_23)
target_compile_definitions(AppModule PRIVATE
    VULKAN_HPP_NO_SMART_HANDLE
    VULKAN_HPP_NO_EXCEPTIONS
    VULKAN_HPP_NO_CONSTRUCTORS
    VK_USE_PLATFORM_WAYLAND_KHR
    VMA_BUILD_CXX_MODULE
    SDL_MAIN_HANDLED
    BS_THREAD_POOL_IMPORT_STD
    XXHASH_BUILD_XXHSUM OFF
    # DMSDFGEN_BUILD_STANDALONE OFF # Disable CLI
    # DMSDFGEN_USE_VCPKG OFF      # Crucial: Disable Vcpkg check
    # DMSDFGEN_DISABLE_SVG ON     # Disable SVG to remove tinyxml2 dependency
    # DMSDFGEN_DISABLE_PNG ON     # Disable PNG if not needed
    # DMSDFGEN_USE_SKIA OFF       # Disable Skia if not needed
    BUILD_SHARED_LIBS OFF
    "$<IF:$<CONFIG:Debug>,ENABLE_VALIDATION=1,ENABLE_VALIDATION=0>"
)

add_subdirectory(${xxHash_SOURCE_DIR}/build/cmake/ xxhash_build EXCLUDE_FROM_ALL)

target_sources(AppModule
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS
            src
            ${Vulkan_INCLUDE_DIR}
            ${VulkanMemoryAllocatorHpp_SOURCE_DIR}
            ${bsThreadPool_SOURCE_DIR}
        FILES
        ${Vulkan_INCLUDE_DIR}/vulkan/vulkan.cppm
        ${VulkanMemoryAllocatorHpp_SOURCE_DIR}/src/vk_mem_alloc.cppm
        ${bsThreadPool_SOURCE_DIR}/modules/BS.thread_pool.cppm
        src/3rd-party/tinygltf_module_wrapper.cppm
        src/SDL_Wrapper.cppm
        src/app.cppm
        src/imgui.cppm
        src/memory_types.cppm
        src/utils.cppm
        src/extra.cppm
        
        src/VulkanWindow.cppm
        src/VulkanDevice.cppm
        src/VulkanInstance.cppm
        src/VulkanPipeline.cppm
        
        src/ui.cppm
        src/text.cppm
        src/UISystem.cppm
        src/TextSystem.cppm
        # src/PieceTable.cppm
        src/TextEditor.cppm
        src/TextView.cppm
        src/3DEngine.cppm

        src/ModelLoader.cppm
        src/types.cppm
        src/mesh.cppm
        src/scene.cppm
        src/SceneBuilder.cppm
        src/texture.cppm
        src/TextureStore.cppm
)

target_sources(AppModule
    PRIVATE
        src/3rd-party/implementations.cpp
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(AppModule PRIVATE
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${freeType_SOURCE_DIR}/include
  ${tinygltf_SOURCE_DIR}
  ${SDL3_INCLUDE_DIRS}
  ${VulkanMemoryAllocatorHpp_SOURCE_DIR}/VulkanMemoryAllocator/include
  ${VulkanMemoryAllocatorHpp_SOURCE_DIR}/include
  ${bsThreadPool_SOURCE_DIR}/include
  ${msdfgen_SOURCE_DIR}
  ${msdfAtlasGen_SOURCE_DIR}/msdf-atlas-gen
)

target_link_libraries(AppModule PRIVATE
    Freetype::Freetype
    msdfgen::msdfgen-core
    msdfgen::msdfgen-ext
    msdf-atlas-gen::msdf-atlas-gen
    SDL3::SDL3
    Vulkan::Vulkan
    tinygltf
    xxHash::xxhash
)

add_executable(${PROJECT_NAME}
  src/main.cpp
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_link_libraries(${PROJECT_NAME} PRIVATE AppModule)

set(SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)

# Create the output directory if it doesn't exist
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Function to compile a single Slang shader to SPIR-V
# Usage: compile_shader(<target_name> <source_path> <stage> <output_name>)
function(compile_shader target_name source_path stage output_name)
    set(output_file ${SHADER_OUTPUT_DIR}/${output_name}.spv)

    add_custom_command(
        OUTPUT ${output_file}
        COMMAND slangc -o ${output_file}
                       -profile glsl_460
                       -entry main
                       -stage ${stage}
                       ${source_path}
        DEPENDS ${source_path}
        COMMENT "Compiling ${source_path} to ${output_file} as ${stage}"
    )

    add_custom_target(${target_name} ALL
        DEPENDS ${output_file}
    )

    add_dependencies(${PROJECT_NAME} ${target_name})
endfunction()

# === Usage Examples ===

# Compile vert.slang as vertex shader to vert.spv
compile_shader(CompileVertShader ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vert.slang vertex vert)

# Compile frag.slang as fragment shader to frag.spv
compile_shader(CompileFragShader ${CMAKE_CURRENT_SOURCE_DIR}/shaders/frag.slang fragment frag)

# Compile text_vert.slang as vertex shader to text_vert.spv
compile_shader(CompileTextVertShader ${CMAKE_CURRENT_SOURCE_DIR}/shaders/text_vert.slang vertex text_vert)

# Compile text_frag.slang as fragment shader to text_frag.spv
compile_shader(CompileTextFragShader ${CMAKE_CURRENT_SOURCE_DIR}/shaders/text_frag.slang fragment text_frag)

# Compile ui_vert.slang as vertex shader to ui_vert.spv
compile_shader(CompileuiVertShader ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui_vert.slang vertex ui_vert)

# Compile ui_frag.slang as fragment shader to ui_frag.spv
compile_shader(CompileuiFragShader ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui_frag.slang fragment ui_frag)
