struct FragmentInput {
    float4 position : SV_Position;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
};

struct FragmentOutput {
    float4 color : SV_Target;
};

FragmentOutput main(FragmentInput input) {
    FragmentOutput output;
    
    // For now, we just output the color.
    // In the future, this is where we'd calculate rounded corners, borders, etc.
    // using the input.uv coordinates and Signed Distance Fields.
    // For example: if (distance > corner_radius) discard;
    
    output.color = input.color;
    
    return output;
}
