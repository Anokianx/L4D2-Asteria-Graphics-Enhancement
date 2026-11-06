#version 450

layout(push_constant) uniform Parameters
{
    int shiftX;
    int shiftY;
} parameters;

layout(location = 0) out vec4 sceneColor;

uint pixelHash(uint x, uint y)
{
    uint hash = x * 0x9e3779b9u + y * 0x85ebca6bu + 17u;
    hash ^= hash >> 16;
    hash *= 0x7feb352du;
    hash ^= hash >> 15;
    return hash;
}

void main()
{
    uvec2 pixel = uvec2(gl_FragCoord.xy);
    ivec2 local = ivec2(pixel) - ivec2(220 + parameters.shiftX, 108 + parameters.shiftY);
    bool object = local.x >= 0 && local.x < 112 && local.y >= 0 && local.y < 136;
    uint hash = object ? pixelHash(uint(local.x), uint(local.y)) : pixelHash(pixel.x, pixel.y);
    uvec3 rgb = object
        ? uvec3(180u + hash % 70u, 70u + (hash >> 8) % 70u, 15u + (hash >> 16) % 40u)
        : uvec3(15u + hash % 50u, 25u + (hash >> 8) % 50u, 60u + (hash >> 16) % 50u);
    sceneColor = vec4(vec3(rgb) / 255.0, 1);
    gl_FragDepth = (object ? 0.4 : 0.8) + float(gl_SampleID) * 0.00001;
}
