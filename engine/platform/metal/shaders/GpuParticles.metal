// ---------------------------------------------------------------------------
// GPU particles (`particles` with simulation "gpu"): compute-driven, indirect-drawn.
//
// Per emitter: a particle pool (64 B each), a dead list (free slots), two alive lists
// (ping-pong) and atomic counters. Each step:
//   gpuEmitArgs    1 thread: how many to spawn (rate + sub-emitter requests, capped by the
//                  free slots) -> indirect dispatch arguments
//   gpuEmit        pop free slots, initialize particles (shapes incl. mesh surfaces), append
//                  them to the current alive list
//   gpuSimArgs     1 thread: dispatch arguments for the alive count
//   gpuSimulate    forces (gravity, drag toward the moving air, curl noise, vortex /
//                  attractor / vector-field fields, wind), collisions (floor, spheres,
//                  capsules, planes and the scene depth buffer with G-buffer normals),
//                  death and sub-emitter requests, ribbon history -> next alive list
//   gpuFinalize    1 thread: indirect draw arguments from the new alive count
//   gpuSortKeys + bitonic passes: back-to-front order for blended looks
//   gpuLightReduce one threadgroup: a few clustered lights for glowing emitters
// The CPU never waits on any of it: counts reach it through a small ring of buffers.
// ---------------------------------------------------------------------------

struct GpuEmitterParams {
    float4x4 world;
    float4x4 delta;
    float4x4 fieldInv;
    float4 spawn;        // x shape, y speed, z speed jitter, w cos(spread)
    float4 shapeSize;    // xyz half size, w lifetime
    float4 direction;    // xyz world, w lifetime jitter
    float4 forces;       // x gravity, y drag, z turbulence, w 1/turbulence scale
    float4 wind;         // xyz, w worldSpace
    float4 size;         // x start, y end, z jitter, w stretch
    float4 look;         // x look, y facing, z intensity, w softness
    float4 floorPlane;   // x enabled, y height, z bounce, w friction
    float4 collision;    // x depth, y stick, z colliders, w sub-emit mask
    float4 field;        // x type, y strength, z radius, w pull
    float4 fieldCenter;  // xyz, w lift
    float4 fieldAxis;
    float4 flipbook;     // x cols, y rows, z fps, w frames
    float4 material;     // x roughness, y metallic, z spin, w has texture
    float4 sub;          // x count, y inherit, z has sub-emitter, w seed
    float4 trail;        // x segments, y interval, z head slot, w 0
    float4 frame;        // x time, y dt, z spawn, w step
    float4 light;
    float4 extra;        // x hue variation, y sort, z particles per sub-emitter event, w thin mesh
    float4 colliders[16];
    float4 colorTable[32];
    float sizeTable[32];
};

struct GpuStep {
    float4x4 prevViewProj;     // the depth buffer's camera (collisions)
    float4x4 prevInvViewProj;
    float4 eye;                // xyz camera, w depth valid
    uint capacity;
    uint cur;                  // alive list holding the live particles before this step
    uint spawn;                // particles from rate / bursts this step
    uint seed;
    uint requestCap;
    uint triangles;            // shape "mesh"
    uint writeTrail;           // write ribbon history this step
    uint trailSlots;
    uint indexCount;           // mesh particles
    uint sortCount;            // power of two >= capacity (0 = unsorted)
    uint pad0, pad1;
};

struct GpuParticle {
    float4 posAge;   // xyz world position, w age (s)
    float4 velLife;  // xyz velocity (m/s), w lifetime (s)
    float4 misc;     // x rotation, y seed, z size scale, w flags (1 stuck)
    float4 misc2;    // x hue seed, y spin (rad/s), z 0, w 0
};

struct GpuRequest {
    float4 posSeed;  // xyz, w hue seed
    float4 vel;      // xyz, w 0
};

// counters: [0] dead, [1] alive list 0, [2] alive list 1, [3] requests received,
//           [4] emit count, [5] requests consumed, [6] alive (stats)
constant uint kCntDead = 0, kCntAlive = 1, kCntRequests = 3, kCntEmit = 4, kCntReqUsed = 5, kCntStat = 6;
// args (uints): [0..2] emit dispatch, [4..6] simulate dispatch, [8..11] draw, [12..16] indexed draw,
//               [20..22] sort dispatch
constant uint kArgEmit = 0, kArgSim = 4, kArgDraw = 8, kArgIndexed = 12;

static uint pcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

struct Rng {
    uint s;
    float next() {
        s = pcg(s);
        return float(s >> 8) * (1.0 / 16777216.0);
    }
    float range(float a, float b) { return a + (b - a) * next(); }
};

static float3 rotateHue(float3 c, float turns) {
    // Rotation around the gray axis (keeps luminance roughly).
    float a = turns * 6.2831853;
    float cs = cos(a), sn = sin(a);
    float3 k = float3(0.57735);
    return c * cs + cross(k, c) * sn + k * dot(k, c) * (1.0 - cs);
}

// --- noise --------------------------------------------------------------------------------

static float3 hashGrad(float3 p) {
    uint3 q = uint3(int3(p) + int3(4096));
    uint h = pcg(q.x + pcg(q.y + pcg(q.z)));
    float3 g = float3(float(h & 1023u), float((h >> 10) & 1023u), float((h >> 20) & 1023u)) * (2.0 / 1023.0) - 1.0;
    return g;
}

