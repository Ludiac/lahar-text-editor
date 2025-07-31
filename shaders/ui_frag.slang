// slang

/**
 * @brief Data received from the vertex shader. Matches VertexOutput.
 */
struct FragmentInput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float2 scale    : TEXCOORD1;

    // --- Style Properties ---
    float4 color    : COLOR0;
    float cornerRadius : TEXCOORD2;
    float  borderWidth  : TEXCOORD3;
    float4 borderColor  : COLOR1;
    float4 gradientColorStart : COLOR2;
    float4 gradientColorEnd   : COLOR3;
    uint   gradientType       : TEXCOORD4;
};

struct FragmentOutput {
    float4 color : SV_Target;
};

/**
 * @brief Calculates the signed distance to a rounded box with potentially different radii for each corner.
 * @param p The coordinate of the current point, centered at (0,0).
 * @param size The dimensions of the box.
 * @param r The corner radii (x=TL, y=TR, z=BR, w=BL).
 * @return The shortest distance from p to the box's boundary. Negative inside, positive outside.
 */
float sdRoundedBox(float2 p, float2 size, float r) {
    float2 q = abs(p) - size + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}


FragmentOutput main(FragmentInput input) {
    FragmentOutput output;

    // --- 1. Coordinate Setup ---
    // Center the UVs so (0,0) is the middle of the rectangle.
    // This simplifies distance calculations.
    float2 p = input.uv - 0.5;

    // The size of our box in this UV-space is (1,1).
    float2 boxSize = float2(1.0, 1.0);

    // Normalize corner radius and border width by the smallest dimension of the rectangle.
    // This makes them behave consistently regardless of aspect ratio.
    float minScale = min(input.scale.x, input.scale.y);
    float cornerRadius = input.cornerRadius / minScale;
    float borderWidth = input.borderWidth / minScale;
    
    // Adjust the box size to account for the largest corner radius on each axis.
    // This prevents visual artifacts when radii are large.
    float2 size = boxSize - cornerRadius;

    // --- 2. SDF Calculation ---
    // Calculate the signed distance from the current fragment to the rectangle's edge.
    float d = sdRoundedBox(p, size * 0.5, cornerRadius);

    // --- 3. Anti-Aliasing Setup ---
    // Calculate the screen-space gradient of the distance. This gives us a measure
    // of how much one pixel covers, which is perfect for anti-aliasing.
    float aa = fwidth(d);

    // --- 4. Determine Fill Color (Solid or Gradient) ---
    float4 fillColor;
    switch (input.gradientType) {
        case 1: // Vertical
            fillColor = lerp(input.gradientColorStart, input.gradientColorEnd, input.uv.y);
            break;
        case 2: // Horizontal
            fillColor = lerp(input.gradientColorStart, input.gradientColorEnd, input.uv.x);
            break;
        case 3: // Radial
            fillColor = lerp(input.gradientColorStart, input.gradientColorEnd, length(p) * 2.0);
            break;
        case 0: // Solid color
        default:
            fillColor = input.gradientColorStart;
            break;
    }

    // --- 5. Color Blending ---
    // Start with a fully transparent color.
    float4 finalColor = float4(0.0, 0.0, 0.0, 0.0);

    // Calculate the opacity of the fill area using smoothstep for a soft, anti-aliased edge.
    // This transitions from 1.0 (inside) to 0.0 (outside) over the width of one pixel.
    float fillAlpha = smoothstep(aa, -aa, d);

    // Blend the fill color.
    finalColor = lerp(finalColor, fillColor, fillAlpha);

    // If there is a border, calculate its opacity.
    if (borderWidth > 0.0) {
        // The border is a band between the outer edge (d) and the inner edge (d + borderWidth).
        // We use smoothstep on both edges and subtract to isolate the anti-aliased band.
        float borderAlpha = smoothstep(aa, -aa, d + borderWidth) - fillAlpha;

        // Blend the border color on top of the fill.
        finalColor = lerp(finalColor, input.borderColor, borderAlpha);
    }

    // --- 6. Final Output ---
    // Apply the instance's base color as a tint and set the final output.
    // The final alpha is determined by the blending of shape and border.
    output.color = finalColor * input.color;
    
    return output;
}
