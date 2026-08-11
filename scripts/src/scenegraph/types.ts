export const TYPE_CRES = 0xe519c933;
export const TYPE_SHPE = 0xfc6eb1f7;
export const TYPE_GMND = 0x7ba3838c;
export const TYPE_GMDC = 0xac4f8687;
export const TYPE_TXMT = 0x49596978;
export const TYPE_TXTR = 0x1c4a276c;
export const TYPE_SKIN_ENTRY = 0xebcf3e27;
export const TYPE_KEY_LIST = 0xac506764;

export const BLOCK_RESOURCE_NODE = 0xe519c933;
export const BLOCK_TRANSFORM_NODE = 0x65246462;
export const BLOCK_SHAPE_REFERENCE_NODE = 0x65245517;

export const COLLECTION_MARK = 0xffff0001;

// The group every fixture resource is filed under. A scenegraph lookup by
// name ignores the group entirely — the name hashes to the instance words —
// so this only has to be consistent. The catalogue's sidecar lookup does NOT
// ignore it: a key list is matched on group AND instance, so an entry and its
// list have to agree, and that agreement is one of the things worth having a
// fixture for.
export const GROUP = 0x1f000000;

// group, instance, instanceHigh, TYPE — the order a file link is written in,
// which is not the order a package index entry uses.
export type ResourceLink = readonly [typeIdentifier: number, group: number, instance: number, instanceHigh: number];

export type Vec3 = readonly [number, number, number];
export type Quat = readonly [number, number, number, number];

export type Bone = readonly [name: string, boneIdentifier: number, parentBoneIndex: number, translation: Vec3];
export type BindPose = readonly [rotation: Quat, translation: Vec3];
export type Colour = readonly [red: number, green: number, blue: number, alpha: number];
