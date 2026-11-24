/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* This is the build configuration header template, processed by CMake's
   configure_file() into SDL_build_config.h. */

#ifndef SDL_build_config_h_
#define SDL_build_config_h_

/* General platform identification */
#cmakedefine HAVE_PLATFORM_BACKEND @HAVE_PLATFORM_BACKEND@
#cmakedefine HAVE_DRIVER_BACKEND @HAVE_DRIVER_BACKEND@

/* C library and standard headers */
#cmakedefine HAVE_LIBC 1
#cmakedefine HAVE_LIBC_ENVIRONMENT 1
#cmakedefine HAVE_STDIO_H 1
#cmakedefine HAVE_STDLIB_H 1
#cmakedefine HAVE_STDDEF_H 1
#cmakedefine HAVE_STDARG_H 1
#cmakedefine HAVE_STDINT_H 1
#cmakedefine HAVE_INTTYPES_H 1
#cmakedefine HAVE_STRING_H 1
#cmakedefine HAVE_STRINGS_H 1
#cmakedefine HAVE_MEMORY_H 1
#cmakedefine HAVE_MALLOC_H 1
#cmakedefine HAVE_USR_INCLUDE_MALLOC_H 1
#cmakedefine HAVE_ALLOCA_H 1
#cmakedefine HAVE_MATH_H 1
#cmakedefine HAVE_FLOAT_H 1
#cmakedefine HAVE_WCHAR_H 1
#cmakedefine HAVE_SIGNAL_H 1
#cmakedefine HAVE_LIMITS_H 1
#cmakedefine HAVE_SYS_TYPES_H 1
#cmakedefine HAVE_ICONV_H 1
#cmakedefine HAVE_ICONV 1
#cmakedefine HAVE_LIBICONV 1
#cmakedefine SDL_USE_LIBICONV 1

/* C library functions */
#cmakedefine HAVE_MALLOC 1
#cmakedefine HAVE_ABS 1
#cmakedefine HAVE_BCOPY 1
#cmakedefine HAVE_MEMCMP 1
#cmakedefine HAVE_MEMCPY 1
#cmakedefine HAVE_MEMMOVE 1
#cmakedefine HAVE_MEMSET 1
#cmakedefine HAVE_STRLEN 1
#cmakedefine HAVE_STRLCPY 1
#cmakedefine HAVE_STRLCAT 1
#cmakedefine HAVE_STRCMP 1
#cmakedefine HAVE_STRNCMP 1
#cmakedefine HAVE_STRNLEN 1
#cmakedefine HAVE_STRCHR 1
#cmakedefine HAVE_STRRCHR 1
#cmakedefine HAVE_STRSTR 1
#cmakedefine HAVE_STRNSTR 1
#cmakedefine HAVE_STRPBRK 1
#cmakedefine HAVE_STRTOD 1
#cmakedefine HAVE_STRTOK_R 1
#cmakedefine HAVE_STRTOL 1
#cmakedefine HAVE_STRTOLL 1
#cmakedefine HAVE_STRTOUL 1
#cmakedefine HAVE_STRTOULL 1
#cmakedefine HAVE_INDEX 1
#cmakedefine HAVE_RINDEX 1
#cmakedefine HAVE_ATOF 1
#cmakedefine HAVE_ATOI 1
#cmakedefine HAVE_ITOA 1
#cmakedefine HAVE__EXIT 1
#cmakedefine HAVE_GETENV 1
#cmakedefine HAVE_PUTENV 1
#cmakedefine HAVE_SETENV 1
#cmakedefine HAVE_UNSETENV 1
#cmakedefine HAVE_VSNPRINTF 1
#cmakedefine HAVE_VSSCANF 1
#cmakedefine HAVE_WCSDUP 1
#cmakedefine HAVE_WCSLEN 1
#cmakedefine HAVE_WCSNLEN 1
#cmakedefine HAVE_WCSLCAT 1
#cmakedefine HAVE_WCSLCPY 1
#cmakedefine HAVE_WCSCMP 1
#cmakedefine HAVE_WCSNCMP 1
#cmakedefine HAVE_WCSSTR 1
#cmakedefine HAVE_WCSTOL 1
#cmakedefine HAVE_FOPEN64 1
#cmakedefine HAVE_FSEEKO 1
#cmakedefine HAVE_FSEEKO64 1
#cmakedefine HAVE__FSEEKI64 1
#cmakedefine HAVE__I64TOA 1
#cmakedefine HAVE__LTOA 1
#cmakedefine HAVE__STRREV 1
#cmakedefine HAVE__UI64TOA 1
#cmakedefine HAVE__UITOA 1
#cmakedefine HAVE__ULTOA 1
#cmakedefine HAVE__COPYSIGN 1
#cmakedefine HAVE__SCALB 1

