#version 300 es
precision mediump float;

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;

uniform vec4 colDiffuse;
uniform sampler2D shadowMap;
uniform mat4 lightMat;
uniform vec3 leftFootPos;
uniform vec3 rightFootPos;

out vec4 finalColor;

float calculateShadow(vec3 fragPos)
{
    vec4 lightSpacePos = lightMat * vec4(fragPos, 1.0);
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords = projCoords * 0.5 + 0.5;
    
    if (projCoords.z > 1.0) return 0.0;
    
    float shadow = 0.0;
    float bias = 0.005;
    float currentDepth = projCoords.z;
    
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMap, 0));
    for(int x = -1; x <= 1; ++x)
    {
        for(int y = -1; y <= 1; ++y)
        {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r; 
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;        
        }    
    }
    return shadow / 9.0;
}

void main()
{
    // Basic checkerboard pattern based on world position
    float total = floor(fragPosition.x * 2.0f) +
                  floor(fragPosition.z * 2.0f);
                  
    vec3 checkColor = mod(total, 2.0f) == 0.0f ? 
        vec3(0.5f, 0.5f, 0.5f) : 
        vec3(0.85f, 0.85f, 0.85f);

    // Studio lighting (3-point light simulation)
    vec3 normal = normalize(fragNormal);
    
    vec3 keyDir = normalize(vec3(1.0f, 2.0f, 1.0f));
    float key = max(dot(normal, keyDir), 0.0f);
    
    vec3 fillDir = normalize(vec3(-1.0f, 1.0f, 1.0f));
    float fill = max(dot(normal, fillDir), 0.0f);
    
    vec3 backDir = normalize(vec3(0.0f, 0.5f, -1.0f));
    float back = max(dot(normal, backDir), 0.0f);
    
    vec3 studioLight = key * vec3(1.0f, 0.95f, 0.9f) * 0.8f + 
                       fill * vec3(0.8f, 0.85f, 1.0f) * 0.4f + 
                       back * vec3(1.0f, 1.0f, 1.0f) * 0.3f;
    
    studioLight += vec3(0.2f);
    
    // Final RGB with studio light blend
    vec3 finalRGB = mix(checkColor, studioLight, 0.2f);
    
    // --- New: Shadows and Contact AO ---
    
    // 1. Shadows from directional light
    float shadow = calculateShadow(fragPosition);
    finalRGB *= (1.0 - shadow * 0.9); // 50% shadow intensity
    
    // 2. Contact AO (Ambient Occlusion) under feet
    // Use distance to feet to create a soft darkening effect
    float distL = length(fragPosition - leftFootPos);
    float distR = length(fragPosition - rightFootPos);
    
    // AO blob strength: stronger when closer, fades out at 0.5 units
    float blobL = 1.0 - smoothstep(0.0, 0.5, distL);
    float blobR = 1.0 - smoothstep(0.0, 0.5, distR);
    float contactAO = max(blobL, blobR);
    
    // Apply AO: darken ground by up to 60% directly under feet
    finalRGB *= (1.0 - contactAO * 0.1);
    
    finalColor = vec4(finalRGB, 1.0f);
}
