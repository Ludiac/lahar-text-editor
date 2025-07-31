// slang
struct PushConstants {
  float4x4 projection;
};

/**
 * @struct UIInstanceData
 * @brief Per-instance data for each UI element.
 *
 * This struct now includes properties for styling the rectangle,
 * such as corner radii, border properties, and gradient settings.
 * This data is sent from the CPU to the GPU for each rectangle you want to draw.
 */
struct UIInstanceData {
    // --- Transform ---
    float2 screenPos;
    float2 scale;
    float4 color;       // Base tint color, multiplied with the final fragment color.

    float4 borderColor;
    float4 gradientColorStart;
    float4 gradientColorEnd;
    
    float cornerRadius;
    float borderWidth;

    /**
     * @brief Type of gradient to apply.
     * 0: Solid Color (uses gradientColorStart)
     * 1: Vertical Linear
     * 2: Horizontal Linear
     * 3: Radial
     */
    uint gradientType;

    float z_layer;

    float4 padding;
    float4 padding2;
};

/**
 * @struct VertexOutput
 * @brief Data passed from the vertex shader to the fragment shader.
 *
 * We pass all the styling information through so the fragment shader
 * can correctly render the final shape.
 */
struct VertexOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0; // UV coordinates (0 to 1) across the quad.
    float2 scale    : TEXCOORD1; // Pass scale for aspect ratio correction in the fragment shader.

    // --- Style Properties ---
    float4 color    : COLOR0;    // Base tint color.
    float cornerRadius : TEXCOORD2;
    float  borderWidth  : TEXCOORD3;
    float4 borderColor  : COLOR1;
    float4 gradientColorStart : COLOR2;
    float4 gradientColorEnd   : COLOR3;
    uint   gradientType       : TEXCOORD4;
};

// The SSBO with our instance data.
[[vk::binding(0, 0)]]
StructuredBuffer<UIInstanceData> instanceBuffer;

[[vk::push_constant]]
PushConstants push_constants;

// Matches the static unit quad vertex format (0,0 to 1,1)
struct VertexInput {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

VertexOutput main(VertexInput input, uint instanceID : SV_InstanceID) {
    VertexOutput output;

    // 1. Fetch the data for the current UI quad instance.
    UIInstanceData instance = instanceBuffer[instanceID];

    // 2. Compute the final vertex position in screen-space.
    float2 finalPos = instance.screenPos + (input.pos * instance.scale);

    // 3. Project into clip space, using the per-instance z_layer.
    output.position = mul(
        push_constants.projection,
        float4(finalPos, instance.z_layer, 1.0)
    );

    // 4. Pass all necessary data to the fragment shader.
    output.uv = input.uv;
    output.scale = instance.scale;
    output.color = instance.color;
    output.cornerRadius = instance.cornerRadius;
    output.borderWidth = instance.borderWidth;
    output.borderColor = instance.borderColor;
    output.gradientColorStart = instance.gradientColorStart;
    output.gradientColorEnd = instance.gradientColorEnd;
    output.gradientType = instance.gradientType;

    return output;
}
