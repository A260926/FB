package race

// Version information for the Pure-Go Race Detector.
const (
	// Version is the current version of the race detector runtime.
	Version = "0.1.0"

	// VersionMajor is the major version number.
	VersionMajor = 0

	// VersionMinor is the minor version number.
	VersionMinor = 1

	// VersionPatch is the patch version number.
	VersionPatch = 0
)

// Info provides runtime information about the race detector.
type Info struct {
	// Version is the runtime version string.
	Version string

	// Algorithm is the race detection algorithm used.
	Algorithm string

	// Enabled indicates whether race detection is active.
	Enabled bool
}

// GetInfo returns information about the race detector runtime.
//
// Example:
//
//	info := race.GetInfo()
//	fmt.Printf("Race Detector %s (%s)\n", info.Version, info.Algorithm)
func GetInfo() Info {
	return Info{
		Version:   Version,
		Algorithm: "FastTrack (PLDI 2009)",
		Enabled:   true, // Always enabled when using this package
	}
}
