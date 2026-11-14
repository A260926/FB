# Copyright (c) 2013 Mozilla Foundation and Contributors
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#  - Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#  - Redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
lib_LTLIBRARIES = libwebvttxx.la
noinst_LTLIBRARIES = libwebvttxx-static.la

WEBVTTXX_SOURCES = abstract_parser.cpp file_parser.cpp
WEBVTTXX_CFLAGS = -I$(top_builddir)/include -I$(top_srcdir)/include

libwebvttxx_la_LDFLAGS = -no-undefined -shared
libwebvttxx_la_CPPFLAGS = -DWEBVTTXX_BUILD_LIBRARY=1 $(WEBVTTXX_CFLAGS)
libwebvttxx_la_CXXFLAGS = $(libwebvttxx_la_CPPFLAGS)
libwebvttxx_la_SOURCES = $(WEBVTTXX_SOURCES)
libwebvttxx_la_LIBADD = ../libwebvtt/libwebvtt.la

libwebvttxx_static_la_LDFLAGS = -no-undefined -static
libwebvttxx_static_la_CPPFLAGS = -DWEBVTT_STATIC=1 -DWEBVTTXX_STATIC=1 $(WEBVTTXX_CFLAGS)
libwebvttxx_static_la_CXXFLAGS = $(libwebvttxx_static_la_CPPFLAGS)
libwebvttxx_static_la_SOURCES = $(WEBVTTXX_SOURCES)
libwebvttxx_static_la_LIBADD = $(top_builddir)/src/libwebvtt/libwebvtt-static.la
