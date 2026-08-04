using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using OpenTS2.Common;
using OpenTS2.Common.Utils;
using OpenTS2.Content;
using OpenTS2.Content.DBPF;
using OpenTS2.Content.DBPF.Scenegraph;
using OpenTS2.Files;
using OpenTS2.Files.Formats.DBPF;
using OpenTS2.Files.Formats.DBPF.Scenegraph.Block;
using OpenTS2.Rendering;
using UnityEngine;
using UnityEngine.Animations;
using UnityEngine.Animations.Rigging;

namespace OpenTS2.Components
{
    /// <summary>
    /// This component represents a rendered out sims character with their head, hair and body meshes in place under one
    /// scenegraph component.
    /// </summary>
    [RequireComponent(typeof(AssetReferenceComponent))]
    public class SimCharacterComponent : MonoBehaviour
    {
        public static SimCharacterComponent CreateNakedBaseSim()
        {
            // Load the skeleton, body, hair and face resources.
            const string skeletonResourceName = "auskel_cres";
            var skeletonAsset = ContentManager.Instance.GetAsset<ScenegraphResourceAsset>(
                new ResourceKey(skeletonResourceName, GroupIDs.Scenegraph, TypeIDs.SCENEGRAPH_CRES));
            const string bodyResourceName = "amBodyNaked_cres";
            var bodyAsset = ContentManager.Instance.GetAsset<ScenegraphResourceAsset>(
                new ResourceKey(bodyResourceName, GroupIDs.Scenegraph, TypeIDs.SCENEGRAPH_CRES));
            const string baldHairResourceName = "amHairBald_cres";
            var baldHairAsset = ContentManager.Instance.GetAsset<ScenegraphResourceAsset>(
                new ResourceKey(baldHairResourceName, GroupIDs.Scenegraph, TypeIDs.SCENEGRAPH_CRES));
            const string baseFaceResourceName = "amFace_cres";
            var baseFaceAsset = ContentManager.Instance.GetAsset<ScenegraphResourceAsset>(
                new ResourceKey(baseFaceResourceName, GroupIDs.Scenegraph, TypeIDs.SCENEGRAPH_CRES));

            // Create a scenegraph with all 3 resources.
            var simsObject =
                ScenegraphComponent.CreateRootScenegraph(skeletonAsset, bodyAsset, baldHairAsset, baseFaceAsset);
            var scenegraph = simsObject.GetComponentInChildren<ScenegraphComponent>();

            var simCharacterObject = new GameObject("sim_character", typeof(SimCharacterComponent));
            // Parent the scenegraph to the created SimCharacterComponent.
            simsObject.transform.parent = simCharacterObject.transform;

            // Hold references to the scenegraph resources we use.
            simCharacterObject.GetComponent<AssetReferenceComponent>().AddReference(skeletonAsset, bodyAsset, baldHairAsset, baseFaceAsset);

            var simsComponent = simCharacterObject.GetComponent<SimCharacterComponent>();
            simsComponent.SetupAnimationRig(scenegraph);

            return simsComponent;
        }

        public ScenegraphComponent Scenegraph { get; private set; }

        /// <summary>
        /// This is the unity object that all the animation rigs get placed into.
        /// </summary>
        private GameObject _animationRigs;
        private Animator _animator;
        private RigBuilder _rigBuilder;

        /// <summary>
        /// Set of IK chains that have already been applied to this sim so they don't get re-applied.
        /// </summary>
        private readonly Dictionary<uint, RigLayer> _appliedIKChains = new Dictionary<uint, RigLayer>();

        /// <summary>
        /// These are basically parent bones of IK chains, this is needed so that if an animation updates the position
        /// or rotation of these, unity's IK system responds and moves appropriately.
        /// </summary>
        private static readonly string[] BonesThatUpdateIK =
        {
            "root_trans", "root_rot", "pelvis", "spine0", "spine1", "spine2", "r_clavicle", "l_clavicle",
            "l_handcontrol0", "r_handcontrol0"
        };

