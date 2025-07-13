struct PushConstants {
  float4x4 projection;
  float _padding[16];
};

// Matches the static unit quad vertex format
struct VertexInput {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0; // Pass UVs for future SDF calculations
};

// Per-instance data coming from the SSBO
struct UIInstanceData {
    float2 screenPos;
    float2 scale;
    float4 color;
    float z_layer;
    float _padding[3];
    // Future SDF parameters:
    float cornerRadius;
    float borderWidth;
    float _padding2[2];
};

// Output to the fragment shader
struct VertexOutput {
    float4 position : SV_Position;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0; // Pass UVs through
};

// The SSBO with our instance data.
// We'll bind this at Set 0, Binding 0 for the UI pipeline.
[[vk::binding(0, 0)]]
StructuredBuffer<UIInstanceData> instanceBuffer;

[[vk::push_constant]]
PushConstants push_constants;

VertexOutput main(VertexInput input, uint instanceID : SV_InstanceID) {
    VertexOutput output;

    // 1. Fetch the data for the current UI quad instance
    UIInstanceData instance = instanceBuffer[instanceID];

    // 2. Compute the final position in screen-space
    float2 finalPos = instance.screenPos + (input.pos * instance.scale);

    // 3. Project into clip space, keeping per-instance z_layer
    output.position = mul(
        push_constants.projection,
        float4(finalPos, instance.z_layer, 1.0)
    );

    // 4. Pass through static UVs from the vertex buffer
    output.uv = input.uv;

    // 5. Pass through per-instance color to the fragment shader
    output.color = instance.color;

    return output;
}