// Gradient noise with analytic derivatives (value in w).
static float4 noised(float3 x) {
    float3 i = floor(x), f = fract(x);
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float3 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    float3 ga = hashGrad(i), gb = hashGrad(i + float3(1, 0, 0)), gc = hashGrad(i + float3(0, 1, 0)), gd = hashGrad(i + float3(1, 1, 0));
    float3 ge = hashGrad(i + float3(0, 0, 1)), gf = hashGrad(i + float3(1, 0, 1)), gg = hashGrad(i + float3(0, 1, 1)), gh = hashGrad(i + float3(1, 1, 1));
    float va = dot(ga, f), vb = dot(gb, f - float3(1, 0, 0)), vc = dot(gc, f - float3(0, 1, 0)), vd = dot(gd, f - float3(1, 1, 0));
    float ve = dot(ge, f - float3(0, 0, 1)), vf = dot(gf, f - float3(1, 0, 1)), vg = dot(gg, f - float3(0, 1, 1)), vh = dot(gh, f - float3(1, 1, 1));
    float v = va + u.x * (vb - va) + u.y * (vc - va) + u.z * (ve - va) + u.x * u.y * (va - vb - vc + vd) +
              u.y * u.z * (va - vc - ve + vg) + u.z * u.x * (va - vb - ve + vf) + u.x * u.y * u.z * (-va + vb + vc - vd + ve - vf - vg + vh);
    float3 d = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.z * (ge - ga) + u.x * u.y * (ga - gb - gc + gd) +
               u.y * u.z * (ga - gc - ge + gg) + u.z * u.x * (ga - gb - ge + gf) +
               u.x * u.y * u.z * (-ga + gb + gc - gd + ge - gf - gg + gh) +
               du * (float3(vb - va, vc - va, ve - va) + u.yzx * float3(va - vb - vc + vd, va - vc - ve + vg, va - vb - ve + vf) +
                     u.zxy * float3(va - vb - ve + vf, va - vb - vc + vd, va - vc - ve + vg) +
                     u.yzx * u.zxy * (-va + vb + vc - vd + ve - vf - vg + vh));
    return float4(d, v);
}

// Curl of a vector potential made of three noise fields: divergence-free swirls (Bridson 2007).
static float3 curlNoise(float3 p) {
    float3 c = 0.0;
    float amp = 1.0;
    for (int o = 0; o < 2; ++o) {
        float3 a = noised(p).xyz;
        float3 b = noised(p + float3(31.416, -47.853, 12.793)).xyz;
        float3 d = noised(p + float3(-91.153, 23.871, 58.217)).xyz;
        c += float3(d.y - b.z, a.z - d.x, b.x - a.y) * amp;
        p = p * 2.03 + float3(5.1, 1.7, -3.3);
        amp *= 0.5;
    }
    return c * 0.7;
}

// --- emission -------------------------------------------------------------------------------

static float3 coneDir(thread Rng& r, float3 dir, float cosMax) {
    float z = 1.0 - r.next() * (1.0 - cosMax);
    float rad = sqrt(max(0.0, 1.0 - z * z));
    float phi = r.next() * 6.2831853;
    float3 up = abs(dir.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, dir));
    float3 b = cross(dir, t);
    return normalize(t * (rad * cos(phi)) + b * (rad * sin(phi)) + dir * z);
}

kernel void gpuInit(uint id [[thread_position_in_grid]], constant GpuStep& s [[buffer(14)]],
                    device uint* dead [[buffer(2)]], device atomic_uint* counters [[buffer(5)]],
                    device float4* history [[buffer(10)]]) {
    if (id >= s.capacity) return;
    dead[id] = s.capacity - 1 - id;
    if (id == 0) {
        atomic_store_explicit(&counters[kCntDead], s.capacity, memory_order_relaxed);
        for (uint i = 1; i < 8; ++i) atomic_store_explicit(&counters[i], 0u, memory_order_relaxed);
    }
    if (s.trailSlots > 0) {
        for (uint k = 0; k < s.trailSlots; ++k) history[id * s.trailSlots + k] = float4(0.0, 0.0, 0.0, -1.0);
    }
}

kernel void gpuEmitArgs(constant GpuEmitterParams& P [[buffer(0)]], constant GpuStep& s [[buffer(14)]],
                        device atomic_uint* counters [[buffer(5)]], device uint* args [[buffer(6)]]) {
    uint free = atomic_load_explicit(&counters[kCntDead], memory_order_relaxed);
    uint req = min(atomic_load_explicit(&counters[kCntRequests], memory_order_relaxed), s.requestCap);
    // Each request spawns `extra.z` particles (the parent emitter's subEmitCount).
    uint total = min(s.spawn + req * uint(max(P.extra.z, 1.0)), free);
    atomic_store_explicit(&counters[kCntEmit], total, memory_order_relaxed);
    atomic_store_explicit(&counters[kCntReqUsed], req, memory_order_relaxed);
    atomic_store_explicit(&counters[s.cur == 0 ? 2u : 1u], 0u, memory_order_relaxed);  // next list starts empty
    args[kArgEmit + 0] = (total + 63) / 64;
    args[kArgEmit + 1] = 1;
    args[kArgEmit + 2] = 1;
}