        private void SetupAnimationRig(ScenegraphComponent scene)
        {
            Scenegraph = scene;
            var skeleton = Scenegraph.BoneNamesToTransform["auskel"].gameObject;

#if UNITY_EDITOR
            // Add a bone-renderer so we can visualize the sim's bones.
            var boneRenderer = skeleton.AddComponent<BoneRenderer>();
            var boneTransforms = new List<Transform>();
            GetAllBoneTransformsForVisualization(Scenegraph.BoneNamesToTransform["root_trans"], boneTransforms);
            boneRenderer.transforms = boneTransforms.ToArray();
#endif

            // Add an animator component to the auskel.
            _animator = skeleton.AddComponent<Animator>();
            // Add a rig builder.
            _rigBuilder = skeleton.AddComponent<RigBuilder>();

            // Add a child rig to the skeleton.
            _animationRigs = new GameObject("UnityAnimationRigs");
            _animationRigs.transform.SetParent(skeleton.transform, worldPositionStays: false);

            // HACK: In the auskel model, for some reason the bones that represent the IK goals for the hands are
            // parented to the root_trans of the skeleton. This is done even though in animations the IK goal bones
            // get moved relative to the model root, NOT root_trans.
            //
            // We re-parent them here to the base model to avoid this issue.
            Scenegraph.BoneNamesToTransform["l_handcontrol0"].SetParent(skeleton.transform, worldPositionStays:true);
            Scenegraph.BoneNamesToTransform["r_handcontrol0"].SetParent(skeleton.transform, worldPositionStays:true);
            Scenegraph.BoneNamesToRelativePaths["l_handcontrol0"]    = "auskel/l_handcontrol0";
            Scenegraph.BoneNamesToRelativePaths["l_handcontrol0rot"] = "auskel/l_handcontrol0/l_handcontrol0rot";
            Scenegraph.BoneNamesToRelativePaths["r_handcontrol0"]    = "auskel/r_handcontrol0";
            Scenegraph.BoneNamesToRelativePaths["r_handcontrol0rot"] = "auskel/r_handcontrol0/r_handcontrol0rot";
            // HACK.

            // Add RigTransforms to the sim's root translation and rotation bones. This allows the unity rigging
            // animation system to move the legs to the proper IK goals if the rest of the skeleton moves.
            foreach (var ikUpdatingBone in BonesThatUpdateIK)
            {
                Scenegraph.BoneNamesToTransform[ikUpdatingBone].gameObject.AddComponent<RigTransform>();
            }

            AddGizmosAroundInverseKinmaticsPositions();

            //_rigBuilder.layers.Add(new RigLayer(_animationRig.GetComponent<Rig>()));
            //_rigBuilder.layers[0].Initialize(animator);
            _rigBuilder.Build();
        }

        private void AddGizmosAroundInverseKinmaticsPositions()
        {
#if UNITY_EDITOR
            // Add some effectors around the foot control points to see what animations should look like.
            var cubePrimitive = GameObject.CreatePrimitive(PrimitiveType.Cube);
            var cubeMesh = cubePrimitive.GetComponent<MeshFilter>().sharedMesh;
            _rigBuilder.AddEffector(Scenegraph.BoneNamesToTransform["l_foot_ikctr"],
                new RigEffectorData.Style()
                    { color = new Color(1.0f, 0.0f, 0.0f, 0.5f), size = 0.05f, shape = cubeMesh });
            _rigBuilder.AddEffector(Scenegraph.BoneNamesToTransform["r_foot_ikctr"],
                new RigEffectorData.Style()
                    { color = new Color(0.0f, 1.0f, 0.0f, 0.5f), size = 0.05f, shape = cubeMesh });
            Destroy(cubePrimitive);

            var spherePrimitive = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            var sphereMesh = cubePrimitive.GetComponent<MeshFilter>().sharedMesh;
            _rigBuilder.AddEffector(Scenegraph.BoneNamesToTransform["l_handcontrol0"],
                new RigEffectorData.Style()
                    { color = new Color(1.0f, 0.0f, 0.0f, 0.5f), size = 0.05f, shape = sphereMesh });
            _rigBuilder.AddEffector(Scenegraph.BoneNamesToTransform["l_hand_ikpole"],
                new RigEffectorData.Style()
                    { color = new Color(0.7f, 0.2f, 0.0f, 0.5f), size = 0.04f, shape = sphereMesh });
            _rigBuilder.AddEffector(Scenegraph.BoneNamesToTransform["r_handcontrol0"],
                new RigEffectorData.Style()
                    { color = new Color(0.0f, 1.0f, 0.0f, 0.5f), size = 0.05f, shape = sphereMesh });
            _rigBuilder.AddEffector(Scenegraph.BoneNamesToTransform["r_hand_ikpole"],
                new RigEffectorData.Style()
                    { color = new Color(0.0f, 0.8f, 0.4f, 0.5f), size = 0.04f, shape = sphereMesh });
            Destroy(spherePrimitive);
#endif
        }

