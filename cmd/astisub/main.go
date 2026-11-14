package main

import (
	"encoding/json"
	"flag"
	"io/ioutil"
	"log"
	"path/filepath"
	"time"

	"github.com/asticode/go-astikit"
	"github.com/asticode/go-astisub"
)

// assembleManifestEntry represents one entry of an assemble manifest
type assembleManifestEntry struct {
	Source   string `json:"source"`
	In       string `json:"in"`
	Out      string `json:"out"`
	Duration string `json:"duration"`
}

// Flags
var (
	actual1          = flag.Duration("a1", 0, "the first actual duration")
	actual2          = flag.Duration("a2", 0, "the second actual duration")
	desired1         = flag.Duration("d1", 0, "the first desired duration")
	desired2         = flag.Duration("d2", 0, "the second desired duration")
	fragmentDuration = flag.Duration("f", 0, "the fragment duration")
	inputPath        = astikit.NewFlagStrings()
	teletextPage     = flag.Int("p", 0, "the teletext page")
	outputPath       = flag.String("o", "", "the output path")
	syncDuration     = flag.Duration("s", 0, "the sync duration")
)

func main() {
	// Init
	cmd := astikit.FlagCmd()
	flag.Var(&inputPath, "i", "the input paths")
	flag.Parse()

	// Assemble reads its clips from a manifest instead of opening the first
	// input path, and must fully succeed before the output is opened.
	if cmd == "assemble" {
		assemble()
		return
	}

	// Validate input path
	if len(*inputPath.Slice) == 0 {
		log.Fatal("Use -i to provide at least one input path")
	}

	// Validate output path
	if len(*outputPath) <= 0 {
		log.Fatal("Use -o to provide an output path")
	}

	// Open first input path
	var sub *astisub.Subtitles
	var err error
	if sub, err = astisub.Open(astisub.Options{Filename: (*inputPath.Slice)[0], Teletext: astisub.TeletextOptions{Page: *teletextPage}}); err != nil {
		log.Fatalf("%s while opening %s", err, (*inputPath.Slice)[0])
	}

	// Switch on subcommand
	switch cmd {
	case "apply-linear-correction":
		// Validate actual and desired durations
		if *actual1 <= 0 {
			log.Fatal("Use -a1 to provide the first actual duration")
		}
		if *desired1 <= 0 {
			log.Fatal("Use -d1 to provide the first desired duration")
		}
		if *actual2 <= 0 {
			log.Fatal("Use -a2 to provide the second actual duration")
		}
		if *desired2 <= 0 {
			log.Fatal("Use -d2 to provide the second desired duration")
		}

		// Apply linear correction
		sub.ApplyLinearCorrection(*actual1, *desired1, *actual2, *desired2)

		// Write
		if err = sub.Write(*outputPath); err != nil {
			log.Fatalf("%s while writing to %s", err, *outputPath)
		}
	case "convert":
		// Write
		if err = sub.Write(*outputPath); err != nil {
			log.Fatalf("%s while writing to %s", err, *outputPath)
		}
	case "fragment":
		// Validate fragment duration
		if *fragmentDuration <= 0 {
			log.Fatal("Use -f to provide a fragment duration")
		}

		// Fragment
		sub.Fragment(*fragmentDuration)

		// Write
		if err = sub.Write(*outputPath); err != nil {
			log.Fatalf("%s while writing to %s", err, *outputPath)
		}
	case "merge":
		// Validate second input path
		if len(*inputPath.Slice) == 1 {
			log.Fatal("Use -i to provide at least two input paths")
		}

		// Open second input path
		var sub2 *astisub.Subtitles
		if sub2, err = astisub.Open(astisub.Options{Filename: (*inputPath.Slice)[1], Teletext: astisub.TeletextOptions{Page: *teletextPage}}); err != nil {
			log.Fatalf("%s while opening %s", err, (*inputPath.Slice)[1])
		}

		// Merge
		sub.Merge(sub2)

		// Write
		if err = sub.Write(*outputPath); err != nil {
			log.Fatalf("%s while writing to %s", err, *outputPath)
		}
	case "optimize":
		// Optimize
		sub.Optimize()

		// Write
		if err = sub.Write(*outputPath); err != nil {
			log.Fatalf("%s while writing to %s", err, *outputPath)
		}
	case "sync":
		// Validate sync duration
		if *syncDuration == 0 {
			log.Fatal("Use -s to provide a sync duration")
		}

		// Fragment
		sub.Add(*syncDuration)

		// Write
		if err = sub.Write(*outputPath); err != nil {
			log.Fatalf("%s while writing to %s", err, *outputPath)
		}
	case "unfragment":
		// Unfragment
		sub.Unfragment()

		// Write
		if err = sub.Write(*outputPath); err != nil {
			log.Fatalf("%s while writing to %s", err, *outputPath)
		}
	default:
		log.Fatalf("Invalid subcommand %s", cmd)
	}
}

