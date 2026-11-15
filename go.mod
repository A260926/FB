module github.com/unixpickle/model3d

go 1.18

require (
	github.com/pkg/errors v0.9.1
	github.com/unixpickle/essentials v1.3.0
	github.com/unixpickle/splaytree v1.2.0
)

// Offline packaging adapters; upstream production files remain unchanged.
replace github.com/pkg/errors => ./compat/pkgerrors
replace github.com/unixpickle/essentials => ./compat/essentials
replace github.com/unixpickle/splaytree => ./compat/splaytree
