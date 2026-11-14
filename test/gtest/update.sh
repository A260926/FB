#!/bin/sh

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

repo_url="http://googletest.googlecode.com/svn/trunk/"
start=$PWD
gtest=$(cd "$(dirname "$0")" && pwd)
rev=
repo_dir="$repo_url"
temp=$gtest/gtest-src

die() {
  if [ -n "$1" ]; then
    echo "$1"
  fi
  exit 1
}

svn --version 2>&1 >/dev/null
if [ $? -ne 0 ]; then
  die "Subversion does not seem to be installed or in your PATH"
fi

if [ $# -gt 0 ]; then
  if [ $# -gt 1 ]; then
    #DIR and REV
    repo_dir="$1"
    rev="$2"
  else
    #Just REV
    rev="-r $1"
  fi
fi

svn export $rev $repo_dir $temp || \
  die "failed to export gtest repository"

if [ ! -f "$temp/scripts/fuse_gtest_files.py" ]; then
  die "gtest repository missing fuse_gtest_files.py"
fi

rm -rf $gtest/gtest $gtest/*.cc || \
  die "failed to cleanup old gtest sources"

cd $temp/scripts
python fuse_gtest_files.py $gtest/
mv $gtest/gtest/*.cc $gtest/ || \
  die "failed to move gtest source files"
cp $temp/src/gtest_main.cc $gtest/ || \
  die "failed to copy gtest_main.cc"
cd $gtest
rm -rf $temp || \
  die "failed to remove temp directory"
cd $start

git --version 2>&1 >/dev/null #Safely ignore error
if [ $? -eq 0 -a -d "$start/.git" ]; then
  git add "$gtest/gtest/*.h" "$gtest/*.cc"
fi