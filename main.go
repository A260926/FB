package main

import (
	"fmt"
	"go/build"
	"log"
	"os"
	"path"

	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/scenes"
	"gopkg.in/alecthomas/kingpin.v2"

	"math/rand"
	"path/filepath"
	"time"

	"github.com/hajimehoshi/ebiten"
	"github.com/hajimehoshi/ebiten/ebitenutil"
)

var (
	outputDir = kingpin.Flag("outpath", "Output file path.").Short('o').ExistingDir()
	showfps   = kingpin.Flag("showfps", "Show FPS").Short('f').Bool()

	isTwoPhase   = kingpin.Flag("twophase", "Two Phase Rendering").Default("true").Bool()
	maxDepth     = kingpin.Flag("maxdepth", "Max ray recursion depth").Default("50").Int()
	numSamples   = kingpin.Flag("numsamples", "Num samples per pixel").Default("10").Int()
	imageWidth   = kingpin.Flag("imagewidth", "Width of image").Default("300").Int()
	imageHeight  = kingpin.Flag("imageHeight", "Height of image").Default("300").Int()
	displayScale = kingpin.Flag("displayscale", "Scale from image to screen").Default("2").Int()
)

// importPathToDir resolves the absolute path from importPath.
// There doesn't need to be a valid Go package inside that import path,
// but the directory must exist.
func importPathToDir(importPath string) (string, error) {
	p, err := build.Import(importPath, "", build.FindOnly)
	if err != nil {
		return "", err
	}
	return p.Dir, nil
}

// Set the working directory to the root of Go package, so that its assets can be accessed.
func init() {
	dir, err := importPathToDir("github.com/markzuber/zgotrace")
	if err != nil {
		log.Fatalln("Unable to find Go package in your GOPATH, it's needed to load assets:", err)
	}
	dir = path.Join(dir, "images")
	err = os.Chdir(dir)
	if err != nil {
		log.Panicln("os.Chdir:", err)
	}
}

type ebitenRender struct {
	offscreen      *ebiten.Image
	pixelBuffer    *raytrace.PixelBuffer
	screenScale    int
	outputFilePath string
	showFps        bool
}

func newEbitenRender(imageWidth int, imageHeight int, screenScale int, outputFilePath string, showFps bool) *ebitenRender {
	rand.Seed(time.Now().UnixNano())
	offscreen, _ := ebiten.NewImage(imageWidth, imageHeight, ebiten.FilterDefault)
	var pixelBuffer = raytrace.NewPixelBuffer(imageWidth, imageHeight)

	return &ebitenRender{offscreen, pixelBuffer, screenScale, outputFilePath, showFps}
}

func (er *ebitenRender) Update(screen *ebiten.Image) error {
	ebImg, _ := ebiten.NewImageFromImage(er.pixelBuffer.GetImage(), ebiten.FilterDefault)

	if ebiten.IsDrawingSkipped() {
		return nil
	}

	screen.DrawImage(ebImg, nil)

	if er.showFps {
		ebitenutil.DebugPrint(screen, fmt.Sprintf("TPS: %0.2f", ebiten.CurrentTPS()))
	}

	return nil
}

func (er *ebitenRender) Draw(screen *ebiten.Image) error {
	return nil
}

func (er *ebitenRender) Layout(outsideWidth int, outsideHeight int) (screenWidth, screenHeight int) {
	return er.pixelBuffer.Width(), er.pixelBuffer.Height()
}

func (er *ebitenRender) StartDisplay(windowTitle string) {
	ebiten.SetRunnableOnUnfocused(true)
	ebiten.SetWindowTitle(windowTitle)
	ebiten.SetWindowSize(er.pixelBuffer.Width()*er.screenScale, er.pixelBuffer.Height()*er.screenScale)
	if err := ebiten.RunGame(er); err != nil {
		log.Fatal(err)
	}
}

func (er *ebitenRender) DoRender(rayTracer raytrace.RayTracer, renderConfig *raytrace.RenderConfig, scene raytrace.Scene, imageOutputFilename string) {
	var render = raytrace.NewRenderer(renderConfig)

	fmt.Printf("Rendering for %v started at: %v\n", imageOutputFilename, time.Now())
	render.Render(rayTracer, er.pixelBuffer, scene, renderConfig)
	outFilePath := filepath.Join(er.outputFilePath, imageOutputFilename)
	fmt.Printf("Saving image to %v ", outFilePath)
	er.pixelBuffer.SavePng(outFilePath)
	fmt.Println("...done")
}

type SceneInfo struct {
	scene               raytrace.Scene
	imageOutputFilename string
}

func (er *ebitenRender) DoRenderMulti(rayTracer raytrace.RayTracer, renderConfig *raytrace.RenderConfig, sceneInfos []SceneInfo) {
	for _, si := range sceneInfos {
		er.DoRender(rayTracer, renderConfig, si.scene, si.imageOutputFilename)
	}
}

func getFileOutputDir() string {
	var fileOutputDir string

	if *outputDir == "" {
		fileOutputDir, _ = os.UserHomeDir()
	} else {
		fileOutputDir = *outputDir
	}
	return fileOutputDir
}

func main() {
	kingpin.Parse()

	fileOutputDir := getFileOutputDir()
	fmt.Printf("fileoutputdir: %v\n", fileOutputDir)

	ebRen := newEbitenRender(*imageWidth, *imageHeight, *displayScale, fileOutputDir, *showfps)

	var renderConfig = raytrace.NewRenderConfig(*maxDepth, *numSamples, *isTwoPhase)
	var rayTracer = raytrace.NewMonteCarloTracer()

	go ebRen.DoRenderMulti(rayTracer, renderConfig, []SceneInfo{
		{scenes.CreateCornellBoxScene(), "cornellbox.png"},
		{scenes.CreateCornellBoxWithSmokeScene(), "cornellbox_withsmoke.png"},
		{scenes.CreateManySpheresScene(), "manyspheres.png"},
		{scenes.CreateImageTextureScene("globetex.jpg"), "imagetex.png"}})

	ebRen.StartDisplay("Zube's RayTracer")
}
