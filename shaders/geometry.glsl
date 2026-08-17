#ifndef GLASSLIGHT_GEOMETRY_GLSL
#define GLASSLIGHT_GEOMETRY_GLSL

const float GL_PI = 3.14159265358979323846;

uint glassHash32(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

float glassRandom01(uint x) {
    return float(glassHash32(x)) * (1.0 / 4294967296.0);
}

vec3 glassRotateAxis(vec3 point, vec3 axis, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return point * c + cross(axis, point) * s + axis * dot(axis, point) * (1.0 - c);
}

vec3 glassQuarterTurn(vec3 p, uint turn) {
    if (turn == 1u) return vec3(-p.y, p.x, p.z);
    if (turn == 2u) return vec3(-p.x, -p.y, p.z);
    if (turn == 3u) return vec3(p.y, -p.x, p.z);
    return p;
}

float glassRegularSupport(vec2 p, uint sides) {
    const float c18 = 0.9510565163;
    const float s18 = 0.3090169944;
    const float c36 = 0.8090169944;
    const float s36 = 0.5877852523;
    const float c30 = 0.8660254038;
    const float s30 = 0.5;
    float support = max(abs(p.x), abs(p.y));
    if (sides == 8u) {
        return max(support, max(abs((p.x + p.y) * 0.7071067812),
                                abs((p.x - p.y) * 0.7071067812)));
    }
    if (sides == 10u) {
        support = max(support, abs(c36 * p.x + s36 * p.y));
        support = max(support, abs(c36 * p.x - s36 * p.y));
        support = max(support, abs(c18 * p.x + s18 * p.y));
        return max(support, abs(c18 * p.x - s18 * p.y));
    }
    support = max(support, abs(c30 * p.x + s30 * p.y));
    support = max(support, abs(c30 * p.x - s30 * p.y));
    support = max(support, abs(s30 * p.x + c30 * p.y));
    return max(support, abs(s30 * p.x - c30 * p.y));
}

float glassPebbleLocal(vec3 p, uint seed) {
    vec3 stretch = vec3(0.88 + 0.12 * glassRandom01(seed ^ 0x21u),
                        1.02 + 0.15 * glassRandom01(seed ^ 0x37u),
                        0.83 + 0.16 * glassRandom01(seed ^ 0x51u));
    vec3 q = p / stretch;
    float seedWarp = float(seed & 255u) / 255.0 - 0.5;
    float ripple = 0.055 * (q.x * q.y + 0.7 * q.y * q.z + 0.5 * q.z * q.x) +
                   0.018 * seedWarp * (q.x * q.x - q.z * q.z);
    return (length(q) - (0.68 + ripple)) * min(stretch.x, min(stretch.y, stretch.z));
}

float glassLensLocal(vec3 p, uint seed) {
    float ellipticity = 0.88 + 0.15 * glassRandom01(seed ^ 0x718u);
    vec2 q = p.xy / vec2(ellipticity, 1.0 / ellipticity);
    float radial = length(q);
    float faces = abs(p.z) + 0.58 * radial * radial - 0.39;
    float rim = radial - 0.78;
    float organic = 0.055 * q.x * q.y * (q.x * q.x - q.y * q.y);
    return max(faces, rim + organic);
}

float glassRibbonLocal(vec3 p, uint seed) {
    float radial = max(length(p.xy), 1e-5);
    vec2 unit = p.xy / radial;
    vec2 turn2 = vec2(unit.x * unit.x - unit.y * unit.y,
                      2.0 * unit.x * unit.y);
    vec2 turn3 = vec2(turn2.x * unit.x - turn2.y * unit.y,
                      turn2.x * unit.y + turn2.y * unit.x);
    vec2 twist = ((seed >> 5u) & 1u) == 0u ? turn2 : turn3;
    vec2 crossSection = vec2(radial - 0.48, p.z);
    mat2 turn = mat2(twist.x, -twist.y, twist.y, twist.x);
    crossSection = turn * crossSection;
    crossSection.x /= 1.45;
    float waviness = 0.035 * turn3.x * (((seed >> 7u) & 1u) == 0u ? 1.0 : -1.0);
    return length(crossSection) - (0.22 + waviness);
}

// Open hollow shell: the cavity is an upward-infinite negative field removed
// from a bounded, seeded faceted outer body. Its raised cavity floor leaves a
// physically thick glass base while the top remains genuinely open.
float glassFacetedVesselLocal(vec3 p, uint seed) {
    uint sides = 8u + 2u * ((seed >> 9u) % 3u);
    float y = p.y;
    uint profile = glassHash32(seed ^ 0x243f6a88u) % 4u;
    float waist;
    if (profile == 0u) {
        float shoulder = max(0.0, 1.0 - abs(y + 0.08) * 1.35);
        waist = 0.47 + 0.105 * shoulder * shoulder + 0.045 * y;
    } else if (profile == 1u) {
        waist = 0.43 + 0.12 * smoothstep(-0.68, 0.68, y);
    } else if (profile == 2u) {
        float bulb = max(0.0, 1.0 - abs(y + 0.28) * 2.25);
        waist = 0.45 + 0.13 * bulb * bulb +
                0.075 * smoothstep(0.1, 0.72, y);
    } else {
        float wave = abs(fract((y + 0.42) * 0.80) * 2.0 - 1.0);
        waist = 0.49 + 0.052 * (1.0 - 2.0 * wave) + 0.03 * y;
    }
    waist *= 0.96 + 0.08 * glassRandom01(seed ^ 0x85ebca6bu);
    vec2 facetPoint = ((seed >> 15u) & 1u) == 0u ? p.xz :
        vec2((p.x + p.z) * 0.7071067812, (p.z - p.x) * 0.7071067812);
    float support = glassRegularSupport(facetPoint, sides);
    float outerRadial = support - waist;
    float outerCaps = max(-0.79 - y, y - 0.78);
    float outer = max(outerRadial, outerCaps);

    float wallThickness = 0.085 + 0.015 * glassRandom01(seed ^ 0xc2b2ae35u);
    float innerRadius = max(waist - wallThickness, 0.24);
    float innerRadial = support - innerRadius;
    float cavityFloor = -0.45 - y;
    float cavity = max(innerRadial, cavityFloor);
    return max(outer, -cavity);
}

float glassCutReliefLocal(vec3 p, uint seed, float outerRadius, float radialLength) {
    vec2 q = ((seed >> 13u) & 1u) == 0u ? p.xz :
        vec2((p.x + p.z) * 0.7071067812, (p.z - p.x) * 0.7071067812);
    // Wheel cuts stay in the outer half of the wall: visibly subtractive, but
    // not accidental perforations that manufacture extra optical interfaces.
    float radialBand = abs(radialLength - (outerRadius - 0.006)) - 0.032;
    float nearestAxis = min(abs(q.x), abs(q.y));
    float nearestDiagonal = min(abs(q.x + q.y), abs(q.x - q.y)) * 0.7071067812;
    uint mixVariant = (seed >> 19u) & 3u;
    if (mixVariant == 0u) {
        float diamond = max(0.55 * (nearestDiagonal + abs(p.y + 0.05)) - 0.10,
                            radialBand);
        float starburst = max(max(min(abs(q.x - 0.34 * (p.y + 0.44)),
                                      abs(q.y + 0.34 * (p.y + 0.44))) - 0.025,
                                  abs(p.y + 0.31) - 0.27), radialBand);
        return min(diamond, starburst);
    }
    if (mixVariant == 1u) {
        float diamond = max(0.55 * (nearestDiagonal + abs(p.y + 0.05)) - 0.10,
                            radialBand);
        float broad = max(max(nearestDiagonal - 0.115, abs(p.y) - 0.19),
                          radialBand + 0.022);
        return min(diamond, broad);
    }
    float flute = max(max(nearestAxis - 0.030, abs(p.y) - 0.51), radialBand);
    if (mixVariant == 2u) {
        float starburst = max(max(min(abs(q.x - 0.34 * (p.y + 0.44)),
                                      abs(q.y + 0.34 * (p.y + 0.44))) - 0.025,
                                  abs(p.y + 0.31) - 0.27), radialBand);
        return min(flute, starburst);
    }
    float broad = max(max(nearestDiagonal - 0.115, abs(p.y) - 0.19),
                      radialBand + 0.022);
    return min(flute, broad);
}

// Open hollow rocks glass. The raised cavity floor forms a heavy base; planar
// outer/rim bevels and genuinely subtracted seeded cutter volumes create mixed
// diamond, flute, starburst and broad-plane relief rather than a normal map.
float glassCutCrystalLocal(vec3 p, uint seed) {
    float y = p.y;
    float outerRadius = 0.50 + 0.075 * smoothstep(-0.70, 0.69, y);
    vec2 q = ((seed >> 7u) & 1u) == 0u ? p.xz :
        vec2((p.x + p.z) * 0.7071067812, (p.z - p.x) * 0.7071067812);
    float primarySupport = glassRegularSupport(q, 12u);
    float bevelSupport = length(q);
    float primary = primarySupport - outerRadius;
    float cornerBevel = bevelSupport - (outerRadius + 0.025);
    float outerCaps = max(-0.72 - y, y - 0.70);
    float baseBevel = bevelSupport -
        (outerRadius - 0.16 * max(-0.53 - y, 0.0));
    float outer = max(max(primary, cornerBevel), max(outerCaps, baseBevel));

    float innerRadius = max(outerRadius - 0.085, 0.30);
    float innerRadial = primarySupport - innerRadius;
    float cavity = max(innerRadial, -0.43 - y);
    float shell = max(outer, -cavity);
    if (abs(bevelSupport - (outerRadius - 0.006)) > 0.040) return shell;
    float relief = glassCutReliefLocal(p, seed, outerRadius, bevelSupport);
    return max(shell, -relief);
}

#ifdef GLASSLIGHT_FRACTURE_SHADER
bool glassFractureShardContains(vec3 p, uint shardIndex, float tolerance) {
    uint planeCount = settings.fractureShards[shardIndex].paletteSidesPlanes.w;
    for (uint plane = 0u; plane < 13u; ++plane) {
        if (plane >= planeCount) break;
        vec4 halfPlane = settings.fracturePlanes[shardIndex].values[plane];
        if (dot(halfPlane.xyz, p) - halfPlane.w > tolerance) return false;
    }
    return true;
}

bool glassFractureContainsLocal(vec3 p) {
    uint count = min(settings.fractureInfo.x, 16u);
    for (uint shard = 0u; shard < 16u; ++shard) {
        if (shard >= count) break;
        if (glassFractureShardContains(p, shard, 0.00002)) return true;
    }
    return false;
}

// A convex shard field is the maximum of its normalized half-plane distances;
// the cluster is their union. This fallback exists for diagnostics only: the
// Fracture render path clips rays analytically below.
float glassFractureLocal(vec3 p) {
    float cluster = 1e10;
    uint count = min(settings.fractureInfo.x, 16u);
    for (uint shard = 0u; shard < 16u; ++shard) {
        if (shard >= count) break;
        float crystal = -1e10;
        uint planeCount = settings.fractureShards[shard].paletteSidesPlanes.w;
        for (uint plane = 0u; plane < 13u; ++plane) {
            if (plane >= planeCount) break;
            vec4 halfPlane = settings.fracturePlanes[shard].values[plane];
            crystal = max(crystal, dot(halfPlane.xyz, p) - halfPlane.w);
        }
        cluster = min(cluster, crystal);
    }
    return cluster;
}
#endif

vec3 glassWorldToLocal(vec3 worldPoint) {
    vec3 axis = normalize(settings.rotation.xyz);
    return glassRotateAxis(worldPoint, axis, -settings.rotation.w * 2.0 * GL_PI) /
           max(settings.geometry.x, 0.05);
}

float glassSdfLocal(vec3 p) {
#ifdef GLASSLIGHT_FRACTURE_SHADER
    return glassFractureLocal(p);
#else
    uint family = settings.shapeRegion.x;
    uint seed = settings.shapeRegion.w;
    if (family == 2u) return glassLensLocal(p, seed);
    if (family == 3u) return glassRibbonLocal(p, seed);
    if (family == 4u) return glassFacetedVesselLocal(p, seed);
    if (family == 5u) return glassCutCrystalLocal(p, seed);
    return glassPebbleLocal(p, seed);
#endif
}

// Negative means glass. Positive means air, including vessel cavities and the
// gaps between disconnected fracture pieces.
float glassSdf(vec3 worldPoint) {
    return glassSdfLocal(glassWorldToLocal(worldPoint)) * max(settings.geometry.x, 0.05);
}

vec3 glassNormal(vec3 p) {
#ifdef GLASSLIGHT_FRACTURE_SHADER
    {
        vec3 localPoint = glassWorldToLocal(p);
        vec3 bestNormal = vec3(0.0, 1.0, 0.0);
        float bestDistance = 1e10;
        uint count = min(settings.fractureInfo.x, 16u);
        for (uint shard = 0u; shard < 16u; ++shard) {
            if (shard >= count) break;
            uint planeCount = settings.fractureShards[shard].paletteSidesPlanes.w;
            for (uint plane = 0u; plane < 13u; ++plane) {
                if (plane >= planeCount) break;
                vec4 halfPlane = settings.fracturePlanes[shard].values[plane];
                float distanceValue = abs(dot(halfPlane.xyz, localPoint) - halfPlane.w);
                if (distanceValue < bestDistance &&
                    !glassFractureContainsLocal(localPoint + halfPlane.xyz * 0.0015)) {
                    bestDistance = distanceValue;
                    bestNormal = halfPlane.xyz;
                }
            }
        }
        vec3 axis = normalize(settings.rotation.xyz);
        return normalize(glassRotateAxis(bestNormal, axis,
                                         settings.rotation.w * 2.0 * GL_PI));
    }
#else
    const float e = 0.00125;
    const vec3 k1 = vec3(1.0, -1.0, -1.0);
    const vec3 k2 = vec3(-1.0, -1.0, 1.0);
    const vec3 k3 = vec3(-1.0, 1.0, -1.0);
    const vec3 k4 = vec3(1.0, 1.0, 1.0);
    return normalize(k1 * glassSdf(p + k1 * e) +
                     k2 * glassSdf(p + k2 * e) +
                     k3 * glassSdf(p + k3 * e) +
                     k4 * glassSdf(p + k4 * e));
#endif
}

#ifdef GLASSLIGHT_FRACTURE_SHADER
bool glassFractureRayInterval(vec3 origin, vec3 direction, uint shardIndex,
                              out float enterT, out float exitT) {
    FractureShardGpu shard = settings.fractureShards[shardIndex];
    vec3 sphereCenter = shard.baseLength.xyz +
                        shard.axisRadius.xyz * (shard.baseLength.w * 0.5);
    uint sides = max(shard.paletteSidesPlanes.z, 3u);
    float radiusFactor = sides == 3u ? 2.0 :
        (sides == 4u ? 1.414214 : (sides == 5u ? 1.236068 : 1.154701));
    float circumradius = shard.axisRadius.w * radiusFactor;
    float sphereRadius = sqrt(shard.baseLength.w * shard.baseLength.w * 0.25 +
                              circumradius * circumradius);
    vec3 relativeOrigin = origin - sphereCenter;
    float sphereB = dot(relativeOrigin, direction);
    float sphereC = dot(relativeOrigin, relativeOrigin) - sphereRadius * sphereRadius;
    if (sphereB * sphereB - sphereC < 0.0) return false;
    enterT = -1e10;
    exitT = 1e10;
    uint planeCount = shard.paletteSidesPlanes.w;
    for (uint plane = 0u; plane < 13u; ++plane) {
        if (plane >= planeCount) break;
        vec4 halfPlane = settings.fracturePlanes[shardIndex].values[plane];
        float numerator = halfPlane.w - dot(halfPlane.xyz, origin);
        float denominator = dot(halfPlane.xyz, direction);
        if (abs(denominator) < 1e-7) {
            if (numerator < 0.0) return false;
            continue;
        }
        float t = numerator / denominator;
        if (denominator < 0.0) {
            enterT = max(enterT, t);
        } else if (t < exitT) {
            exitT = t;
        }
        if (enterT > exitT) return false;
    }
    return exitT >= max(enterT, 0.0);
}

vec3 glassFractureBoundaryNormal(vec3 localPoint, uint shardIndex) {
    vec3 result = vec3(0.0, 1.0, 0.0);
    float nearest = 1e10;
    uint planeCount = settings.fractureShards[shardIndex].paletteSidesPlanes.w;
    for (uint plane = 0u; plane < 13u; ++plane) {
        if (plane >= planeCount) break;
        vec4 halfPlane = settings.fracturePlanes[shardIndex].values[plane];
        float distanceValue = abs(dot(halfPlane.xyz, localPoint) - halfPlane.w);
        if (distanceValue < nearest) {
            nearest = distanceValue;
            result = halfPlane.xyz;
        }
    }
    return result;
}

// Finds the next boundary of the union, not merely the next shard interval.
// Expanding the connected interval component suppresses internal facets
// wherever rooted crystals overlap.
bool glassFractureNextInterface(vec3 worldOrigin, vec3 worldDirection,
                                float maximumDistance, out vec3 hit,
                                out float traveled, out vec3 outwardNormal) {
    float scale = max(settings.geometry.x, 0.05);
    vec3 axis = normalize(settings.rotation.xyz);
    float angle = -settings.rotation.w * 2.0 * GL_PI;
    vec3 origin = glassWorldToLocal(worldOrigin);
    vec3 direction = normalize(glassRotateAxis(worldDirection, axis, angle));
    float maximumLocal = maximumDistance / scale;
    float cursor = 0.00045 / scale;
    uint count = min(settings.fractureInfo.x, 16u);
    float enters[16];
    float exits[16];
    bool startsInside = false;
    float nextEntry = 1e10;
    uint boundaryShard = 0u;
    float componentExit = -1e10;
    for (uint shard = 0u; shard < 16u; ++shard) {
        enters[shard] = 1e10;
        exits[shard] = -1e10;
        if (shard >= count) continue;
        if (!glassFractureRayInterval(origin, direction, shard,
                                      enters[shard], exits[shard])) continue;
        if (enters[shard] <= cursor && exits[shard] >= cursor) {
            startsInside = true;
            if (exits[shard] > componentExit) {
                componentExit = exits[shard];
                boundaryShard = shard;
            }
        } else if (enters[shard] >= cursor && enters[shard] < nextEntry) {
            nextEntry = enters[shard];
            boundaryShard = shard;
        }
    }

    float nextT = nextEntry;
    if (startsInside) {
        // Grow the connected interval component until every shard that begins
        // before its current end is included. Only simple interval comparisons
        // repeat; every expensive plane clip above runs exactly once.
        for (int mergePass = 0; mergePass < 16; ++mergePass) {
            bool expanded = false;
            for (uint shard = 0u; shard < 16u; ++shard) {
                if (shard >= count || exits[shard] < -1e9) continue;
                if (enters[shard] <= componentExit + 0.00002 &&
                    exits[shard] > componentExit) {
                    componentExit = exits[shard];
                    boundaryShard = shard;
                    expanded = true;
                }
            }
            if (!expanded) break;
        }
        nextT = componentExit;
    }
    if (nextT < cursor || nextT >= 1e9 || nextT > maximumLocal) return false;
    traveled = nextT * scale;
    hit = worldOrigin + worldDirection * traveled;
    vec3 boundaryNormal = glassFractureBoundaryNormal(origin + direction * nextT,
                                                      boundaryShard);
    outwardNormal = normalize(glassRotateAxis(boundaryNormal, axis, -angle));
    return true;
}

float glassFractureEdgeProximity(vec3 worldPoint) {
    vec3 p = glassWorldToLocal(worldPoint);
    float nearest = 1e10;
    float second = 1e10;
    uint count = min(settings.fractureInfo.x, 16u);
    for (uint shard = 0u; shard < 16u; ++shard) {
        if (shard >= count) break;
        if (!glassFractureShardContains(p, shard, 0.003)) continue;
        uint planeCount = settings.fractureShards[shard].paletteSidesPlanes.w;
        for (uint plane = 0u; plane < 13u; ++plane) {
            if (plane >= planeCount) break;
            vec4 halfPlane = settings.fracturePlanes[shard].values[plane];
            float d = abs(dot(halfPlane.xyz, p) - halfPlane.w);
            if (d < nearest) {
                second = nearest;
                nearest = d;
            } else if (d < second) {
                second = d;
            }
        }
    }
    return 1.0 - smoothstep(0.006, 0.045, second);
}
#endif

vec3 glassObjectBoundsCenter() {
#ifdef GLASSLIGHT_FRACTURE_SHADER
    vec3 axis = normalize(settings.rotation.xyz);
    vec3 localCenter = settings.fractureBounds.xyz * max(settings.geometry.x, 0.05);
    return glassRotateAxis(localCenter, axis, settings.rotation.w * 2.0 * GL_PI);
#else
    return vec3(0.0);
#endif
}

float glassObjectBoundsRadius() {
#ifdef GLASSLIGHT_FRACTURE_SHADER
    float localRadius = settings.fractureBounds.w;
#else
    float localRadius = 1.08;
#endif
    return localRadius * max(settings.geometry.x, 0.05);
}

#ifndef GLASSLIGHT_FRACTURE_SHADER
vec3 glassCellPosition(uint index) {
    uint base = glassHash32(settings.shapeRegion.w ^ (index * 0x9e3779b9u));
    return vec3(glassRandom01(base), glassRandom01(base ^ 0x68bc21ebu),
                glassRandom01(base ^ 0x02e5be93u)) * 1.7 - 0.85;
}

vec3 glassCellTint(uint index) {
    if (settings.shapeRegion.z != 0u &&
        glassHash32(settings.shapeRegion.w + index * 19u) % 5u == 0u) {
        return vec3(0.975);
    }
    return settings.palette[index % 5u].rgb;
}

vec3 glassRegionTint(vec3 localPoint) {
#else
vec3 glassRegionTint(vec3 localPoint) {
    {
        uint count = min(settings.fractureInfo.x, 16u);
        uint selected = 0u;
        float selectedBoundary = 1e10;
        for (uint shard = 0u; shard < 16u; ++shard) {
            if (shard >= count) break;
            if (!glassFractureShardContains(localPoint, shard, 0.001)) continue;
            float boundary = 1e10;
            uint planeCount = settings.fractureShards[shard].paletteSidesPlanes.w;
            for (uint plane = 0u; plane < 13u; ++plane) {
                if (plane >= planeCount) break;
                vec4 halfPlane = settings.fracturePlanes[shard].values[plane];
                boundary = min(boundary, abs(dot(halfPlane.xyz, localPoint) - halfPlane.w));
            }
            if (boundary < selectedBoundary) {
                selectedBoundary = boundary;
                selected = shard;
            }
        }
        FractureShardGpu shard = settings.fractureShards[selected];
        float axial = clamp(dot(localPoint - shard.baseLength.xyz,
                                shard.axisRadius.xyz) / max(shard.baseLength.w, 0.02),
                            0.0, 1.0);
        uint salt = glassHash32(settings.shapeRegion.w ^ (selected * 0x9e3779b9u));
        float bands = max(float(settings.shapeRegion.y), 1.0);
        float band = axial * bands + glassRandom01(salt) * bands;
        float transition = 0.5 + 0.5 * sin(2.0 * GL_PI * band);
        float softness = max(settings.geometry.y, 0.001);
        transition = smoothstep(0.5 - softness * 0.48,
                                0.5 + softness * 0.48, transition);
        vec3 primary = settings.palette[shard.paletteSidesPlanes.x % 5u].rgb;
        vec3 secondary = settings.palette[shard.paletteSidesPlanes.y % 5u].rgb;
        if (settings.shapeRegion.z != 0u &&
            glassHash32(salt ^ uint(floor(band))) % 5u == 0u) {
            secondary = vec3(0.975);
        }
        return mix(primary, secondary, transition);
    }
#endif
#ifndef GLASSLIGHT_FRACTURE_SHADER
    uint nearest = 0u;
    uint second = 0u;
    float nearestDistance = 1e10;
    float secondDistance = 1e10;
    uint count = clamp(settings.shapeRegion.y, 1u, 24u);
    for (uint index = 0u; index < 24u; ++index) {
        if (index >= count) break;
        vec3 cell = glassCellPosition(index);
        float distanceValue = dot(localPoint - cell, localPoint - cell);
        if (distanceValue < nearestDistance) {
            secondDistance = nearestDistance;
            second = nearest;
            nearestDistance = distanceValue;
            nearest = index;
        } else if (distanceValue < secondDistance) {
            secondDistance = distanceValue;
            second = index;
        }
    }
    float softness = max(settings.geometry.y, 0.001);
    float edgeBlend = 0.5 * (1.0 - smoothstep(0.0, softness,
                                             secondDistance - nearestDistance));
    return mix(glassCellTint(nearest), glassCellTint(second), edgeBlend);
#endif
}

#endif
