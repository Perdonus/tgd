cmake_minimum_required(VERSION 3.24)

project(TgdPluginCatalog LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC OFF)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

if (MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets)

set(TGD_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/SourceFiles")
if (NOT EXISTS "${TGD_SOURCE_ROOT}/plugins/plugins_api.h")
    message(FATAL_ERROR "Telegram SourceFiles/plugins/plugins_api.h was not found.")
endif()

set(TGD_INCLUDE_DIRS
    "${TGD_SOURCE_ROOT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/lib_base"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/lib_crl"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/lib_rpl"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/lib_tl"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/lib_ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/lib_storage"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/lib_spellcheck"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/lib_lottie"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/lib_qr"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/lib_webview"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/ThirdParty/GSL/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/ThirdParty/expected/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/ThirdParty/range-v3/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Telegram/ThirdParty/xxHash"
)

set(TGD_PLUGIN_TARGETS)

function(add_catalog_plugin source_path)
    get_filename_component(abs_source "${source_path}" ABSOLUTE)
    get_filename_component(output_dir "${abs_source}" DIRECTORY)
    get_filename_component(output_name "${abs_source}" NAME_WE)
    file(READ "${abs_source}" source_contents)
    string(FIND "${source_contents}" "QtWidgets/" has_qtwidgets)
    string(FIND "${source_contents}" "QApplication" has_qapplication)
    file(RELATIVE_PATH rel_path "${CMAKE_CURRENT_SOURCE_DIR}" "${abs_source}")
    string(REPLACE "/" "_" target_suffix "${rel_path}")
    string(REPLACE "\\" "_" target_suffix "${target_suffix}")
    string(MAKE_C_IDENTIFIER "${target_suffix}" target_suffix)
    set(target_name "CatalogPlugin_${target_suffix}")
    set(needs_widgets FALSE)
    if (NOT has_qtwidgets EQUAL -1 OR NOT has_qapplication EQUAL -1)
        set(needs_widgets TRUE)
    endif()

    add_library(${target_name} MODULE "${abs_source}")
    target_compile_definitions(${target_name} PRIVATE QT_NO_KEYWORDS)
    target_include_directories(${target_name} PRIVATE ${TGD_INCLUDE_DIRS})
    target_link_libraries(${target_name} PRIVATE Qt${QT_VERSION_MAJOR}::Core)
    if (needs_widgets)
        target_link_libraries(${target_name} PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
        if (COMMAND qt_import_plugins)
            qt_import_plugins(
                ${target_name}
                EXCLUDE_BY_TYPE platforms imageformats styles iconengines)
        elseif (COMMAND qt5_import_plugins)
            qt5_import_plugins(
                ${target_name}
                EXCLUDE_BY_TYPE platforms imageformats styles iconengines)
        endif()
    endif()
    set_target_properties(${target_name} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX ""
        SUFFIX ".tgd"
        LIBRARY_OUTPUT_DIRECTORY "${output_dir}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${output_dir}"
        RUNTIME_OUTPUT_DIRECTORY "${output_dir}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${output_dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/import-libs"
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/import-libs"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/pdb"
        PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/pdb"
    )
    list(APPEND TGD_PLUGIN_TARGETS ${target_name})
    set(TGD_PLUGIN_TARGETS "${TGD_PLUGIN_TARGETS}" PARENT_SCOPE)
endfunction()

set(catalog_sources)
file(GLOB plugin_dirs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")

foreach(plugin_dir IN LISTS plugin_dirs)
    if (NOT IS_DIRECTORY "${plugin_dir}")
        continue()
    endif()

    file(GLOB version_entries RELATIVE "${plugin_dir}" CONFIGURE_DEPENDS "${plugin_dir}/*")
    set(version_dirs)
    foreach(version_entry IN LISTS version_entries)
        if (IS_DIRECTORY "${plugin_dir}/${version_entry}")
            list(APPEND version_dirs "${version_entry}")
        endif()
    endforeach()

    if (NOT version_dirs)
        continue()
    endif()

    list(SORT version_dirs COMPARE NATURAL ORDER ASCENDING)
    list(GET version_dirs -1 latest_version)
    file(GLOB latest_sources CONFIGURE_DEPENDS "${plugin_dir}/${latest_version}/*.cpp")
    list(SORT latest_sources)
    list(APPEND catalog_sources ${latest_sources})
endforeach()

list(REMOVE_DUPLICATES catalog_sources)

foreach(source_path IN LISTS catalog_sources)
    add_catalog_plugin("${source_path}")
endforeach()

add_custom_target(plugin_catalog ALL DEPENDS ${TGD_PLUGIN_TARGETS})
