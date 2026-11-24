# Install script for directory: /Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/.task-build/cmake/core/zlib/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/.task-build/cmake/core/libopennurbsStatic.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libopennurbsStatic.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libopennurbsStatic.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libopennurbsStatic.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/.task-build/cmake/core/CMakeFiles/opennurbsStatic.dir/install-cxx-module-bmi-Release.cmake" OPTIONAL)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/opennurbsStatic" TYPE FILE FILES
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_3dm.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_3dm_attributes.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_3dm_properties.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_3dm_settings.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_annotationbase.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_apple_nsfont.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_arc.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_arccurve.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_archivable_dictionary.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_archive.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_array.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_array_defs.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_base32.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_base64.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_beam.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_bezier.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_bitmap.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_bounding_box.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_box.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_brep.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_circle.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_color.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_compress.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_compstat.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_cone.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_convex_poly.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_crc.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_curve.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_curveonsurface.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_curveproxy.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_cylinder.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_date.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_decals.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_defines.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_detail.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_dimension.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_dimensionformat.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_dimensionstyle.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_dithering.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_ellipse.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_embedded_file.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_error.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_evaluate_nurbs.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_extensions.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_file_utilities.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_font.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_fpoint.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_freetype.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_freetype_include.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_fsp.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_fsp_defs.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_function_list.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_geometry.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_gl.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_ground_plane.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_group.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_hash_table.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_hatch.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_hsort_template.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_instance.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_internal_V2_annotation.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_internal_V5_annotation.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_internal_V5_dimstyle.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_internal_defines.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_internal_glyph.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_intersect.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_ipoint.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_knot.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_layer.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_leader.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_light.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_line.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_linear_workflow.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_linecurve.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_linestyle.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_linetype.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_locale.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_lock.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_lookup.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_mapchan.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_material.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_math.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_matrix.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_md5.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_memory.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_mesh.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_mesh_modifiers.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_model_component.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_model_geometry.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_nurbscurve.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_nurbssurface.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_object.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_object_history.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_objref.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_offsetsurface.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_optimize.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_parse.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_photogrammetry.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_plane.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_planesurface.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_pluginlist.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_point.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_pointcloud.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_pointgeometry.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_pointgrid.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_polycurve.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_polyedgecurve.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_polyline.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_polylinecurve.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_post_effects.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_private_wrap.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_private_wrap_defs.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_progress_reporter.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_qsort_template.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_quacksort_template.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_quaternion.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_rand.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_render_channels.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_render_content.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_rendering.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_revsurface.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_rtree.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_safe_frame.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_sectionstyle.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_sha1.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_skylight.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_sleeplock.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_sphere.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_std_string.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_string.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_string_value.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_subd.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_subd_data.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_sumsurface.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_sun.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_surface.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_surfaceproxy.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_symmetry.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_system.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_system_compiler.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_system_runtime.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_terminator.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_testclass.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_text.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_text_style.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_textcontext.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_textdraw.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_textglyph.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_textiterator.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_textlog.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_textobject.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_textrun.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_texture.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_texture_mapping.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_topology.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_torus.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_unicode.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_userdata.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_uuid.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_version.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_version_number.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_viewport.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_wip.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_workspace.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_xform.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_xml.h"
    "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/opennurbs_zlib.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/Frameworks" TYPE DIRECTORY FILES "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/.task-build/cmake/core/OpenNURBS.framework" USE_SOURCE_PERMISSIONS)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/Frameworks/OpenNURBS.framework/Versions/A/OpenNURBS" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/Frameworks/OpenNURBS.framework/Versions/A/OpenNURBS")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/usr/bin/strip" -x "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/Frameworks/OpenNURBS.framework/Versions/A/OpenNURBS")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/.task-build/cmake/core/CMakeFiles/OpenNURBS.dir/install-cxx-module-bmi-Release.cmake" OPTIONAL)
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Volumes/UserFBWorkspace/questions/FSC058DF3D8F89U6623R25345320A0/attempts/20261124T171357371896Z/repo/.task-build/cmake/core/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
