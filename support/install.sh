#!/bin/sh
set -eu
sh support/env.sh go version
sh support/env.sh go mod download
sh support/env.sh go mod verify
