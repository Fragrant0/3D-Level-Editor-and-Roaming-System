sampler2D RT : register(s0);

struct VS_INPUT {
    float4 position : POSITION;
};

struct VS_OUTPUT {
    float4 position : POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT VS(VS_INPUT input) {
    VS_OUTPUT output;
    
    // 强制截断到边界 [-1, 1]，消除浮点数精度误差
    float4 pos = input.position;
    pos.xy = sign(pos.xy);
    
    output.position = float4(pos.xy, 0, 1);
    
    // Ogre 传递的 2D 绘制面片只包含顶点位置，不含纹理坐标 (UV)
    // 我们必须在顶点着色器中根据 NDC 顶点位置手动计算出 UV 纹理坐标
    output.uv.x = 0.5 * (1.0 + pos.x);
    output.uv.y = 0.5 * (1.0 - pos.y);
    
    return output;
}

float4 PS(VS_OUTPUT input) : COLOR {
    float2 uv = input.uv;

    // 1. 边缘色差效果 (Chromatic Aberration) - 适度增强以使其更易观察
    float2 distFromCenter = uv - 0.5;
    float aberrationStrength = dot(distFromCenter, distFromCenter) * 0.015;
    
    float r = tex2D(RT, uv + float2(aberrationStrength, 0.0)).r;
    float g = tex2D(RT, uv).g;
    float b = tex2D(RT, uv - float2(aberrationStrength, 0.0)).b;
    float3 color = float3(r, g, b);

    // 2. 对比度与高饱和度增强 (Vibrant Contrast & Saturation)
    color = (color - 0.5) * 1.35 + 0.5;
    
    // 强力提升饱和度，让霓虹色表现力加倍
    float luma = dot(color, float3(0.299, 0.587, 0.114));
    color = lerp(float3(luma, luma, luma), color, 1.60);

    // 3. 赛博朋克经典高亮霓虹青/品红双色调 (Teal/Magenta)
    float3 teal = float3(0.05, 0.85, 1.0);     // 霓虹青
    float3 magenta = float3(1.0, 0.05, 0.85);  // 霓虹洋红
    
    // 调色板映射系数
    float3 tint = lerp(teal, magenta, saturate(luma * 1.3));
    
    // 屏幕混合 (Screen Blend) 叠加色彩，提亮并引入强烈霓虹感
    float3 graded = 1.0 - (1.0 - color) * (1.0 - tint * 0.55);
    
    // 增大融合权重 (85% 强度)，让霓虹色滤镜变得极其瞩目
    color = lerp(color, graded, 0.85);

    // 4. 平滑暗角 (Smooth Vignette)
    float vignette = uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y);
    vignette = saturate(pow(16.0 * vignette, 0.25));
    color *= vignette;

    return float4(saturate(color), 1.0);
}