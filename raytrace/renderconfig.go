package raytrace

type RenderConfig struct {
	maxDepth        int
	samplesPerPixel int
	isTwoPhase      bool
}

func NewRenderConfig(maxDepth int, samplesPerPixel int, isTwoPhase bool) *RenderConfig {
	return &RenderConfig{maxDepth, samplesPerPixel, isTwoPhase}
}

func (c *RenderConfig) IsTwoPhase() bool {
	return c.isTwoPhase
}
