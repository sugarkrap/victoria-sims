using OpenTS2.Content.DBPF.Scenegraph;
using UnityEngine;

namespace OpenTS2.Rendering.Materials
{
    public class WaterAnimatingTexturesMaterial : StandardMaterial
    {
        public override string Name => "WaterAnimatingTextures";

        private static readonly int WaterSpeed = Shader.PropertyToID("_WaterSpeed");

        // The default from the shader class' own declaration of the parameter,
        // "#extraparam float waterSpeed 0.3 0 5".
        private const float DefaultWaterSpeed = 0.3f;

        protected override Shader GetShader(ScenegraphMaterialDefinitionAsset definition)
        {
            return Shader.Find("OpenTS2/StandardMaterial/WaterAnimatingTextures");
        }

        public override Material Parse(ScenegraphMaterialDefinitionAsset definition)
        {
            var material = base.Parse(definition);

            var waterSpeed = definition.GetProperty("waterSpeed");
            material.SetFloat(WaterSpeed,
                waterSpeed == null ? DefaultWaterSpeed : float.Parse(waterSpeed));

            // "textureAddressing tile tile tile" - the animation scales the texture coordinates
            // past 1, so the texture has to repeat rather than clamp at the edge.
            if (material.mainTexture != null)
            {
                material.mainTexture.wrapMode = TextureWrapMode.Repeat;
            }

            return material;
        }
    }
}