        public void AdjustInverseKinematicWeightsForAnimation(AnimResourceConstBlock anim)
        {
            // TODO: This should eventually live in the animation controller for the sim. This is here for easy testing
            // for now.
            var auSkelTarget = anim.AnimTargets.FirstOrDefault(t => t.TagName.ToLower() == "auskel");
            if (auSkelTarget == null)
            {
                return;
            }
            AddInverseKinematicsFromAnimation(auSkelTarget);

            // Disable all current rigs.
            foreach (var rigLayer in _appliedIKChains.Values)
            {
                rigLayer.active = false;
            }
            // Activate the ones present in the current animation.
            foreach (var chain in auSkelTarget.IKChains)
            {
                if (!_appliedIKChains.TryGetValue(chain.NameCrc, out var rigLayer))
                {
                    continue;
                }
                rigLayer.active = true;
            }
        }

        private void AddInverseKinematicsFromAnimation(AnimResourceConstBlock.AnimTarget auSkelTarget)
        {
            AddInverseKinematicsFromIKChains(auSkelTarget.IKChains);
        }

        private void AddInverseKinematicsFromIKChains(IEnumerable<AnimResourceConstBlock.IKChain> ikChains)
        {
            foreach (var chain in ikChains)
            {
                if (_appliedIKChains.ContainsKey(chain.NameCrc))
                {
                    continue;
                }
                var rigObject = new GameObject($"ik-rig-{chain.NameCrc:X}", typeof(Rig));
                rigObject.transform.SetParent(_animationRigs.transform);
                var rig = rigObject.GetComponent<Rig>();

                Debug.Assert(chain.BeginBoneCrc != 0, "ikChain without a beginning bone");
                Debug.Assert(chain.EndBoneCrc != 0, "ikChain without an end bone");
                if (!Scenegraph.BoneCRC32ToTransform.ContainsKey(chain.BeginBoneCrc))
                {
                    Debug.LogWarning($"begin bone crc from ik chain {chain.BeginBoneCrc:X} not found");
                    continue;
                }

                if (!Scenegraph.BoneCRC32ToTransform.ContainsKey(chain.EndBoneCrc))
                {
                    Debug.LogWarning($"end bone crc from ik chain {chain.EndBoneCrc:X} not found");
                    continue;
                }

                var root = Scenegraph.BoneCRC32ToTransform[chain.BeginBoneCrc];
                var tip = Scenegraph.BoneCRC32ToTransform[chain.EndBoneCrc];
                // Find the mid bone between root and tip.
                var middleBone = FindTransformInTheMiddleOf(tip, root);

                for (var i = 0; i < chain.IkTargets.Length; i++)
                {
                    var target = chain.IkTargets[i];
                    CreateIKConstraintsFromTarget(i, rigObject, target, chain, root, middleBone, tip);
                }

                var rigLayer = new RigLayer(rig);
                _rigBuilder.layers.Add(rigLayer);
                rigLayer.Initialize(_animator);
                _appliedIKChains[chain.NameCrc] = rigLayer;
            }

            _rigBuilder.Build();
        }

