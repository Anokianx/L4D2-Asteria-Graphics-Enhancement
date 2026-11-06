#version 450
void main()
{
    uint x=uint(gl_FragCoord.x);
    uint y=uint(gl_FragCoord.y);
    gl_FragDepth=0.125+float(x%8u)*0.0625+float(y%4u)*0.03125+float(gl_SampleID)*0.0078125;
}
