// SPDX-License-Identifier: GPL-3.0-or-later
#include "tp_scene.h"
#include <math.h>

typedef struct { float x, y, z; } v3;

static v3    add(v3 a, v3 b)    { return (v3){ a.x + b.x, a.y + b.y, a.z + b.z }; }
static v3    sub(v3 a, v3 b)    { return (v3){ a.x - b.x, a.y - b.y, a.z - b.z }; }
static v3    mul(v3 a, float k) { return (v3){ a.x * k, a.y * k, a.z * k }; }
static float dot(v3 a, v3 b)    { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3    cross(v3 a, v3 b)
{
    return (v3){ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
static v3    norm(v3 a)         { return mul(a, 1.0f / sqrtf(dot(a, a))); }
static float clamp01(float v)   { return v < 0 ? 0 : v > 1 ? 1 : v; }

static const struct { v3 c; float r; v3 kd; } balls[] = {
    { { -0.80f, 0.55f,  0.30f }, 0.55f, { 0.86f, 0.12f, 0.10f } },   // red
    { {  0.42f, 0.40f, -0.25f }, 0.40f, { 0.10f, 0.62f, 0.25f } },   // green
    { {  1.05f, 0.28f,  0.55f }, 0.28f, { 0.12f, 0.30f, 0.85f } },   // blue
    { { -0.05f, 0.18f, -0.85f }, 0.18f, { 0.95f, 0.78f, 0.10f } },   // yellow
};
#define NBALLS ((int)(sizeof balls / sizeof balls[0]))
#define FLOOR  NBALLS
#define SKY    (-1)

// A soft shadow is the share of six rays towards the light, each a little
// off it, that reach it.
static const v3 jitter[6] = {
    { -0.07458f, -0.04737f,  0.05423f }, {  0.01479f, -0.07306f, -0.01204f },
    { -0.00377f, -0.06125f,  0.04222f }, { -0.06954f, -0.01958f,  0.00301f },
    { -0.01249f,  0.01562f,  0.04281f }, {  0.08213f, -0.03884f,  0.02674f },
};

static v3 light(void) { return norm((v3){ -0.55f, 0.75f, -0.40f }); }

// What the ray hits first, and how far along it.
static int trace(v3 ro, v3 rd, float *t)
{
    int hit = SKY;
    *t = INFINITY;
    for (int i = 0; i < NBALLS; i++) {
        v3 oc = sub(ro, balls[i].c);
        float b = dot(oc, rd), c = dot(oc, oc) - balls[i].r * balls[i].r;
        float disc = b * b - c;
        if (disc <= 0) continue;
        float s = -b - sqrtf(disc);
        if (s > 1e-4f && s < *t) { *t = s; hit = i; }
    }
    if (rd.y < 0) {
        float s = -ro.y / rd.y;
        if (s > 1e-4f && s < *t) { *t = s; hit = FLOOR; }
    }
    return hit;
}

static float shadow(v3 p)
{
    v3 l = light();
    int lit = 0;
    for (int k = 0; k < 6; k++) {
        v3 j = norm(add(l, jitter[k]));
        float t;
        int hit = trace(add(p, mul(j, 1e-3f)), j, &t);
        lit += hit == SKY || hit == FLOOR;
    }
    return lit / 6.0f;
}

static v3 shade(v3 ro, v3 rd)
{
    float t;
    int hit = trace(ro, rd, &t);
    if (hit == SKY) {                       // pale at the horizon, deeper overhead
        float h = clamp01(rd.y * 2.2f);
        return add(mul((v3){ 0.60f, 0.74f, 0.90f }, 1 - h), mul((v3){ 0.16f, 0.36f, 0.80f }, h));
    }
    v3 p = add(ro, mul(rd, t));
    v3 col;
    if (hit == FLOOR) {                     // warm paper, a soft checker fading off
        int chk = (((int)floorf(p.x * 2) + (int)floorf(p.z * 2)) % 2 + 2) % 2;
        float d = p.z + 1.0f;
        float fade = expf(-(d > 0 ? d : 0) * 0.35f);
        float base = 0.62f - 0.26f * (float)chk * fade;
        float lit = 0.35f + 0.65f * shadow(p);
        col = mul((v3){ 1.0f, 0.97f, 0.90f }, base * lit);
    } else {
        v3 l = light();
        v3 n = mul(sub(p, balls[hit].c), 1.0f / balls[hit].r);
        float diff = clamp01(dot(n, l));
        float s = shadow(add(p, mul(n, 1e-3f)));
        v3 refl = sub(rd, mul(n, 2 * dot(rd, n)));
        float sp = clamp01(dot(refl, l));
        sp *= sp; sp *= sp; sp *= sp;                           // ^8
        float sp40 = sp * sp; sp40 *= sp40; sp40 *= sp;         // ^40
        float amb = 0.18f + 0.10f * clamp01(n.y);
        col = add(mul(balls[hit].kd, amb + 0.85f * diff * s), mul((v3){ 1, 1, 1 }, 0.9f * sp40 * s));
    }
    return (v3){ clamp01(col.x), clamp01(col.y), clamp01(col.z) };
}

static uint8_t srgb(float lin)
{
    float v = lin <= 0.0031308f ? lin * 12.92f : 1.055f * powf(lin, 1 / 2.4f) - 0.055f;
    return (uint8_t)(clamp01(v) * 255 + 0.5f);
}

void tp_scene_row(uint32_t w, uint32_t h, uint32_t y, uint8_t *rgb)
{
    const v3 eye = { 0.15f, 1.05f, -3.6f }, look = { 0.15f, 0.40f, 0.0f };
    v3 fwd = norm(sub(look, eye));
    v3 right = norm(cross((v3){ 0, 1, 0 }, fwd));
    v3 up = cross(fwd, right);
    const float fov = 0.62f;                // half the image plane's width at distance 1
    float aspect = ((float)h / 72) / ((float)w / 144);

    for (uint32_t x = 0; x < w; x++) {
        v3 acc = { 0, 0, 0 };
        for (int sy = 0; sy < 2; sy++)      // 2 x 2 samples a pixel
            for (int sx = 0; sx < 2; sx++) {
                float X = ((x + (sx + 0.5f) / 2) / w * 2 - 1) * fov;
                float Y = (1 - (y + (sy + 0.5f) / 2) / h * 2) * fov * aspect;
                v3 rd = norm(add(fwd, add(mul(right, X), mul(up, Y))));
                acc = add(acc, shade(eye, rd));
            }
        rgb[x * 3]     = srgb(acc.x / 4);
        rgb[x * 3 + 1] = srgb(acc.y / 4);
        rgb[x * 3 + 2] = srgb(acc.z / 4);
    }
}
