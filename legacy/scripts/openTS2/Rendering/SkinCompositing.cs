using System.Collections.Generic;
using OpenTS2.Common;
using OpenTS2.Content;
using OpenTS2.Content.DBPF.Scenegraph;
using OpenTS2.Files.Formats.DBPF;
using OpenTS2.Files.Formats.DBPF.Scenegraph.Block;
using OpenTS2.Rendering.Materials;
using UnityEngine;

namespace OpenTS2.Rendering
{
    /// <summary>
    /// Builds per-sim composited materials that layer a sim's own skin texture underneath a
    /// clothing material's texture, using the clothing texture's own alpha channel as the blend
    /// mask (alpha=0 shows skin, alpha=255 shows the clothing's own color). This mirrors the real
    /// engine's skin-composition support.
    /// </summary>
    public static class SkinCompositing
    {
        /// <summary>
        /// Returns a material identical to clothingMaterial except its base texture has the sim's
        /// own skin texture composited underneath it. The result (and the synthetic texture it
        /// wraps) is registered under a TGI derived from compositeName in the content cache, so a
        /// repeated call for the same (sim, clothing material) pair is a cache hit rather than
        /// rebuilding the texture, and so the normal material-parsing pipeline
        /// (ScenegraphMaterialDefinitionAsset.GetAsUnityMaterial -> MaterialManager.Parse ->
        /// StandardMaterial.Parse -> StandardMaterial.GetTextureAssetByName) resolves it with no
        /// special-casing.
        /// </summary>
        public static ScenegraphMaterialDefinitionAsset BuildCompositedMaterial(
            ScenegraphMaterialDefinitionAsset skinMaterial, ScenegraphMaterialDefinitionAsset clothingMaterial,
            string compositeName)
        {
            var materialKey = new ResourceKey(compositeName, GroupIDs.RuntimeSkinComposite, TypeIDs.SCENEGRAPH_TXMT);
            return (ScenegraphMaterialDefinitionAsset)ContentManager.Instance.Cache.GetOrAdd(materialKey,
                _ => BuildCompositedMaterialUncached(skinMaterial, clothingMaterial, compositeName, materialKey));
        }

        private static ScenegraphMaterialDefinitionAsset BuildCompositedMaterialUncached(
            ScenegraphMaterialDefinitionAsset skinMaterial, ScenegraphMaterialDefinitionAsset clothingMaterial,
            string compositeName, ResourceKey materialKey)
        {
            var skinTextureName = skinMaterial.GetProperty("stdMatBaseTextureName", defaultValue: null);
            var clothingTextureName = clothingMaterial.GetProperty("stdMatBaseTextureName", defaultValue: null);
            if (skinTextureName == null || clothingTextureName == null)
            {
                return clothingMaterial;
            }

            var skinTextureAsset = StandardMaterial.GetTextureAssetByName(skinTextureName, skinMaterial.GlobalTGI.GroupID);
            var clothingTextureAsset = StandardMaterial.GetTextureAssetByName(clothingTextureName, clothingMaterial.GlobalTGI.GroupID);
            if (skinTextureAsset == null || clothingTextureAsset == null)
            {
                Debug.LogWarning($"SkinCompositing: could not find skin texture '{skinTextureName}' or clothing " +
                    $"texture '{clothingTextureName}' - leaving '{compositeName}' uncomposited");
                return clothingMaterial;
            }

            var skinTexture = skinTextureAsset.GetSelectedImageAsUnityTexture();
            var clothingTexture = clothingTextureAsset.GetSelectedImageAsUnityTexture();
            if (skinTexture.width != clothingTexture.width || skinTexture.height != clothingTexture.height)
            {
                Debug.LogWarning($"SkinCompositing: skin texture '{skinTextureName}' ({skinTexture.width}x{skinTexture.height}) " +
                    $"and clothing texture '{clothingTextureName}' ({clothingTexture.width}x{clothingTexture.height}) size " +
                    $"mismatch - leaving '{compositeName}' uncomposited");
                return clothingMaterial;
            }

            var compositedTexture = StandardMaterial.AlphaComposite(skinTexture, clothingTexture);

            var textureAsset = new ScenegraphTextureAsset(compositedTexture)
            {
                TGI = new ResourceKey(compositeName + "_txtr", GroupIDs.RuntimeSkinComposite, TypeIDs.SCENEGRAPH_TXTR)
            };
            ContentManager.Instance.Cache.GetOrAdd(textureAsset.TGI, _ => textureAsset);

            // Copy every property from the original clothing material, only swapping in the
            // synthetic composited texture. numTexturesToComposite is forced to 1 since we've
            // already fully resolved/composited the texture ourselves.
            var newProperties = new Dictionary<string, string>(clothingMaterial.MaterialDefinition.MaterialProperties)
            {
                ["stdMatBaseTextureName"] = compositeName,
                ["numTexturesToComposite"] = "1",
            };

            var newBlock = new MaterialDefinitionBlock(
                clothingMaterial.MaterialDefinition.BlockTypeInfo,
                clothingMaterial.MaterialDefinition.Resource,
                clothingMaterial.MaterialDefinition.MaterialName,
                clothingMaterial.MaterialDefinition.Type,
                newProperties,
                clothingMaterial.MaterialDefinition.TextureNames);

            return new ScenegraphMaterialDefinitionAsset(newBlock)
            {
                TGI = materialKey
            };
        }
    }
}
