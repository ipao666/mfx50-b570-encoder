cmake_minimum_required(VERSION 3.16)
project(mfx50_encoder LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wextra")
set(MFX50RT_PACKAGE_VERSION 0.5.0)
option(MFX50RT_BUILD_FFMPEG_DEMOS "Build RTSP/UDP demos with FFmpeg" OFF)

add_library(mfx50_policy SHARED
    src/policy/mfx50_policy.cpp
)
target_include_directories(mfx50_policy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(mfx50_policy PRIVATE MFX50_POLICY_DLL_BUILD)
set_target_properties(mfx50_policy PROPERTIES
    OUTPUT_NAME "mfx50_policy"
    VERSION 1.0.0
    SOVERSION 1
)

add_executable(policy_decision_demo examples/policy_decision_demo.c)
target_link_libraries(policy_decision_demo PRIVATE mfx50_policy)
target_include_directories(policy_decision_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(MFX50RT_HYBRIDTSRQ_SOURCES
    src/api/mfx50rt_api.cpp
    src/algo/hybridtsrq/fast_analyzer.cpp
    src/algo/hybridtsrq/heavy_roi_analyzer.cpp
    src/algo/hybridtsrq/quality_guard.cpp
    src/algo/hybridtsrq/tuning_profile.cpp
    src/algo/hybridtsrq/temporal_qp_controller.cpp
    src/algo/hybridtsrq/static_reuse_gate.cpp
    src/algo/hybridtsrq/qp_map.cpp
    src/algo/hybridtsrq/spatial_qp_controller.cpp
    src/algo/hybridtsrq/decision_trace.cpp
    src/algo/hybridtsrq/hybridtsrq_controller.cpp
    src/backend/onevpl/onevpl_backend.cpp
    src/backend/onevpl/onevpl_realtime_adapter.cpp
    src/backend/onevpl/onevpl_mbqp_adapter.cpp
    src/backend/onevpl/onevpl_roi_adapter.cpp
)

add_library(mfx50rt SHARED ${MFX50RT_HYBRIDTSRQ_SOURCES})
target_include_directories(mfx50rt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(mfx50rt PRIVATE pthread ${CMAKE_DL_LIBS})
set_target_properties(mfx50rt PROPERTIES
    OUTPUT_NAME "mfx50rt"
    VERSION 0.6.0
    SOVERSION 0
)

add_library(mfx50_encoder SHARED
    mfx50_encoder.cpp
    mfx50_codec_api.cpp
)
target_include_directories(mfx50_encoder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
set_target_properties(mfx50_encoder PROPERTIES
    OUTPUT_NAME "mfx50_encoder"
    VERSION 0.1.0
    SOVERSION 0
)

add_executable(demo_mfx50_benchmark demo_mfx50_benchmark.cpp)
target_link_libraries(demo_mfx50_benchmark PRIVATE mfx50_encoder)
target_include_directories(demo_mfx50_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(mfx50_encoder_minimal_demo examples/mfx50_encoder_minimal_demo.cpp)
target_link_libraries(mfx50_encoder_minimal_demo PRIVATE mfx50_encoder)
target_include_directories(mfx50_encoder_minimal_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(simple_transcode examples/simple_transcode.c)
target_link_libraries(simple_transcode PRIVATE mfx50rt)
target_include_directories(simple_transcode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(create_from_json examples/create_from_json.cpp)
target_link_libraries(create_from_json PRIVATE mfx50rt)
target_include_directories(create_from_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(query_capabilities examples/query_capabilities.cpp)
target_link_libraries(query_capabilities PRIVATE mfx50rt)
target_include_directories(query_capabilities PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(decision_trace_demo examples/decision_trace_demo.cpp)
target_link_libraries(decision_trace_demo PRIVATE mfx50rt)
target_include_directories(decision_trace_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(async_45_routes examples/async_45_routes.cpp)
target_link_libraries(async_45_routes PRIVATE mfx50rt)
target_include_directories(async_45_routes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(bench_real_45_files examples/bench_real_45_files.cpp)
target_link_libraries(bench_real_45_files PRIVATE mfx50rt)
target_include_directories(bench_real_45_files PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_library(VPL_LIBRARY vpl)
if(NOT VPL_LIBRARY)
    find_library(VPL_LIBRARY mfx-gen)
endif()
if(NOT VPL_LIBRARY)
    message(FATAL_ERROR "oneVPL library not found (tried: vpl, mfx-gen)")
endif()
find_library(VA_LIBRARY va)
find_library(VA_DRM_LIBRARY va-drm)
if(NOT VA_LIBRARY OR NOT VA_DRM_LIBRARY)
    message(FATAL_ERROR "libva/libva-drm not found")
endif()
target_link_libraries(mfx50_encoder PRIVATE
    ${VPL_LIBRARY}
    ${VA_LIBRARY}
    ${VA_DRM_LIBRARY}
    pthread
)
target_link_libraries(mfx50rt PRIVATE ${VPL_LIBRARY} ${VA_LIBRARY} ${VA_DRM_LIBRARY})

add_library(mfx50_realtime SHARED
    mfx50_realtime.cpp
    src/algo/mfx50_algo_config.cpp
    src/algo/mfx50_preprocess.cpp
    src/algo/mfx50_scene_analyzer.cpp
    src/algo/mfx50_adaptive_decision.cpp
    src/algo/mfx50_mbqp_builder.cpp
    src/core/mfx50_output_queue.cpp
)
target_include_directories(mfx50_realtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/algo
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)
target_link_libraries(mfx50_realtime PRIVATE ${VPL_LIBRARY} ${VA_LIBRARY} ${VA_DRM_LIBRARY} pthread dl)
set_target_properties(mfx50_realtime PROPERTIES
    OUTPUT_NAME "mfx50_realtime"
    VERSION ${MFX50RT_PACKAGE_VERSION}
    SOVERSION 0
)
add_dependencies(mfx50rt mfx50_realtime)

add_executable(demo_mfx50_realtime_file demo_mfx50_realtime_file.cpp)
target_link_libraries(demo_mfx50_realtime_file PRIVATE mfx50_realtime)
target_include_directories(demo_mfx50_realtime_file PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(mfx50rt_stream_probe tools/mfx50rt_stream_probe.cpp)
target_link_libraries(mfx50rt_stream_probe PRIVATE mfx50_realtime)
target_include_directories(mfx50rt_stream_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(mfx50_split_file_transcode tools/mfx50_split_file_transcode.cpp)
target_link_libraries(mfx50_split_file_transcode PRIVATE mfx50_encoder)
target_include_directories(mfx50_split_file_transcode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(mfx50rt_api_probe tools/mfx50rt_api_probe.cpp)
target_link_libraries(mfx50rt_api_probe PRIVATE mfx50_realtime)
target_include_directories(mfx50rt_api_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(mfx50rt_mbqp_probe tools/mfx50rt_mbqp_probe.cpp)
target_link_libraries(mfx50rt_mbqp_probe PRIVATE ${VPL_LIBRARY} ${VA_LIBRARY} ${VA_DRM_LIBRARY} pthread dl)
target_include_directories(mfx50rt_mbqp_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MFX50RT_BUILD_FFMPEG_DEMOS)
    find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h)
    find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
    find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h)
    find_library(AVFORMAT_LIBRARY avformat)
    find_library(AVCODEC_LIBRARY avcodec)
    find_library(AVUTIL_LIBRARY avutil)
    if(NOT AVFORMAT_INCLUDE_DIR OR NOT AVCODEC_INCLUDE_DIR OR NOT AVUTIL_INCLUDE_DIR OR
       NOT AVFORMAT_LIBRARY OR NOT AVCODEC_LIBRARY OR NOT AVUTIL_LIBRARY)
        message(FATAL_ERROR "FFmpeg development files not found; set MFX50RT_BUILD_FFMPEG_DEMOS=OFF or install libavformat/libavcodec/libavutil")
    endif()

    add_executable(demo_mfx50_realtime_rtsp demo/demo_mfx50_realtime_rtsp.cpp)
    target_include_directories(demo_mfx50_realtime_rtsp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${AVFORMAT_INCLUDE_DIR}
        ${AVCODEC_INCLUDE_DIR}
        ${AVUTIL_INCLUDE_DIR}
    )
    target_link_libraries(demo_mfx50_realtime_rtsp PRIVATE
        mfx50_realtime
        ${AVFORMAT_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
    )
endif()

configure_file(
    packaging/mfx50_realtime.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/mfx50_realtime.pc
    @ONLY
)

install(TARGETS mfx50_policy mfx50_encoder mfx50_realtime mfx50rt
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES
    include/mfx50_types.h
    include/mfx50_policy.h
    mfx50_api.h
    mfx50_device.h
    mfx50_surface.h
    mfx50_decoder.h
    mfx50_encoder.h
    mfx50_transcoder.h
    mfx50_realtime.h
    include/mfx50rt.h
    include/mfx50_realtime_algo.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/mfx50_realtime.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
install(TARGETS demo_mfx50_realtime_file mfx50rt_stream_probe mfx50rt_api_probe mfx50rt_mbqp_probe
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(TARGETS policy_decision_demo simple_transcode create_from_json query_capabilities decision_trace_demo async_45_routes bench_real_45_files mfx50_encoder_minimal_demo mfx50_split_file_transcode
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if(TARGET demo_mfx50_realtime_rtsp)
    install(TARGETS demo_mfx50_realtime_rtsp
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

enable_testing()

add_executable(test_policy_api tests/test_policy_api.c)
target_link_libraries(test_policy_api PRIVATE mfx50_policy)
target_include_directories(test_policy_api PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME policy_api COMMAND test_policy_api)

add_executable(test_public_header tests/test_public_header.c)
target_link_libraries(test_public_header PRIVATE mfx50rt)
target_include_directories(test_public_header PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME public_header_c COMMAND test_public_header)

add_executable(test_split_codec_headers tests/test_split_codec_headers.c)
target_link_libraries(test_split_codec_headers PRIVATE mfx50_encoder)
target_include_directories(test_split_codec_headers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME split_codec_headers COMMAND test_split_codec_headers)

add_executable(test_split_encoder_surface_shape tests/test_split_encoder_surface_shape.cpp)
target_link_libraries(test_split_encoder_surface_shape PRIVATE mfx50_encoder)
target_include_directories(test_split_encoder_surface_shape PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME split_encoder_surface_shape COMMAND test_split_encoder_surface_shape)

add_executable(test_transcoder_input_codecs tests/test_transcoder_input_codecs.cpp)
target_link_libraries(test_transcoder_input_codecs PRIVATE mfx50_encoder)
target_include_directories(test_transcoder_input_codecs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME transcoder_input_codecs COMMAND test_transcoder_input_codecs)


add_executable(test_qp_map
    tests/test_qp_map.cpp
    src/algo/hybridtsrq/qp_map.cpp
)
target_include_directories(test_qp_map PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME qp_map COMMAND test_qp_map)

add_executable(test_onevpl_mbqp_adapter
    tests/test_onevpl_mbqp_adapter.cpp
    src/backend/onevpl/onevpl_mbqp_adapter.cpp
)
target_include_directories(test_onevpl_mbqp_adapter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME onevpl_mbqp_adapter COMMAND test_onevpl_mbqp_adapter)

add_executable(test_hybridtsrq tests/test_hybridtsrq.cpp)
target_link_libraries(test_hybridtsrq PRIVATE mfx50rt)
target_include_directories(test_hybridtsrq PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME hybridtsrq_api COMMAND test_hybridtsrq)

add_executable(test_hybridtsrq_tuning tests/test_hybridtsrq_tuning.cpp)
target_link_libraries(test_hybridtsrq_tuning PRIVATE mfx50rt)
target_include_directories(test_hybridtsrq_tuning PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME hybridtsrq_tuning COMMAND test_hybridtsrq_tuning)

add_executable(test_heavy_roi_analyzer
    tests/test_heavy_roi_analyzer.cpp
    src/algo/hybridtsrq/heavy_roi_analyzer.cpp
)
target_include_directories(test_heavy_roi_analyzer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME heavy_roi_analyzer COMMAND test_heavy_roi_analyzer)

add_executable(test_quality_guard tests/test_quality_guard.cpp)
target_link_libraries(test_quality_guard PRIVATE mfx50rt)
target_include_directories(test_quality_guard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME quality_guard COMMAND test_quality_guard)

add_executable(test_static_reuse_gate tests/test_static_reuse_gate.cpp)
target_link_libraries(test_static_reuse_gate PRIVATE mfx50rt)
target_include_directories(test_static_reuse_gate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME static_reuse_gate COMMAND test_static_reuse_gate)

add_executable(test_mfx50_preprocess
    tests/test_mfx50_preprocess.cpp
    src/algo/mfx50_preprocess.cpp
)
target_include_directories(test_mfx50_preprocess PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_test(NAME mfx50_preprocess COMMAND test_mfx50_preprocess)
