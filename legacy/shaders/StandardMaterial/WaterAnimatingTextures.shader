Shader "OpenTS2/StandardMaterial/WaterAnimatingTextures"
{
    Properties
    {
        // Parameters common to all StandardMaterial shaders.
        [NoScaleOffset] _MainTex ("Texture", 2D) = "white" {}
        _AlphaMultiplier("Alpha Multiplier", Range(0.0, 1.0)) = 1.0
        _DiffuseCoefficient("Diffuse color coefficient", Color) = (1, 1, 1, 1)
        _UVScale("UV scale", Vector) = (1, 1, 0, 0)

        // The material definition's own "waterSpeed" parameter: how many times per second
        // each texture plane runs through its scale cycle.
        _WaterSpeed("Water speed", float) = 0.3
    }
    SubShader
    {
        ZWrite Off
        Tags { "RenderType"="Transparent" "Queue"="Transparent" "ForceNoShadowCasting"="True" }

        CGPROGRAM
        #pragma surface surf Lambert fullforwardshadows alpha:blend

        // Use shader model 3.0 target, to get nicer looking lighting
        #pragma target 3.0

        sampler2D _MainTex;
        float _AlphaMultiplier;
        float4 _DiffuseCoefficient;
        float4 _UVScale;
        float _WaterSpeed;

        struct Input
        {
            float2 uv_MainTex;
        };

        void surf (Input IN, inout SurfaceOutput o)
        {
            float2 uv = IN.uv_MainTex * _UVScale.xy;

            // Two planes of the same texture, each with its own animated texture matrix: a
            // uniform scale about (0, 0) sawtoothing from 1x to 2x, which makes the pattern
            // travel outwards. The second plane runs half a cycle out of step with the first.
            float phase0 = frac(_Time.y * _WaterSpeed);
            float phase1 = frac(_Time.y * _WaterSpeed + 0.5);
            fixed4 plane0 = tex2D(_MainTex, uv * lerp(1, 2, phase0));
            fixed4 plane1 = tex2D(_MainTex, uv * lerp(1, 2, phase1));

            // Cross-fade weight - a triangular wave locked to the same period, phased so that
            // each plane has faded out completely at the moment its own scale wraps back to
            // 1x, hiding the jump. plane0 is fully hidden at phase0 == 0, plane1 at phase1 == 0.
            float blend = 2 * abs(phase1 - 0.5);

            fixed4 c = lerp(plane1, plane0, blend) * _DiffuseCoefficient;
            c.a *= _AlphaMultiplier;

            o.Albedo = c.rgb;
            o.Alpha = c.a;
        }
        ENDCG
    }
    FallBack "Diffuse"
}
