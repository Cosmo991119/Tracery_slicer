get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests 
	${_TEST_NAME}_tests.cpp
	test_3mf.cpp
	test_aabbindirect.cpp
	test_clipper_offset.cpp
	test_clipper_utils.cpp
	test_config.cpp
	test_custom_gcode.cpp
	test_elephant_foot_compensation.cpp
	test_geometry.cpp
	test_mixed_filament.cpp
	test_mixed_filament_color_golden.cpp
	test_triangle_selector.cpp
	test_tracery_partition.cpp
	test_tracery_material_plan.cpp
	test_tracery_plate_materials.cpp
	test_local_z_order_optimizer.cpp
	test_placeholder_parser.cpp
	test_polygon.cpp
	test_profile_load_util.cpp
	test_mutable_polygon.cpp
	test_mutable_priority_queue.cpp
	test_stl.cpp
	test_meshboolean.cpp
	test_marchingsquares.cpp
	test_timeutils.cpp
	test_voronoi.cpp
    test_optimizers.cpp
    test_png_io.cpp
	test_sswcp_protocol.cpp
    test_indexed_triangle_set.cpp
	TestMD5.cpp
    ../libnest2d/printer_parts.cpp
	)

if (TARGET OpenVDB::openvdb)
    target_sources(${_TEST_NAME}_tests PRIVATE test_hollowing.cpp)
    # libslic3r links OpenVDB PRIVATE, so its symbols don't propagate to test
    # executables. test_hollowing.cpp references OpenVDB directly and must link
    # it explicitly here.
    target_link_libraries(${_TEST_NAME}_tests PRIVATE OpenVDB::openvdb)
endif()
    
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(libnoise REQUIRED)
find_package(Qhull 7.2 QUIET)
if(UNIX AND NOT APPLE)
    target_link_options(${_TEST_NAME}_tests PRIVATE "-Wl,--no-as-needed")
endif()
target_link_libraries(${_TEST_NAME}_tests PRIVATE ${TEST_LINK_LIBS})
if (WIN32)
    target_link_libraries(${_TEST_NAME}_tests PRIVATE bcrypt.lib crypt32.lib)
endif()
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

# Keep the geometry regression runnable independently of unrelated test suites.
add_executable(tracery_partition_tests test_tracery_partition.cpp)
target_link_libraries(tracery_partition_tests PRIVATE ${TEST_LINK_LIBS})
if (WIN32)
    target_link_libraries(tracery_partition_tests PRIVATE bcrypt.lib crypt32.lib)
endif()
add_test(NAME tracery_partition COMMAND tracery_partition_tests "[TraceryPartition]")

add_executable(tracery_material_tests test_tracery_material_plan.cpp)
target_include_directories(tracery_material_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(tracery_material_tests PRIVATE Catch2::Catch2WithMain)
add_test(NAME tracery_materials COMMAND tracery_material_tests "[TraceryMaterials]")

if (WIN32)
	if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
	Snapmaker_Orca_copy_dlls(COPY_DLLS "Debug" "d" output_dlls_Debug)
	elseif("${CMAKE_BUILD_TYPE}" STREQUAL "RelWithDebInfo")
	Snapmaker_Orca_copy_dlls(COPY_DLLS "RelWithDebInfo" "" output_dlls_Release)
	else()
	Snapmaker_Orca_copy_dlls(COPY_DLLS "Release" "" output_dlls_Release)
	endif()
endif()

catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: " EXTRA_ARGS ${CATCH_EXTRA_ARGS})
# add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests ${CATCH_EXTRA_ARGS})

add_executable(tracery_plate_tests test_tracery_plate_materials.cpp)
target_link_libraries(tracery_plate_tests PRIVATE ${TEST_LINK_LIBS})
if (WIN32)
    target_link_libraries(tracery_plate_tests PRIVATE bcrypt.lib crypt32.lib)
endif()
add_test(NAME tracery_plate COMMAND tracery_plate_tests "[TraceryPlate]")

# Explicit performance diagnostics: real production paths, no wall-time pass/fail
# thresholds. Keep separate from the general suite and its unrelated fixtures.
if(TARGET libslic3r_gui)
    add_executable(tracery_performance_tests
        test_tracery_performance.cpp
        ${PROJECT_SOURCE_DIR}/src/sentry_wrapper/SentryWrapper.cpp
        ${PROJECT_SOURCE_DIR}/src/bury_cfg/bury_point.cpp
        ${PROJECT_SOURCE_DIR}/src/common_func/common_func.cpp)
    # wxWidgets is discovered in src/, whose directory settings do not reach tests/.
    get_directory_property(_tracery_wx_includes DIRECTORY "${PROJECT_SOURCE_DIR}/src" DEFINITION wxWidgets_INCLUDE_DIRS)
    get_directory_property(_tracery_wx_definitions DIRECTORY "${PROJECT_SOURCE_DIR}/src" DEFINITION wxWidgets_DEFINITIONS)
    target_include_directories(tracery_performance_tests SYSTEM PRIVATE ${_tracery_wx_includes})
    target_compile_definitions(tracery_performance_tests PRIVATE ${_tracery_wx_definitions})
    # The GUI already provides NanoSVG's implementation via BitmapCache.cpp.
    set(_tracery_perf_libraries ${TEST_LINK_LIBS})
    list(REMOVE_ITEM _tracery_perf_libraries test_nanosvg_impl)
    target_link_libraries(tracery_performance_tests PRIVATE libslic3r_gui ${_tracery_perf_libraries})
    if(WIN32)
        target_link_libraries(tracery_performance_tests PRIVATE bcrypt.lib crypt32.lib)
    endif()
    add_test(NAME tracery_performance COMMAND tracery_performance_tests "[TraceryPerformance]")
    set_tests_properties(tracery_performance PROPERTIES LABELS "performance" RUN_SERIAL TRUE TIMEOUT 180)
endif()

add_executable(tracery_mapping_tests test_tracery_region_mapping.cpp)
target_link_libraries(tracery_mapping_tests PRIVATE ${TEST_LINK_LIBS})
if(WIN32)
    target_link_libraries(tracery_mapping_tests PRIVATE bcrypt.lib crypt32.lib)
endif()
add_test(NAME tracery_mapping COMMAND tracery_mapping_tests "[TraceryMapping]")
add_test(NAME tracery_mapping_performance COMMAND tracery_mapping_tests "[TraceryMappingPerformance]")
set_tests_properties(tracery_mapping_performance PROPERTIES LABELS "performance" RUN_SERIAL TRUE TIMEOUT 180)

add_executable(tracery_document_tests test_tracery_document.cpp test_tracery_document_window.cpp)
target_link_libraries(tracery_document_tests PRIVATE ${TEST_LINK_LIBS})
if(WIN32)
    target_link_libraries(tracery_document_tests PRIVATE bcrypt.lib crypt32.lib)
endif()
add_test(NAME tracery_document COMMAND tracery_document_tests "[TraceryDocument]")

add_test(NAME tracery_vector COMMAND tracery_document_tests "[TraceryVector]")