        private void CreateIKConstraintsFromTarget(int i, GameObject rigObject,
            AnimResourceConstBlock.IKChain.Target target,
            AnimResourceConstBlock.IKChain chain, Transform root, Transform middleBone, Transform tip)
        {
            Debug.Assert(target.TranslationCrc == target.RotationCrc,
                "ikTarget with different translation and rotation objects");

            // TODO: if there's more of these we should make a mapping instead of just hard-coding these two here..
            if (target.TranslationCrc == FileUtils.HighHash("l_handcontrol1_noxyzoffset"))
            {
                Scenegraph.BoneCRC32ToTransform[target.TranslationCrc] = Scenegraph.BoneNamesToTransform["l_handcontrol1"];
                Scenegraph.BoneNamesToRelativePaths["l_handcontrol1_noxyzoffset"] = "auskel/root_trans/l_handcontrol1";
            } else if (target.TranslationCrc == FileUtils.HighHash("r_handcontrol1_noxyzoffset"))
            {
                Scenegraph.BoneCRC32ToTransform[target.TranslationCrc] = Scenegraph.BoneNamesToTransform["r_handcontrol1"];
                Scenegraph.BoneNamesToRelativePaths["r_handcontrol1_noxyzoffset"] = "auskel/root_trans/r_handcontrol1";
            }

            if (!Scenegraph.BoneCRC32ToTransform.ContainsKey(target.TranslationCrc))
            {
                Debug.LogWarning($"ik target {target.TranslationCrc:X} not found");
                return;
            }

            var ikChainObj = new GameObject($"ikChain-{chain.NameCrc:X}-{i}", typeof(TwoBoneIKConstraint));
            var twoBoneConstraint = new TwoBoneIKConstraintData
            {
                root = root,
                mid = middleBone,
                tip = tip,
                target = Scenegraph.BoneCRC32ToTransform[target.TranslationCrc],
                targetPositionWeight = 1.0f,
                targetRotationWeight = 1.0f,
                hintWeight = 1.0f,
            };

            if (chain.TwistVectorCrc != 0)
            {
                twoBoneConstraint.hint = Scenegraph.BoneCRC32ToTransform[chain.TwistVectorCrc];
            }

            ikChainObj.GetComponent<TwoBoneIKConstraint>().data = twoBoneConstraint;
            ikChainObj.transform.parent = rigObject.transform;

            if (target.Rotation2Crc != 0)
            {
                var rotationConstraint =
                    new GameObject($"ikChain{chain.NameCrc:X}-rotation-{i}", typeof(OverrideTransform));
                var rotationOverrideConstraint = new OverrideTransformData
                {
                    constrainedObject = tip,
                    sourceObject = Scenegraph.BoneCRC32ToTransform[target.Rotation2Crc],
                    rotationWeight = 1.0f,
                    positionWeight = 0.0f,
                    space = OverrideTransformData.Space.Local,
                };
                rotationConstraint.GetComponent<OverrideTransform>().data = rotationOverrideConstraint;
                rotationConstraint.transform.parent = rigObject.transform;
            }
        }

        private static Transform FindTransformInTheMiddleOf(Transform tip, Transform root)
        {
            var numBones = 0;
            var boneFinder = tip;
            while (boneFinder != root)
            {
                boneFinder = boneFinder.parent;
                numBones++;
            }

            Debug.Assert(numBones > 1, $"No bone between {tip} and {root}");

            var middle = tip;
            for (var i = 0; i < numBones / 2; i++)
            {
                middle = middle.parent;
            }

            return middle;
        }

        private static void GetAllBoneTransformsForVisualization(Transform rootBone, ICollection<Transform> bones)
        {
            for (var i = 0; i < rootBone.childCount; i++)
            {
                var bone = rootBone.GetChild(i);
                // Ignore "surface" and "grip" transforms, they just make the skeleton look messy.
                if (bone.name.Contains("surface") || bone.name.Contains("grip"))
                {
                    continue;
                }

                bones.Add(bone);
                GetAllBoneTransformsForVisualization(bone, bones);
            }
        }