/* Math library and math functions */
#cmakedefine HAVE_LIBM 1
#cmakedefine HAVE_ACOS 1
#cmakedefine HAVE_ACOSF 1
#cmakedefine HAVE_ASIN 1
#cmakedefine HAVE_ASINF 1
#cmakedefine HAVE_ATAN 1
#cmakedefine HAVE_ATANF 1
#cmakedefine HAVE_ATAN2 1
#cmakedefine HAVE_ATAN2F 1
#cmakedefine HAVE_CEIL 1
#cmakedefine HAVE_CEILF 1
#cmakedefine HAVE_COPYSIGN 1
#cmakedefine HAVE_COPYSIGNF 1
#cmakedefine HAVE_COS 1
#cmakedefine HAVE_COSF 1
#cmakedefine HAVE_EXP 1
#cmakedefine HAVE_EXPF 1
#cmakedefine HAVE_FABS 1
#cmakedefine HAVE_FABSF 1
#cmakedefine HAVE_FLOOR 1
#cmakedefine HAVE_FLOORF 1
#cmakedefine HAVE_FMOD 1
#cmakedefine HAVE_FMODF 1
#cmakedefine HAVE_LOG 1
#cmakedefine HAVE_LOGF 1
#cmakedefine HAVE_LOG10 1
#cmakedefine HAVE_LOG10F 1
#cmakedefine HAVE_LROUND 1
#cmakedefine HAVE_LROUNDF 1
#cmakedefine HAVE_MODF 1
#cmakedefine HAVE_MODFF 1
#cmakedefine HAVE_POW 1
#cmakedefine HAVE_POWF 1
#cmakedefine HAVE_ROUND 1
#cmakedefine HAVE_ROUNDF 1
#cmakedefine HAVE_SCALBN 1
#cmakedefine HAVE_SCALBNF 1
#cmakedefine HAVE_SIN 1
#cmakedefine HAVE_SINF 1
#cmakedefine HAVE_SQRT 1
#cmakedefine HAVE_SQRTF 1
#cmakedefine HAVE_TAN 1
#cmakedefine HAVE_TANF 1
#cmakedefine HAVE_TRUNC 1
#cmakedefine HAVE_TRUNCF 1
#cmakedefine HAVE_ISINF 1
#cmakedefine HAVE_ISINFF 1
#cmakedefine HAVE_ISINF_FLOAT_MACRO 1
#cmakedefine HAVE_ISNAN 1
#cmakedefine HAVE_ISNANF 1
#cmakedefine HAVE_ISNAN_FLOAT_MACRO 1

/* POSIX and platform facilities */
#cmakedefine HAVE_CLOCK_GETTIME 1
#cmakedefine HAVE_FDATASYNC 1
#cmakedefine HAVE_GMTIME_R 1
#cmakedefine HAVE_LOCALTIME_R 1
#cmakedefine HAVE_NANOSLEEP 1
#cmakedefine HAVE_NL_LANGINFO 1
#cmakedefine HAVE_O_CLOEXEC 1
#cmakedefine HAVE_GETHOSTNAME 1
#cmakedefine HAVE_GETPAGESIZE 1
#cmakedefine HAVE_GETRESGID 1
#cmakedefine HAVE_GETRESUID 1
#cmakedefine HAVE_GETAUXVAL 1
#cmakedefine HAVE_ELF_AUX_INFO 1
#cmakedefine HAVE_PPOLL 1
#cmakedefine HAVE_MEMFD_CREATE 1
#cmakedefine HAVE_MMAP 1
#cmakedefine HAVE_MREMAP 1
#cmakedefine HAVE_POSIX_FALLOCATE 1
#cmakedefine HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCHDIR 1
#cmakedefine HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCHDIR_NP 1
#cmakedefine HAVE_SA_SIGACTION 1
#cmakedefine HAVE_SETJMP 1
#cmakedefine HAVE_SIGACTION 1
#cmakedefine HAVE_SIGTIMEDWAIT 1
#cmakedefine HAVE_SYSCONF 1
#cmakedefine HAVE_SYSCTLBYNAME 1
#cmakedefine HAVE_ST_MTIM 1
#cmakedefine HAVE_DLOPEN 1
#cmakedefine HAVE_SIGNAL_SUPPORT 1
#cmakedefine HAVE_ATOMIC_LOAD_N 1
#cmakedefine HAVE_FORCE_ALIGN_ARG_POINTER 1

