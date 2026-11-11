set -eu
. tools/go-env.sh
go run -mod=vendor tools/baseline.go
go test -mod=vendor -count=1 ./congestion/live ./packet
go test -mod=vendor -count=1 . -run '^(TestDefaultConfig|TestMarshalUnmarshal)$'
