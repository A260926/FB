#!/bin/sh
set -eu
export GOWORK=off GOTOOLCHAIN=local GOENV=off
export GOCACHE="$PWD/.userfb-build/cache" GOMODCACHE="$PWD/.userfb-build/mod"
case "$(go version)" in
  'go version go1.26.4 '*) ;;
  *) echo 'requires Go 1.26.4' >&2; exit 2 ;;
esac
exec go "$@"