/* Threading */
#cmakedefine HAVE_PTHREAD 1
#cmakedefine HAVE_PTHREADS 1
#cmakedefine HAVE_PTHREAD_H 1
#cmakedefine HAVE_PTHREAD_NP_H 1
#cmakedefine HAVE_PTHREAD_SETNAME_NP 1
#cmakedefine HAVE_PTHREAD_SET_NAME_NP 1
#cmakedefine HAVE_PTHREADS_SEM 1
#cmakedefine HAVE_SEM_TIMEDWAIT 1
#cmakedefine HAVE_RECURSIVE_MUTEXES 1
#cmakedefine HAVE_RECURSIVE_MUTEXES_NP 1
#cmakedefine SDL_THREAD_PTHREAD 1
#cmakedefine SDL_THREAD_PTHREAD_RECURSIVE_MUTEX 1
#cmakedefine SDL_THREAD_PTHREAD_RECURSIVE_MUTEX_NP 1
#cmakedefine SDL_THREAD_GENERIC_COND_SUFFIX 1
#cmakedefine SDL_THREAD_GENERIC_RWLOCK_SUFFIX 1
#cmakedefine SDL_THREADS_DISABLED 1

/* Compiler and CPU capabilities */
#cmakedefine HAVE_GCC_ATOMICS 1
#cmakedefine HAVE_GCC_SYNC_LOCK_TEST_AND_SET 1
#cmakedefine HAVE_MSC_ATOMICS 1
#cmakedefine HAVE_GCC_DIAGNOSTIC_PRAGMA 1
#cmakedefine HAVE_GCC_FVISIBILITY 1
#cmakedefine HAVE_GCC_WFLOAT_CONVERSION 1
#cmakedefine HAVE_GCC_NO_CYGWIN 1
#cmakedefine HAVE_MMX 1
#cmakedefine HAVE_SSE 1
#cmakedefine HAVE_SSE2 1
#cmakedefine HAVE_SSE3 1
#cmakedefine HAVE_SSE4_1 1
#cmakedefine HAVE_SSE4_2 1
#cmakedefine HAVE_AVX 1
#cmakedefine HAVE_AVX2 1
#cmakedefine HAVE_AVX512F 1
#cmakedefine HAVE_ALTIVEC 1
#cmakedefine HAVE_ARMNEON 1
#cmakedefine HAVE_ARM_MODE 1
#cmakedefine HAVE_LSX 1
#cmakedefine HAVE_LASX 1
#cmakedefine HAVE_ASSEMBLY 1
#cmakedefine SDL_DISABLE_MMX 1
#cmakedefine SDL_DISABLE_SSE 1
#cmakedefine SDL_DISABLE_SSE2 1
#cmakedefine SDL_DISABLE_SSE3 1
#cmakedefine SDL_DISABLE_SSE4_1 1
#cmakedefine SDL_DISABLE_SSE4_2 1
#cmakedefine SDL_DISABLE_AVX 1
#cmakedefine SDL_DISABLE_AVX2 1
#cmakedefine SDL_DISABLE_AVX512F 1
#cmakedefine SDL_DISABLE_ALLOCA 1
#cmakedefine SDL_DISABLE_NEON 1
#cmakedefine SDL_DISABLE_LSX 1
#cmakedefine SDL_DISABLE_LASX 1
#cmakedefine SDL_ALTIVEC_BLITTERS 1

/* Assertion level */
#cmakedefine HAVE_ASSERTIONS @HAVE_ASSERTIONS@
#if @SDL_DEFAULT_ASSERT_LEVEL_CONFIGURED@
#define SDL_DEFAULT_ASSERT_LEVEL @SDL_DEFAULT_ASSERT_LEVEL@
#endif

/* Dynamic API */
#cmakedefine SDL_DYNAMIC_API 1

/* Enabled/disabled subsystems */
#cmakedefine SDL_AUDIO_DISABLED 1
#cmakedefine SDL_VIDEO_DISABLED 1
#cmakedefine SDL_GPU_DISABLED 1
#cmakedefine SDL_RENDER_DISABLED 1
#cmakedefine SDL_CAMERA_DISABLED 1
#cmakedefine SDL_JOYSTICK_DISABLED 1
#cmakedefine SDL_HAPTIC_DISABLED 1
#cmakedefine SDL_HIDAPI_DISABLED 1
#cmakedefine SDL_POWER_DISABLED 1
#cmakedefine SDL_SENSOR_DISABLED 1
#cmakedefine SDL_DIALOG_DISABLED 1
#cmakedefine SDL_CPUINFO_DISABLED 1
#cmakedefine SDL_FILESYSTEM_DISABLED 1
#cmakedefine SDL_LEAN_AND_MEAN 1

