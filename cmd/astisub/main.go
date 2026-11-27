package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io/ioutil"
	"log"
	"path/filepath"
	"time"

	"github.com/asticode/go-astikit"
	"github.com/asticode/go-astisub"
)

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

	// Validate input path
	if len(*inputPath.Slice) == 0 {
		log.Fatal("Use -i to provide at least one input path")
	}

	// Validate output path
	if len(*outputPath) <= 0 {
		log.Fatal("Use -o to provide an output path")
	}

	// The assemble subcommand reads an edit list manifest instead of subtitles
	if cmd == "assemble" {
		if err := assemble((*inputPath.Slice)[0], *outputPath); err != nil {
			log.Fatal(err)
		}
		return
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

// assembleManifestEntry represents one entry of an assemble manifest
type assembleManifestEntry struct {
	Source   string `json:"source"`
	In       string `json:"in"`
	Out      string `json:"out"`
	Duration string `json:"duration"`
}

// assemble reads an edit list manifest, assembles its clips and writes the
// result. The manifest is a JSON array of entries with source, in, out and
// duration string fields; times use Go duration syntax and sources are
// resolved relative to the manifest's directory. The whole manifest is read
// and assembled before the output is opened, so a bad manifest cannot
// truncate an existing file.
func assemble(manifestPath, outputPath string) (err error) {
	// Read the whole manifest
	var b []byte
	if b, err = ioutil.ReadFile(manifestPath); err != nil {
		err = fmt.Errorf("reading manifest %s failed: %w", manifestPath, err)
		return
	}
	var entries []assembleManifestEntry
	if err = json.Unmarshal(b, &entries); err != nil {
		err = fmt.Errorf("parsing manifest %s failed: %w", manifestPath, err)
		return
	}

	// Build clips, loading each canonical source path only once so it keeps a
	// single source identity
	var clips []astisub.EditClip
	var sources = make(map[string]*astisub.Subtitles)
	var manifestDir = filepath.Dir(manifestPath)
	for i, e := range entries {
		var in, out, duration time.Duration
		if in, err = time.ParseDuration(e.In); err != nil {
			err = fmt.Errorf("parsing in of manifest entry %d failed: %w", i, err)
			return
		}
		if out, err = time.ParseDuration(e.Out); err != nil {
			err = fmt.Errorf("parsing out of manifest entry %d failed: %w", i, err)
			return
		}
		if duration, err = time.ParseDuration(e.Duration); err != nil {
			err = fmt.Errorf("parsing duration of manifest entry %d failed: %w", i, err)
			return
		}
		var sourcePath = e.Source
		if !filepath.IsAbs(sourcePath) {
			sourcePath = filepath.Join(manifestDir, sourcePath)
		}
		if sourcePath, err = filepath.Abs(sourcePath); err != nil {
			err = fmt.Errorf("resolving source of manifest entry %d failed: %w", i, err)
			return
		}
		source, ok := sources[sourcePath]
		if !ok {
			if source, err = astisub.OpenFile(sourcePath); err != nil {
				err = fmt.Errorf("opening source of manifest entry %d failed: %w", i, err)
				return
			}
			sources[sourcePath] = source
		}
		clips = append(clips, astisub.EditClip{Source: source, In: in, Out: out, Duration: duration})
	}

	// Assemble before opening the output
	var sub *astisub.Subtitles
	if sub, err = astisub.Assemble(clips); err != nil {
		err = fmt.Errorf("assembling failed: %w", err)
		return
	}

	// Write
	if err = sub.Write(outputPath); err != nil {
		err = fmt.Errorf("writing to %s failed: %w", outputPath, err)
		return
	}
	return
}
