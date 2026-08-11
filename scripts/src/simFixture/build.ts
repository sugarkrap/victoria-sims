import { instanceOf, instanceHighOf } from "../../lib/hash.ts";
import {
    TYPE_CRES,
    TYPE_SHPE,
    TYPE_GMND,
    TYPE_GMDC,
    TYPE_TXMT,
    TYPE_TXTR,
    TYPE_SKIN_ENTRY,
    TYPE_KEY_LIST,
    GROUP,
    type Colour,
} from "../scenegraph/types.ts";
import { buildSkeleton, buildPartTree } from "../scenegraph/cres.ts";
import { buildShape, type MaterialBinding } from "../scenegraph/shpe.ts";
import { buildGeometryNode } from "../scenegraph/gmnd.ts";
import { buildContainer, type Component, type Primitive, type MorphChannel } from "../scenegraph/gmdc.ts";
import { buildMaterial, buildTexture } from "../scenegraph/material.ts";
import { buildSkinEntry, buildKeyList } from "../scenegraph/catalog.ts";
import { buildPackage, type PackagedResource } from "../scenegraph/package.ts";
import { box, simpleComponent, type Geometry } from "../scenegraph/geometry.ts";
import { BONES, BIND_POSES } from "./skeleton.ts";

export interface SimFixture {
    resources: PackagedResource[];
    data: Buffer;
}

