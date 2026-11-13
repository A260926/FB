# Install script for directory: /Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs

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
    set(CMAKE_INSTALL_CONFIG_NAME "")
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

if(CMAKE_INSTALL_COMPONENT STREQUAL "Devel" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/man/man3" TYPE FILE FILES
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_cancel.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_create_query.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_destroy.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_destroy_options.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_dup.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_expand_name.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_expand_string.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_fds.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_free_data.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_free_hostent.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_free_string.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_freeaddrinfo.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_get_servers.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_get_servers_ports.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_getaddrinfo.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_gethostbyaddr.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_gethostbyname.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_gethostbyname_file.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_getnameinfo.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_getsock.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_inet_ntop.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_inet_pton.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_init.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_init_options.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_library_cleanup.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_library_init.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_library_init_android.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_library_initialized.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_mkquery.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_parse_a_reply.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_parse_aaaa_reply.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_parse_caa_reply.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_parse_mx_reply.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_parse_naptr_reply.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_parse_ns_reply.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_parse_ptr_reply.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_parse_soa_reply.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_parse_srv_reply.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_parse_txt_reply.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_process.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_query.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_save_options.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_search.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_send.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_local_dev.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_local_ip4.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_local_ip6.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_servers.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_servers_csv.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_servers_ports.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_servers_ports_csv.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_socket_callback.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_socket_configure_callback.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_socket_functions.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_set_sortlist.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_strerror.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_timeout.3"
    "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/docs/ares_version.3"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/solo/pro/UserFB/workspace/FS87F42F093E05U6647/attempts/20261113T164705087266Z/repo/.task-build/docs/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