kernel void gpuEmit(uint id [[thread_position_in_grid]], constant GpuEmitterParams& P [[buffer(0)]],
                    constant GpuStep& s [[buffer(14)]], device GpuParticle* particles [[buffer(1)]],
                    device uint* dead [[buffer(2)]], device uint* aliveCur [[buffer(3)]],
                    device atomic_uint* counters [[buffer(5)]], device const GpuRequest* requests [[buffer(7)]],
                    device float4* history [[buffer(10)]], device const float4* triPos [[buffer(11)]],
                    device const float4* triNrm [[buffer(12)]], device const float* triCdf [[buffer(13)]]) {
    uint total = atomic_load_explicit(&counters[kCntEmit], memory_order_relaxed);
    if (id >= total) return;
    uint slot = atomic_fetch_sub_explicit(&counters[kCntDead], 1u, memory_order_relaxed) - 1u;
    uint idx = dead[slot];
    Rng r{pcg(id * 9781u + s.seed * 6271u + 17u)};
    float3 pos, vel;
    float hue = r.next();
    float speed = P.spawn.y * (1.0 + P.spawn.z * r.range(-1.0, 1.0));
    if (id < s.spawn) {
        int shape = int(P.spawn.x + 0.5);
        float3 h = P.shapeSize.xyz;
        float3 local = 0.0;
        float3 dir = P.direction.xyz;
        float3 nrm = dir;
        if (shape == 1) {  // sphere
            float3 u;
            for (int k = 0; k < 8; ++k) {
                u = float3(r.range(-1, 1), r.range(-1, 1), r.range(-1, 1));
                if (dot(u, u) <= 1.0) break;
            }
            local = u * h;
        } else if (shape == 2) {  // box
            local = float3(r.range(-1, 1), r.range(-1, 1), r.range(-1, 1)) * h;
        } else if (shape == 3 || shape == 4) {  // disc / cone
            float a = r.next() * 6.2831853, rad = sqrt(r.next());
            local = float3(cos(a) * rad * h.x, 0.0, sin(a) * rad * h.z);
        }
        pos = (P.world * float4(local, 1.0)).xyz;
        if (shape == 4) dir = normalize(dir + (P.world * float4(local.x, 0.0, local.z, 0.0)).xyz * 0.8);
        if (shape == 5 && s.triangles > 0) {  // mesh surface: area-weighted triangle, uniform barycentrics
            float u = r.next();
            uint lo = 0, hi = s.triangles - 1;
            while (lo < hi) {
                uint mid = (lo + hi) / 2;
                if (triCdf[mid] < u) lo = mid + 1; else hi = mid;
            }
            float r1 = sqrt(r.next()), r2 = r.next();
            float3 bc = float3(1.0 - r1, r1 * (1.0 - r2), r1 * r2);
            float3 lp = triPos[lo * 3].xyz * bc.x + triPos[lo * 3 + 1].xyz * bc.y + triPos[lo * 3 + 2].xyz * bc.z;
            float3 ln = triNrm[lo * 3].xyz * bc.x + triNrm[lo * 3 + 1].xyz * bc.y + triNrm[lo * 3 + 2].xyz * bc.z;
            pos = (P.world * float4(lp, 1.0)).xyz;
            nrm = normalize((P.world * float4(ln, 0.0)).xyz + 1e-6);
            dir = nrm;
        }
        vel = coneDir(r, dir, P.spawn.w) * speed + P.wind.xyz * 0.7;  // born moving with the air
    } else {
        // Sub-emitter: spawn at the parent's event, inheriting part of its velocity.
        uint q = (id - s.spawn) / uint(max(P.extra.z, 1.0));
        GpuRequest rq = requests[q];
        pos = rq.posSeed.xyz;
        hue = rq.posSeed.w;
        float3 d = coneDir(r, P.direction.xyz, P.spawn.w);
        vel = d * speed + rq.vel.xyz * rq.vel.w;
    }
    GpuParticle p;
    p.posAge = float4(pos, 0.0);
    p.velLife = float4(vel, max(0.02, P.shapeSize.w * (1.0 + P.direction.w * r.range(-1.0, 1.0))));
    p.misc = float4(r.next() * 6.2831853, r.next(), 1.0 + P.size.z * r.range(-1.0, 1.0), 0.0);
    p.misc2 = float4(hue, P.material.z * r.range(-1.0, 1.0), 0.0, 0.0);
    particles[idx] = p;
    if (s.trailSlots > 0) {
        for (uint k = 0; k < s.trailSlots; ++k) history[idx * s.trailSlots + k] = float4(pos, -1.0);
    }
    uint a = atomic_fetch_add_explicit(&counters[kCntAlive + s.cur], 1u, memory_order_relaxed);
    aliveCur[a] = idx;
}

kernel void gpuSimArgs(constant GpuStep& s [[buffer(14)]], device atomic_uint* counters [[buffer(5)]],
                       device uint* args [[buffer(6)]]) {
    uint n = atomic_load_explicit(&counters[kCntAlive + s.cur], memory_order_relaxed);
    args[kArgSim + 0] = (n + 255) / 256;
    args[kArgSim + 1] = 1;
    args[kArgSim + 2] = 1;
    atomic_store_explicit(&counters[kCntRequests], 0u, memory_order_relaxed);  // consumed by gpuEmit
}

