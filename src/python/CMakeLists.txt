# Copyright (C) 2018-2025 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
#

include(FetchContent)

FetchContent_Declare(
    pybind11
    URL https://github.com/pybind/pybind11/archive/refs/tags/v3.0.0.tar.gz
    URL_HASH SHA256=453b1a3e2b266c3ae9da872411cadb6d693ac18063bd73226d96cfb7015a200c
)
FetchContent_GetProperties(pybind11)
# search for FindPython3.cmake instead of legacy modules
set(PYBIND11_FINDPYTHON ON)

if(NOT pybind11_POPULATED)
    FetchContent_Populate(pybind11)
    add_subdirectory(${pybind11_SOURCE_DIR} ${pybind11_BINARY_DIR})
endif()

file(GLOB python_sources "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

set(TARGET_NAME py_openvino_genai)
pybind11_add_module(${TARGET_NAME} ${python_sources})
if(NOT ENABLE_GIL_PYTHON_API)
    # disable GIL for free-threaded python build
    target_compile_definitions(${TARGET_NAME} PRIVATE Py_GIL_DISABLED=1)
endif()

target_link_libraries(${TARGET_NAME} PRIVATE openvino::genai)
target_include_directories(${TARGET_NAME} PRIVATE "${OpenVINOGenAI_SOURCE_DIR}/src/cpp/src") # for tokenizer/tokenizers_path.hpp
set_target_properties(${TARGET_NAME} PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/openvino_genai/>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/openvino_genai/>"
)
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/openvino_genai/__init__.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/openvino_genai/__init__.pyi"
          "${CMAKE_CURRENT_SOURCE_DIR}/openvino_genai/py_openvino_genai.pyi"
     DESTINATION "${CMAKE_BINARY_DIR}/openvino_genai/")

if(OpenVINODeveloperPackage_FOUND)
    # TODO: commit changes separately
    # ov_add_clang_format_target(${TARGET_NAME}_clang FOR_TARGETS ${TARGET_NAME})
endif()

# setting RPATH / LC_RPATH depending on platform
if(LINUX)
    # to find libopenvino_genai.so in the same folder
    set(py_rpaths "$ORIGIN")
elseif(APPLE)
    # to find libopenvino_genai.dylib in the same folder
    set(py_rpaths "@loader_path")
    if(DEFINED PY_BUILD_CMAKE_PACKAGE_NAME)
        # in case we build pip package, we need to refer to libopenvino.dylib from 'openvino' package
        list(APPEND py_rpaths "@loader_path/../openvino/libs")
    endif()
endif()

if(py_rpaths)
    set_target_properties(${TARGET_NAME} PROPERTIES INSTALL_RPATH "${py_rpaths}")
endif()

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/openvino_genai/__init__.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/openvino_genai/__init__.pyi"
              "${CMAKE_CURRENT_SOURCE_DIR}/openvino_genai/py_openvino_genai.pyi"
        DESTINATION python/openvino_genai
        COMPONENT pygenai_${Python3_VERSION_MAJOR}_${Python3_VERSION_MINOR})
install(TARGETS ${TARGET_NAME}
        LIBRARY DESTINATION python/openvino_genai
        COMPONENT pygenai_${Python3_VERSION_MAJOR}_${Python3_VERSION_MINOR})

install(FILES "${OpenVINOGenAI_SOURCE_DIR}/LICENSE"
              "${OpenVINOGenAI_SOURCE_DIR}/third-party-programs.txt"
              "${OpenVINOGenAI_SOURCE_DIR}/SECURITY.md"
        DESTINATION "${PY_BUILD_CMAKE_PACKAGE_NAME}-${PY_BUILD_CMAKE_PACKAGE_VERSION}.dist-info"
        COMPONENT wheel_genai
        EXCLUDE_FROM_ALL)

# wheel_genai component is used for wheel generation in pyproject.toml.
# Exclude wheel_genai from normal packaging because there's pygenai_X_Y component for that.
install(TARGETS openvino_genai ${TARGET_NAME}
        LIBRARY DESTINATION openvino_genai COMPONENT wheel_genai EXCLUDE_FROM_ALL
        RUNTIME DESTINATION openvino_genai COMPONENT wheel_genai EXCLUDE_FROM_ALL)

# Generate or check .pyi stub files generated by pybind11-stub

set(pyproject_toml "${OpenVINOGenAI_SOURCE_DIR}/pyproject.toml")
file(STRINGS ${pyproject_toml} pybind11_stubgen_dep REGEX "pybind11-stubgen")

if(pybind11_stubgen_dep MATCHES "pybind11-stubgen==[0-9\.]+")
    set(pybind11_stubgen_dep "${CMAKE_MATCH_0}")
else()
    message(FATAL_ERROR "Internal error: failed to parse pybind11-stubgen version from from '${pyproject_toml}'")
endif()

if(OpenVINODeveloperPackage_FOUND)
    ov_check_pip_package(REQUIREMENT ${pybind11_stubgen_dep}
                         RESULT_VAR pybind11_stubgen_AVAILABLE
                         WARNING_MESSAGE "Please, install ${pybind11_stubgen_dep} if you plan to develop Python OpenVINO GenAI API"
                         MESSAGE_MODE WARNING)
