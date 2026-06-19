include_guard(GLOBAL)

# Embedded RenderDoc capture build bridge.
#
# By default this does NOT rebuild RenderDoc from source - it stages an existing
# renderdoc.dll beside UEVRBackend.dll and builds the suspended launcher + smoke
# validator. Set UEVR_RENDERDOC_ALWAYS_BUILD=ON to (re)build RenderDoc's own
# Visual Studio solution from UEVR_RENDERDOC_SOURCE_DIR. Every step here is
# best-effort: if RenderDoc isn't available the function logs a warning and
# returns without failing the UEVR build (the capture service late-loads a
# stock renderdoc.dll at runtime instead).

option(UEVR_BUILD_FULL_RENDERDOC "Build/stage the RenderDoc runtime alongside UEVR" ON)
option(UEVR_RENDERDOC_ALWAYS_BUILD "Run RenderDoc's MSBuild target whenever the UEVR target builds" OFF)
set(UEVR_RENDERDOC_SOURCE_DIR "E:/Github/renderdoc" CACHE PATH "Path to the full RenderDoc source checkout")
set(UEVR_RENDERDOC_CONFIGURATION "Development" CACHE STRING "RenderDoc Visual Studio configuration to build")
set_property(CACHE UEVR_RENDERDOC_CONFIGURATION PROPERTY STRINGS Debug Development Release)
set(UEVR_RENDERDOC_PLATFORM "x64" CACHE STRING "RenderDoc Visual Studio platform to build")
set_property(CACHE UEVR_RENDERDOC_PLATFORM PROPERTY STRINGS x64 Win32)
set(UEVR_RENDERDOC_DLL "" CACHE FILEPATH "Explicit renderdoc.dll to stage beside UEVRBackend.dll (optional)")

