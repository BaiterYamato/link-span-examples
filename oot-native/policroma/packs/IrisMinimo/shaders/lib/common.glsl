#ifndef IRIS_MINIMO_COMMON
#define IRIS_MINIMO_COMMON

// Linear view depth from the depth buffer, the usual Iris formula with near/far.
float linearDepth(float depth) {
    float z = depth * 2.0 - 1.0;
    return 2.0 * near * far / (far + near - z * (far - near));
}

float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

#endif