/* Audio drivers */
#cmakedefine SDL_AUDIO_DRIVER_AAUDIO 1
#cmakedefine SDL_AUDIO_DRIVER_ALSA 1
#cmakedefine SDL_AUDIO_DRIVER_ALSA_DYNAMIC "@SDL_AUDIO_DRIVER_ALSA_DYNAMIC@"
#cmakedefine SDL_AUDIO_DRIVER_COREAUDIO 1
#cmakedefine SDL_AUDIO_DRIVER_DISK 1
#cmakedefine SDL_AUDIO_DRIVER_DSOUND 1
#cmakedefine SDL_AUDIO_DRIVER_DUMMY 1
#cmakedefine SDL_AUDIO_DRIVER_EMSCRIPTEN 1
#cmakedefine SDL_AUDIO_DRIVER_HAIKU 1
#cmakedefine SDL_AUDIO_DRIVER_JACK 1
#cmakedefine SDL_AUDIO_DRIVER_JACK_DYNAMIC "@SDL_AUDIO_DRIVER_JACK_DYNAMIC@"
#cmakedefine SDL_AUDIO_DRIVER_N3DS 1
#cmakedefine SDL_AUDIO_DRIVER_NETBSD 1
#cmakedefine SDL_AUDIO_DRIVER_NGAGE 1
#cmakedefine SDL_AUDIO_DRIVER_OPENSLES 1
#cmakedefine SDL_AUDIO_DRIVER_OSS 1
#cmakedefine SDL_AUDIO_DRIVER_PIPEWIRE 1
#cmakedefine SDL_AUDIO_DRIVER_PIPEWIRE_DYNAMIC "@SDL_AUDIO_DRIVER_PIPEWIRE_DYNAMIC@"
#cmakedefine SDL_AUDIO_DRIVER_PRIVATE 1
#cmakedefine SDL_AUDIO_DRIVER_PS2 1
#cmakedefine SDL_AUDIO_DRIVER_PSP 1
#cmakedefine SDL_AUDIO_DRIVER_PULSEAUDIO 1
#cmakedefine SDL_AUDIO_DRIVER_PULSEAUDIO_DYNAMIC "@SDL_AUDIO_DRIVER_PULSEAUDIO_DYNAMIC@"
#cmakedefine SDL_AUDIO_DRIVER_QNX 1
#cmakedefine SDL_AUDIO_DRIVER_SNDIO 1
#cmakedefine SDL_AUDIO_DRIVER_SNDIO_DYNAMIC "@SDL_AUDIO_DRIVER_SNDIO_DYNAMIC@"
#cmakedefine SDL_AUDIO_DRIVER_VITA 1
#cmakedefine SDL_AUDIO_DRIVER_WASAPI 1

/* Camera drivers */
#cmakedefine SDL_CAMERA_DRIVER_ANDROID 1
#cmakedefine SDL_CAMERA_DRIVER_COREMEDIA 1
#cmakedefine SDL_CAMERA_DRIVER_DUMMY 1
#cmakedefine SDL_CAMERA_DRIVER_EMSCRIPTEN 1
#cmakedefine SDL_CAMERA_DRIVER_MEDIAFOUNDATION 1
#cmakedefine SDL_CAMERA_DRIVER_PIPEWIRE 1
#cmakedefine SDL_CAMERA_DRIVER_PIPEWIRE_DYNAMIC "@SDL_CAMERA_DRIVER_PIPEWIRE_DYNAMIC@"
#cmakedefine SDL_CAMERA_DRIVER_V4L2 1
#cmakedefine SDL_CAMERA_DRIVER_VITA 1

