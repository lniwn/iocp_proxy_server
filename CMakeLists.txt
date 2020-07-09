cmake_minimum_required(VERSION 3.16)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

project(iocp_proxy_server)

# =========================== 全局属性设置
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${${CMAKE_PROJECT_NAME}_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${${CMAKE_PROJECT_NAME}_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${${CMAKE_PROJECT_NAME}_BINARY_DIR}/lib)

if (WIN32)
    set(CMAKE_WIN32_EXECUTABLE ON)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()

if (CMAKE_VERSION VERSION_LESS "3.7.0")
    set(CMAKE_INCLUDE_CURRENT_DIR ON)
endif ()

# =========================== 文件分组
FILE(GLOB ROOT_HEADER src/*.h)

FILE(GLOB ROOT_SOURCE src/*.cpp;src/*.c;)

SOURCE_GROUP("src" FILES ${ROOT_HEADER} ${ROOT_SOURCE})


# =========================== 文件定义

# =========================== 查找路径
INCLUDE_DIRECTORIES(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)


# =========================== 资源
set(RES_FILES "")
if (WIN32)
    set(RES_FILES "src/resource.rc")
    set(CMAKE_RC_COMPILER_INIT windres)
    enable_language(RC)
endif ()


add_executable(${CMAKE_PROJECT_NAME} ${RES_FILES}
        ${ROOT_HEADER}

        ${ROOT_SOURCE}
        )

# precompile headers
target_precompile_headers(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/framework.h")

# Add compiler options
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/MP /W4>
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wall>
        $<$<CXX_COMPILER_ID:GNU>:
        -pedantic -Wall -Wextra -Wfloat-equal -Wshadow -Wdisabled-optimization -Wunsafe-loop-optimizations -Wconversion
        -Wsign-conversion>
        )

# Add compiler definitions
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        _UNICODE
        UNICODE
        )

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES VS_DPI_AWARE "PerMonitor")

# Add link library
#target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE dbghelp.lib)

# Add privilege
#target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'")
#set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'")
