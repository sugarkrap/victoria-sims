import { Component } from "./gmdc.ts";

export interface Geometry {
    positions: number[];
    normals: number[];
    textures: number[];
    faces: number[];
}

// Eight corners and twelve triangles. Normals point out along y, which is
// enough for a fixture: what is being tested is the plumbing, not the
// shading.
export function box(originX: number, originY: number, originZ: number, width: number, height: number, depth: number): Geometry {
    const positions: number[] = [];
    const normals: number[] = [];
    const textures: number[] = [];
    for (const cornerY of [0.0, height]) {
        for (const cornerZ of [0.0, depth]) {
            for (const cornerX of [0.0, width]) {
                positions.push(originX + cornerX, originY + cornerY, originZ + cornerZ);
                normals.push(0.0, 1.0, 0.0);
                textures.push(cornerX / Math.max(width, 0.001), cornerZ / Math.max(depth, 0.001));
            }
        }
    }
    const faces = [
        0, 1, 2, 1, 3, 2, // bottom
        4, 6, 5, 5, 6, 7, // top
        0, 4, 1, 1, 4, 5,
        2, 3, 6, 3, 7, 6,
        0, 2, 4, 2, 6, 4,
        1, 5, 3, 3, 5, 7,
    ];
    return { positions, normals, textures, faces };
}

// Every vertex on one bone, at full weight. The remaining three slots say
// 'no bone' rather than bone zero, which would drag the part along with
// whatever joint happened to be first.
function weightedTo(vertexCount: number, boneIndex: number): { slots: number[][]; weights: number[] } {
    const slots = Array.from({ length: vertexCount }, () => [boneIndex, 0xff, 0xff, 0xff]);
    const weights: number[] = [];
    for (let i = 0; i < vertexCount; i += 1) {
        weights.push(1.0, 0.0, 0.0);
    }
    return { slots, weights };
}

export interface SimpleComponentResult {
    component: Component;
    faces: number[];
}

export function simpleComponent(
    geometry: Geometry,
    boneIndex: number,
    morphChannelSlots: readonly number[] | null = null,
    morphDelta: readonly number[] | null = null,
): SimpleComponentResult {
    const { positions, normals, textures, faces } = geometry;
    const vertexCount = Math.floor(positions.length / 3);
    const { slots, weights } = weightedTo(vertexCount, boneIndex);
    let morphMap: number[][] | null = null;
    let morphDeltas: number[][] | null = null;
    if (morphChannelSlots !== null) {
        morphMap = Array.from({ length: vertexCount }, () => [...morphChannelSlots]);
        morphDeltas = [Array<readonly number[]>(vertexCount).fill(morphDelta!).flat()];
    }
    return { component: new Component(positions, normals, textures, slots, weights, morphMap, morphDeltas), faces };
}
