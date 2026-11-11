#!/bin/sh
set -eu
sh author_support/env.sh go version
sh author_support/env.sh go mod download
sh author_support/env.sh go mod verify
