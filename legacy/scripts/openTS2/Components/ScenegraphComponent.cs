using System;
using System.Collections.Generic;
using System.Linq;
using OpenTS2.Common;
using OpenTS2.Common.Utils;
using OpenTS2.Content;
using OpenTS2.Content.DBPF.Scenegraph;
using OpenTS2.Files.Formats.DBPF;
using OpenTS2.Files.Formats.DBPF.Scenegraph;
using OpenTS2.Files.Formats.DBPF.Scenegraph.Block;
using UnityEngine;

namespace OpenTS2.Components
{
    /// <summary>
    /// This is a Sims 2 Scenegraph rendering component for unity.
    /// </summary>
    [RequireComponent(typeof(AssetReferenceComponent))]
    public class ScenegraphComponent : MonoBehaviour
    {
        /// <summary>
        /// This renders the full scenegraph graph with the asset being the root object. This will traverse
        /// the full graph and render any sub-resources with their proper transformations etc.
        ///
        /// The returned game object carries a transform to convert it from sims coordinate space to unity space.
        /// </summary>
        public static GameObject CreateRootScenegraph(ScenegraphResourceAsset resourceAsset)
        {
            return CreateRootScenegraph(new []{ resourceAsset });
        }

        public static GameObject CreateRootScenegraph(params ScenegraphResourceAsset[] resourceAssets)
        {
            var scenegraph = CreateScenegraphComponent(resourceAssets, materialOverridesBySubset: null);
            return WrapInSimsToUnityTransform(scenegraph.gameObject);
        }

        /// <summary>
        /// Same as `CreateRootScenegraph` except it doesn't apply the transform from sims to unity space.
        /// </summary>
        public static ScenegraphComponent CreateScenegraphComponent(ScenegraphResourceAsset resourceAsset,
            Dictionary<string, ScenegraphMaterialDefinitionAsset> materialOverridesBySubset = null)
        {
            return CreateScenegraphComponent(new[] { resourceAsset }, materialOverridesBySubset);
        }

        /// <summary>
        /// Wraps a GameObject that's in Sims coordinate space with the transform that converts it to
        /// Unity's coordinate space - the same wrapping `CreateRootScenegraph` applies, exposed for
        /// content assembled from loose pieces rather than a single scenegraph component.
        /// </summary>
        public static GameObject WrapInSimsToUnityTransform(GameObject content)
        {
            var simsTransform = new GameObject(content.name + "_transform");

            // Apply a transformation to convert from the sims coordinate space to unity.
            simsTransform.transform.Rotate(-90, 0, 0);
            simsTransform.transform.localScale = new Vector3(1, -1, 1);

            content.transform.SetParent(simsTransform.transform, worldPositionStays:false);
            return simsTransform;
        }

        /// <summary>
        /// Creates a scenegraph component from a multiple scenegraph resource assets.
        /// </summary>
        public static ScenegraphComponent CreateScenegraphComponent(ScenegraphResourceAsset[] resourceAssets,
            Dictionary<string, ScenegraphMaterialDefinitionAsset> materialOverridesBySubset = null)
        {
            var scenegraph = new GameObject("scenegraph", typeof(ScenegraphComponent));
            var scenegraphComponent = scenegraph.GetComponent<ScenegraphComponent>();

            scenegraphComponent.CreateFromScenegraphComponents(resourceAssets.Select(asset => asset.ResourceCollection),
                resourceAssets[0].GlobalTGI, materialOverridesBySubset);

            return scenegraphComponent;
        }

        private ResourceKey _resourceAssetKey;

        /// <summary>
        /// Per-subset material overrides (cTSSkinEntry::MaterialOverrides - see SkinEntryAsset) applied
        /// on top of whatever material a shape's own SHPE data names for that subset, e.g. genetics
        /// (eye color, etc.) layered onto a base head resource's neutral placeholder materials. Keyed
        /// by subset/primitive name, matching ScenegraphShapeAsset.Materials' own keys. Applies
        /// uniformly to every shape rendered within this component - see RenderShapePrimitives.
        /// </summary>
        private Dictionary<string, ScenegraphMaterialDefinitionAsset> _materialOverridesBySubset =
            new Dictionary<string, ScenegraphMaterialDefinitionAsset>();