elseif(DEFINED PY_BUILD_CMAKE_PACKAGE_NAME AND NOT WIN32)
    # in case of wheel build, pybind11-stubgen is always available via pyproject.toml's build-system
    # except Win32 where we have issues with pybind11_stubgen executable which cannot import its own module
    set(pybind11_stubgen_AVAILABLE ON)

    # by default, wheel build is performed with build-isolation, which means that some variables like PYTHONPATH
    # are not available. But if user called setupvars.sh, then OpenVINO dir is available, while PYTHONPATH - no.
    # In this case, we will have mismatch on Linux when OpenVINO can point on build dir / install dir, while
    # PYTHONPATH points out to locally installed tmp OpenVINO wheel (build against wheel).
    # Ways to handle it:
    # - setting PYTHONPATH to $ENV{INTEL_OPENVINO_DIR}/python if INTEL_OPENVINO_DIR is defined. It means we are building against
    #   OpenVINO archive or installation tree
    # - if it's not defined, we cannot do any guesses and hence, disable pybind11-stubgen usage
    if(DEFINED ENV{INTEL_OPENVINO_DIR})
        set(openvino_pythonpath "$ENV{INTEL_OPENVINO_DIR}/python")
    elseif(LINUX AND NOT OpenVINO_DIR STREQUAL OpenVINO_DIR_PY)
        # here we imply that OpenVINO_DIR_PY points to manylinux, while OpenVINO_DIR point to Ubuntu binaries
        set(pybind11_stubgen_AVAILABLE OFF)
    endif()
endif()

# but we also need to check whether OpenVINO is installed
if(CMAKE_CROSSCOMPILING)
    # we cannot check OpenVINO during cross-compile
    set(pybind11_stubgen_AVAILABLE OFF)
else()
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import openvino"
        RESULT_VARIABLE EXIT_CODE
        OUTPUT_VARIABLE OUTPUT_TEXT
        ERROR_VARIABLE ERROR_TEXT)

    # OpenVINO is not available because of import error
    if(NOT EXIT_CODE EQUAL 0)
        set(pybind11_stubgen_AVAILABLE OFF)
    endif()
endif()

if(pybind11_stubgen_AVAILABLE)
    if(DEFINED ENV{CI} OR DEFINED ENV{TF_BUILD} OR DEFINED ENV{JENKINS_URL})
        set(ci_run ON)
    endif()

    set(stub_files_location "${OpenVINOGenAI_BINARY_DIR}/src/python")
    set(init_pyi_file "${stub_files_location}/openvino_genai/__init__.pyi")
    set(generated_files ${init_pyi_file}
                        ${stub_files_location}/openvino_genai/py_openvino_genai.pyi)
    set_source_files_properties(${generated_files} PROPERTIES GENERATED ON)

    if(COMMAND find_host_program)
        find_host_program(pybind11_stubgen NAMES pybind11-stubgen NO_CACHE REQUIRED)
    else()
        find_program(pybind11_stubgen NAMES pybind11-stubgen NO_CACHE REQUIRED)
    endif()

    if(ci_run)
        set(validation_command
            COMMAND "${CMAKE_COMMAND}"
                -D generated_pyi_files_location=${stub_files_location}
                -D source_pyi_files_location=${CMAKE_CURRENT_SOURCE_DIR}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/compare_pyi.cmake")
        set(validation_dependencies
            "${CMAKE_CURRENT_SOURCE_DIR}/openvino_genai/__init__.pyi"
            "${CMAKE_CURRENT_SOURCE_DIR}/openvino_genai/py_openvino_genai.pyi")
    else()
        set(copy_to_source_command
            COMMAND "${CMAKE_COMMAND}" -E copy ${generated_files} "${CMAKE_CURRENT_SOURCE_DIR}/openvino_genai/")
    endif()

    set(output_file pybind11_stub_gen_completed.txt)
    add_custom_command(OUTPUT ${output_file}
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${CMAKE_BINARY_DIR}/openvino_genai/__init__.pyi"
                                            "${CMAKE_BINARY_DIR}/openvino_genai/py_openvino_genai.pyi"
        COMMAND "${CMAKE_COMMAND}" -E env PYTHONPATH=${CMAKE_BINARY_DIR}:${openvino_pythonpath}:$ENV{PYTHONPATH}
                ${pybind11_stubgen} --output-dir ${stub_files_location} openvino_genai
        COMMAND "${CMAKE_COMMAND}"
                -D init_pyi_file=${init_pyi_file}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/clean_version.cmake"
        COMMAND "${CMAKE_COMMAND}"
                -D init_pyi_file=${init_pyi_file}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/remove_abi_specific_info.cmake"
        ${validation_command}
        ${copy_to_source_command}
        COMMAND "${CMAKE_COMMAND}" -E copy ${generated_files} "${CMAKE_BINARY_DIR}/openvino_genai/"
        COMMAND "${CMAKE_COMMAND}" -E touch ${output_file}
        DEPENDS
            ${python_sources}
            ${validation_dependencies}
            "${CMAKE_CURRENT_SOURCE_DIR}/openvino_genai/__init__.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/clean_version.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/remove_abi_specific_info.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/compare_pyi.cmake"
        COMMENT "[${pybind11_stubgen_dep}] Generate .pyi files"
        VERBATIM)

    add_custom_target(${TARGET_NAME}_stub ALL DEPENDS ${output_file})
elseif(OpenVINODeveloperPackage_FOUND)
    # Produce warning message at build time as well
    add_custom_command(OUTPUT pybind11_stub_gen_not_found.txt
        COMMAND ${CMAKE_COMMAND}
            -E cmake_echo_color --red "Warning: Please, install ${pybind11_stubgen_dep}")
    add_custom_target(${TARGET_NAME}_stub ALL DEPENDS pybind11_stub_gen_not_found.txt)
else()
    add_custom_target(${TARGET_NAME}_stub ALL)
endif()

add_dependencies(${TARGET_NAME}_stub ${TARGET_NAME})
