cmake_minimum_required(VERSION 3.24)

project(TgdPluginCatalog LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

set(TGD_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/SourceFiles")
if (NOT EXISTS "${TGD_SOURCE_ROOT}/plugins/plugins_api.h")
    message(FATAL_ERROR "Telegram SourceFiles/plugins/plugins_api.h was not found.")
endif()

set(TGD_PLUGIN_TARGETS)

function(add_catalog_plugin source_path)
    get_filename_component(abs_source "${source_path}" ABSOLUTE)
    get_filename_component(output_dir "${abs_source}" DIRECTORY)
    get_filename_component(output_name "${abs_source}" NAME_WE)
    file(RELATIVE_PATH rel_path "${CMAKE_CURRENT_SOURCE_DIR}" "${abs_source}")
    string(REPLACE "/" "_" target_suffix "${rel_path}")
    string(REPLACE "\\" "_" target_suffix "${target_suffix}")
    string(MAKE_C_IDENTIFIER "${target_suffix}" target_suffix)
    set(target_name "CatalogPlugin_${target_suffix}")

    add_library(${target_name} MODULE "${abs_source}")
    target_compile_definitions(${target_name} PRIVATE QT_NO_KEYWORDS)
    target_include_directories(${target_name} PRIVATE "${TGD_SOURCE_ROOT}")
    target_link_libraries(${target_name} PRIVATE Qt6::Core Qt6::Widgets)
    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX ""
        SUFFIX ".tgd"
        LIBRARY_OUTPUT_DIRECTORY "${output_dir}"
        RUNTIME_OUTPUT_DIRECTORY "${output_dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/import-libs"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb"
    )
    list(APPEND TGD_PLUGIN_TARGETS ${target_name})
    set(TGD_PLUGIN_TARGETS "${TGD_PLUGIN_TARGETS}" PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE catalog_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*/*.cpp")
list(SORT catalog_sources)

foreach(source_path IN LISTS catalog_sources)
    add_catalog_plugin("${source_path}")
endforeach()

add_custom_target(plugin_catalog ALL DEPENDS ${TGD_PLUGIN_TARGETS})