        private void CreateFromScenegraphComponents(IEnumerable<ScenegraphResourceCollection> resourceCollections,
            ResourceKey resourceAssetKey, Dictionary<string, ScenegraphMaterialDefinitionAsset> materialOverridesBySubset)
        {
            _resourceAssetKey = resourceAssetKey;
            if (materialOverridesBySubset != null)
            {
                _materialOverridesBySubset = materialOverridesBySubset;
            }

            foreach (var rCol in resourceCollections)
            {
                RenderScenegraphResourceCollection(rCol);
            }

            BindBonesInMeshes();
            ComputeRelativeBoneAndBlendPaths();
        }

        private void RenderScenegraphResourceCollection(ScenegraphResourceCollection resourceCollection)
        {
            var firstResourceNode = resourceCollection.Blocks.OfType<ResourceNodeBlock>().First();
            name = firstResourceNode.ResourceName;

            // Traverse the graph if present and render out any sub-nodes.
            try
            {
                if (firstResourceNode.Tree != null)
                {
                    RenderCompositionTree(gameObject, resourceCollection, firstResourceNode.Tree);
                }
            }
            catch (Exception e)
            {
                Debug.LogError($"Error while traversing graph for {name}");
                Debug.LogException(e);
            }
        }

        /// <summary>
        /// A mapping of bone names such as "body_rot" to their path relative to the scenegraph component
        /// "vehiclepizza/root_trans/root_rot/body_trans/body_rot". This is used for animating bones as unity
        /// needs their relative paths for animations.
        /// </summary>
        public readonly Dictionary<string, string> BoneNamesToRelativePaths = new Dictionary<string, string>();
        /// <summary>
        /// A mapping of the CRC32 of bone names to their transforms.
        /// </summary>
        public readonly Dictionary<uint, Transform> BoneCRC32ToTransform = new Dictionary<uint, Transform>();
        public readonly Dictionary<string, Transform> BoneNamesToTransform = new Dictionary<string, Transform>();
        /// <summary>
        /// Rendered primitive GameObjects grouped by the bone tag they're attached at (a ShapeRefNode's
        /// own transform tag for CRES content, or a bare SHPE's own ShapeBlock.Tag). Used by
        /// TryAttachPart to graft another part's content onto a matching bone here, and to find/replace
        /// whatever was already at that bone slot - see SimCharacterComponent.CreatePersonOutfit.
        /// </summary>
        public readonly Dictionary<string, List<GameObject>> BoneNamesToAttachedObjects = new Dictionary<string, List<GameObject>>();
        /// <summary>
        /// A mapping of blend shapes such as "recliningbend" and "slot_0_indent" to the paths relative to the
        /// scenegraph component "chair_living/fabric". This can include multiple components, hence the list of strings.
        /// </summary>
        public readonly Dictionary<string, List<string>> BlendNamesToRelativePaths = new Dictionary<string, List<string>>();
        private readonly Dictionary<Transform, List<string>> _unityObjectsToBlends = new Dictionary<Transform, List<string>>();

        private void ComputeRelativeBoneAndBlendPaths()
        {
            // For now I'm just going down the children and computing the relative path, but we can probably do this
            // more cleanly when we build the nodes.
            if (transform.childCount > 0)
            {
                TraverseChildrenGameObjectsAndAddRelativeBonesAndPaths("", transform.GetChild(0));
            }
        }

        private void TraverseChildrenGameObjectsAndAddRelativeBonesAndPaths(string pathSoFar, Transform parentObj)
        {
            if (BoneNamesToTransform.ContainsKey(parentObj.name))
            {
                BoneNamesToRelativePaths[parentObj.name] = $"{pathSoFar}{parentObj.name}";
            }
            if (_unityObjectsToBlends.TryGetValue(parentObj, out var blends))
            {
                foreach (var blend in blends)
                {
                    if (!BlendNamesToRelativePaths.ContainsKey(blend))
                    {
                        BlendNamesToRelativePaths[blend] = new List<string>();
                    }
                    BlendNamesToRelativePaths[blend].Add($"{pathSoFar}{parentObj.name}");
                }
            }

            pathSoFar += $"{parentObj.name}/";
            for (var i = 0; i < parentObj.childCount; i++)
            {
                var child = parentObj.GetChild(i);
                TraverseChildrenGameObjectsAndAddRelativeBonesAndPaths(pathSoFar, child);
            }
        }