static bool collideShape(float4 a, float4 b, float3 p, thread float3& hitN, thread float3& hitP) {
    int kind = int(a.w + 0.5);
    if (kind == 2) {  // plane: point a, normal b
        float d = dot(p - a.xyz, b.xyz);
        if (d >= 0.0) return false;
        hitN = b.xyz;
        hitP = p - b.xyz * d;
        return true;
    }
    float3 c = a.xyz;
    if (kind == 1) {  // capsule a..b
        float3 ab = b.xyz - a.xyz;
        float t = clamp(dot(p - a.xyz, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);
        c = a.xyz + ab * t;
    }
    float3 d = p - c;
    float l = length(d);
    if (l >= b.w) return false;
    hitN = l > 1e-6 ? d / l : float3(0, 1, 0);
    hitP = c + hitN * b.w;
    return true;
}

kernel void gpuSimulate(uint id [[thread_position_in_grid]], constant GpuEmitterParams& P [[buffer(0)]],
                        constant GpuStep& s [[buffer(14)]], device GpuParticle* particles [[buffer(1)]],
                        device uint* dead [[buffer(2)]], device const uint* aliveCur [[buffer(3)]],
                        device uint* aliveNext [[buffer(4)]], device atomic_uint* counters [[buffer(5)]],
                        device atomic_uint* subCounters [[buffer(8)]], device GpuRequest* subRequests [[buffer(9)]],
                        device float4* history [[buffer(10)]], depth2d<float> sceneDepth [[texture(0)]],
                        texture2d<float> sceneNormals [[texture(1)]], texture3d<float> field [[texture(2)]]) {
    uint n = atomic_load_explicit(&counters[kCntAlive + s.cur], memory_order_relaxed);
    if (id >= n) return;
    uint idx = aliveCur[id];
    GpuParticle p = particles[idx];
    float dt = P.frame.y;
    float3 pos = p.posAge.xyz, vel = p.velLife.xyz;
    float age = p.posAge.w + dt, life = p.velLife.w;
    uint flags = uint(p.misc.w + 0.5);
    bool hasSub = P.sub.z > 0.5;
    uint subMask = uint(P.collision.w + 0.5);
    bool dieNow = age >= life;
    bool collided = false;
    float3 hitN = float3(0, 1, 0);
    if (!dieNow) {
        if (P.wind.w < 0.5) pos = (P.delta * float4(pos, 1.0)).xyz;  // local-space emitters drag particles along
        if ((flags & 1u) == 0u) {
            vel.y -= P.forces.x * dt;
            float3 air = P.wind.xyz;
            if (P.forces.z > 0.0) {
                float3 q = pos * P.forces.w + float3(p.misc.y * 17.0, P.frame.x * 0.11, p.misc.y * 7.0);
                air += curlNoise(q) * P.forces.z;
            }
            int ft = int(P.field.x + 0.5);
            if (ft == 1) {  // vortex
                float3 axis = P.fieldAxis.xyz;
                float3 rv = pos - P.fieldCenter.xyz;
                float3 radial = rv - axis * dot(rv, axis);
                float d = length(radial);
                float R = P.field.z;
                float3 rn = d > 1e-5 ? radial / d : float3(1, 0, 0);
                float swirl = P.field.y * (d / R) * exp(0.5 - 0.5 * (d * d) / (R * R)) * 1.6487;  // peak at R
                float near = exp(-d / (R * 2.5));
                air += cross(axis, rn) * swirl - rn * P.field.w * near + axis * P.fieldCenter.w * near;
            } else if (ft == 2) {  // attractor
                float3 to = P.fieldCenter.xyz - pos;
                float d2 = dot(to, to);
                vel += normalize(to + 1e-6) * P.field.y * dt / (1.0 + d2 / (P.field.z * P.field.z));
            } else if (ft == 3) {  // vector field texture
                float3 uvw = (P.fieldInv * float4(pos, 1.0)).xyz;
                if (all(uvw >= 0.0) && all(uvw <= 1.0)) {
                    constexpr sampler fs(coord::normalized, filter::linear, address::clamp_to_edge);
                    air += field.sample(fs, uvw).xyz * P.field.y;
                }
            }
            vel += (air - vel) * (1.0 - exp(-P.forces.y * dt));
            float3 np = pos + vel * dt;
            float3 hitP = np;
            // Floor and simple colliders.
            if (P.floorPlane.x > 0.5 && np.y < P.floorPlane.y) {
                collided = true;
                hitN = float3(0, 1, 0);
                hitP = float3(np.x, P.floorPlane.y, np.z);
            }
            int nc = int(P.collision.z + 0.5);
            for (int c = 0; c < nc && !collided; ++c) {
                float3 cn, cp;
                if (collideShape(P.colliders[c * 2], P.colliders[c * 2 + 1], np, cn, cp)) {
                    collided = true;
                    hitN = cn;
                    hitP = cp;
                }
            }
            // Scene depth (previous frame) + G-buffer normals: everything visible collides.
            if (!collided && P.collision.x > 0.5 && s.eye.w > 0.5) {
                float4 c = s.prevViewProj * float4(np, 1.0);
                if (c.w > 1e-4) {
                    float2 uv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
                    if (all(uv > 0.0) && all(uv < 1.0)) {
                        float sd = sceneDepth.sample(pointClamp, uv);
                        if (sd < 0.99999) {
                            float4 wp = s.prevInvViewProj * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, sd, 1.0);
                            float3 sp = wp.xyz / wp.w;
                            float dScene = distance(sp, s.eye.xyz), dNew = distance(np, s.eye.xyz), dOld = distance(pos, s.eye.xyz);
                            float thick = max(0.25, length(vel) * dt * 3.0) + dScene * 0.01;
                            if (dNew > dScene && dNew - dScene < thick && dOld <= dScene + thick * 0.5) {
                                float3 nn = octDecode(sceneNormals.sample(pointClamp, uv).xy);
                                collided = true;
                                hitN = dot(nn, vel) < 0.0 ? nn : -normalize(vel);
                                hitP = sp;
                            }
                        }
                    }
                }
            }
            if (collided) {
                if (P.collision.y > 0.5) {  // stick
                    np = hitP + hitN * 0.003;
                    vel = 0.0;
                    flags |= 1u;
                } else if (P.floorPlane.z > 0.0) {  // bounce with friction
                    float3 vn = hitN * dot(vel, hitN);
                    float3 vt = vel - vn;
                    vel = vt * (1.0 - P.floorPlane.w) - vn * P.floorPlane.z;
                    np = hitP + hitN * 0.004;
                } else {
                    dieNow = true;
                    np = hitP;
                }
            }
            pos = np;
        }
    }
    // Sub-emitter events.
    if (hasSub && ((dieNow && age >= life && (subMask & 1u)) || (collided && (subMask & 2u)))) {
        uint q = atomic_fetch_add_explicit(&subCounters[kCntRequests], 1u, memory_order_relaxed);
        if (q < s.requestCap) {
            subRequests[q].posSeed = float4(pos + hitN * (collided ? 0.01 : 0.0), p.misc2.x);
            subRequests[q].vel = float4(collided ? reflect(vel, hitN) : vel, P.sub.y);
        }
    }
    if (dieNow) {
        uint slot = atomic_fetch_add_explicit(&counters[kCntDead], 1u, memory_order_relaxed);
        dead[slot] = idx;
        return;
    }
    p.posAge = float4(pos, age);
    p.velLife.xyz = vel;
    p.misc.x += p.misc2.y * dt;
    p.misc.w = float(flags);
    particles[idx] = p;
    if (s.writeTrail != 0u && s.trailSlots > 0) {
        history[idx * s.trailSlots + uint(P.trail.z)] = float4(pos, age);
    }
    uint a = atomic_fetch_add_explicit(&counters[kCntAlive + (1u - s.cur)], 1u, memory_order_relaxed);
    aliveNext[a] = idx;
}