        /// <summary>
        /// Builds a sim's CAS outfit as a renderable GameObject in Unity coordinate space, or null if
        /// nothing could be loaded. Loads the dedicated per-sim package at
        /// Neighborhoods/{prefix}/Characters/{prefix}_User{instanceId:D5}.package, where
        /// instanceId is the sim's own LOT_OBJECT entry's raw (local) instance ID. Content private to the
        /// sim inside that package is keyed with GroupIDs.Local; the sim's outfit data itself is always
        /// {instance: 1}.
        ///
        /// The Body category resolves to the sim's full rigged skeleton - it's rendered first and treated
        /// as the skeleton every other part attaches onto. A part attaches onto a bone whose tag matches
        /// one of its own bone tags (a CRES's tagged transform nodes, or a bare SHPE's ShapeBlock.Tag -
        /// see ScenegraphComponent.TryAttachPart/BoneNamesToAttachedObjects). If nothing matches (or the
        /// skeleton is missing) the part still renders as loose, unattached geometry at the sim's position
        /// rather than being dropped - not correctly posed, but visible.
        /// </summary>
        public static GameObject CreatePersonOutfit(string neighborhoodPrefix, uint userInstanceId,
            ResourceKey lotObjectGlobalTgi)
        {
            var charactersFolderPath = Path.Combine(Filesystem.UserDataDirectory, $"Neighborhoods/{neighborhoodPrefix}/Characters");
            var characterFullPath = Path.Combine(charactersFolderPath, $"{neighborhoodPrefix}_User{userInstanceId:D5}.package");

            if (!File.Exists(characterFullPath))
            {
                Debug.Log($"No character file for person TGI: {lotObjectGlobalTgi} (expected {characterFullPath})");
                return null;
            }

            var characterPackage = ContentManager.Instance.AddPackage(characterFullPath);

            var appearanceKey = new ResourceKey(1, GroupIDs.Local, TypeIDs.SIM_APPEARANCE);
            var appearance = GetCharacterAsset<SimAppearanceAsset>(characterPackage, appearanceKey);
            if (appearance == null)
            {
                Debug.Log($"No SimAppearanceAsset for person TGI: {lotObjectGlobalTgi}");
                return null;
            }

            // The sim's own bare-skin material, composited underneath every Body/Top/Bottom clothing
            // subset below - null if it couldn't be resolved, in which case compositing is skipped.
            var skinMaterial = ResolveSkinBaseMaterial(appearance);

            var partObjects = new List<GameObject>();
            ScenegraphComponent[] skeletonComponents = null;

            // Body first, since it's what provides the skeleton other parts attach onto.
            var categoriesInAttachmentOrder = appearance.EntryLists
                .Where(kv => kv.Key.OutfitType == OutfitType.Casual1)
                .OrderBy(kv => kv.Key.Category == OutfitCategory.Body ? 0 : 1);

            foreach (var categoryEntries in categoriesInAttachmentOrder)
            {
                var category = categoryEntries.Key.Category;
                foreach (var entry in categoryEntries.Value)
                {
                    var part = ResolveOutfitPartKey(characterPackage, entry, appearanceKey);
                    if (part == null)
                    {
                        continue;
                    }

                    if (skinMaterial != null &&
                        (category == OutfitCategory.Body || category == OutfitCategory.Top || category == OutfitCategory.Bottom))
                    {
                        part = ApplySkinComposite(characterPackage, part.Value, skinMaterial, neighborhoodPrefix, userInstanceId);
                    }

                    var partObject = LoadOutfitPartGameObject(characterPackage, part.Value);
                    if (partObject == null)
                    {
                        Debug.Log($"person TGI: {lotObjectGlobalTgi}, outfit part {category}: could not load {part.Value.Key}");
                        continue;
                    }

                    // A composition tree can reference sub-resources (e.g. a skeleton "rig" resource) via
                    // nested cResourceNodes, each rendered as its own separate ScenegraphComponent - bones
                    // and tags can live on any of them, not just the top-level one, so bone-attachment
                    // considers the whole subtree.
                    var partComponents = partObject.GetComponentsInChildren<ScenegraphComponent>(true);
                    if (skeletonComponents == null && category == OutfitCategory.Body)
                    {
                        skeletonComponents = partComponents;
                        partObjects.Add(partObject);
                        continue;
                    }

                    if (skeletonComponents != null && TryAttachToSkeleton(skeletonComponents, partComponents))
                    {
                        Destroy(partObject);
                        continue;
                    }

                    partObjects.Add(partObject);
                }
            }

            if (partObjects.Count == 0)
            {
                return null;
            }

            var outfit = new GameObject("outfit");
            foreach (var partObject in partObjects)
            {
                partObject.transform.SetParent(outfit.transform, worldPositionStays: false);
            }

            return ScenegraphComponent.WrapInSimsToUnityTransform(outfit);
        }

