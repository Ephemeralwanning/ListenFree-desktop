#version 440
layout(location=0) in vec2 qt_TexCoord0;
layout(location=0) out vec4 fragColor;
layout(std140,binding=0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec4 sourceRect;
    vec4 clipRect;
    vec2 sourceSize;
    vec4 glowColor;
    float glowRadius;
    float glowAlpha;
    float progress;
    float edgeWidth;
    float sungAlpha;
    float unsungAlpha;
};
layout(binding=1) uniform sampler2D source;
vec4 glyph(vec2 p) {
    if (p.x<clipRect.x || p.x>=clipRect.z || p.y<0.0 || p.y>=sourceSize.y) return vec4(0.0);
    return texture(source,p/sourceSize);
}
void main() {
    vec2 p=sourceRect.xy+qt_TexCoord0*sourceRect.zw;
    vec4 ink=glyph(p);
    float halo=0.0;
    if (glowAlpha>0.001) {
        // Local binomial Gaussian kernel; no per-glyph render target/blur pass.
        // A sparse ring produces visible duplicate strokes at large font sizes.
        float r=max(0.5,glowRadius)*0.5;
        for (int y=-2;y<=2;++y) {
            float wy=y==0?6.0:(abs(y)==1?4.0:1.0);
            for (int x=-2;x<=2;++x) {
                float wx=x==0?6.0:(abs(x)==1?4.0:1.0);
                halo+=glyph(p+vec2(x,y)*r).a*wx*wy/256.0;
            }
        }
    }
    float edge=max(.001,edgeWidth), head=mix(-edge,1.0+edge,progress);
    float mask=1.0-smoothstep(head-edge,head+edge,p.x/sourceSize.x);
    float alpha=mix(unsungAlpha,sungAlpha,mask);
    vec4 shadow=glowColor*(halo*glowAlpha);
    fragColor=(ink+shadow*(1.0-ink.a))*alpha*qt_Opacity;
}