/* Video drivers */
#cmakedefine SDL_VIDEO_DRIVER_ANDROID 1
#cmakedefine SDL_VIDEO_DRIVER_COCOA 1
#cmakedefine SDL_VIDEO_DRIVER_DUMMY 1
#cmakedefine SDL_VIDEO_DRIVER_EMSCRIPTEN 1
#cmakedefine SDL_VIDEO_DRIVER_HAIKU 1
#cmakedefine SDL_VIDEO_DRIVER_KMSDRM 1
#cmakedefine SDL_VIDEO_DRIVER_KMSDRM_DYNAMIC "@SDL_VIDEO_DRIVER_KMSDRM_DYNAMIC@"
#cmakedefine SDL_VIDEO_DRIVER_KMSDRM_DYNAMIC_GBM "@SDL_VIDEO_DRIVER_KMSDRM_DYNAMIC_GBM@"
#cmakedefine SDL_VIDEO_DRIVER_N3DS 1
#cmakedefine SDL_VIDEO_DRIVER_NGAGE 1
#cmakedefine SDL_VIDEO_DRIVER_OFFSCREEN 1
#cmakedefine SDL_VIDEO_DRIVER_OPENVR 1
#cmakedefine SDL_VIDEO_DRIVER_PRIVATE 1
#cmakedefine SDL_VIDEO_DRIVER_PS2 1
#cmakedefine SDL_VIDEO_DRIVER_PSP 1
#cmakedefine SDL_VIDEO_DRIVER_QNX 1
#cmakedefine SDL_VIDEO_DRIVER_RISCOS 1
#cmakedefine SDL_VIDEO_DRIVER_ROCKCHIP 1
#cmakedefine SDL_VIDEO_DRIVER_RPI 1
#cmakedefine SDL_VIDEO_DRIVER_UIKIT 1
#cmakedefine SDL_VIDEO_DRIVER_VITA 1
#cmakedefine SDL_VIDEO_DRIVER_VIVANTE 1
#cmakedefine SDL_VIDEO_DRIVER_VIVANTE_VDK 1
#cmakedefine SDL_VIDEO_DRIVER_WAYLAND 1
#cmakedefine SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC "@SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC@"
#cmakedefine SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_CURSOR "@SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_CURSOR@"
#cmakedefine SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_EGL "@SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_EGL@"
#cmakedefine SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR "@SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_LIBDECOR@"
#cmakedefine SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_XKBCOMMON "@SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC_XKBCOMMON@"
#cmakedefine SDL_VIDEO_DRIVER_WINDOWS 1
#cmakedefine SDL_VIDEO_DRIVER_X11 1
#cmakedefine SDL_VIDEO_DRIVER_X11_DYNAMIC "@SDL_VIDEO_DRIVER_X11_DYNAMIC@"
#cmakedefine SDL_VIDEO_DRIVER_X11_DYNAMIC_XCURSOR "@SDL_VIDEO_DRIVER_X11_DYNAMIC_XCURSOR@"
#cmakedefine SDL_VIDEO_DRIVER_X11_DYNAMIC_XEXT "@SDL_VIDEO_DRIVER_X11_DYNAMIC_XEXT@"
#cmakedefine SDL_VIDEO_DRIVER_X11_DYNAMIC_XFIXES "@SDL_VIDEO_DRIVER_X11_DYNAMIC_XFIXES@"
#cmakedefine SDL_VIDEO_DRIVER_X11_DYNAMIC_XINPUT2 "@SDL_VIDEO_DRIVER_X11_DYNAMIC_XINPUT2@"
#cmakedefine SDL_VIDEO_DRIVER_X11_DYNAMIC_XRANDR "@SDL_VIDEO_DRIVER_X11_DYNAMIC_XRANDR@"
#cmakedefine SDL_VIDEO_DRIVER_X11_DYNAMIC_XSS "@SDL_VIDEO_DRIVER_X11_DYNAMIC_XSS@"
#cmakedefine SDL_VIDEO_DRIVER_X11_DYNAMIC_XTEST "@SDL_VIDEO_DRIVER_X11_DYNAMIC_XTEST@"
#cmakedefine SDL_VIDEO_DRIVER_X11_HAS_XKBLIB 1
#cmakedefine SDL_VIDEO_DRIVER_X11_SUPPORTS_GENERIC_EVENTS 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XCURSOR 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XDBE 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XFIXES 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XINPUT2 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XINPUT2_SUPPORTS_GESTURE 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XINPUT2_SUPPORTS_MULTITOUCH 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XINPUT2_SUPPORTS_SCROLLINFO 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XRANDR 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XSCRNSAVER 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XSHAPE 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XSYNC 1
#cmakedefine SDL_VIDEO_DRIVER_X11_XTEST 1

/* Rendering and GPU */
#cmakedefine SDL_VIDEO_OPENGL 1
#cmakedefine SDL_VIDEO_OPENGL_CGL 1
#cmakedefine SDL_VIDEO_OPENGL_EGL 1
#cmakedefine SDL_VIDEO_OPENGL_ES 1
#cmakedefine SDL_VIDEO_OPENGL_ES2 1
#cmakedefine SDL_VIDEO_RENDER_OGL 1
#cmakedefine SDL_VIDEO_RENDER_OGL_ES2 1
#cmakedefine SDL_VIDEO_VULKAN 1
#cmakedefine SDL_VIDEO_METAL 1
#cmakedefine SDL_GPU_METAL 1
#cmakedefine SDL_GPU_VULKAN 1
#cmakedefine SDL_GPU_D3D12 1
#cmakedefine SDL_GPU_PRIVATE 1
#cmakedefine SDL_GPU_DRIVER_H 1
#cmakedefine HAVE_METAL 1
#cmakedefine HAVE_VULKAN 1
#cmakedefine HAVE_OPENGL 1
#cmakedefine HAVE_OPENGLES 1
#cmakedefine HAVE_RENDER_METAL 1
#cmakedefine HAVE_RENDER_VULKAN 1
#cmakedefine HAVE_FRAMEWORK_METAL 1