kernel void gpuFinalize(constant GpuEmitterParams& P [[buffer(0)]], constant GpuStep& s [[buffer(14)]],
                        device atomic_uint* counters [[buffer(5)]], device uint* args [[buffer(6)]]) {
    uint n = atomic_load_explicit(&counters[kCntAlive + (1u - s.cur)], memory_order_relaxed);
    atomic_store_explicit(&counters[kCntStat], n, memory_order_relaxed);
    int facing = int(P.look.y + 0.5);
    args[kArgDraw + 0] = facing == 3 ? (s.trailSlots + 1) * 2 : 4;  // ribbon strip / quad strip
    args[kArgDraw + 1] = n;
    args[kArgDraw + 2] = 0;
    args[kArgDraw + 3] = 0;
    args[kArgIndexed + 0] = s.indexCount;
    args[kArgIndexed + 1] = n;
    args[kArgIndexed + 2] = 0;
    args[kArgIndexed + 3] = 0;
    args[kArgIndexed + 4] = 0;
}

// --- sorting (bitonic, key = -distance^2: far first) ---------------------------------------

kernel void gpuSortKeys(uint id [[thread_position_in_grid]], constant GpuStep& s [[buffer(14)]],
                        device const GpuParticle* particles [[buffer(1)]], device const uint* alive [[buffer(4)]],
                        device atomic_uint* counters [[buffer(5)]], device float2* keys [[buffer(15)]]) {
    if (id >= s.sortCount) return;
    uint n = atomic_load_explicit(&counters[kCntStat], memory_order_relaxed);
    if (id < n) {
        uint idx = alive[id];
        float3 d = particles[idx].posAge.xyz - s.eye.xyz;
        keys[id] = float2(-dot(d, d), as_type<float>(idx));
    } else {
        keys[id] = float2(INFINITY, as_type<float>(0u));
    }
}

static void cmpSwap(threadgroup float2* t, uint a, uint b, bool up) {
    float2 x = t[a], y = t[b];
    if ((x.x > y.x) == up) {
        t[a] = y;
        t[b] = x;
    }
}