        /// <summary>
        /// Mapping of bone ids to their unity transform objects. Used to look up and bind bones to animated meshes.
        /// </summary>
        private Dictionary<uint, Transform> _boneIdToTransform = new Dictionary<uint, Transform>();
        /// <summary>
        /// A queue of meshes that need to have their animation bones bound after the scenegraph is done rendering.
        /// </summary>
        private Queue<(ScenegraphModelAsset.ModelPrimitive, SkinnedMeshRenderer)> _meshesToBindBonesFor = new Queue<(ScenegraphModelAsset.ModelPrimitive, SkinnedMeshRenderer)>();

        private void BindBonesInMeshes()
        {
            while (_meshesToBindBonesFor.Count > 0)
            {
                var primitiveAndRenderer = _meshesToBindBonesFor.Dequeue();
                BindBonesInMesh(primitiveAndRenderer.Item1, primitiveAndRenderer.Item2);
            }
        }

        private void BindBonesInMesh(ScenegraphModelAsset.ModelPrimitive primitive, SkinnedMeshRenderer renderer)
        {
            var bones = new Transform[primitive.ScenegraphBoneIds.Length];
            for (var i = 0; i < primitive.ScenegraphBoneIds.Length; i++)
            {
                var boneId = primitive.ScenegraphBoneIds[i];
                if (!_boneIdToTransform.TryGetValue(boneId, out var bone))
                {
                    Debug.LogWarning($"Scenegraph GMDC had bone id {boneId} not found in scenegraph.");
                    return;
                }
                bones[i] = bone;
            }

            renderer.bones = bones;
            renderer.sharedMesh.bindposes = primitive.BindPoses;
        }

        private void RenderCompositionTree(GameObject parent, ScenegraphResourceCollection rCol, CompositionTreeNodeBlock tree)
        {
            foreach (var reference in tree.References)
            {
                switch (reference)
                {
                    case InternalReference internalRef:
                        RenderInternalCompositionTreeChild(parent, rCol, internalRef);
                        break;
                    case ExternalReference externalRef:
                        RenderExternalCompositionTreeChild(parent, rCol, externalRef);
                        break;
                    case NullReference nullRef:
                        throw new ArgumentException("Got null reference in CompositionTree");
                }
            }

            // Next iterate over all the extension references.
            foreach (var extensionReference in tree.Graph.Extensions)
            {
                if (!(extensionReference is InternalReference internalRef))
                {
                    throw new ArgumentException("Got non-internal reference for Extension in resource node");
                }

                var extension = rCol.Blocks[internalRef.BlockIndex];
                switch (extension)
                {
                    case DataListExtensionBlock extensionBlock:
                        HandleExtension(parent, extensionBlock);
                        break;
                    case BoneDataExtensionBlock _:
                        // TODO: see if anything needs to be done for bone data blocks.
                        break;
                    default:
                        throw new ArgumentException($"Unknown extension block type: {extension}");
                }
            }
        }

        private static void HandleExtension(GameObject parent, DataListExtensionBlock extension)
        {
            // This is the only extension we care about right now.
            if (extension.Value.Name != "EffectsList")
            {
                return;
            }

            var effectsList = (DataListValue<DataListValue[]>)extension.Value;
            foreach (var dataListValue in effectsList.Value)
            {
                var effectSlotToName = (DataListValue<string>)dataListValue;

                var slotName = effectSlotToName.Name;
                var effectName = effectSlotToName.Value;
                // The game seriously has effects in the scenegraph that don't exist... ignore those
                if (!EffectsManager.Instance.Ready || !EffectsManager.Instance.HasEffect(effectName))
                {
                    continue;
                }

                // TODO: these effects are not in the right place, they need to be placed in the right slot. Right now
                // this isn't easy to do as we don't have a mapping of tags -> GameObject. This will be easier once
                // the scenegraph has its own MonoBehavior.
                try
                {
                    var swarmParticleSystem = EffectsManager.Instance.CreateEffect(effectName);
                    swarmParticleSystem.transform.SetParent(parent.transform, worldPositionStays: false);
                    swarmParticleSystem.PlayEffect();
                }
                catch (Exception e)
                {
                    Debug.LogError($"Unable to render effect {effectName}");
                    Debug.LogException(e);
                }
            }
        }