/* Joysticks and haptics */
#cmakedefine SDL_JOYSTICK_ANDROID 1
#cmakedefine SDL_JOYSTICK_DINPUT 1
#cmakedefine SDL_JOYSTICK_DUMMY 1
#cmakedefine SDL_JOYSTICK_EMSCRIPTEN 1
#cmakedefine SDL_JOYSTICK_GAMEINPUT 1
#cmakedefine SDL_JOYSTICK_HAIKU 1
#cmakedefine SDL_JOYSTICK_HIDAPI 1
#cmakedefine SDL_JOYSTICK_HIDAPI_8BITDO 1
#cmakedefine SDL_JOYSTICK_HIDAPI_FLYDIGI 1
#cmakedefine SDL_JOYSTICK_HIDAPI_GAMECUBE 1
#cmakedefine SDL_JOYSTICK_HIDAPI_GIP 1
#cmakedefine SDL_JOYSTICK_HIDAPI_LG4FF 1
#cmakedefine SDL_JOYSTICK_HIDAPI_LUNA 1
#cmakedefine SDL_JOYSTICK_HIDAPI_PS3 1
#cmakedefine SDL_JOYSTICK_HIDAPI_PS4 1
#cmakedefine SDL_JOYSTICK_HIDAPI_PS5 1
#cmakedefine SDL_JOYSTICK_HIDAPI_SHIELD 1
#cmakedefine SDL_JOYSTICK_HIDAPI_SINPUT 1
#cmakedefine SDL_JOYSTICK_HIDAPI_STADIA 1
#cmakedefine SDL_JOYSTICK_HIDAPI_STEAM 1
#cmakedefine SDL_JOYSTICK_HIDAPI_STEAMDECK 1
#cmakedefine SDL_JOYSTICK_HIDAPI_STEAM_HORI 1
#cmakedefine SDL_JOYSTICK_HIDAPI_STEAM_TRITON 1
#cmakedefine SDL_JOYSTICK_HIDAPI_STEAMXBOX 1
#cmakedefine SDL_JOYSTICK_HIDAPI_SWITCH 1
#cmakedefine SDL_JOYSTICK_HIDAPI_SWITCH2 1
#cmakedefine SDL_JOYSTICK_HIDAPI_WII 1
#cmakedefine SDL_JOYSTICK_HIDAPI_XBOX360 1
#cmakedefine SDL_JOYSTICK_HIDAPI_XBOXONE 1
#cmakedefine SDL_JOYSTICK_HIDAPI_ZUIKI 1
#cmakedefine SDL_JOYSTICK_HIDAPI_H 1
#cmakedefine SDL_JOYSTICK_IOKIT 1
#cmakedefine SDL_JOYSTICK_IOKIT_H 1
#cmakedefine SDL_JOYSTICK_IOS_H 1
#cmakedefine SDL_JOYSTICK_LINUX 1
#cmakedefine SDL_JOYSTICK_MFI 1
#cmakedefine SDL_JOYSTICK_N3DS 1
#cmakedefine SDL_JOYSTICK_PRIVATE 1
#cmakedefine SDL_JOYSTICK_PS2 1
#cmakedefine SDL_JOYSTICK_PSP 1
#cmakedefine SDL_JOYSTICK_RAWINPUT 1
#cmakedefine SDL_JOYSTICK_RAWINPUT_MATCHING 1
#cmakedefine SDL_JOYSTICK_RAWINPUT_MATCH_AXES 1
#cmakedefine SDL_JOYSTICK_RAWINPUT_MATCH_TRIGGERS 1
#cmakedefine SDL_JOYSTICK_RAWINPUT_WGI 1
#cmakedefine SDL_JOYSTICK_RAWINPUT_XINPUT 1
#cmakedefine SDL_JOYSTICK_USBHID 1
#cmakedefine SDL_JOYSTICK_VIRTUAL 1
#cmakedefine SDL_JOYSTICK_VITA 1
#cmakedefine SDL_JOYSTICK_WGI 1
#cmakedefine SDL_JOYSTICK_WINMM 1
#cmakedefine SDL_JOYSTICK_XINPUT 1
#cmakedefine HAVE_HIDAPI 1
#cmakedefine HAVE_HIDAPI_JOYSTICK 1
#cmakedefine HAVE_VIRTUAL_JOYSTICK 1
#cmakedefine SDL_HAPTIC_ANDROID 1
#cmakedefine SDL_HAPTIC_DINPUT 1
#cmakedefine SDL_HAPTIC_DUMMY 1
#cmakedefine SDL_HAPTIC_IOKIT 1
#cmakedefine SDL_HAPTIC_LINUX 1
#cmakedefine SDL_HAPTIC_HIDAPI_LG4FF 1

/* HIDAPI / USB */
#cmakedefine HAVE_LIBUSB 1
#cmakedefine HAVE_LIBUSB_H 1
#cmakedefine HAVE_USB_H 1
#cmakedefine HAVE_USBHID_H 1
#cmakedefine HAVE_LIBUSBHID_H 1
#cmakedefine HAVE_HIDAPI_LIBUSB 1
#cmakedefine HAVE_HIDAPI_LIBUSB_SHARED 1
#cmakedefine SDL_LIBUSB_DYNAMIC "@SDL_LIBUSB_DYNAMIC@"