// assemble loads an assemble manifest and writes the assembled subtitle track.
// The whole manifest is read and assembled before the output is opened, so a
// bad manifest never truncates an existing output file.
func assemble() {
	// Validate the paths
	if len(*inputPath.Slice) == 0 {
		log.Fatal("Use -i to provide an assemble manifest path")
	}
	if len(*outputPath) <= 0 {
		log.Fatal("Use -o to provide an output path")
	}
	manifestPath := (*inputPath.Slice)[0]
	manifestDir := filepath.Dir(manifestPath)

	// Read the manifest
	data, err := ioutil.ReadFile(manifestPath)
	if err != nil {
		log.Fatalf("%s while reading %s", err, manifestPath)
	}
	var entries []assembleManifestEntry
	if err = json.Unmarshal(data, &entries); err != nil {
		log.Fatalf("%s while parsing %s", err, manifestPath)
	}

	// Load each distinct source once. Identity is the normalized absolute path
	// so that repeated references to the same file share one *Subtitles.
	sources := make(map[string]*astisub.Subtitles)
	clips := make([]astisub.EditClip, 0, len(entries))
	for _, entry := range entries {
		sourcePath := entry.Source
		if !filepath.IsAbs(sourcePath) {
			sourcePath = filepath.Join(manifestDir, sourcePath)
		}
		absolutePath, err := filepath.Abs(sourcePath)
		if err != nil {
			log.Fatalf("%s while resolving %s", err, entry.Source)
		}

		var sub *astisub.Subtitles
		if existing, ok := sources[absolutePath]; ok {
			sub = existing
		} else {
			if sub, err = astisub.Open(astisub.Options{Filename: absolutePath, Teletext: astisub.TeletextOptions{Page: *teletextPage}}); err != nil {
				log.Fatalf("%s while opening %s", err, absolutePath)
			}
			sources[absolutePath] = sub
		}

		in, err := time.ParseDuration(entry.In)
		if err != nil {
			log.Fatalf("%s while parsing in %q", err, entry.In)
		}
		out, err := time.ParseDuration(entry.Out)
		if err != nil {
			log.Fatalf("%s while parsing out %q", err, entry.Out)
		}
		duration, err := time.ParseDuration(entry.Duration)
		if err != nil {
			log.Fatalf("%s while parsing duration %q", err, entry.Duration)
		}

		clips = append(clips, astisub.EditClip{
			Source:   sub,
			In:       in,
			Out:      out,
			Duration: duration,
		})
	}

	// The whole manifest must be read and assembled before the output is
	// opened: a bad manifest must never truncate an existing file.
	assembled, err := astisub.Assemble(clips)
	if err != nil {
		log.Fatalf("%s while assembling clips", err)
	}

	// Write
	if err = assembled.Write(*outputPath); err != nil {
		log.Fatalf("%s while writing to %s", err, *outputPath)
	}
}
