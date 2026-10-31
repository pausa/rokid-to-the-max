cmake_minimum_required(VERSION 3.16)
project(rokid-screen C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client)

# Generate Wayland protocol marshalling code from the vendored XML.
set(PROTO_XML ${CMAKE_CURRENT_SOURCE_DIR}/protocol/wlr-screencopy-unstable-v1.xml)
set(PROTO_CODE ${CMAKE_CURRENT_BINARY_DIR}/screencopy-protocol.c)
set(PROTO_HDR ${CMAKE_CURRENT_BINARY_DIR}/screencopy-client-protocol.h)
add_custom_command(
  OUTPUT ${PROTO_CODE} ${PROTO_HDR}
  COMMAND wayland-scanner private-code ${PROTO_XML} ${PROTO_CODE}
  COMMAND wayland-scanner client-header ${PROTO_XML} ${PROTO_HDR}
  DEPENDS ${PROTO_XML}
  COMMENT "wayland-scanner wlr-screencopy"
  VERBATIM)

set(OPENXR_SDK ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr-sdk-source)
set(LOADER_LIB ${OPENXR_SDK}/build-hr/src/loader/libopenxr_loader.so)

add_executable(rokid-screen
  src/main.cpp
  ${PROTO_CODE})

target_include_directories(rokid-screen PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}
  ${OPENXR_SDK}/include
  ${WAYLAND_INCLUDE_DIRS})

target_link_libraries(rokid-screen PRIVATE
  Vulkan::Vulkan
  ${WAYLAND_LIBRARIES}
  ${LOADER_LIB}
  pthread)

target_compile_options(rokid-screen PRIVATE -Wall -Wextra)

# compile shaders and embed them as C arrays (glslang --vn)
foreach(sh vert frag)
  set(spv_h ${CMAKE_CURRENT_BINARY_DIR}/shaders/${sh}_spv.h)
  add_custom_command(
    OUTPUT ${spv_h}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
    COMMAND glslang -S ${sh} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh}.glsl -V -o ${CMAKE_CURRENT_BINARY_DIR}/shaders/${sh}.spv.tmp
    COMMAND glslang --vn ${sh}_spv --quiet ${CMAKE_CURRENT_BINARY_DIR}/shaders/${sh}.spv.tmp -o ${spv_h} || python3 -c "import struct,sys; d=open('${CMAKE_CURRENT_BINARY_DIR}/shaders/${sh}.spv.tmp','rb').read(); a=struct.unpack('<%dI'%(len(d)//4), d); open('${spv_h}','w').write('static const uint32_t ${sh}_spv[] = {%s};\n' % ','.join(str(x) for x in a))"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh}.glsl
    COMMENT "glslang+embed ${sh}"
    VERBATIM)
  target_sources(rokid-screen PRIVATE ${spv_h})
endforeach()