/* Loadable objects */
#cmakedefine SDL_LOADSO_DLOPEN 1
#cmakedefine SDL_LOADSO_DUMMY 1
#cmakedefine SDL_LOADSO_WINDOWS 1

/* Timers and time */
#cmakedefine SDL_TIMER_HAIKU 1
#cmakedefine SDL_TIMER_N3DS 1
#cmakedefine SDL_TIMER_PS2 1
#cmakedefine SDL_TIMER_PSP 1
#cmakedefine SDL_TIMER_UNIX 1
#cmakedefine SDL_TIMER_VITA 1
#cmakedefine SDL_TIMER_WINDOWS 1
#cmakedefine SDL_TIME_N3DS 1
#cmakedefine SDL_TIME_NGAGE 1
#cmakedefine SDL_TIME_PS2 1
#cmakedefine SDL_TIME_PSP 1
#cmakedefine SDL_TIME_UNIX 1
#cmakedefine SDL_TIME_VITA 1
#cmakedefine SDL_TIME_WINDOWS 1
#cmakedefine HAVE_TIME_BEGIN_PERIOD 1

/* Thread backends for non-pthread platforms */
#cmakedefine SDL_THREAD_N3DS 1
#cmakedefine SDL_THREAD_PS2 1
#cmakedefine SDL_THREAD_PSP 1
#cmakedefine SDL_THREAD_VITA 1
#cmakedefine SDL_THREAD_WINDOWS 1

/* Filesystem and storage */
#cmakedefine SDL_FILESYSTEM_ANDROID 1
#cmakedefine SDL_FILESYSTEM_COCOA 1
#cmakedefine SDL_FILESYSTEM_DUMMY 1
#cmakedefine SDL_FILESYSTEM_EMSCRIPTEN 1
#cmakedefine SDL_FILESYSTEM_HAIKU 1
#cmakedefine SDL_FILESYSTEM_N3DS 1
#cmakedefine SDL_FILESYSTEM_NGAGE 1
#cmakedefine SDL_FILESYSTEM_PS2 1
#cmakedefine SDL_FILESYSTEM_PSP 1
#cmakedefine SDL_FILESYSTEM_RISCOS 1
#cmakedefine SDL_FILESYSTEM_UNIX 1
#cmakedefine SDL_FILESYSTEM_VITA 1
#cmakedefine SDL_FILESYSTEM_WINDOWS 1
#cmakedefine SDL_FSOPS_DUMMY 1
#cmakedefine SDL_FSOPS_POSIX 1
#cmakedefine SDL_FSOPS_WINDOWS 1
#cmakedefine SDL_STORAGE_GENERIC 1
#cmakedefine SDL_STORAGE_PRIVATE 1
#cmakedefine SDL_STORAGE_STEAM 1

/* Process, dialog, tray, locale, misc, sensors dummies and backends */
#cmakedefine SDL_PROCESS_DUMMY 1
#cmakedefine SDL_PROCESS_POSIX 1
#cmakedefine SDL_PROCESS_WINDOWS 1
#cmakedefine SDL_PROCESS_STDIO 1
#cmakedefine SDL_DIALOG_DUMMY 1
#cmakedefine SDL_TRAY_DUMMY 1
#cmakedefine SDL_LOCALE_DUMMY 1
#cmakedefine SDL_MISC_DUMMY 1
#cmakedefine SDL_SENSOR_DUMMY 1
#cmakedefine SDL_SENSOR_ANDROID 1
#cmakedefine SDL_SENSOR_COREMOTION 1
#cmakedefine SDL_SENSOR_EMSCRIPTEN 1
#cmakedefine SDL_SENSOR_N3DS 1
#cmakedefine SDL_SENSOR_VITA 1
#cmakedefine SDL_SENSOR_WINDOWS 1

/* Render backends */
#cmakedefine SDL_VIDEO_RENDER_D3D 1
#cmakedefine SDL_VIDEO_RENDER_D3D11 1
#cmakedefine SDL_VIDEO_RENDER_D3D12 1
#cmakedefine SDL_VIDEO_RENDER_GPU 1
#cmakedefine SDL_VIDEO_RENDER_METAL 1
#cmakedefine SDL_VIDEO_RENDER_NGAGE 1
#cmakedefine SDL_VIDEO_RENDER_PS2 1
#cmakedefine SDL_VIDEO_RENDER_PSP 1
#cmakedefine SDL_VIDEO_RENDER_VITA_GXM 1
#cmakedefine SDL_VIDEO_RENDER_VULKAN 1
#cmakedefine SDL_VIDEO_OPENGL_GLX 1
#cmakedefine SDL_VIDEO_OPENGL_HAIKU 1
#cmakedefine SDL_VIDEO_OPENGL_WGL 1

