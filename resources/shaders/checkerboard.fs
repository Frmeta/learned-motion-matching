#version 300 es
precision mediump float;

in vec3 fragPosition;
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;

uniform vec4 colDiffuse;

out vec4 finalColor;

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
    
    // 1. Key Light: Main light from the top-front-right
    vec3 keyDir = normalize(vec3(1.0f, 2.0f, 1.0f));
    float key = max(dot(normal, keyDir), 0.0f);
    
    // 2. Fill Light: Softer light from the top-front-left to fill shadows
    vec3 fillDir = normalize(vec3(-1.0f, 1.0f, 1.0f));
    float fill = max(dot(normal, fillDir), 0.0f);
    
    // 3. Back Light: Rim light from the back to separate from background
    vec3 backDir = normalize(vec3(0.0f, 0.5f, -1.0f));
    float back = max(dot(normal, backDir), 0.0f);
    
    // Combine lights with some color tinting for a premium "Blender-like" look
    vec3 studioLight = key * vec3(1.0f, 0.95f, 0.9f) * 0.8f + 
                       fill * vec3(0.8f, 0.85f, 1.0f) * 0.4f + 
                       back * vec3(1.0f, 1.0f, 1.0f) * 0.3f;
    
    // Add a base ambient term
    studioLight += vec3(0.0f);
    
    // Blend the studio light into the checkerboard pattern (approx 0.2 opacity as requested)
    vec3 finalRGB = mix(checkColor, studioLight, 0.8f);
    
    finalColor = vec4(finalRGB, 1.0f);
}
