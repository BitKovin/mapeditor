// Shared by the lighting passes of the native editor: the sample / job layout and the encodings (light/samples.h, light/passes.h).
// Included with #include (glslangValidator -I).

// SampleRecord, 32 bytes = two uvec4 per slot: (pos.xyz, randomId) (normalOct, xy, page, aux)
layout(std430, set = 0, binding = 0) readonly buffer Samples { uvec4 samples[]; };

// One job = one chunk's samples in one pass: (slotBase, count, prefix = sum of the counts before it, passes the chunk had before this pass)
layout(std430, set = 0, binding = 1) readonly buffer Jobs { uvec4 jobs[]; };

// The job and the slot of invocation `g` of a dispatch over all jobs (binary search over the prefix sums). False beyond the last sample.
bool findSlot(uint g, uint jobCount, out uvec4 job, out uint slot) {
    if (jobCount == 0u) return false;
    uint lo = 0u, hi = jobCount - 1u;
    while (lo < hi) {
        uint mid = (lo + hi + 1u) >> 1u;
        if (jobs[mid].z <= g) lo = mid; else hi = mid - 1u;
    }
    job = jobs[lo];
    if (g - job.z >= job.y) return false;
    slot = job.x + (g - job.z);
    return true;
}

vec3 samplePos(uint slot) { return uintBitsToFloat(samples[2u * slot].xyz); }
uint sampleRandomId(uint slot) { return samples[2u * slot].w; }

float unpackSnorm16Lo(uint v) { return max(-1.0, float(int(v << 16u) >> 16) / 32767.0); }
float unpackSnorm16Hi(uint v) { return max(-1.0, float(int(v) >> 16) / 32767.0); }

// Octahedral decode, equal to decodeNormalOct in light/samples.cpp.
vec3 sampleNormal(uint slot) {
    uint packed = samples[2u * slot + 1u].x;
    float x = unpackSnorm16Lo(packed), y = unpackSnorm16Hi(packed);
    float z = 1.0 - abs(x) - abs(y);
    if (z < 0.0) {
        float ox = (1.0 - abs(y)) * (x >= 0.0 ? 1.0 : -1.0), oy = (1.0 - abs(x)) * (y >= 0.0 ? 1.0 : -1.0);
        x = ox;
        y = oy;
    }
    return normalize(vec3(x, y, z));
}
