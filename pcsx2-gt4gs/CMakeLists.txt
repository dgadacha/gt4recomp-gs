# GT4Recomp fork: gt4-gs.exe, the GT4Recomp GS process (see gt4gs/README.md and gt4gs/BUILD.md).
# Based on pcsx2-gsrunner; adds the live GS stream consumer and the .gscap replay.

# The vendored MIT transport uses C++ exceptions; PCSX2 builds without them (_HAS_EXCEPTIONS=0, no /EHsc). It is
# compiled into its own static library with exceptions enabled. StreamTransport.cpp catches every exception, so none
# crosses into PCSX2 code. The directory-wide _HAS_EXCEPTIONS=0 is removed here and given back to the executable.
get_directory_property(gt4gs_dir_defs COMPILE_DEFINITIONS)
list(FILTER gt4gs_dir_defs EXCLUDE REGEX "^_HAS_EXCEPTIONS=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${gt4gs_dir_defs}")

set(GT4GS_STREAM_DIR "${CMAKE_SOURCE_DIR}/3rdparty/gt4recomp-gs-stream")

# macOS and Linux use the POSIX transport of the vendored files (gs_stream_ipc_posix.cpp).
if(WIN32)
	set(gt4gs_transport_source "${GT4GS_STREAM_DIR}/gs_stream_ipc.cpp")
else()
	set(gt4gs_transport_source "${GT4GS_STREAM_DIR}/gs_stream_ipc_posix.cpp")
endif()
add_library(gt4gs_stream_transport STATIC
	StreamTransport.cpp
	StreamTransport.h
	"${gt4gs_transport_source}"
	"${GT4GS_STREAM_DIR}/gs_stream_file.cpp"
)
target_include_directories(gt4gs_stream_transport PRIVATE "${GT4GS_STREAM_DIR}")
target_compile_features(gt4gs_stream_transport PRIVATE cxx_std_20)
target_compile_definitions(gt4gs_stream_transport PRIVATE _HAS_EXCEPTIONS=1)
if(MSVC)
	target_compile_options(gt4gs_stream_transport PRIVATE /EHsc /wd4005)
else()
	# After the directory-wide -fno-exceptions; PosixCompat.h gives gs_stream_file.cpp its fopen_s.
	target_compile_options(gt4gs_stream_transport PRIVATE -fexceptions
		"SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/PosixCompat.h")
endif()
if(WIN32)
	target_link_libraries(gt4gs_stream_transport PUBLIC kernel32 advapi32)
endif()

add_executable(pcsx2-gt4gs)
set_target_properties(pcsx2-gt4gs PROPERTIES OUTPUT_NAME "gt4-gs")