# Resolve a usable renderdoc.dll at configure time. Preference order:
#   1. UEVR_RENDERDOC_DLL cache override
#   2. the source-tree build output (custom UEVR RenderDoc with RefreshHooks)
#   3. a stock RenderDoc install
# Returns the resolved path (or "") in the named output variable.
function(_uevr_resolve_renderdoc_dll out_var)
    set(_candidates
        "${UEVR_RENDERDOC_DLL}"
        "${UEVR_RENDERDOC_SOURCE_DIR}/${UEVR_RENDERDOC_PLATFORM}/${UEVR_RENDERDOC_CONFIGURATION}/renderdoc.dll"
        "${UEVR_RENDERDOC_SOURCE_DIR}/${UEVR_RENDERDOC_PLATFORM}/Development/renderdoc.dll"
        "${UEVR_RENDERDOC_SOURCE_DIR}/${UEVR_RENDERDOC_PLATFORM}/Release/renderdoc.dll"
        "$ENV{ProgramFiles}/RenderDoc/renderdoc.dll"
        "$ENV{ProgramFiles\(x86\)}/RenderDoc/renderdoc.dll")
    foreach(_c IN LISTS _candidates)
        if(_c AND EXISTS "${_c}")
            set(${out_var} "${_c}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

function(uevr_attach_full_renderdoc target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "uevr_attach_full_renderdoc target does not exist: ${target_name}")
    endif()

    if(NOT UEVR_BUILD_FULL_RENDERDOC)
        message(STATUS "Embedded RenderDoc staging disabled for ${target_name}")
        return()
    endif()

    if(NOT WIN32)
        message(WARNING "Embedded RenderDoc bridge is Windows-only; skipping")
        return()
    endif()

    file(TO_CMAKE_PATH "${UEVR_RENDERDOC_SOURCE_DIR}" renderdoc_source_dir)
    set(renderdoc_solution "${renderdoc_source_dir}/renderdoc.sln")
    set(renderdoc_solution_target "DLL\\renderdoc")
    set(renderdoc_output "${renderdoc_source_dir}/${UEVR_RENDERDOC_PLATFORM}/${UEVR_RENDERDOC_CONFIGURATION}/renderdoc.dll")
    set(renderdoc_pdb "${renderdoc_source_dir}/${UEVR_RENDERDOC_PLATFORM}/${UEVR_RENDERDOC_CONFIGURATION}/renderdoc.pdb")

    # Optionally (re)build RenderDoc from source. Only wired up when explicitly
    # requested so the UEVR build never depends on MSBuild succeeding here.
    set(renderdoc_built_target "")
    if(UEVR_RENDERDOC_ALWAYS_BUILD AND EXISTS "${renderdoc_solution}")
        if(CMAKE_VS_MSBUILD_COMMAND)
            set(renderdoc_msbuild "${CMAKE_VS_MSBUILD_COMMAND}")
        else()
            find_program(renderdoc_msbuild MSBuild.exe
                HINTS
                    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin"
                    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin"
                    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Enterprise/MSBuild/Current/Bin"
                    "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin")
        endif()

        if(renderdoc_msbuild)
            if(NOT TARGET renderdoc_full)
                add_custom_target(renderdoc_full
                    COMMAND "${renderdoc_msbuild}" "${renderdoc_solution}"
                        /t:${renderdoc_solution_target}
                        /p:Configuration=${UEVR_RENDERDOC_CONFIGURATION}
                        /p:Platform=${UEVR_RENDERDOC_PLATFORM}
                        /m /v:minimal /clp:Summary
                    BYPRODUCTS "${renderdoc_output}" "${renderdoc_pdb}"
                    WORKING_DIRECTORY "${renderdoc_source_dir}"
                    COMMENT "Building full RenderDoc ${UEVR_RENDERDOC_CONFIGURATION}|${UEVR_RENDERDOC_PLATFORM}"
                    VERBATIM)
            endif()
            add_dependencies(${target_name} renderdoc_full)
            set(renderdoc_built_target "renderdoc_full")
        else()
            message(WARNING "UEVR_RENDERDOC_ALWAYS_BUILD=ON but MSBuild.exe was not found; using a prebuilt renderdoc.dll instead")
        endif()
    endif()

    # Prefer the source-tree app header (custom RefreshHooks export) when present.
    if(EXISTS "${renderdoc_source_dir}/renderdoc/api/app/renderdoc_app.h")
        target_compile_definitions(${target_name} PRIVATE UEVR_FULL_RENDERDOC_BUILD=1)
        target_include_directories(${target_name} BEFORE PRIVATE "${renderdoc_source_dir}/renderdoc/api/app")
    endif()

    # Suspended launcher (injects renderdoc.dll first, then UEVRBackend.dll).
    if(NOT TARGET uevr_renderdoc_launcher)
        add_executable(uevr_renderdoc_launcher
            "${CMAKE_SOURCE_DIR}/tools/renderdoc-launcher/RenderDocLauncher.cpp")
        target_compile_features(uevr_renderdoc_launcher PRIVATE cxx_std_20)
        target_compile_options(uevr_renderdoc_launcher PRIVATE /EHsc /MP)
        set_target_properties(uevr_renderdoc_launcher PROPERTIES
            OUTPUT_NAME "UEVRRenderDocLauncher"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/${target_name}"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin/${target_name}"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/${target_name}")
        add_dependencies(${target_name} uevr_renderdoc_launcher)
    endif()

    # Minimal D3D12 swapchain app for validating capture without a game.
    if(NOT TARGET uevr_renderdoc_smoke)
        add_executable(uevr_renderdoc_smoke
            "${CMAKE_SOURCE_DIR}/tools/renderdoc-smoke/D3D12Smoke.cpp")
        target_compile_features(uevr_renderdoc_smoke PRIVATE cxx_std_20)
        target_compile_options(uevr_renderdoc_smoke PRIVATE /EHsc /MP)
        target_link_libraries(uevr_renderdoc_smoke PRIVATE d3d12 dxgi)
        set_target_properties(uevr_renderdoc_smoke PROPERTIES
            OUTPUT_NAME "UEVRRenderDocSmoke"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/${target_name}"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin/${target_name}"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/${target_name}")
        add_dependencies(${target_name} uevr_renderdoc_smoke)
    endif()

    # Stage renderdoc.dll (+ pdb if available) beside the backend. When
    # ALWAYS_BUILD wired a build target, copy its fresh output; otherwise resolve
    # an existing renderdoc.dll at configure time.
    if(renderdoc_built_target)
        set(staged_dll "${renderdoc_output}")
        set(staged_pdb "${renderdoc_pdb}")
    else()
        _uevr_resolve_renderdoc_dll(staged_dll)
        set(staged_pdb "")
        if(staged_dll)
            get_filename_component(_staged_dir "${staged_dll}" DIRECTORY)
            if(EXISTS "${_staged_dir}/renderdoc.pdb")
                set(staged_pdb "${_staged_dir}/renderdoc.pdb")
            endif()
        endif()
    endif()

    if(NOT staged_dll)
        message(WARNING "No renderdoc.dll found to stage beside ${target_name}. "
            "Embedded capture will rely on a stock RenderDoc install or "
            "UEVR_RENDERDOC_DLL at runtime. Set UEVR_RENDERDOC_DLL or install RenderDoc.")
        return()
    endif()

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${staged_dll}" "$<TARGET_FILE_DIR:${target_name}>/renderdoc.dll"
        COMMENT "Staging RenderDoc runtime beside $<TARGET_FILE_NAME:${target_name}> (${staged_dll})"
        VERBATIM)
    if(staged_pdb)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${staged_pdb}" "$<TARGET_FILE_DIR:${target_name}>/renderdoc.pdb"
            VERBATIM)
    endif()

    message(STATUS "Embedded RenderDoc attached to ${target_name}; staging ${staged_dll}")
endfunction()
