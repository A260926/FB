#
#   Copyright 2024 Pixar
#
#   Licensed under the terms set forth in the LICENSE.txt file available at
#   https://opensubdiv.org/license.
#

# Headless, standard-library-only example exercising
# Far::HierarchicalDisplacement.
add_executable(hierarchical_displacement
    hierarchical_displacement.cpp
)

target_include_directories(hierarchical_displacement
    PRIVATE
        "${OPENSUBDIV_INCLUDE_DIR}"
)

# Link the static CPU OpenSubdiv library when it is being built.
if (TARGET osd_static_cpu)
    target_link_libraries(hierarchical_displacement
        PRIVATE
            osd_static_cpu
    )
elseif (TARGET osd)
    target_link_libraries(hierarchical_displacement
        PRIVATE
            osd
    )
endif()

set_target_properties(hierarchical_displacement
    PROPERTIES
        FOLDER "opensubdiv/examples"
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
)