        private void RenderInternalCompositionTreeChild(GameObject parent, ScenegraphResourceCollection rCol, InternalReference reference)
        {
            var block = rCol.Blocks[reference.BlockIndex];
            switch (block)
            {
                case ShapeRefNodeBlock shapeRef:
                    RenderShapeRefNode(parent, rCol, shapeRef);
                    break;
                case TransformNodeBlock transformNode:
                    RenderTransformNode(parent, rCol, transformNode);
                    break;
                case ResourceNodeBlock resourceNode:
                    RenderResourceNode(parent, rCol, resourceNode);
                    break;
                case LightRefNodeBlock lightRef:
                    RenderLightRefNode(parent, rCol, lightRef);
                    break;
                default:
                    throw new ArgumentException($"Unsupported block type in render composition tree {block}");
            }
        }

        private static void RenderExternalCompositionTreeChild(GameObject parent, ScenegraphResourceCollection rCol, ExternalReference reference)
        {
            var resourceKey = rCol.FileLinks[reference.FileLinksIndex];
            switch (resourceKey.TypeID)
            {
                default:
                    throw new ArgumentException($"Unsupported external type in render composition tree: {resourceKey}");
            }
        }

        private void RenderTransformNode(GameObject parent, ScenegraphResourceCollection rCol, TransformNodeBlock transformNode)
        {
            var transformName = "transform";

            var transformTag = transformNode.CompositionTree.Graph.Tag;
            if (transformTag != "")
            {
                transformName = transformTag;
            }
            // If we've already rendered this transform node, just use that. Otherwise make a new unity GameObject for it.
            if (transformTag != "" && BoneNamesToTransform.TryGetValue(transformName, out var existing))
            {
                RenderCompositionTree(existing.gameObject, rCol, transformNode.CompositionTree);
                return;
            }

            var transformObj = new GameObject(transformName, typeof(AssetReferenceComponent));
            RenderCompositionTree(transformObj, rCol, transformNode.CompositionTree);

            transformObj.transform.localRotation = transformNode.Rotation;
            transformObj.transform.position = transformNode.Transform;

            transformObj.transform.SetParent(parent.transform, worldPositionStays:false);
            _boneIdToTransform[transformNode.BoneId] = transformObj.transform;
            BoneNamesToTransform[transformName] = transformObj.transform;
            if (transformTag != "")
            {
                BoneCRC32ToTransform[FileUtils.HighHash(transformTag)] = transformObj.transform;
            }
        }

        private static void RenderResourceNode(GameObject parent, ScenegraphResourceCollection rCol, ResourceNodeBlock resource)
        {
            // TODO: handle non-external resources, maybe merge with the `CreateRootGameObject` code.
            var resourceRef = resource.ResourceLocation;
            Debug.Assert(resourceRef is ExternalReference);
            var key = rCol.FileLinks[((ExternalReference)resourceRef).FileLinksIndex];

            var resourceAsset = ContentManager.Instance.GetAsset<ScenegraphResourceAsset>(key);
            if (resourceAsset == null)
            {
                Debug.LogWarning($"Unable to find cResourceNode with key {key} and name {resource.ResourceName}");
                return;
            }
            var resourceObject = resourceAsset.CreateGameObject();
            resourceObject.transform.SetParent(parent.transform, worldPositionStays:false);
        }

