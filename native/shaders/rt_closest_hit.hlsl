// R1: the closest-hit test kernel in HLSL (compiled by DXC to SPIR-V, the path the ray traced viewport kernels of R3 and later use): the same job as accel_test.comp.
// rays[2 i] = (origin.xyz, tmax), rays[2 i + 1] = (direction.xyz, 0); hits[i] = (t or -1, b1, b2, instance custom index bits)
[[vk::binding(0, 0)]] RaytracingAccelerationStructure tlas;
[[vk::binding(1, 0)]] StructuredBuffer<float4> rays;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> hits;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint count, stride;
    rays.GetDimensions(count, stride);
    if (id.x >= count / 2) return;
    const float4 a = rays[2 * id.x];
    const float4 b = rays[2 * id.x + 1];
    RayDesc ray;
    ray.Origin = a.xyz;
    ray.TMin = 0.0;
    ray.Direction = b.xyz;
    ray.TMax = a.w;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {}
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        const float2 bary = q.CommittedTriangleBarycentrics();
        hits[id.x] = float4(q.CommittedRayT(), bary.x, bary.y, asfloat(q.CommittedInstanceID()));
    } else {
        hits[id.x] = float4(-1.0, 0.0, 0.0, 0.0);
    }
}
