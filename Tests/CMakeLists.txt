# Standalone tests for the plugin's Unreal-free cores (BasisEncoder, BasisTranscodeCore) built
# against the vendored library exactly as the UE modules compile it: the same generated wrapper
# translation units and the same configuration (Source/ThirdParty/BasisUniversalBegin.h).
#
#   cmake -S Tests -B Build/Tests -DCMAKE_BUILD_TYPE=Release
#   cmake --build Build/Tests --config Release
#   ctest --test-dir Build/Tests -C Release --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(TextureFormatBasisTests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

get_filename_component(PLUGIN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(VENDOR "${PLUGIN_ROOT}/Source/ThirdParty/basis_universal")
set(ENCODER_PRIVATE "${PLUGIN_ROOT}/Source/TextureFormatBasis/Private")
set(TRANSCODER_PRIVATE "${PLUGIN_ROOT}/Source/BasisTranscoder/Private")

file(GLOB ENCODER_WRAPPERS "${ENCODER_PRIVATE}/BasisUniversal/*.cpp" "${ENCODER_PRIVATE}/BasisUniversal/*.c")

add_executable(basis_plugin_tests
	BasisPluginTests.cpp
	"${ENCODER_PRIVATE}/BasisEncoder.cpp"
	"${TRANSCODER_PRIVATE}/BasisTranscodeCore.cpp"
	${ENCODER_WRAPPERS})

target_include_directories(basis_plugin_tests PRIVATE "${VENDOR}" "${PLUGIN_ROOT}/Source/ThirdParty" "${ENCODER_PRIVATE}" "${TRANSCODER_PRIVATE}")
# A modular build: the encoder carries its own transcoder and zstd (see BasisTranscoder.Build.cs).
target_compile_definitions(basis_plugin_tests PRIVATE BASIS_UE_ENCODER_TRANSCODER=1 BASIS_UE_ENCODER_ZSTD=1)
target_link_libraries(basis_plugin_tests PRIVATE Threads::Threads)

# BasisUniversalBegin.h enables the SSE4.1 kernels on x64, where Unreal compiles with -msse4.2.
# CMAKE_SYSTEM_PROCESSOR is the host's on macOS, so an x86_64 build from arm64 shows only in
# CMAKE_OSX_ARCHITECTURES.
if(NOT MSVC AND (CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64"
	OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")))
	target_compile_options(basis_plugin_tests PRIVATE -msse4.2)
endif()

# Close to the engine's own warnings-as-errors set, so the plugin's code is held to it here too.
# BasisUniversalBegin.h/End.h silence warnings inside the vendored sources. On MSVC, Unreal
# disables C4100, C4127, C4245, C4267 and C4389 and does not error on C4244.
if(MSVC)
	set(STRICT_WARNINGS /bigobj /W4 /WX /EHsc /we4456 /we4457 /we4458 /we4459 /we4668
		/wd4100 /wd4127 /wd4244 /wd4245 /wd4267 /wd4389)
else()
	set(STRICT_WARNINGS -Wall -Wextra -Wshadow -Wundef -Werror -Wno-unused-parameter)
endif()
target_compile_options(basis_plugin_tests PRIVATE ${STRICT_WARNINGS})

# The runtime transcoder exactly as BasisTranscoder compiles it for a game (no encoder sharing it),
# checked against payloads basis_plugin_tests writes.
add_executable(basis_runtime_tests
	RuntimeTests.cpp
	"${TRANSCODER_PRIVATE}/BasisTranscodeCore.cpp"
	"${TRANSCODER_PRIVATE}/BasisUniversal/basisu_transcoder.cpp"
	"${TRANSCODER_PRIVATE}/BasisUniversal/zstd.c")
target_include_directories(basis_runtime_tests PRIVATE "${VENDOR}" "${PLUGIN_ROOT}/Source/ThirdParty" "${TRANSCODER_PRIVATE}")
target_compile_definitions(basis_runtime_tests PRIVATE BASIS_UE_TRANSCODER_ONLY=1 BASIS_UE_ZSTD_SOURCE=1)
target_compile_options(basis_runtime_tests PRIVATE ${STRICT_WARNINGS})

enable_testing()
set(FIXTURES "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${FIXTURES}")
add_test(NAME basis_plugin_tests COMMAND basis_plugin_tests --write-fixtures "${FIXTURES}")
set_tests_properties(basis_plugin_tests PROPERTIES FIXTURES_SETUP payloads)
add_test(NAME basis_runtime_tests COMMAND basis_runtime_tests "${FIXTURES}")
set_tests_properties(basis_runtime_tests PROPERTIES FIXTURES_REQUIRED payloads)