        /// <summary>
        /// Attaches every one of a part's bone-tagged content (see
        /// ScenegraphComponent.BoneNamesToAttachedObjects) onto whichever skeleton component owns a
        /// matching bone. A rig's bones can live on a nested sub-resource's own ScenegraphComponent rather
        /// than the top-level one, so both sides are given every ScenegraphComponent in their respective
        /// subtrees rather than just the top-level one. If anything attached, the part components' asset
        /// references are migrated onto the skeleton's outermost component (assumed to be index 0 -
        /// GetComponentsInChildren returns the queried object's own components first) so the caller can
        /// safely discard the now-emptied part hierarchy. Returns false if nothing matched, in which case
        /// the caller should keep rendering the part as its own standalone object.
        /// </summary>
        private static bool TryAttachToSkeleton(ScenegraphComponent[] skeletonComponents, ScenegraphComponent[] partComponents)
        {
            var attachedAny = false;
            foreach (var part in partComponents)
            {
                foreach (var boneNameAndObjects in part.BoneNamesToAttachedObjects)
                {
                    foreach (var skeleton in skeletonComponents)
                    {
                        if (skeleton.TryAttachPart(boneNameAndObjects.Key, boneNameAndObjects.Value))
                        {
                            attachedAny = true;
                            break;
                        }
                    }
                }
            }
            if (!attachedAny) return false;

            var skeletonReferences = skeletonComponents[0].GetComponent<AssetReferenceComponent>();
            foreach (var part in partComponents)
            {
                var partReferences = part.GetComponent<AssetReferenceComponent>();
                skeletonReferences.AddReference(partReferences.References.ToArray());
            }
            return true;
        }

        /// <summary>
        /// A resolved outfit-part key plus any per-subset material overrides that apply to it (e.g.
        /// genetics such as eye color - see SkinEntryAsset.MaterialOverrides).
        /// </summary>
        private readonly struct OutfitPart
        {
            public readonly ResourceKey Key;
            public readonly Dictionary<string, ScenegraphMaterialDefinitionAsset> MaterialOverridesBySubset;

            public OutfitPart(ResourceKey key, Dictionary<string, ScenegraphMaterialDefinitionAsset> materialOverridesBySubset)
            {
                Key = key;
                MaterialOverridesBySubset = materialOverridesBySubset;
            }
        }

        /// <summary>
        /// Resolves one outfit-category entry down to a renderable key - either SCENEGRAPH_SHPE or
        /// SCENEGRAPH_CRES (see LoadOutfitPartGameObject).
        ///
        /// An entry either points directly at a scenegraph resource already, or at a skin entry record.
        /// In the latter case, prefer its ShapeResourceKey (a SCENEGRAPH_SHPE - a shape, not a full
        /// composition tree), which is what the game resolves directly for humans. Some categories (e.g.
        /// Face) legitimately have no shape - their ShapeResourceKey resolves to an all-zero key on real
        /// sim data - so fall back to the skin entry's other key (ResourceKey), observed as a real
        /// SCENEGRAPH_CRES for Face (most likely the head geometry itself, which is why it carries no
        /// separate attachable shape).
        /// </summary>
        private static OutfitPart? ResolveOutfitPartKey(DBPFFile characterPackage, IResourceKeyOrIndex entry, ResourceKey ownerKey)
        {
            var key = ResolveKeyOrIndex(characterPackage, entry, ownerKey);

            if (key.TypeID == TypeIDs.SCENEGRAPH_SHPE || key.TypeID == TypeIDs.SCENEGRAPH_CRES)
            {
                return new OutfitPart(key, null);
            }

            var skinEntry = GetCharacterAsset<SkinEntryAsset>(characterPackage, key);
            if (skinEntry == null)
            {
                return null;
            }

            var materialOverrides = ResolveMaterialOverrides(characterPackage, skinEntry, key);

            var shapeKey = ResolveKeyOrIndex(characterPackage, skinEntry.ShapeResourceKey, key);
            if (shapeKey.Valid)
            {
                return new OutfitPart(shapeKey, materialOverrides);
            }

            var resourceKey = ResolveKeyOrIndex(characterPackage, skinEntry.ResourceKey, key);
            return resourceKey.Valid ? new OutfitPart(resourceKey, materialOverrides) : (OutfitPart?)null;
        }

