set -eu
. tools/go-env.sh
go run -mod=readonly tools/baseline.go
go test -mod=readonly -count=1 ./pkg/chunk ./pkg/rawmessage ./pkg/message
