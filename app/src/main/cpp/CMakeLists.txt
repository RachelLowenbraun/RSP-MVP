cmake_minimum_required(VERSION 3.22.1)
project("rsp_timing")

# Strict flags. This is a timing-critical library; -Werror keeps us honest.
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)

# Reject exceptions and RTTI in the render/timing hot path. The build gradle
# also sets these; we assert them here.
add_compile_options(-fno-exceptions -fno-rtti)

find_library(log-lib log)
find_library(android-lib android)
find_library(vulkan-lib vulkan)
find_library(egl-lib EGL)
find_library(glesv3-lib GLESv3)

add_library(rsp_timing SHARED
    jni/JniBridge.cpp
    renderer/RenderPath.cpp
    renderer/VulkanRenderer.cpp
    renderer/GlesRenderer.cpp
    fiducial/Fiducial.cpp
    timing/FrameScheduler.cpp
    timing/TimingLog.cpp
)

target_include_directories(rsp_timing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/fiducial
    ${CMAKE_CURRENT_SOURCE_DIR}/timing
)

target_link_libraries(rsp_timing
    ${log-lib}
    ${android-lib}
    ${vulkan-lib}
    ${egl-lib}
    ${glesv3-lib}
)