        /// <summary>
        /// Resolves a skin entry's MaterialOverrides (see SkinEntryAsset) into actual loaded materials,
        /// keyed by subset name - the same keys ScenegraphShapeAsset.Materials uses, since that's what
        /// ScenegraphComponent matches them against when rendering. An override's resource key indexes
        /// into the same RES_KEY_LIST sidecar as the rest of the skin entry, hence the shared ownerKey.
        /// </summary>
        private static Dictionary<string, ScenegraphMaterialDefinitionAsset> ResolveMaterialOverrides(
            DBPFFile characterPackage, SkinEntryAsset skinEntry, ResourceKey ownerKey)
        {
            var result = new Dictionary<string, ScenegraphMaterialDefinitionAsset>();
            foreach (var materialOverride in skinEntry.MaterialOverrides)
            {
                var materialKey = ResolveKeyOrIndex(characterPackage, materialOverride.ResourceKey, ownerKey);
                var material = GetCharacterAsset<ScenegraphMaterialDefinitionAsset>(characterPackage, materialKey);
                if (material != null)
                {
                    result[materialOverride.SubsetName] = material;
                }
            }

            return result;
        }

        /// <summary>
        /// Age bracket letter for the "{agegenderprefix}bodynaked_nude_s{tone}" material naming convention
        /// (see ResolveSkinBaseMaterial). The sim's real age isn't parsed from any per-sim resource yet,
        /// so this is hardcoded to Adult for now.
        /// </summary>
        private const string DefaultAgeBracketLetter = "a";

        /// <summary>
        /// Resolves a sim's bare-skin material, composited underneath every Body/Top/Bottom clothing
        /// subset (see ApplySkinComposite). The base game ships a fixed set of plain skin TXMT resources
        /// in TSData/Res/Sims3D/*.package named "{agegenderprefix}bodynaked_nude_s{1-5}_txmt" - one opaque
        /// skin texture per age/gender bracket times 5 discrete skin-tone variants - all living in
        /// GroupIDs.Scenegraph, so the TGI can be constructed by name directly with no catalog lookup.
        /// </summary>
        private static ScenegraphMaterialDefinitionAsset ResolveSkinBaseMaterial(SimAppearanceAsset appearance)
        {
            var materialName = $"{DefaultAgeBracketLetter}{SkinBaseGenderLetter(appearance.Gender)}bodynaked_nude_s{SkinBaseToneIndex(appearance.SkinTone)}";
            var materialKey = new ResourceKey($"{materialName}_txmt", GroupIDs.Scenegraph, TypeIDs.SCENEGRAPH_TXMT);
            return ContentManager.Instance.GetAsset<ScenegraphMaterialDefinitionAsset>(materialKey);
        }

        /// <summary>
        /// Gender letter for the "{agegenderprefix}bodynaked_nude_s{tone}" naming convention: a sim's
        /// Gender of 1 maps to "f" (female), anything else to "m" (male).
        /// </summary>
        private static string SkinBaseGenderLetter(uint? gender) => gender == 1 ? "f" : "m";

        /// <summary>
        /// Maps a SimAppearanceAsset.SkinTone GUID (e.g. "00000002-0000-0000-0000-000000000000") to one
        /// of the 5 discrete "sN" skin-tone texture variants - skin tones use small sequential integer
        /// "family" GUIDs (00000001, 00000002, ...) that line up 1:1 with the sN suffixes. Falls back to
        /// s1 if the GUID can't be parsed this way.
        /// </summary>
        private static int SkinBaseToneIndex(string skinTone)
        {
            if (skinTone != null && skinTone.Length >= 8 &&
                uint.TryParse(skinTone.Substring(0, 8), NumberStyles.HexNumber, null, out var value) &&
                value >= 1 && value <= 5)
            {
                return (int)value;
            }

            return 1;
        }

