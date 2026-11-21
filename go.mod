module github.com/markzuber/zgotrace

go 1.20

require github.com/go-gl/mathgl v1.0.0

require golang.org/x/image v0.0.0-20190321063152-3fc05d484e9f // indirect

replace github.com/go-gl/mathgl => ./third_party/mathgl

replace golang.org/x/image => ./third_party/ximage
