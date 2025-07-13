struct UniformBufferObject {
    float4x4 model;
    float4x4 view;
    float4x4 projection;
    float4x4 inverseView; 
    float4x4 normalMatrix;
};

// NEW: UBO for shader toggles
struct ShaderTogglesUBO {
    bool useNormalMapping;
    bool useOcclusion;
    bool useEmission;
    bool useLights;
    bool useAmbient;
};


// Input from the vertex buffer
struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float2 uv       : TEXCOORD0;
    [[vk::location(3)]] float4 tangent  : TANGENT;
};

// Output to the fragment shader
struct VertexOutput {
    float4 position   : SV_Position;
    float3 worldPos   : WORLD_POSITION;
    float2 uv         : TEXCOORD0;
    float3x3 tbn      : TBN;
};


// C-Buffers
[[vk::binding(0, 0)]] ConstantBuffer<UniformBufferObject> ubo;
[[vk::binding(1, 1)]] ConstantBuffer<ShaderTogglesUBO> toggles; // NEW: set 1, binding 1


VertexOutput main(VertexInput input) {
    VertexOutput output;

    // Calculate the vertex's position in world space
    float4 worldPos4 = mul(ubo.model, float4(input.position, 1.0));
    output.worldPos = worldPos4.xyz;

    // Transform the world position to clip space for the rasterizer
    output.position = mul(ubo.projection, mul(ubo.view, worldPos4));

    // Pass the UV coordinates directly to the fragment shader
    output.uv = input.uv;

    // Default TBN matrix (identity)
    float3 N = normalize(mul((float3x3)ubo.normalMatrix, input.normal));
    float3 T = normalize(mul((float3x3)ubo.normalMatrix, input.tangent.xyz));
    T = normalize(T - dot(T, N) * N);
    float3 B = cross(N, T) * input.tangent.w;
    output.tbn = float3x3(T, B, N);

    return output;
}
