#include "ik/build_info_static.h"
#include "ik/build_info_for_every_compile.h"

#define QUOTE(str) #str
#define EXPAND_AND_QUOTE(str) QUOTE(str)

#define IK_VERSION "0.2.0." EXPAND_AND_QUOTE(IK_BUILD_NUMBER)
#define IK_CMAKE_CONFIG \
    "    -DCMAKE_BUILD_TYPE=Release \\\n" \
    "    -DCMAKE_INSTALL_PREFIX=/usr/local \\\n" \
    "    -DCMAKE_PREFIX_PATH= \\\n" \
    "    -DIK_API_NAME=\"ik\" \\\n" \
    "    -DIK_BENCHMARKS=OFF \\\n" \
    "    -DIK_DOT_EXPORT=OFF \\\n" \
    "    -DIK_LIB_TYPE=STATIC \\\n" \
    "    -DIK_MEMORY_DEBUGGING=OFF \\\n" \
    "    -DIK_MEMORY_BACKTRACE=OFF \\\n" \
    "    -DIK_PIC=ON \\\n" \
    "    -DIK_PRECISION=double \\\n" \
    "    -DIK_PROFILING=OFF \\\n" \
    "    -DIK_PYTHON=OFF \\\n" \
    "    -DIK_TESTS=OFF"

/* ------------------------------------------------------------------------- */
const char*
ik_build_info_static_version(void)
{
    return IK_VERSION;
}

/* ------------------------------------------------------------------------- */
int
ik_build_info_static_build_number(void)
{
    return IK_BUILD_NUMBER;
}

/* ------------------------------------------------------------------------- */
const char*
ik_build_info_static_host(void)
{
    return "Darwin";
}

/* ------------------------------------------------------------------------- */
const char*
ik_build_info_static_date(void)
{
    return IK_BUILD_TIME;
}

/* ------------------------------------------------------------------------- */
const char*
ik_build_info_static_commit(void)
{
    return "(unknown tag) ((commit hash unknown))";
}

/* ------------------------------------------------------------------------- */
const char*
ik_build_info_static_compiler(void)
{
    return "AppleClang";
}

/* ------------------------------------------------------------------------- */
const char*
ik_build_info_static_cmake(void)
{
    return IK_CMAKE_CONFIG;
}

/* ------------------------------------------------------------------------- */
const char*
ik_build_info_static_all(void)
{
    return
    "This is IK " IK_VERSION " (major,minor,patch,build number)\n"
    "Host: Darwin\n"
    "Time compiled: " IK_BUILD_TIME "\n"
    "Commit: (unknown tag) ((commit hash unknown))\n"
    "Compiler: AppleClang\n"
    "CMake configuration:\n"
    IK_CMAKE_CONFIG "\n"
    "Other interesting variables:\n"
    "    IK_HAVE_STDINT_H=1\n"
    "    IK_WARN_UNUSED=__attribute__((warn_unused_result))\n"
    "    IK_PUBLIC_API=" EXPAND_AND_QUOTE(IK_PUBLIC_API) "\n"
    "    IK_PRIVATE_API=" EXPAND_AND_QUOTE(IK_PRIVATE_API)
    ;
}
