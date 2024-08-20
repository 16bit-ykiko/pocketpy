#pragma once

typedef struct c11_vec2i {
    int x;
    int y;
} c11_vec2i;

typedef struct c11_vec3i {
    int x;
    int y;
    int z;
} c11_vec3i;

typedef struct c11_vec2 {
    float x;
    float y;
} c11_vec2;

typedef struct c11_vec3 {
    float x;
    float y;
    float z;
} c11_vec3;

typedef struct c11_mat3x3 {
    union {
        struct {
            float _11, _12, _13;
            float _21, _22, _23;
            float _31, _32, _33;
        };
        float m[3][3];
        float v[9];
    };
} c11_mat3x3;