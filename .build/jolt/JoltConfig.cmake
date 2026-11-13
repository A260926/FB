
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was Config.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################

include("${CMAKE_CURRENT_LIST_DIR}/JoltTargets.cmake")

check_required_components(Jolt)

file(GLOB libTargets_files "${CMAKE_CURRENT_LIST_DIR}/JoltTargets-*.cmake")
list(LENGTH libTargets_files num_files)

if(num_files EQUAL 1)
    list(GET libTargets_files 0 target_file)
    get_filename_component(filename "${target_file}" NAME)

    if(filename STREQUAL "JoltTargets-debug.cmake")
        set_property(TARGET Jolt::Jolt APPEND PROPERTY
            INTERFACE_COMPILE_DEFINITIONS "JPH_DEBUG")
    endif()
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/JoltTargets-debug.cmake")
    # Note that when evaluated on an imported target, $<CONFIG:Debug> matches the configuration of Jolt that was selected
    set_property(TARGET Jolt::Jolt APPEND PROPERTY
        INTERFACE_COMPILE_DEFINITIONS
        "$<$<CONFIG:Debug>:JPH_DEBUG>"
    )

    # When the consumer uses a configuration that Jolt doesn't have, CMake picks the first installed configuration,
    # which could be the debug library. Prefer an optimized library instead (unless the user specified a mapping).
    get_target_property(imported_configs Jolt::Jolt IMPORTED_CONFIGURATIONS)
    set(optimized_configs)
    foreach(config RELEASE DISTRIBUTION)
        if(config IN_LIST imported_configs)
            list(APPEND optimized_configs ${config})
        endif()
    endforeach()
    if(optimized_configs)
        foreach(config RELWITHDEBINFO MINSIZEREL)
            get_target_property(mapped_configs Jolt::Jolt MAP_IMPORTED_CONFIG_${config})
            if(NOT mapped_configs AND NOT config IN_LIST imported_configs)
                set_property(TARGET Jolt::Jolt PROPERTY MAP_IMPORTED_CONFIG_${config} ${optimized_configs})
            endif()
        endforeach()
    endif()
endif()
