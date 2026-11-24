#version 430

layout(binding = 2, std140) uniform Control
{
    uint image_index;
    uint sampler_index;
} control;

layout(binding = 7) uniform sampler2D paired;

layout(location = 0) out vec4 out_color;

vec4 leaf(sampler2D SPIRV_Cross_Combined_34_35)
{
    return textureLod(SPIRV_Cross_Combined_34_35, vec2(0.0), 0.0);
}

vec4 wrapper(uint _41, uint _42, sampler2D SPIRV_Cross_Combined_39_40)
{
    return leaf(SPIRV_Cross_Combined_39_40);
}

void main()
{
    out_color = wrapper(control.image_index, control.sampler_index, paired);
}

