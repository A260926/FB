#version 430

layout(binding = 2, std140) uniform Control
{
    uint image_index;
    uint sampler_index;
} control;

layout(binding = 7) uniform sampler2D paired[6];

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = textureLod(paired[(control.image_index * 3 + control.sampler_index)], vec2(0.0), 0.0);
}

