struct UniformBufferObject {
    float4x4 model;
    float4x4 view;
    float4x4 projection;
    float4x4 inverseView;
    float4x4 normalMatrix;
};

struct Material
{
    float4 baseColorFactor;    
    float  metallicFactor;     
    float  roughnessFactor;    
    float  occlusionStrength;  
    float  _pad0;              

    float3 emissiveFactor;     
    float  _pad1;              

    float  normalScale;        
    float  heightScale;        
    float2 _pad2;              

    float  transmissionFactor; 
    float3 _pad3;              

    float  clearcoatFactor;    
    float  clearcoatRoughness; 
    float2 _pad4;              

    float3 sheenColorFactor;   
    float  sheenRoughness;     
    float  _pad5;              
    float3 __pad;              
};

struct PointLight {
    float4 position;
    float4 color;
};

#define MAX_LIGHTS 16 

struct SceneLightsUBO {
    PointLight lights[MAX_LIGHTS];
    int        lightCount;
    float3     _pad; 
};

struct ShaderTogglesUBO {
    bool useNormalMapping;
    bool useOcclusion;
    bool useEmission;
    bool useLights;
    bool useAmbient;
};


// C-Buffers
[[vk::binding(0, 0)]] ConstantBuffer<UniformBufferObject> ubo;
[[vk::binding(1, 0)]] ConstantBuffer<Material> material;
[[vk::binding(0, 1)]] ConstantBuffer<SceneLightsUBO> scene; 
[[vk::binding(1, 1)]] ConstantBuffer<ShaderTogglesUBO> toggles;


// Samplers
[[vk::binding(2, 0)]] Sampler2D baseColorSampler;
[[vk::binding(3, 0)]] Sampler2D normalSampler;
[[vk::binding(4, 0)]] Sampler2D metallicRoughnessSampler;
[[vk::binding(5, 0)]] Sampler2D occlusionSampler;
[[vk::binding(6, 0)]] Sampler2D emissiveSampler;


struct FragmentInput { 
    float4 position   : SV_Position;
    float3 worldPos   : WORLD_POSITION;
    float2 uv         : TEXCOORD0;
    float3x3 tbn      : TBN;
};

struct FragmentOutput {
    float4 color : SV_Target;
};

// --- PBR Helper Functions (unchanged) ---
static const float PI = 3.14159265359;

float DistributionGGX(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return nom / denom;
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float3 fresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}


FragmentOutput main(FragmentInput input) {
    // --- Material Properties ---
    float4 albedoMap = baseColorSampler.Sample(input.uv);
    float3 albedo = albedoMap.rgb * material.baseColorFactor.rgb;

    float2 metallicRoughness = metallicRoughnessSampler.Sample(input.uv).bg; 
    float metallic = metallicRoughness.x * material.metallicFactor;
    float roughness = metallicRoughness.y * material.roughnessFactor;
    
    float occlusion = 1.0;
    if (toggles.useOcclusion) {
        occlusion = occlusionSampler.Sample(input.uv).r * material.occlusionStrength;
    }

    float3 emissive = float3(0.0, 0.0, 0.0);
    if (toggles.useEmission) {
        emissive = emissiveSampler.Sample(input.uv).rgb * material.emissiveFactor;
    }

    float3 N = normalize(input.tbn[2]); // Default to vertex normal
    if (toggles.useNormalMapping) {
        float3 rawNormal    = normalSampler.Sample(input.uv).xyz * 2.0 - 1.0;
        float3 tangentNormal = rawNormal * 1; //replace 1 with normalScale
        if (length(tangentNormal) > 0.01) {
             N = normalize(mul(input.tbn, tangentNormal));
        }
    }
   
    // --- Common Lighting Vectors ---
    float3 cameraPos = ubo.inverseView[3].xyz;
    float3 V = normalize(cameraPos - input.worldPos);
    
    // --- PBR Calculation ---
    float3 Lo = float3(0.0, 0.0, 0.0); 

    if (toggles.useLights) {
        float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
        
        for (int i = 0; i < scene.lightCount; ++i) {
            PointLight light = scene.lights[i];
            
            float3 L = normalize(light.position.xyz - input.worldPos);
            float3 H = normalize(V + L);
            
            float distance = length(light.position.xyz - input.worldPos);
            float attenuation = 1.0 / (distance * distance + 1);
            
            float3 radiance = light.color.rgb * light.color.a * attenuation;

            float NDF = DistributionGGX(N, H, roughness);
            float G   = GeometrySmith(N, V, L, roughness);
            float3 F  = fresnelSchlick(max(dot(H, V), 0.0), F0);

            float3 nominator    = NDF * G * F;
            float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;
            float3 specular     = nominator / denominator;
            
            float3 kS = F;
            float3 kD = float3(1.0, 1.0, 1.0) - kS;
            kD *= (1.0 - metallic);
            
            float NdotL = max(dot(N, L), 0.0);
            Lo += (kD * albedo / PI + specular) * radiance * NdotL;
        }
    }


    // --- Final Color Assembly ---
    float3 ambient = float3(0.0, 0.0, 0.0);
    if (toggles.useAmbient) {
        ambient = float3(0.03, 0.03, 0.03) * albedo * occlusion;
    }

    float3 finalColor = ambient + Lo + emissive;

    // Tone mapping and gamma correction
    finalColor = finalColor / (finalColor + float3(1.0, 1.0, 1.0));
    finalColor = pow(finalColor, float3(1.0/2.2, 1.0/2.2, 1.0/2.2));

    return FragmentOutput(float4(finalColor, albedoMap.a));
}
