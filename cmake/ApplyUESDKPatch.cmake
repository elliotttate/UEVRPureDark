# ApplyUESDKPatch.cmake
#
# UESDK (praydog's Unreal SDK) is a PRIVATE submodule, so it cannot be forked
# publicly to carry local compatibility fixes. Instead we apply the fixes to
# the builder's OWN UESDK checkout at configure time. Anyone building UEVR
# already has legitimate UESDK access (it is required to build at all), so this
# republishes nothing -- these are just local working-tree patches.
#
# One fix teaches FRHITexture::get_native_resource()'s vtable probe to accept
# RenderDoc-wrapped D3D12 resources. Another makes the UE5 render-target-pool
# scanner robust enough for stripped 5.5/5.6 builds used by AFW frame-resource
# discovery.
#
# Must run BEFORE add_subdirectory(dependencies/submodules/UESDK) so the patched
# files are on disk before they are compiled. Idempotent: skips if already applied,
# warns (never fails) on upstream drift.

set(_uesdk_dir   "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/submodules/UESDK")

function(_uevr_apply_uesdk_patch _label _patch _sentinel_file _failure_hint)
    if(NOT EXISTS "${_sentinel_file}")
        message(STATUS "UESDK ${_label} patch: ${_sentinel_file} not found (submodule not checked out?) -- skipping")
        return()
    endif()

    if(NOT EXISTS "${_patch}")
        message(WARNING "UESDK ${_label} patch: ${_patch} missing -- ${_failure_hint}")
        return()
    endif()

    find_package(Git QUIET)
    if(NOT GIT_EXECUTABLE)
        set(GIT_EXECUTABLE git)
    endif()

    # Already applied? A clean reverse-apply check means the fix is present.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${_uesdk_dir}" apply --reverse --check --ignore-whitespace "${_patch}"
        RESULT_VARIABLE _uesdk_rev_rc OUTPUT_QUIET ERROR_QUIET)

    if(_uesdk_rev_rc EQUAL 0)
        message(STATUS "UESDK ${_label} patch: already applied")
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${_uesdk_dir}" apply --check --ignore-whitespace "${_patch}"
            RESULT_VARIABLE _uesdk_fwd_rc OUTPUT_QUIET ERROR_QUIET)
        if(_uesdk_fwd_rc EQUAL 0)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" -C "${_uesdk_dir}" apply --ignore-whitespace "${_patch}"
                RESULT_VARIABLE _uesdk_ap_rc)
            if(_uesdk_ap_rc EQUAL 0)
                message(STATUS "UESDK ${_label} patch: APPLIED")
            else()
                message(WARNING "UESDK ${_label} patch: failed to apply -- ${_failure_hint}")
            endif()
        else()
            message(WARNING "UESDK ${_label} patch: does not apply cleanly to this UESDK "
                            "revision (upstream drift?). ${_failure_hint}. See ${_patch}")
        endif()
    endif()
endfunction()

_uevr_apply_uesdk_patch(
    "embedded-RenderDoc"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/UESDK-StereoStuff-renderdoc.patch"
    "${_uesdk_dir}/src/sdk/StereoStuff.cpp"
    "embedded RenderDoc may crash on inject")

_uevr_apply_uesdk_patch(
    "UE5 render-target-pool scanner"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/UESDK-FRenderTargetPool-ue5-finder.patch"
    "${_uesdk_dir}/src/sdk/FRenderTargetPool.cpp"
    "AFW frame-resource discovery may fail to locate pooled render targets")