        private void RenderShapeRefNode(GameObject parent, ScenegraphResourceCollection rCol, ShapeRefNodeBlock shapeRef)
        {
            var shapeTransform = shapeRef.Renderable.Bounded.Transform;
            // Render any sub-objects in the transform node.
            RenderTransformNode(parent, rCol, shapeTransform);

            // TODO: handle multiple shapes here.
            if (shapeRef.Shapes.Length == 0)
            {
                return;
            }
            Debug.Assert(shapeRef.Shapes[0] is ExternalReference);
            var shapeKey = rCol.FileLinks[((ExternalReference) shapeRef.Shapes[0]).FileLinksIndex];

            if (shapeKey.GroupID == GroupIDs.Local)
            {
                // Use our groupId if the reference has a local group id.
                shapeKey = shapeKey.WithGroupID(_resourceAssetKey.GroupID);
            }
            var shape = ContentManager.Instance.GetAsset<ScenegraphShapeAsset>(shapeKey);

            // Hold a strong reference to the shape.
            parent.GetComponent<AssetReferenceComponent>().AddReference(shape);

            var boneTag = shapeTransform.CompositionTree.Graph.Tag;
            foreach (var primitiveObject in RenderShapePrimitives(shape, allowSkinning: true))
            {
                primitiveObject.transform.rotation = shapeTransform.Rotation;
                primitiveObject.transform.position = shapeTransform.Transform;
                primitiveObject.transform.SetParent(parent.transform, worldPositionStays:false);

                if (boneTag != "")
                {
                    if (!BoneNamesToAttachedObjects.TryGetValue(boneTag, out var attached))
                    {
                        attached = new List<GameObject>();
                        BoneNamesToAttachedObjects[boneTag] = attached;
                    }
                    attached.Add(primitiveObject);
                }
            }
        }

        /// <summary>
        /// Renders a shape's primitives as GameObjects (not yet parented/positioned). With
        /// `allowSkinning`, meshes that need bone binding become `SkinnedMeshRenderer`s queued for
        /// `BindBonesInMeshes()` (requires this component's own composition tree to have already
        /// rendered a matching skeleton via `BoneNamesToTransform`/`_boneIdToTransform`) - otherwise
        /// (or if there are no bones to bind against) everything renders as plain static geometry.
        /// </summary>
        private IEnumerable<GameObject> RenderShapePrimitives(ScenegraphShapeAsset shape, bool allowSkinning)
        {
            shape.LoadModelsAndMaterials();
            foreach (var model in shape.Models)
            {
                foreach (var primitive in model.Primitives)
                {
                    // A material override for this subset (genetics, e.g. eye color) takes priority
                    // over whatever material the shape's own SHPE data names for it. If this group is
                    // not listed in either, we don't render it.
                    if (!_materialOverridesBySubset.TryGetValue(primitive.Key, out var material) &&
                        !shape.Materials.TryGetValue(primitive.Key, out material))
                    {
                        continue;
                    }

                    GameObject primitiveObject;
                    // Make a SkinnedMeshRenderer or a regular renderer depending on if we have blend shapes or bones.
                    if (allowSkinning && primitive.Value.NeedsSkinnedRenderer)
                    {
                        // Add an animation component so we can play animations in this scenegraph.
                        if (GetComponent<Animation>() == null)
                        {
                            gameObject.AddComponent<Animation>();
                        }

                        primitiveObject = new GameObject(primitive.Key, typeof(SkinnedMeshRenderer));

                        var skinnedRenderer = primitiveObject.GetComponent<SkinnedMeshRenderer>();
                        skinnedRenderer.sharedMesh = primitive.Value.Mesh;
                        skinnedRenderer.sharedMaterial = material.GetAsUnityMaterial();
                        if (primitive.Value.HasBones)
                        {
                            _meshesToBindBonesFor.Enqueue((primitive.Value, skinnedRenderer));
                        }

                        var blends = new List<string>();
                        for (var i = 0; i < skinnedRenderer.sharedMesh.blendShapeCount; i++)
                        {
                            blends.Add(skinnedRenderer.sharedMesh.GetBlendShapeName(i));
                        }
                        _unityObjectsToBlends[primitiveObject.transform] = blends;
                    }
                    else
                    {
                        primitiveObject = new GameObject(primitive.Key, typeof(MeshFilter), typeof(MeshRenderer));
                        primitiveObject.GetComponent<MeshFilter>().sharedMesh = primitive.Value.Mesh;
                        primitiveObject.GetComponent<MeshRenderer>().sharedMaterial =
                            material.GetAsUnityMaterial();
                    }

                    yield return primitiveObject;
                }
            }
        }

