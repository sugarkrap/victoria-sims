import type { Bone, BindPose } from "../scenegraph/types.ts";

// Small enough to read in a log, deep enough to have a hierarchy. Bone
// identifiers are the numbers nodes carry, not positions in the node list —
// they are deliberately not 0, 1, 2 so that a reader confusing the two is
// caught rather than accidentally right.
export const BONES: readonly Bone[] = [
    ["root", 10, -1, [0.0, 0.0, 0.0]],
    ["spine", 11, 0, [0.0, 0.5, 0.0]],
    ["head", 12, 1, [0.0, 1.0, 0.0]],
];

// The inverse bind, one per bone, in the order the primitives' bone lists
// index. Identity rotation and the negated translation, which is what the
// inverse of a pure translation is — so at rest every pair multiplies out to
// the identity and a correct skin moves nothing.
export const BIND_POSES: readonly BindPose[] = [
    [[0.0, 0.0, 0.0, 1.0], [0.0, 0.0, 0.0]],
    [[0.0, 0.0, 0.0, 1.0], [0.0, -0.5, 0.0]],
    [[0.0, 0.0, 0.0, 1.0], [0.0, -1.5, 0.0]],
];
