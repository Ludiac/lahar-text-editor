#define AA_NONE 0
#define AA_SMOOTHSTEP 1
#define AA_LINEAR_CLAMP 2
#define AA_STAGES 3

struct TextPushConstants2D {
    float4x4 projection;
    float sdf_weight;
    float pxRangeDirectAdditive;
    uint anti_aliasing_mode;
    float start_fade_px;
    float end_fade_px;
    uint num_stages;
    uint rounding_direction;
    float _padding[9];
};

struct VertexToFragment {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float pxRange : TEXCOORD1;
};

struct FragmentOutput {
    float4 color : SV_Target0;
};

[[vk::push_constant]]
TextPushConstants2D push_constants;

[[vk::binding(0, 1)]]
Sampler2D fontAtlas;

// Finds the median of three values, used for Multi-channel Signed Distance Fields.
float median(float r, float g, float b) {
    return max(min(r, g), min(max(r, g), b));
}

FragmentOutput main(VertexToFragment input) {
    FragmentOutput output;

    // Sample the 4-channel MTSDF texture (MSDF in RGB, true SDF in Alpha).
    float4 texSample = fontAtlas.Sample(input.uv);

    // The texture values are in the [0, 1] range, where 0.5 is the contour edge.
    float sd = median(texSample.r, texSample.g, texSample.b);
    float sdf = texSample.a;

    // Mix the MSDF and SDF values based on a weight.
    float msd = lerp(sd, sdf, push_constants.sdf_weight);

    // Convert the distance field value to screen-space pixel distance.
    float screenPxDistance = (input.pxRange + push_constants.pxRangeDirectAdditive) * (msd - 0.5);

    float opacity;

    // Select anti-aliasing method.
    switch (push_constants.anti_aliasing_mode) {
        case AA_SMOOTHSTEP: {
            // High-quality anti-aliasing using smoothstep over the screen-space gradient.
            float screenPxGradient = fwidth(screenPxDistance);
            opacity = smoothstep(-screenPxGradient, screenPxGradient, screenPxDistance);
            break;
        }
        case AA_LINEAR_CLAMP: {
            // A linear transition between two pixel boundaries.
            float start_fade = -push_constants.start_fade_px;
            float end_fade = push_constants.end_fade_px;
            float unclamped_opacity = (screenPxDistance - start_fade) / (end_fade - start_fade);
            opacity = clamp(unclamped_opacity, 0.0, 1.0);
            break;
        }
        case AA_STAGES: {
            // A simple staged opacity, useful for debugging or specific effects.
            // The full implementation for this is not yet complete.
            opacity = clamp(screenPxDistance + 0.5, 0.0, 1.0);
            break;
        }
        case AA_NONE:
        default: {
            // Sharp, aliased cutoff at the contour edge.
            opacity = step(0.0, screenPxDistance);
            // Staged anti-aliasing with configurable number of stages and rounding direction.                                                                                                               │
            // Ensures that the number of stages is at least 1 to prevent division by zero.                                                                                                                  │
            // uint num_stages = max(1u, push_constants.num_stages);                                                                                                                                         │
            // float step_value;                                                                                                                                                                             │
            //                                                                                                                                                                                               │
            // if (num_stages == 1) {                                                                                                                                                                        │
            //     // If only one stage, it's effectively a sharp cutoff (binary opacity: 0 or 1).                                                                                                           │
            //     opacity = step(0.0, screenPxDistance);                                                                                                                                                    │
            // } else {                                                                                                                                                                                      │
            //     float baseOpacity = clamp(screenPxDistance + 0.5, 0.0, 1.0);                                                                                                                              │
            //     step_value = 1.0 / (float)(num_stages - 1);                                                                                                                                               │
            //                                                                                                                                                                                               │
            //     float target_index_float = baseOpacity / step_value;                                                                                                                                      │
            //     float rounded_index;                                                                                                                                                                      │
            //                                                                                                                                                                                               │
            //     // Apply rounding based on the specified direction.                                                                                                                                       │
            //     if (push_constants.rounding_direction == 0u) { // Round down (floor)                                                                                                                      │
            //         rounded_index = floor(target_index_float);                                                                                                                                            │
            //     } else if (push_constants.rounding_direction == 1u) { // Round up (ceil)                                                                                                                  │
            //         rounded_index = ceil(target_index_float);                                                                                                                                             │
            //     } else { // Default or 2u: Round to nearest (round)                                                                                                                                       │
            //         rounded_index = round(target_index_float);                                                                                                                                            │
            //     }                                                                                                                                                                                         │
            //                                                                                                                                                                                               │
            //     // Clamp the rounded index to ensure it stays within the valid stage range [0, num_stages - 1].                                                                                           │
            //     rounded_index = clamp(rounded_index, 0.0, (float)(num_stages - 1));                                                                                                                       │
            //                                                                                                                                                                                               │
            //     // Calculate the final quantized opacity value.                                                                                                                                           │
            //     opacity = rounded_index * step_value;                                                                                                                                                     │
            // }        
            break;
        }
    }

    // Apply the final color with the computed opacity.
    output.color = float4(input.color.rgb, input.color.a * opacity);

    return output;
}