        /// <summary>
        /// Wraps every subset of a Body/Top/Bottom outfit part's material with the sim's bare-skin texture
        /// composited underneath it (see SkinCompositing and ResolveSkinBaseMaterial). The bare-skin
        /// material is a single texture covering the whole arms/legs/torso, so the same material is
        /// composited under every subset uniformly. Any existing genetics override for a subset (see
        /// ResolveMaterialOverrides) is used as the compositing input rather than being bypassed, so the
        /// composited result - which always wins as the final override for that subset - reflects genetics
        /// too.
        /// </summary>
        private static OutfitPart ApplySkinComposite(DBPFFile characterPackage, OutfitPart part,
            ScenegraphMaterialDefinitionAsset skinMaterial, string neighborhoodPrefix, uint userInstanceId)
        {
            if (part.Key.TypeID != TypeIDs.SCENEGRAPH_SHPE)
            {
                // CRES-resolved parts (e.g. Face) are out of scope for skin compositing.
                return part;
            }

            var shape = GetCharacterAsset<ScenegraphShapeAsset>(characterPackage, part.Key);
            if (shape == null)
            {
                return part;
            }
            shape.LoadModelsAndMaterials();

            var mergedOverrides = part.MaterialOverridesBySubset == null
                ? new Dictionary<string, ScenegraphMaterialDefinitionAsset>()
                : new Dictionary<string, ScenegraphMaterialDefinitionAsset>(part.MaterialOverridesBySubset);

            foreach (var subsetAndMaterial in shape.Materials)
            {
                var subsetName = subsetAndMaterial.Key;
                var clothingMaterial = mergedOverrides.TryGetValue(subsetName, out var existingOverride)
                    ? existingOverride
                    : subsetAndMaterial.Value;

                // Deterministic per (sim, material) name, so re-resolving the same part is a cache hit in
                // SkinCompositing rather than rebuilding the composited texture every time.
                var compositeName = $"~simskincomposite_{neighborhoodPrefix}_{userInstanceId:x8}_" +
                    $"{clothingMaterial.GlobalTGI.TypeID:x8}{clothingMaterial.GlobalTGI.GroupID:x8}" +
                    $"{clothingMaterial.GlobalTGI.InstanceID:x8}{clothingMaterial.GlobalTGI.InstanceHigh:x8}";

                mergedOverrides[subsetName] = SkinCompositing.BuildCompositedMaterial(skinMaterial, clothingMaterial, compositeName);
            }

            return new OutfitPart(part.Key, mergedOverrides);
        }

        /// <summary>
        /// Loads and renders a resolved outfit-part key. SCENEGRAPH_CRES resources (e.g. Face's head
        /// geometry) are full composition resources rendered the same way as any other scenegraph
        /// resource; SCENEGRAPH_SHPE resources (e.g. Hair/Body) render as loose static geometry - see
        /// ScenegraphComponent.CreateStandaloneShapeGameObject's doc comment for the bone-attachment
        /// caveat.
        /// </summary>
        private static GameObject LoadOutfitPartGameObject(DBPFFile characterPackage, OutfitPart part)
        {
            if (part.Key.TypeID == TypeIDs.SCENEGRAPH_CRES)
            {
                var resource = GetCharacterAsset<ScenegraphResourceAsset>(characterPackage, part.Key);
                return resource?.CreateGameObject(part.MaterialOverridesBySubset);
            }

            var shape = GetCharacterAsset<ScenegraphShapeAsset>(characterPackage, part.Key);
            return shape != null ? ScenegraphComponent.CreateStandaloneShapeGameObject(shape, part.MaterialOverridesBySubset) : null;
        }

        /// <summary>
        /// Resolves a direct resource key, or an index into the RES_KEY_LIST sidecar that shares
        /// <paramref name="ownerKey"/>'s group/instance.
        /// </summary>
        private static ResourceKey ResolveKeyOrIndex(DBPFFile characterPackage, IResourceKeyOrIndex entry, ResourceKey ownerKey)
        {
            if (entry is ResourceKeyProp direct)
            {
                return direct.Value;
            }

            var index = ((ResourceKeyIndexProp)entry).Value;
            var keyList = GetCharacterAsset<ResourceKeyListAsset>(characterPackage,
                new ResourceKey(ownerKey.InstanceID, ownerKey.InstanceHigh, ownerKey.GroupID, TypeIDs.RES_KEY_LIST));
            return keyList.Keys[index];
        }

        /// <summary>
        /// Some outfit-part keys (e.g. Hair, Body) point at shared/stock CAS content living in a
        /// completely different, already-loaded package (an explicit non-Local group), not the sim's own
        /// character package. Try the character package first (covers GroupIDs.Local keys and anything
        /// genuinely private to this sim), then fall back to ContentManager's global resource map -
        /// populated for every package that has been added via AddPackage, which is how shared CAS content
        /// resolves.
        /// </summary>
        private static T GetCharacterAsset<T>(DBPFFile characterPackage, ResourceKey key) where T : AbstractAsset
        {
            return (ContentManager.Instance.GetAsset(key, characterPackage) as T) ?? ContentManager.Instance.GetEntry(key)?.GetAsset<T>();
        }
    }
}