if (PACKAGE_MODE)
	install(TARGETS pcsx2-gt4gs DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
	install(TARGETS pcsx2-gt4gs DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif()

target_sources(pcsx2-gt4gs PRIVATE
	Main.cpp
)

target_include_directories(pcsx2-gt4gs PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
	"${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(pcsx2-gt4gs PRIVATE _HAS_EXCEPTIONS=0)

target_link_libraries(pcsx2-gt4gs PRIVATE
	PCSX2_FLAGS
	PCSX2
	gt4gs_stream_transport
)

if(APPLE)
	# macOS port. Live mode shows the picture through a remote layer context (LiveViewMac.mm, built with ARC). The Metal
	# renderer loads its shaders from Metal23.metallib in resources/ next to gt4-gs (EmuFolders::Resources of an
	# unbundled executable), with the rest of bin/resources (fonts, GameIndex.yaml).
	target_sources(pcsx2-gt4gs PRIVATE LiveViewMac.mm LiveViewMac.h)
	set_source_files_properties(LiveViewMac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc" SKIP_PRECOMPILE_HEADERS ON)

	get_property(gt4gs_pcsx2_dir GLOBAL PROPERTY PCSX2_SOURCE_DIR)
	get_property(gt4gs_metal_shaders GLOBAL PROPERTY PCSX2_METAL_SHADERS)
	set(gt4gs_air_files "")
	foreach(shader IN LISTS gt4gs_metal_shaders)
		set(air "${CMAKE_CURRENT_BINARY_DIR}/Metal23/${shader}.air")
		get_filename_component(air_dir "${air}" DIRECTORY)
		add_custom_command(OUTPUT "${air}"
			COMMAND ${CMAKE_COMMAND} -E make_directory "${air_dir}"
			COMMAND xcrun metal -ffast-math -std=macos-metal2.3 -target air64-apple-macos11.0 -o "${air}"
				-c "${gt4gs_pcsx2_dir}/${shader}"
			DEPENDS "${gt4gs_pcsx2_dir}/${shader}" "${gt4gs_pcsx2_dir}/GS/Renderers/Metal/GSMTLSharedHeader.h"
				"${gt4gs_pcsx2_dir}/GS/Renderers/Metal/GSMTLShaderCommon.h"
		)
		list(APPEND gt4gs_air_files "${air}")
	endforeach()
	set(gt4gs_metallib "${CMAKE_CURRENT_BINARY_DIR}/Metal23.metallib")
	add_custom_command(OUTPUT "${gt4gs_metallib}"
		COMMAND xcrun metallib -o "${gt4gs_metallib}" ${gt4gs_air_files}
		DEPENDS ${gt4gs_air_files}
	)
	target_sources(pcsx2-gt4gs PRIVATE "${gt4gs_metallib}")
	add_custom_command(TARGET pcsx2-gt4gs POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/bin/resources"
			"$<TARGET_FILE_DIR:pcsx2-gt4gs>/resources"
		COMMAND ${CMAKE_COMMAND} -E copy "${gt4gs_metallib}" "$<TARGET_FILE_DIR:pcsx2-gt4gs>/resources/Metal23.metallib"
	)
endif()

# gt4-gs 1.0.0: the GS settings allow-list of the game repository (docs/launcher/gs-settings-allowlist.json, vendored
# by hash in 3rdparty/gt4recomp-gs-stream) is turned into a C++ table at configure time. Main.cpp ignores, with one
# warning line each, INI keys and values outside it (never refuses to start).
set(gt4gs_allowlist_json "${GT4GS_STREAM_DIR}/gs-settings-allowlist.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${gt4gs_allowlist_json}")
file(READ "${gt4gs_allowlist_json}" gt4gs_allowlist)
string(JSON gt4gs_allow_count LENGTH "${gt4gs_allowlist}" entries)
if(gt4gs_allow_count LESS 1)
	message(FATAL_ERROR "gs-settings-allowlist.json has no entries")
endif()
math(EXPR gt4gs_allow_last "${gt4gs_allow_count} - 1")
set(gt4gs_allow_rows "")
function(gt4gs_c_string out text)
	string(REPLACE "\\" "\\\\" text "${text}")
	string(REPLACE "\"" "\\\"" text "${text}")
	set(${out} "\"${text}\"" PARENT_SCOPE)
endfunction()
foreach(i RANGE ${gt4gs_allow_last})
	string(JSON section GET "${gt4gs_allowlist}" entries ${i} section)
	string(JSON key GET "${gt4gs_allowlist}" entries ${i} key)
	string(JSON type GET "${gt4gs_allowlist}" entries ${i} type)
	set(values "")
	string(JSON value_count ERROR_VARIABLE no_values LENGTH "${gt4gs_allowlist}" entries ${i} values)
	if(NOT no_values AND value_count GREATER 0)
		math(EXPR value_last "${value_count} - 1")
		foreach(v RANGE ${value_last})
			string(JSON value GET "${gt4gs_allowlist}" entries ${i} values ${v})
			if(value MATCHES "[|]")
				message(FATAL_ERROR "gs-settings-allowlist.json: value '${value}' contains the separator |")
			endif()
			if(v EQUAL 0)
				set(values "${value}")
			else()
				set(values "${values}|${value}")
			endif()
		endforeach()
	endif()
	set(has_range "false")
	set(minimum "0")
	set(maximum "0")
	string(JSON minimum_json ERROR_VARIABLE no_minimum GET "${gt4gs_allowlist}" entries ${i} min)
	string(JSON maximum_json ERROR_VARIABLE no_maximum GET "${gt4gs_allowlist}" entries ${i} max)
	if(NOT no_minimum AND NOT no_maximum)
		set(has_range "true")
		set(minimum "${minimum_json}")
		set(maximum "${maximum_json}")
	endif()
	gt4gs_c_string(section_c "${section}")
	gt4gs_c_string(key_c "${key}")
	gt4gs_c_string(type_c "${type}")
	gt4gs_c_string(values_c "${values}")
	string(APPEND gt4gs_allow_rows "\t{${section_c}, ${key_c}, ${type_c}, ${values_c}, ${has_range}, ${minimum}, ${maximum}},\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gt4gs_settings_allowlist.inc" CONTENT
	"// Generated by pcsx2-gt4gs/CMakeLists.txt from 3rdparty/gt4recomp-gs-stream/gs-settings-allowlist.json; do not edit.\n// section, key, type, values (| separated; bool and enum), has range, min, max (int and float)\n${gt4gs_allow_rows}" @ONLY)
target_include_directories(pcsx2-gt4gs PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