// Sorts blocks of 1024 keys in threadgroup memory (512 threads), or (merge = 1) finishes the
// steps j <= 512 of a global stage k.
kernel void gpuBitonicLocal(uint tid [[thread_index_in_threadgroup]], uint gid [[threadgroup_position_in_grid]],
                            constant uint2& km [[buffer(16)]], device float2* keys [[buffer(15)]]) {
    threadgroup float2 t[1024];
    uint base = gid * 1024;
    t[tid] = keys[base + tid];
    t[tid + 512] = keys[base + tid + 512];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint kStart = km.y != 0 ? km.x : 2;
    uint kEnd = km.y != 0 ? km.x : 1024;
    for (uint k = kStart; k <= kEnd; k <<= 1) {
        for (uint j = min(k >> 1, 512u); j > 0; j >>= 1) {
            uint i = 2 * tid - (tid & (j - 1));
            uint gi = base + i;
            bool up = ((gi & k) == 0);
            cmpSwap(t, i, i + j, up);
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
    }
    keys[base + tid] = t[tid];
    keys[base + tid + 512] = t[tid + 512];
}

kernel void gpuBitonicGlobal(uint id [[thread_position_in_grid]], constant uint2& kj [[buffer(16)]],
                             device float2* keys [[buffer(15)]]) {
    uint k = kj.x, j = kj.y;
    uint i = 2 * id - (id & (j - 1));
    bool up = ((i & k) == 0);
    float2 x = keys[i], y = keys[i + j];
    if ((x.x > y.x) == up) {
        keys[i] = y;
        keys[i + j] = x;
    }
}

// --- lights ----------------------------------------------------------------------------------

// One threadgroup of 256: samples up to 4096 live particles, finds the energy-weighted
// centroid, then splits around it into 4 quadrants (x/z) -> up to 4 lights.
kernel void gpuLightReduce(uint tid [[thread_index_in_threadgroup]], constant GpuEmitterParams& P [[buffer(0)]],
                           device const GpuParticle* particles [[buffer(1)]], device const uint* alive [[buffer(4)]],
                           device atomic_uint* counters [[buffer(5)]], device float4* out [[buffer(17)]]) {
    threadgroup float4 acc[256];
    threadgroup float4 quad[4][256];
    uint n = atomic_load_explicit(&counters[kCntStat], memory_order_relaxed);
    uint samples = min(n, 4096u);
    uint stride = max(n / max(samples, 1u), 1u);
    float4 a = 0.0;
    for (uint i = tid; i < samples; i += 256) {
        GpuParticle p = particles[alive[i * stride]];
        float w = saturate(1.0 - p.posAge.w / p.velLife.w);
        a += float4(p.posAge.xyz * w, w);
    }
    acc[tid] = a;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = 128; s > 0; s >>= 1) {
        if (tid < s) acc[tid] += acc[tid + s];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    float3 center = acc[0].w > 0.0 ? acc[0].xyz / acc[0].w : float3(0.0);
    float4 q[4] = {0.0, 0.0, 0.0, 0.0};
    for (uint i = tid; i < samples; i += 256) {
        GpuParticle p = particles[alive[i * stride]];
        float w = saturate(1.0 - p.posAge.w / p.velLife.w);
        float3 d = p.posAge.xyz - center;
        uint k = (d.x > 0.0 ? 1u : 0u) + (d.z > 0.0 ? 2u : 0u);
        q[k] += float4(p.posAge.xyz * w, w);
    }
    for (uint k = 0; k < 4; ++k) quad[k][tid] = q[k];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint s = 128; s > 0; s >>= 1) {
        if (tid < s) {
            for (uint k = 0; k < 4; ++k) quad[k][tid] += quad[k][tid + s];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (tid == 0) {
        for (uint k = 0; k < 4; ++k) {
            float4 v = quad[k][0];
            out[k] = v.w > 0.0 ? float4(v.xyz / v.w, v.w) : float4(center, 0.0);
        }
        out[4] = float4(float(n), float(samples), acc[0].w, 0.0);
    }
}

// --- rendering ---------------------------------------------------------------------------------

struct GpuDraw {
    uint stride;      // draw list: alive (1, 0) or sorted keys (2, 1)
    uint offset;
    uint trailSlots;
    uint pad;
};

struct GpuParticleOut {
    float4 position [[position]];
    float3 worldPos;
    float2 uv;
    float4 color;
    float age;
    float seed;
    float softness;
    float size;
    float frameBlend;
    float2 frameA;  // flipbook cell origins
    float2 frameB;
    float look [[flat]];
};

static float3 particleColorAt(constant GpuEmitterParams& P, float t, float hue, thread float& alpha) {
    float x = saturate(t) * 31.0;
    uint i = uint(x);
    float f = x - float(i);
    float4 c = mix(P.colorTable[i], P.colorTable[min(i + 1, 31u)], f);
    alpha = c.w;
    float3 rgb = c.rgb;
    if (P.extra.x > 0.0) rgb = max(rotateHue(rgb, (hue - 0.5) * 2.0 * P.extra.x), 0.0);
    return rgb;
}

static float particleSizeAt(constant GpuEmitterParams& P, float t) {
    float x = saturate(t) * 31.0;
    uint i = uint(x);
    return mix(P.sizeTable[i], P.sizeTable[min(i + 1, 31u)], x - float(i));
}

static GpuParticleOut gpuParticleCommon(constant GpuEmitterParams& P, constant FrameUniforms& f, GpuParticle p,
                                        float3 world, float2 c) {
    GpuParticleOut o;
    float t = p.posAge.w / p.velLife.w;
    float alpha;
    float3 rgb = particleColorAt(P, t, p.misc2.x, alpha);
    o.position = f.viewProj * float4(world, 1.0);
    o.worldPos = world;
    o.uv = c;
    o.color = float4(rgb * P.look.z, alpha);
    o.age = t;
    o.seed = p.misc.y;
    o.softness = P.look.w;
    o.size = particleSizeAt(P, t) * p.misc.z;
    o.look = P.look.x;
    // Flipbook: cross-fade between neighbouring frames (no motion vectors needed).
    float frames = max(P.flipbook.w, 1.0);
    float fr = P.flipbook.z > 0.0 ? p.posAge.w * P.flipbook.z + p.misc.y * frames : t * (frames - 0.001);
    float f0 = floor(fr);
    o.frameBlend = fr - f0;
    float i0 = fmod(f0, frames), i1 = fmod(f0 + 1.0, frames);
    if (P.flipbook.z <= 0.0) i1 = min(f0 + 1.0, frames - 1.0);
    float2 cell = 1.0 / P.flipbook.xy;
    o.frameA = float2(fmod(i0, P.flipbook.x), floor(i0 / P.flipbook.x)) * cell;
    o.frameB = float2(fmod(i1, P.flipbook.x), floor(i1 / P.flipbook.x)) * cell;
    return o;
}

// Quads (camera-facing, velocity-stretched, horizontal) as 4-vertex strips.
vertex GpuParticleOut gpuParticleVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                        constant GpuEmitterParams& P [[buffer(0)]],
                                        device const GpuParticle* particles [[buffer(1)]],
                                        device const uint* drawList [[buffer(2)]], constant GpuDraw& dr [[buffer(3)]],
                                        constant FrameUniforms& f [[buffer(4)]]) {
    GpuParticle p = particles[drawList[iid * dr.stride + dr.offset]];
    float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);
    float t = p.posAge.w / p.velLife.w;
    float r = particleSizeAt(P, t) * p.misc.z * 0.5;
    float3 center = p.posAge.xyz;
    float3 toCam = normalize(f.cameraPos.xyz - center);
    if (f.cameraForward.w > 0.5) toCam = -f.cameraForward.xyz;
    float3 fwd = -toCam;
    float3 worldUp = abs(fwd.y) > 0.99 ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 right = normalize(cross(fwd, worldUp));
    float3 up = cross(right, fwd);
    int facing = int(P.look.y + 0.5);
    float3 world;
    float3 v = p.velLife.xyz * P.size.w;
    float3 vs = v - fwd * dot(v, fwd);
    float len = length(vs);
    if (facing == 1 && len > r * 0.25 && (p.misc.w < 0.5)) {
        float3 along = vs / len;
        float3 across = normalize(cross(along, toCam));
        float3 mid = center - v * 0.5;
        world = mid + along * (c.y * (len * 0.5 + r)) + across * (c.x * r);
    } else if (facing == 2) {  // flat on the ground plane (ripples, decals, shockwaves)
        float cr = cos(p.misc.x), sr = sin(p.misc.x);
        float2 rc = float2(c.x * cr - c.y * sr, c.x * sr + c.y * cr);
        world = center + float3(rc.x, 0.0, rc.y) * r + float3(0.0, 0.01, 0.0);
    } else {
        int look = int(P.look.x + 0.5);
        float cr = cos(p.misc.x), sr = sin(p.misc.x);
        float2 rc = look == 1 ? c : float2(c.x * cr - c.y * sr, c.x * sr + c.y * cr);
        world = center + (right * rc.x + up * rc.y) * r;
    }
    return gpuParticleCommon(P, f, p, world, c);
}

static float3 ribbonSample(GpuParticle p, device const float4* history, uint idx, uint K, uint head, uint s) {
    if (s == 0) return p.posAge.xyz;
    return history[idx * K + (head + K - (s - 1)) % K].xyz;
}

