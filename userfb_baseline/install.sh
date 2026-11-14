#!/bin/sh
set -eu
export GEM_HOME="${TMPDIR:-/tmp}/userfb-z1227-gems"
export GEM_PATH="$GEM_HOME"
export GEM_SPEC_CACHE="$GEM_HOME/spec-cache"
for spec in sexp_processor:4.11.0 ruby_parser:3.11.0 ruby2ruby:2.4.4 safe_yaml:1.0.5 erubis:2.7.0 minitest:5.11.3; do
 name=${spec%:*}; version=${spec#*:}
 if ! gem list --local --exact "$name" --version "$version" --installed >/dev/null; then
  mkdir -p "$GEM_HOME/downloads"
  curl -fLsS "https://rubygems.org/downloads/$name-$version.gem" -o "$GEM_HOME/downloads/$name-$version.gem"
  expected=$(awk -v f="$name-$version.gem" '$2==f {print $1}' userfb_baseline/gems.sha256)
  actual=$(shasum -a 256 "$GEM_HOME/downloads/$name-$version.gem" | awk '{print $1}')
  test "$actual" = "$expected"
  gem install --local --install-dir "$GEM_HOME" --no-document --ignore-dependencies "$GEM_HOME/downloads/$name-$version.gem"
 fi
done
