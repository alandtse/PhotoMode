cmake_minimum_required(VERSION 3.20)
set(NAME "po3_PhotoMode" CACHE STRING "")
set(VERSION 2.0.3 CACHE STRING "")
# VR variant counter appended as the 4th version part. 0 = base (Nexus shows the 3-part
# upstream version); bump to 1+ for a re-release on the same upstream base (Nexus then
# shows all 4 parts).
set(VR_VERSION 0)

# ---- Options ----

option(COPY_BUILD "Copy the build output to the Skyrim directories." TRUE)

# ---- Cache build vars ----

macro(set_from_environment VARIABLE)
	if (NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
		set(${VARIABLE} $ENV{${VARIABLE}})
	endif ()
endmacro()

macro(find_commonlib_path)
	if (CommonLibName AND NOT ${CommonLibName} STREQUAL "")
		# Check extern
		find_path(CommonLibPath
		include/REL/Relocation.h
		PATHS extern/${CommonLibName})
		if (${CommonLibPath} STREQUAL "CommonLibPath-NOTFOUND")
			#Check path
			set_from_environment(${CommonLibName}Path)
			set(CommonLibPath ${${CommonLibName}Path})
		endif()
	endif()
endmacro()

set_from_environment(VCPKG_ROOT)

set(VERSION ${VERSION}.${VR_VERSION})

# One cross-runtime CommonLibSSE-NG DLL (alandtse/CommonLibVR) for Skyrim SE, AE and VR.
# Enabling all three runtimes selects SKYRIM_CROSS_VR and defines COMMONLIBSSE_NG, so the
# code resolves addresses and engine struct layouts per game version at load time. The NG
# CMake target is named "CommonLibSSE".
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
set(CommonLibName "CommonLibSSE")
set(CommonLibPath "${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibVR")
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
set(SkyrimVersion "Skyrim SE/AE/VR")
set_from_environment(Skyrim64Path)
set_from_environment(SkyrimAEPath)
set_from_environment(SkyrimVRPath)
find_commonlib_path()
message(
	STATUS
	"Building ${NAME} ${VERSION} for ${SkyrimVersion} with ${CommonLibName} at ${CommonLibPath}."
)

if (DEFINED VCPKG_ROOT)
	set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
	set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
else ()
	message(
		WARNING
		"Variable VCPKG_ROOT is not set. Continuing without vcpkg."
	)
endif ()

set(Boost_USE_STATIC_RUNTIME OFF CACHE BOOL "")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")

# ---- Project ----

project(
	${NAME}
	VERSION ${VERSION}
	LANGUAGES CXX
)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in
	${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
	@ONLY
)

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	@ONLY
)

# ---- Include guards ----

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(
		FATAL_ERROR
			"In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
	)
endif()

# ---- Globals ----

add_compile_definitions(
	SKSE_SUPPORT_XBYAK
)

if (MSVC)
	if (NOT ${CMAKE_GENERATOR} STREQUAL "Ninja")
		add_compile_options(
			/MP	# Build with Multiple Processes
		)
	endif ()
endif ()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

set(Boost_USE_STATIC_LIBS ON)

# ---- Dependencies ----
if (DEFINED CommonLibPath AND NOT ${CommonLibPath} STREQUAL "" AND IS_DIRECTORY ${CommonLibPath})
	add_subdirectory(${CommonLibPath} ${CommonLibName})
else ()
	message(
		FATAL_ERROR
		"Variable ${CommonLibName}Path is not set or in extern/."
	)
endif()

find_package(boost_unordered CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(rapidfuzz CONFIG REQUIRED)

find_path(SRELL_INCLUDE_DIRS "srell.hpp")
find_path(CLIB_UTIL_INCLUDE_DIRS "ClibUtil/utils.hpp")

# ---- Add source files ----

include(cmake/headerlist.cmake)
include(cmake/sourcelist.cmake)

source_group(
	TREE
		${CMAKE_CURRENT_SOURCE_DIR}
	FILES
		${headers}
		${sources}
)

source_group(
	TREE
		${CMAKE_CURRENT_BINARY_DIR}
	FILES
		${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
)

# ---- Create DLL ----

add_library(
	${PROJECT_NAME}
	SHARED
	${headers}
	${sources}
	${CMAKE_CURRENT_BINARY_DIR}/include/Version.h
	${CMAKE_CURRENT_BINARY_DIR}/version.rc
	.clang-format
	.editorconfig
)

target_compile_features(
	${PROJECT_NAME}
	PRIVATE
		cxx_std_23
)

target_compile_definitions(
	${PROJECT_NAME}
	PRIVATE
		_UNICODE
)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}/src
		${SRELL_INCLUDE_DIRS}
		${CLIB_UTIL_INCLUDE_DIRS}
)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
		${CommonLibName}::${CommonLibName}
		Boost::unordered
		Microsoft::DirectXTex
		Freetype::Freetype
		glaze::glaze
		imgui::imgui
		rapidfuzz::rapidfuzz
)

target_precompile_headers(
	${PROJECT_NAME}
	PRIVATE
		src/PCH.h
)

# On VR the ImGui menu renders through the ImGuiVRHelper plugin (header-only client SDK);
# on flat screen the client is dormant and ImGui presents directly.
include(FetchContent)
FetchContent_Declare(
	ImGuiVRHelper
	GIT_REPOSITORY https://github.com/alandtse/imgui-vr-helper.git
	GIT_TAG        v1.1.0
	GIT_SUBMODULES ""
	SOURCE_SUBDIR  api
)
FetchContent_MakeAvailable(ImGuiVRHelper)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
		ImGuiVRHelper::api
		nlohmann_json::nlohmann_json
)

if (MSVC)
	target_compile_options(
		${PROJECT_NAME}
		PRIVATE
			/sdl             # Enable Additional Security Checks
			/utf-8           # Set Source and Executable character sets to UTF-8
			/Zi              # Debug Information Format

			/permissive-     # Standards conformance
			/Zc:preprocessor # Enable preprocessor conformance mode

			/wd4200          # nonstandard extension used : zero-sized array in struct/union

			"$<$<CONFIG:DEBUG>:>"
			"$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
	)

	target_link_options(
		${PROJECT_NAME}
		PRIVATE
			"$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
			"$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
	)
endif ()

# ---- Post build ----

if (COPY_BUILD)
	# One DLL serves every runtime, so deploy it to each game directory that is configured.
	set(_copied FALSE)
	foreach (_gamePath ${Skyrim64Path} ${SkyrimAEPath} ${SkyrimVRPath})
		if (DEFINED _gamePath AND NOT "${_gamePath}" STREQUAL "")
			add_custom_command(
				TARGET ${PROJECT_NAME}
				POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> ${_gamePath}/SKSE/Plugins/
				COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_PDB_FILE:${PROJECT_NAME}> ${_gamePath}/SKSE/Plugins/
			)
			set(_copied TRUE)
		endif ()
	endforeach ()
	if (NOT _copied)
		message(WARNING "No Skyrim path (Skyrim64Path/SkyrimAEPath/SkyrimVRPath) is set. Skipping post-build copy.")
	endif ()
endif ()