// A Sim, authored from nothing.
//
// The engine's whole-Sim path — find four resources by name, walk each to a
// container, join them, hang them on a skeleton, then dress them out of the
// catalogue — has never had a fixture before this. The test disc carries a
// teapot, which is one rigid model with no bones, no skeleton and no
// catalogue entry, so `make verify` could not catch a regression in any of
// it. Three defects reached a screen because of that: a Sim's head left
// behind by a merge, part textures lost on the first pose, and a face
// painted green because a part index was handed to something that wanted a
// primitive index.
//
// Nothing here may come from a retail disc, so none of it does. Every byte
// is written out from the format the engine's own readers expect, which is
// why this is long: it is a writer for seven formats that only had readers.
//
// The Sim is boxes. It is not meant to look like anything — it is meant to
// have the STRUCTURE a Sim has: several parts, one of them with two
// primitives and two materials, all of them weighted to one skeleton, and
// catalogue entries that name replacements for them.
export function buildSimFixture(): SimFixture {
    const resources: PackagedResource[] = [];

    const add = (typeIdentifier: number, name: string, payload: Buffer): void => {
        resources.push([typeIdentifier, GROUP, instanceOf(name), instanceHighOf(name), payload]);
    };

    // ---- the skeleton every part hangs on -------------------------------
    add(TYPE_CRES, "auskel_cres", buildSkeleton("auskel_cres", BONES));

    // ---- a part: CRES, SHPE, GMND, GMDC, and what paints it -------------
    const addPart = (
        partName: string,
        shapeName: string,
        nodeName: string,
        containerName: string,
        components: readonly Component[],
        primitives: readonly Primitive[],
        bindings: readonly MaterialBinding[],
        morphChannels: readonly MorphChannel[],
    ): void => {
        add(TYPE_CRES, partName, buildPartTree(partName, shapeName));
        add(TYPE_SHPE, shapeName, buildShape(shapeName, [nodeName], bindings));
        add(TYPE_GMND, nodeName, buildGeometryNode(nodeName, containerName));
        add(TYPE_GMDC, containerName, buildContainer(containerName, components, primitives, BIND_POSES, morphChannels));
    };

    const addPaint = (materialName: string, textureName: string, colour: Colour, size = 4): void => {
        add(TYPE_TXMT, `${materialName}_txmt`, buildMaterial(`${materialName}_txmt`, materialName, textureName));
        add(TYPE_TXTR, `${textureName}_txtr`, buildTexture(`${textureName}_txtr`, size, size, colour));
    };

    // The body: TWO components and TWO primitives with TWO materials.
    //
    // This is the shape that mattered. While every part had exactly one
    // primitive, a part index and a primitive index were the same number and
    // nothing could tell them apart — the first garment with two primitives
    // made every texture after it land one range early, and a Sim came out
    // with a green face. A fixture whose parts all had one primitive would
    // have agreed with the bug.
    const { component: torso, faces: torsoFaces } = simpleComponent(
        box(-0.3, 0.5, -0.15, 0.6, 0.7, 0.3),
        1,
        [1, 0, 0, 0],
        [0.05, 0.0, 0.0],
    );
    const { component: legs, faces: legFaces } = simpleComponent(box(-0.25, 0.0, -0.12, 0.5, 0.5, 0.24), 0);
    addPart(
        "amBodyNaked_cres",
        "ambodynaked_shpe",
        "amBodyNaked_tslocator_gmnd",
        "ambodynaked_gmdc",
        [torso, legs],
        [
            [0, "body", torsoFaces, [1]],
            [1, "legs", legFaces, [0]],
        ],
        [
            ["body", "ambodynaked_torso"],
            ["legs", "ambodynaked_legs"],
        ],
        [
            ["", ""],
            ["botmorphs", "fatbot"],
        ],
    );
    // The tone is read off whatever the body ended up wearing — "s1" out of
    // "ambodynaked-nude-s1" — so the texture has to be named the way the
    // disc names one or the face's tone rule has nothing to match against.
    addPaint("ambodynaked_torso", "ambodynaked-nude-s1", [200, 160, 130, 255]);
    addPaint("ambodynaked_legs", "ambodynaked-legs-s1", [120, 110, 160, 255]);

    const { component: head, faces: headFaces } = simpleComponent(box(-0.2, 1.5, -0.2, 0.4, 0.4, 0.4), 2);
    addPart(
        "amFace_cres",
        "amface_shpe",
        "amFace_tslocator_gmnd",
        "amface_gmdc",
        [head],
        [[0, "face", headFaces, [2]]],
        [["face", "uuface_browbushy_brown"]],
        [],
    );
    addPaint("uuface_browbushy_brown", "uuface-browbushy-brown", [90, 60, 40, 255]);
    addPaint("amface-s1", "amface-s1", [210, 170, 140, 255]);

    const { component: scalp, faces: scalpFaces } = simpleComponent(box(-0.21, 1.88, -0.21, 0.42, 0.06, 0.42), 2);
    addPart(
        "amHairBald_cres",
        "amhairbald_shpe",
        "amHairBald_tslocator_gmnd",
        "amhairbald_gmdc",
        [scalp],
        [[0, "hair", scalpFaces, [2]]],
        [["hair", "amhairbald_skin_s1"]],
        [],
    );
    addPaint("amhairbald_skin_s1", "umhairbald-skin-s1", [200, 160, 130, 255]);

    // ---- the catalogue: what this Sim can wear instead ------------------
    //
    // An entry names a shape by an INDEX into a key list beside it, and the
    // key list is matched on group and instance both. Nothing about the
    // entry says which shape; the index and the sidecar together do.
    const addGarment = (
        entryName: string,
        outfitSlot: number,
        geometry: Geometry,
        boneIndex: number,
        subsetName: string,
        materialName: string,
        textureName: string,
        colour: Colour,
        morphChannels: readonly MorphChannel[],
    ): void => {
        const shapeName = `${entryName}_shpe`;
        const nodeName = `${entryName}_gmnd`;
        const containerName = `${entryName}_gmdc`;
        const { component, faces } = simpleComponent(geometry, boneIndex);
        add(TYPE_SHPE, shapeName, buildShape(shapeName, [nodeName], [[subsetName, `${materialName}_asbound`]]));
        add(TYPE_GMND, nodeName, buildGeometryNode(nodeName, containerName));
        add(
            TYPE_GMDC,
            containerName,
            buildContainer(containerName, [component], [[0, subsetName, faces, [boneIndex]]], BIND_POSES, morphChannels),
        );
        // What the SHAPE binds is one arbitrary colourway, and it is
        // deliberately the wrong one here: the entry's own override names
        // the right one, and a check can tell which arrived by the pixel.
        addPaint(`${materialName}_asbound`, `${textureName}-asbound`, [255, 0, 255, 255]);
        addPaint(materialName, textureName, colour);

        // The entry and its sidecar. Key 0 is the shape and key 1 the
        // material, so shapekeyidx and override0resourcekeyidx point at
        // different things and swapping them cannot pass.
        const keys = [
            [TYPE_SHPE, GROUP, instanceOf(shapeName), instanceHighOf(shapeName)] as const,
            [TYPE_TXMT, GROUP, instanceOf(`${materialName}_txmt`), instanceHighOf(`${materialName}_txmt`)] as const,
        ];
        add(
            TYPE_SKIN_ENTRY,
            entryName,
            buildSkinEntry([
                ["type", "skin"],
                ["name", entryName],
                ["outfit", outfitSlot],
                ["category", 0x07],
                ["age", 0x10],
                ["gender", 0x02],
                ["species", 0x01],
                ["shapekeyidx", 0],
                ["resourcekeyidx", 0],
                ["numoverrides", 1],
                ["override0subset", subsetName],
                ["override0resourcekeyidx", 1],
                ["override0shape", 0],
            ]),
        );
        // Matched on group AND instance, so the list must share both with
        // the entry that indexes it.
        resources.push([TYPE_KEY_LIST, GROUP, instanceOf(entryName), instanceHighOf(entryName), buildKeyList(keys)]);
    };

    addGarment(
        "ambodyoveralls_blue",
        0x08,
        box(-0.32, 0.0, -0.17, 0.64, 1.2, 0.34),
        1,
        "body",
        "ambodyoveralls_blue",
        "ambodyoveralls-blue",
        [40, 70, 200, 255],
        [
            ["", ""],
            ["botmorphs", "fatbot"],
        ],
    );
    addGarment(
        "amtopjersey_green",
        0x04,
        box(-0.33, 0.5, -0.18, 0.66, 0.72, 0.36),
        1,
        "top",
        "amtopjersey_green",
        "amtopjersey-green",
        [40, 180, 70, 255],
        [
            ["", ""],
            ["topmorphs", "fattop"],
        ],
    );
    addGarment(
        "ambottomshorts_red",
        0x10,
        box(-0.27, 0.0, -0.14, 0.54, 0.52, 0.28),
        0,
        "bottom",
        "ambottomshorts_red",
        "ambottomshorts-red",
        [200, 50, 50, 255],
        [
            ["", ""],
            ["botmorphs", "fatbot"],
        ],
    );
    // Two faces: the wrong tone first, so the walk meets it first and the
    // right one has to displace it. A fixture that only offered the right
    // one would pass whether or not the tone rule existed.
    addGarment(
        "CASIE_amface_s9",
        0x02,
        box(-0.2, 1.5, -0.2, 0.4, 0.4, 0.4),
        2,
        "face",
        "amface_s9",
        "amface-s9",
        [10, 200, 10, 255],
        [],
    );
    addGarment(
        "CASIE_amface_s1",
        0x02,
        box(-0.2, 1.5, -0.2, 0.4, 0.4, 0.4),
        2,
        "face",
        "amface_s1_worn",
        "amface-s1-worn",
        [210, 170, 140, 255],
        [],
    );
    addGarment(
        "amhairshort_brown",
        0x01,
        box(-0.22, 1.86, -0.22, 0.44, 0.1, 0.44),
        2,
        "hair",
        "amhairshort_brown",
        "amhairshort-brown",
        [80, 50, 30, 255],
        [],
    );
    // A child's body, which must be refused: it is authored for a skeleton
    // this Sim has not got, and taking it would resolve perfectly and come
    // apart.
    addGarment(
        "cmbodyromper_yellow",
        0x08,
        box(-0.2, 0.0, -0.1, 0.4, 0.8, 0.2),
        1,
        "body",
        "cmbodyromper_yellow",
        "cmbodyromper-yellow",
        [230, 210, 60, 255],
        [
            ["", ""],
            ["botmorphs", "fatbot"],
        ],
    );

    return { resources, data: buildPackage(resources) };
}