// Ribbons: the particle's current position followed by its history ring (newest first),
// expanded to a camera-facing strip that tapers and fades toward the tail.
vertex GpuParticleOut gpuRibbonVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                      constant GpuEmitterParams& P [[buffer(0)]],
                                      device const GpuParticle* particles [[buffer(1)]],
                                      device const uint* drawList [[buffer(2)]], constant GpuDraw& dr [[buffer(3)]],
                                      constant FrameUniforms& f [[buffer(4)]], device const float4* history [[buffer(5)]]) {
    uint idx = drawList[iid * dr.stride + dr.offset];
    GpuParticle p = particles[idx];
    uint K = dr.trailSlots;
    uint seg = vid >> 1;
    float side = (vid & 1) ? 1.0 : -1.0;
    uint head = uint(P.trail.z);
    float age = p.posAge.w;
    // Walk back through valid samples; invalid ones collapse onto the last valid point.
    float3 pt = p.posAge.xyz, prev = pt, next = pt;
    uint valid = 0;
    for (uint s = 1; s <= K; ++s) {
        float4 h = history[idx * K + (head + K - (s - 1)) % K];
        if (h.w < 0.0 || h.w > age + 1e-4) break;
        valid = s;
    }
    uint sIdx = min(seg, valid);
    pt = ribbonSample(p, history, idx, K, head, sIdx);
    prev = ribbonSample(p, history, idx, K, head, sIdx > 0 ? sIdx - 1 : 0);
    next = ribbonSample(p, history, idx, K, head, min(sIdx + 1, valid));
    float3 tang = prev - next;
    if (dot(tang, tang) < 1e-12) tang = p.velLife.xyz + float3(0, 1e-4, 0);
    tang = normalize(tang);
    float3 toCam = normalize(f.cameraPos.xyz - pt);
    float3 across = normalize(cross(tang, toCam) + 1e-6);
    float tail = valid > 0 ? float(sIdx) / float(max(valid, 1u)) : 0.0;
    float t = age / p.velLife.w;
    float w = particleSizeAt(P, t) * p.misc.z * 0.5 * (1.0 - tail * 0.85);
    float3 world = pt + across * side * w;
    GpuParticleOut o = gpuParticleCommon(P, f, p, world, float2(side, 1.0 - 2.0 * tail));
    o.color *= (1.0 - tail) * (1.0 - tail * 0.3);
    return o;
}

struct GpuEffectOut {
    float4 color [[color(0)]];
    float reactive [[color(1)]];
};

fragment GpuEffectOut gpuParticleFragment(GpuParticleOut in [[stage_in]], constant GpuEmitterParams& P [[buffer(0)]],
                                          constant FrameUniforms& f [[buffer(1)]], constant GPULight* lights [[buffer(2)]],
                                          depth2d<float> shadowAtlas [[texture(1)]], texturecube<float> envTex [[texture(5)]],
                                          depth2d<float> sceneDepth [[texture(7)]], texture2d<float> sheet [[texture(8)]]) {
    float2 suv = in.position.xy * f.viewport.zw;
    float sd = sceneDepth.sample(pointClamp, suv);
    float camDist = distance(f.cameraPos.xyz, in.worldPos);
    float soft = 1.0;
    if (sd < 0.99999) {
        float gap = distance(f.cameraPos.xyz, reconstructWorld(f, suv, sd)) - camDist;
        if (gap <= 0.0) discard_fragment();
        soft = saturate(gap / max(in.softness, 0.005));
    }
    soft *= saturate((camDist - 0.1) / max(in.size, 0.05));
    soft *= saturate((camDist - 0.25) / 0.75);  // particles brushing past the lens fade out
    int look = int(in.look + 0.5);
    int facing = int(P.look.y + 0.5);
    float2 p = in.uv;
    float r2 = dot(p, p);
    float time = f.cameraPos.w;
    float3 toCam = normalize(f.cameraPos.xyz - in.worldPos);
    float3 right = normalize(cross(-toCam, abs(toCam.y) > 0.99 ? float3(0, 0, 1) : float3(0, 1, 0)));
    float3 up = cross(right, -toCam);
    float3 rgb = 0.0;
    float a = 0.0;
    if (facing == 3) {  // ribbon: soft across, already faded along
        float across = exp(-p.x * p.x * 3.0);
        if (look == 2 || look == 6 || look == 4 || look == 5) {
            float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, toCam, in.position.xy, 0.5);
            a = in.color.a * across;
            rgb = in.color.rgb * lit * a;
        } else {
            rgb = in.color.rgb * in.color.a * across;
        }
    } else if (look == 8) {  // sprite / flipbook sheet, lit or emissive
        float2 tuv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5) / P.flipbook.xy;
        float4 s0 = sheet.sample(materialSampler, in.frameA + tuv);
        float4 s1 = sheet.sample(materialSampler, in.frameB + tuv);
        float4 s = P.material.w > 0.5 ? mix(s0, s1, in.frameBlend) : float4(1.0, 1.0, 1.0, saturate(1.0 - r2));
        a = s.a * in.color.a;
        if (P.look.z > 1.001) {
            rgb = s.rgb * in.color.rgb * a;  // emissive (intensity > 1)
        } else {
            float3 n3 = normalize(right * p.x + up * p.y + toCam * sqrt(max(0.0, 1.0 - r2)));
            float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, n3, in.position.xy, 0.5);
            rgb = s.rgb * in.color.rgb * lit * a;
        }
    } else if (look == 1) {  // flame tongue
        float y01 = p.y * 0.5 + 0.5;
        float flow = time * 2.6 + in.seed * 11.0;
        float n1 = fbm(float2(p.x * 1.6 + in.seed * 7.0, p.y * 0.9 - flow));
        float n2 = fbm(float2(p.x * 3.4 - in.seed * 3.0, p.y * 1.8 - flow * 1.7));
        float sway = (n1 - 0.5) * 0.7 * y01 * y01;
        float width = 0.62 * pow(saturate(1.0 - y01), 0.75) + 0.04;
        float body = saturate(1.0 - abs(p.x + sway) / width);
        float erode = saturate((n2 * 1.25 + (1.0 - y01) * 0.95 - 0.78) * 3.2);
        float d = body * erode * smoothstep(0.0, 0.18, y01) * in.color.a;
        float heat = saturate(d * (1.15 - y01 * 0.75));
        float3 col = mix(in.color.rgb * float3(0.75, 0.32, 0.12), in.color.rgb, smoothstep(0.08, 0.5, heat));
        col = mix(col, in.color.rgb * float3(1.0, 1.35, 1.9), smoothstep(0.55, 1.0, heat) * 0.7);
        a = saturate(d * 1.3) * 0.6;
        rgb = col * a;
    } else if (look == 2 || look == 6) {  // smoke / mist
        bool mist = look == 6;
        float2 q = p * (mist ? 0.7 : 1.25) + float2(in.seed * 17.0, in.seed * 5.0 - in.age * 0.7);
        float n = fbm(q * 1.6) * 0.65 + fbm(q * 3.7 + float2(time * 0.05, -time * 0.03)) * 0.35;
        float sphere = pow(saturate(1.0 - r2), mist ? 2.2 : 1.4);
        float density = saturate(sphere * (0.35 + n * 1.25) - 0.12);
        a = density * in.color.a;
        float3 n3 = normalize(right * p.x + up * p.y + toCam * sqrt(max(0.0, 1.0 - r2)) + float3(0, (n - 0.5) * 0.6, 0));
        float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, n3, in.position.xy, mist ? 0.8 : 0.5);
        rgb = in.color.rgb * lit * (0.75 + 0.25 * n) * a;
    } else if (look == 4 || look == 7) {  // rain streak / droplet
        float across = exp(-p.x * p.x * 4.0);
        float along = smoothstep(1.0, 0.55, abs(p.y));
        a = in.color.a * across * along;
        float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, toCam, in.position.xy, 0.6);
        rgb = in.color.rgb * (lit * 0.55 + 0.02) * a;
    } else if (look == 5) {  // snow
        a = smoothstep(1.0, 0.25, sqrt(r2)) * in.color.a;
        float3 lit = particleLighting(f, lights, shadowAtlas, envTex, in.worldPos, toCam, in.position.xy, 0.3);
        rgb = in.color.rgb * lit * 0.8 * a;
    } else if (look == 3) {  // spark streak
        float g = exp(-p.x * p.x * 5.0) * (1.0 - smoothstep(0.6, 1.0, abs(p.y)));
        rgb = in.color.rgb * g * in.color.a;
    } else {  // glow
        float g = max(exp(-r2 * 5.0) - 0.0067, 0.0);
        rgb = in.color.rgb * g * in.color.a;
    }
    float fogAmt = fogFactor(f, in.worldPos);
    bool blended = look == 2 || look == 4 || look == 5 || look == 6 || look == 7 || (look == 8 && P.look.z <= 1.001);
    if (blended) rgb = mix(rgb, f.fog.rgb * a, fogAmt);
    else rgb *= 1.0 - fogAmt;
    GpuEffectOut o;
    o.color = float4(rgb, a) * soft;
    // Reactive mask: tells TAA to trust this frame where particles move (no ghost trails).
    float lum = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    o.reactive = saturate(max(a, lum * 0.6) * soft);
    return o;
}