/* Power backends */
#cmakedefine SDL_POWER_ANDROID 1
#cmakedefine SDL_POWER_EMSCRIPTEN 1
#cmakedefine SDL_POWER_HAIKU 1
#cmakedefine SDL_POWER_LINUX 1
#cmakedefine SDL_POWER_MACOSX 1
#cmakedefine SDL_POWER_N3DS 1
#cmakedefine SDL_POWER_PSP 1
#cmakedefine SDL_POWER_UIKIT 1
#cmakedefine SDL_POWER_VITA 1
#cmakedefine SDL_POWER_WINDOWS 1

/* Platform input and IME details */
#cmakedefine SDL_HAVE_MACHINE_JOYSTICK_H 1
#cmakedefine SDL_INPUT_FBSDKBIO 1
#cmakedefine SDL_INPUT_LINUXEV 1
#cmakedefine SDL_INPUT_LINUXKD 1
#cmakedefine SDL_INPUT_WSCONS 1
#cmakedefine SDL_USE_IME 1
#cmakedefine SDL_UDEV_DYNAMIC "@SDL_UDEV_DYNAMIC@"
#cmakedefine SDL_FRIBIDI_DYNAMIC "@SDL_FRIBIDI_DYNAMIC@"
#cmakedefine SDL_LIBTHAI_DYNAMIC "@SDL_LIBTHAI_DYNAMIC@"

/* Vita specifics */
#cmakedefine SDL_VIDEO_VITA_PIB 1
#cmakedefine SDL_VIDEO_VITA_PVR 1
#cmakedefine SDL_VIDEO_VITA_PVR_OGL 1


/* iOS specifics */
#cmakedefine SDL_IPHONE_KEYBOARD 1
#cmakedefine SDL_IPHONE_LAUNCHSCREEN 1

/* Platform-specific libraries and headers not used on this build */
#cmakedefine HAVE_DBUS_DBUS_H 1
#cmakedefine HAVE_FCITX 1
#cmakedefine HAVE_IBUS_IBUS_H 1
#cmakedefine HAVE_IBUS 1
#cmakedefine HAVE_LIBDECOR_H 1
#cmakedefine HAVE_LIBTHAI_H 1
#cmakedefine HAVE_LIBUDEV_H 1
#cmakedefine HAVE_LIBUNWIND_H 1
#cmakedefine HAVE_LIBURING_H 1
#cmakedefine HAVE_LIBURING 1
#cmakedefine HAVE_INOTIFY 1
#cmakedefine HAVE_INOTIFY_INIT1 1
#cmakedefine HAVE_SYS_INOTIFY_H 1
#cmakedefine HAVE_LINUX_INPUT_H 1
#cmakedefine HAVE_MACHINE_JOYSTICK_H 1
#cmakedefine HAVE_FRIBIDI_H 1
#cmakedefine HAVE_FRIBIDI 1
#cmakedefine HAVE_LIBTHAI 1
#cmakedefine HAVE_DDRAW_H 1
#cmakedefine HAVE_DINPUT_H 1
#cmakedefine HAVE_DSOUND_H 1
#cmakedefine HAVE_DXGI_H 1
#cmakedefine HAVE_DXGI1_5_H 1
#cmakedefine HAVE_DXGI1_6_H 1
#cmakedefine HAVE_GAMEINPUT_H 1
#cmakedefine HAVE_IDXGIINFOQUEUE 1
#cmakedefine HAVE_IORINGAPI_H 1
#cmakedefine HAVE_MMDEVICEAPI_H 1
#cmakedefine HAVE_ROAPI_H 1
#cmakedefine HAVE_SENSORSAPI_H 1
#cmakedefine HAVE_SHELLSCALINGAPI_H 1
#cmakedefine HAVE_TPCSHRD_H 1
#cmakedefine HAVE_WINDOWS_GAMING_INPUT_H 1
#cmakedefine HAVE_XINPUT_H 1
#cmakedefine HAVE_XINTERNATOMS 1
#cmakedefine HAVE_WIN32_ENVIRONMENT 1
#cmakedefine HAVE_LASXINTRIN_H 1
#cmakedefine HAVE_LSXINTRIN_H 1
#cmakedefine SDL_RENDER_VITA_GXM_MEMORY_H 1
#cmakedefine SDL_RENDER_VITA_GXM_SHADERS_H 1
#cmakedefine SDL_RENDER_VITA_GXM_TOOLS_H 1
#cmakedefine SDL_RENDER_VITA_GXM_TYPES_H 1

#endif /* SDL_build_config_h_ */
