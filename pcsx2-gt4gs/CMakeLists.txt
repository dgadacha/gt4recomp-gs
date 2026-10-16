# GT4Recomp fork: gt4-gs.exe, the GT4Recomp GS process (see gt4gs/README.md and gt4gs/BUILD.md).
# Based on pcsx2-gsrunner; adds the live GS stream consumer and the .gscap replay.

# The vendored MIT transport uses C++ exceptions; PCSX2 builds without them (_HAS_EXCEPTIONS=0, no /EHsc). It is
# compiled into its own static library with exceptions enabled. StreamTransport.cpp catches every exception, so none
# crosses into PCSX2 code. The directory-wide _HAS_EXCEPTIONS=0 is removed here and given back to the executable.
get_directory_property(gt4gs_dir_defs COMPILE_DEFINITIONS)
list(FILTER gt4gs_dir_defs EXCLUDE REGEX "^_HAS_EXCEPTIONS=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${gt4gs_dir_defs}")

set(GT4GS_STREAM_DIR "${CMAKE_SOURCE_DIR}/3rdparty/gt4recomp-gs-stream")

add_library(gt4gs_stream_transport STATIC
	StreamTransport.cpp
	StreamTransport.h
	"${GT4GS_STREAM_DIR}/gs_stream_ipc.cpp"
	"${GT4GS_STREAM_DIR}/gs_stream_file.cpp"
)
target_include_directories(gt4gs_stream_transport PRIVATE "${GT4GS_STREAM_DIR}")
target_compile_features(gt4gs_stream_transport PRIVATE cxx_std_20)
target_compile_definitions(gt4gs_stream_transport PRIVATE _HAS_EXCEPTIONS=1)
if(MSVC)
	target_compile_options(gt4gs_stream_transport PRIVATE /EHsc /wd4005)
endif()
target_link_libraries(gt4gs_stream_transport PUBLIC kernel32 advapi32)

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