        /// <summary>
        /// Renders a shape's meshes as loose, unskinned static geometry parented under a new
        /// GameObject - for content that isn't attached to any skeleton (e.g. resolved CAS outfit
        /// parts, see SimCharacterComponent.CreatePersonOutfit - clothing/hair meshes are normally
        /// bone-attached onto a Sim's body via bone-tag matching in game, which this doesn't do).
        /// </summary>
        public static GameObject CreateStandaloneShapeGameObject(ScenegraphShapeAsset shape,
            Dictionary<string, ScenegraphMaterialDefinitionAsset> materialOverridesBySubset = null)
        {
            var shapeObject = new GameObject(shape.GlobalTGI.ToString(), typeof(ScenegraphComponent), typeof(AssetReferenceComponent));
            var component = shapeObject.GetComponent<ScenegraphComponent>();
            shapeObject.GetComponent<AssetReferenceComponent>().AddReference(shape);
            if (materialOverridesBySubset != null)
            {
                component._materialOverridesBySubset = materialOverridesBySubset;
            }

            var boneTag = shape.ShapeBlock.Tag;
            List<GameObject> attached = null;
            if (boneTag != "")
            {
                attached = new List<GameObject>();
                component.BoneNamesToAttachedObjects[boneTag] = attached;
            }

            foreach (var primitiveObject in component.RenderShapePrimitives(shape, allowSkinning: false))
            {
                primitiveObject.transform.SetParent(shapeObject.transform, worldPositionStays: false);
                attached?.Add(primitiveObject);
            }

            return shapeObject;
        }

        /// <summary>
        /// Grafts <paramref name="objects"/> onto this component's own bone tagged <paramref name="tag"/>,
        /// replacing whatever was already attached there (if anything) - see BoneNamesToAttachedObjects.
        /// This is a simplification of the real engine's AddSkin, which merges/accumulates onto the
        /// target bone's existing shape-ref rather than replacing it; that's not reproduced here since
        /// our one-shape-per-slot rendering model has no equivalent notion of an accumulating shape-ref,
        /// and replacing is what's needed to stop an unattached placeholder (e.g. a skeleton's own
        /// default head/hand shape) from rendering underneath the real, correctly-overridden content.
        /// Callers are responsible for migrating the source's AssetReferenceComponent references onto
        /// this component before discarding the (now emptied) source - see SimCharacterComponent.CreatePersonOutfit.
        /// Returns false (no-op) if this component has no bone tagged <paramref name="tag"/>.
        /// </summary>
        public bool TryAttachPart(string tag, IReadOnlyList<GameObject> objects)
        {
            if (!BoneNamesToTransform.TryGetValue(tag, out var boneTransform))
            {
                return false;
            }

            if (BoneNamesToAttachedObjects.TryGetValue(tag, out var existing))
            {
                foreach (var existingObject in existing)
                {
                    Destroy(existingObject);
                }
                existing.Clear();
            }
            else
            {
                existing = new List<GameObject>();
                BoneNamesToAttachedObjects[tag] = existing;
            }

            foreach (var attachedObject in objects)
            {
                attachedObject.transform.SetParent(boneTransform.parent, worldPositionStays: false);
                attachedObject.transform.position = boneTransform.position;
                attachedObject.transform.rotation = boneTransform.rotation;
                existing.Add(attachedObject);
            }

            return true;
        }

        private void RenderLightRefNode(GameObject parent, ScenegraphResourceCollection rCol, LightRefNodeBlock lightRef)
        {
            var shapeTransform = lightRef.Renderable.Bounded.Transform;
            // Render any sub-objects in the transform node.
            RenderTransformNode(parent, rCol, shapeTransform);

            Debug.Assert(lightRef.Light is ExternalReference);
            var lightKey = rCol.FileLinks[((ExternalReference) lightRef.Light).FileLinksIndex];

            if (lightKey.GroupID == GroupIDs.Local)
            {
                // Use our groupId if the reference has a local group id.
                lightKey = lightKey.WithGroupID(_resourceAssetKey.GroupID);
            }
            // TODO: read and render the light here.
        }
    }
}