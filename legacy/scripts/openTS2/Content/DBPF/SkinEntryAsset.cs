using System;
using System.Collections.Generic;
using OpenTS2.Common;
using OpenTS2.Files.Formats.XML;

namespace OpenTS2.Content.DBPF
{
    public class SkinEntryAsset : AbstractAsset
    {
        public SkinEntryAsset(PropertySet propertySet)
        {
            _properties = propertySet;
            ShapeResourceKey = ParseKeyOrIndex(propertySet, "shape");
            ResourceKey = ParseKeyOrIndex(propertySet, "resource");
            MaterialOverrides = ParseMaterialOverrides(propertySet);
        }

        private readonly PropertySet _properties;

        /// <summary>
        /// The catalog entry's own kind, e.g. "skin" (any CAS-selectable content - hair, clothes,
        /// swimwear, or a bare-body default), "facearchetype", "faceneutral", "skintone", "hairtone", "textureoverlay",
        /// "meshoverlay", "facemodifier". Only present on entries loaded as part of the global skin catalog
        /// (TSData/Res/Catalog/Skins/Skins.package). Null if absent.
        /// </summary>
        public string Type => _properties.Properties.TryGetValue("type", out var value) ? ((StringProp)value).Value : null;

        /// <summary>Human-readable catalog name, e.g. "afbodyswimwear_redbikini". Null if absent.</summary>
        public string Name => _properties.Properties.TryGetValue("name", out var value) ? ((StringProp)value).Value : null;

        /// <summary>Age flag this catalog entry is valid for. Null if absent.</summary>
        public uint? Age => ParseOptionalUint32("age");

        /// <summary>Gender flag this catalog entry is valid for. Null if absent.</summary>
        public uint? Gender => ParseOptionalUint32("gender");

        /// <summary>Species flag this catalog entry is valid for. Null if absent.</summary>
        public uint? Species => ParseOptionalUint32("species");

        /// <summary>
        /// Body-part-slot <see cref="OutfitCategory"/> bitwise ANDed.
        /// </summary>
        public uint? Category => ParseOptionalUint32("category");

        /// <summary>
        /// The skin tone this catalog entry represents, as a GUID-style ID string - matched
        /// against SimAppearanceAsset's own SkinTone. Null if absent.
        /// </summary>
        public string SkinTone => _properties.Properties.TryGetValue("skintone", out var value)
            ? ((StringProp)value).Value : null;

        private uint? ParseOptionalUint32(string key)
        {
            return _properties.Properties.TryGetValue(key, out var value) ? PropertySet.ParsePropAsUint32(value) : (uint?)null;
        }

        public IResourceKeyOrIndex ShapeResourceKey { get; }

        /// <summary>
        /// cTSSkinEntry::ResourceKey() - a second key alongside ShapeResourceKey. For some categories with no shape
        /// (e.g. Face) this is what actually resolves to something renderable, seen as a SCENEGRAPH_CRES on real
        /// data.
        /// </summary>
        public IResourceKeyOrIndex ResourceKey { get; }

        /// <summary>
        /// Swaps the material used for a named subset of the entry's shape/resource, e.g. how genetics
        /// (eye color, etc.) get applied on top of a base head resource's own neutral/placeholder subset materials.
        /// </summary>
        public IReadOnlyList<MaterialOverride> MaterialOverrides { get; }

        private static IResourceKeyOrIndex ParseKeyOrIndex(PropertySet properties, string prefix)
        {
            // cTSSkinEntry::ReadProperties gates on "resourcerestypeid" for whether the whole record
            // (both its "resource" and "shape" keys) was written directly or via the external key vector.
            if (properties.Properties.ContainsKey("resourcerestypeid"))
            {
                var type = PropertySet.ParsePropAsUint32(properties.Properties[$"{prefix}restypeid"]);
                var group = PropertySet.ParsePropAsUint32(properties.Properties[$"{prefix}groupid"]);
                var instance = PropertySet.ParsePropAsUint32(properties.Properties[$"{prefix}id"]);
                return new ResourceKeyProp { Value = new ResourceKey(instance, group, type) };
            }

            return new ResourceKeyIndexProp { Value = (int)PropertySet.ParsePropAsUint32(properties.Properties[$"{prefix}keyidx"]) };
        }

        private static List<MaterialOverride> ParseMaterialOverrides(PropertySet properties)
        {
            var overrides = new List<MaterialOverride>();
            if (!properties.Properties.TryGetValue("numoverrides", out var numOverridesProp))
            {
                return overrides;
            }

            var numOverrides = PropertySet.ParsePropAsUint32(numOverridesProp);
            for (var i = 0; i < numOverrides; i++)
            {
                if (!properties.Properties.TryGetValue($"override{i}subset", out var subsetProp) ||
                    !(subsetProp is StringProp { Value: var subsetName }))
                {
                    continue;
                }

                overrides.Add(new MaterialOverride(subsetName, ParseOverrideResourceKey(properties, $"override{i}resource")));
            }

            return overrides;
        }

        private static IResourceKeyOrIndex ParseOverrideResourceKey(PropertySet properties, string prefix)
        {
            if (properties.Properties.TryGetValue($"{prefix}keyidx", out var keyIdxProp))
            {
                return new ResourceKeyIndexProp { Value = (int)PropertySet.ParsePropAsUint32(keyIdxProp) };
            }

            var type = PropertySet.ParsePropAsUint32(properties.Properties[$"{prefix}restypeid"]);
            var group = PropertySet.ParsePropAsUint32(properties.Properties[$"{prefix}groupid"]);
            var instance = PropertySet.ParsePropAsUint32(properties.Properties[$"{prefix}id"]);
            return new ResourceKeyProp { Value = new ResourceKey(instance, group, type) };
        }
    }

    public readonly struct MaterialOverride
    {
        public readonly string SubsetName;
        public readonly IResourceKeyOrIndex ResourceKey;

        public MaterialOverride(string subsetName, IResourceKeyOrIndex resourceKey)
        {
            SubsetName = subsetName;
            ResourceKey = resourceKey;
        }
    }
}
