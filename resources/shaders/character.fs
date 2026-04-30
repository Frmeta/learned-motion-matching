#version 300 es
precision mediump float;

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;

uniform vec4 colDiffuse;
uniform sampler2D shadowMap;
uniform mat4 lightMat;

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
    
    // Simple 3x3 PCF (Percentage Closer Filtering)
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
    vec3 light_dir = normalize(vec3(0.25, -0.8, 0.1));

    float half_lambert = (dot(-light_dir, fragNormal) + 1.0) / 2.0;

    float shadow = calculateShadow(fragPosition);
    vec3 color = half_lambert * colDiffuse.xyz + 0.1;
    
    // Apply shadow (darken color in shadowed areas)
    color *= (1.0 - shadow * 0.4);

    finalColor = vec4(color, 1.0);
}