// --- mesh particles (instanced, lit by the standard surface shader `meshFragment`) ----------

static float3x3 particleRotation(float angle, float seed) {
    float3 axis = normalize(float3(sin(seed * 41.3), cos(seed * 17.7) * 0.7 + 0.3, sin(seed * 29.1 + 1.3)));
    float c = cos(angle), s = sin(angle), t = 1.0 - c;
    float3 a = axis;
    return float3x3(float3(t * a.x * a.x + c, t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y),
                    float3(t * a.x * a.y - s * a.z, t * a.y * a.y + c, t * a.y * a.z + s * a.x),
                    float3(t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c));
}

vertex MeshOut gpuMeshParticleVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                     const device Vertex* verts [[buffer(0)]],
                                     constant FrameUniforms& f [[buffer(2)]],
                                     constant GpuEmitterParams& P [[buffer(3)]],
                                     device const GpuParticle* particles [[buffer(4)]],
                                     device const uint* drawList [[buffer(5)]]) {
    GpuParticle p = particles[drawList[iid]];
    Vertex v = verts[vid];
    float t = p.posAge.w / p.velLife.w;
    float size = particleSizeAt(P, t) * p.misc.z;
    float3x3 R = particleRotation(p.misc.x + p.misc.y * 6.2831853, p.misc.y);
    float3 world = p.posAge.xyz + R * (float3(v.position) * size);
    float alpha;
    float3 rgb = particleColorAt(P, t, p.misc2.x, alpha);
    MeshOut o;
    o.fade = 0.0;
    o.position = f.viewProj * float4(world, 1.0);
    o.worldPos = world;
    o.normal = R * float3(v.normal);
    if (P.extra.w > 0.5) {
        // Thin translucent meshes (leaves, petals): the side facing away from the sun is lit by
        // the light coming through it. meshFragment flips the normal of back faces, so pre-flip.
        float3 V = normalize(f.cameraPos.xyz - world);
        float3 L = -f.sunDir.xyz;
        float sv = dot(o.normal, V) >= 0.0 ? 1.0 : -1.0;
        float3 Nv = o.normal * sv;
        if (dot(Nv, L) < 0.0) o.normal = normalize(Nv + L * 1.2) * sv;
    }
    o.uv = float2(v.uv);
    o.color = float4(rgb * float3(float4(v.color).rgb), 1.0);
    return o;
}

vertex float4 gpuMeshParticleShadowVertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                          const device Vertex* verts [[buffer(0)]],
                                          constant float4x4& lightViewProj [[buffer(2)]],
                                          constant GpuEmitterParams& P [[buffer(3)]],
                                          device const GpuParticle* particles [[buffer(4)]],
                                          device const uint* drawList [[buffer(5)]]) {
    GpuParticle p = particles[drawList[iid]];
    float t = p.posAge.w / p.velLife.w;
    float size = particleSizeAt(P, t) * p.misc.z;
    float3x3 R = particleRotation(p.misc.x + p.misc.y * 6.2831853, p.misc.y);
    return lightViewProj * float4(p.posAge.xyz + R * (float3(verts[vid].position) * size), 1.0);
}
