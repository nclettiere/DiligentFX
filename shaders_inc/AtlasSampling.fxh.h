"#ifndef _ATLAS_SAMPLING_FXH_\n"
"#define _ATLAS_SAMPLING_FXH_\n"
"\n"
"/// Attributes of the SampleTextureAtlas function\n"
"struct SampleTextureAtlasAttribs\n"
"{\n"
"    /// Sampling location in the texture atlas.\n"
"    ///\n"
"    /// \\remarks    This is the final location, *not* relative\n"
"    ///             location within the region.\n"
"    float2 f2UV;\n"
"\n"
"    /// Smooth texture coordinates that will be used to compute the level of detail.\n"
"    ///\n"
"    /// \\remarks    When using texture atlas, texture coordinates may need to be\n"
"    ///             explicitly wrapped using the frac() function. If these\n"
"    ///             coordinates are used for the level-of-detail calculation, the\n"
"    ///             result will be incorrect at the texture region boundaries.\n"
"    ///             To avoid this, smooth texture coordinates are required.\n"
"    ///\n"
"    ///             Smooth coordinates are not used for sampling, only for\n"
"    ///             gradient calculation by the GPU. Therefore, they may\n"
"    ///             use arbitrary translation.\n"
"    float2 f2SmoothUV;\n"
"\n"
"    /// Texture array slice.\n"
"    float fSlice;\n"
"\n"
"    /// Texture region in the atlas:\n"
"    ///   - (x,y) - region size\n"
"    ///   - (z,w) - region offset\n"
"    float4 f4UVRegion;\n"
"\n"
"    /// Smooth texture coordinate gradient in x direction (ddx(f2SmoothUV)).\n"
"    float2 f2dSmoothUV_dx;\n"
"\n"
"    /// Smooth texture coordinate gradient in y direction (ddy(f2SmoothUV)).\n"
"    float2 f2dSmoothUV_dy;\n"
"\n"
"    /// The dimension of the smallest mip level that contains valid data.\n"
"    /// For example, for a 4x4 block-compressed texture atlas, the dimension of the smallest\n"
"    /// level will be 4. This is because 4x4 blocks at higher mip levels require data from\n"
"    /// neighboring regions, which may not be available at the time the region is packed into\n"
"    /// the atlas.\n"
"    float fSmallestValidLevelDim; /* = 4.0 */\n"
"\n"
"    /// Indicates if the texture data is non-filterable (e.g. material indices).\n"
"    bool IsNonFilterable;\n"
"};\n"
"\n"
"\n"
"/// Samples texture atlas in a way that avoids artifacts at the texture region boundaries.\n"
"/// This function is intended to be used with the dynamic texture atlas (IDynamicTextureAtlas).\n"
"\n"
"/// \\param [in] Atlas         - Texture atlas.\n"
"/// \\param [in] Atlas_sampler - Sampler state for the texture atlas.\n"
"/// \\param [in] Attribs       - Texture atlas sampling attributes.\n"
"float4 SampleTextureAtlas(Texture2DArray            Atlas,\n"
"                          SamplerState              Atlas_sampler,\n"
"                          SampleTextureAtlasAttribs Attribs)\n"
"{\n"
"    // Properly handle upside-down and mirrored regions.\n"
"    float4 f4UVRegion;\n"
"    f4UVRegion.xy = abs(Attribs.f4UVRegion.xy);\n"
"    f4UVRegion.zw = min(Attribs.f4UVRegion.zw, Attribs.f4UVRegion.zw + Attribs.f4UVRegion.xy);\n"
"\n"
"    float2 f2dUV_dx = Attribs.f2dSmoothUV_dx;\n"
"    float2 f2dUV_dy = Attribs.f2dSmoothUV_dy;\n"
"\n"
"    // Calculate the texture LOD using smooth coordinates\n"
"    float LOD = Atlas.CalculateLevelOfDetail(Atlas_sampler, Attribs.f2SmoothUV);\n"
"\n"
"    // Make sure that texture filtering does not use samples outside of the texture region.\n"
"    float2 f2AtlasDim;\n"
"    float  fElements;\n"
"    Atlas.GetDimensions(f2AtlasDim.x, f2AtlasDim.y, fElements);\n"
"    // The margin must be no less than half the pixel size in the selected LOD.\n"
"    float2 f2LodMargin = 0.5 / f2AtlasDim * exp2(ceil(LOD));\n"
"\n"
"\n"
"    // Use gradients to make sure that the sampling area does not\n"
"    // go beyond the texture region.\n"
"    //  ____________________                     ________\n"
"    // |               .\'.  |                  .\'.       A\n"
"    // |             .\'  .\' |                .\'  .\'      |\n"
"    // |           .\' *.\'   |              .\'  .\'  |     | abs(f2dUV_dx.y) + abs(f2dUV_dy.y)\n"
"    // |          \'. .\'     |            .\'  .\'    |     |\n"
"    // |            \'       |           | \'.\'______|_____V\n"
"    // |                    |           |          |\n"
"    // |____________________|            <-------->\n"
"    //                                       abs(f2dUV_dx.x) + abs(f2dUV_dy.x)\n"
"    //\n"
"    float2 f2GradientMargin = 0.5 * (abs(f2dUV_dx) + abs(f2dUV_dy));\n"
"\n"
"    float2 f2Margin = f2LodMargin + f2GradientMargin;\n"
"    // Limit the margin by 1/2 of the texture region size to prevent boundaries from overlapping.\n"
"    f2Margin = min(f2Margin, f4UVRegion.xy * 0.5);\n"
"\n"
"    // Clamp UVs using the margin.\n"
"    float2 f2UV = clamp(Attribs.f2UV,\n"
"                        f4UVRegion.zw + f2Margin,\n"
"                        f4UVRegion.zw + f4UVRegion.xy - f2Margin);\n"
"\n"
"    // Compute the maximum gradient length in pixels\n"
"    float2 f2dIJ_dx = f2dUV_dx * f2AtlasDim.xy;\n"
"    float2 f2dIJ_dy = f2dUV_dy * f2AtlasDim.xy;\n"
"    float  fMaxGrad = max(length(f2dIJ_dx), length(f2dIJ_dy));\n"
"\n"
"    // Note that dynamic texture atlas aligns allocations using the minimum dimension.\n"
"    // This guarantees that filtering will not sample from the neighboring regions in all levels\n"
"    // up to the one where the smallest dimension becomes max(1, Attribs.fSmallestValidLevelDim).\n"
"    //\n"
"    //    8   [ * ]\n"
"    //        |\n"
"    //   16   [ *  * ]\n"
"    //        |\n"
"    //   32   [ *  *  *  * ]\n"
"    //        |\n"
"    //   64   [ *  *  *  *  *  *  *  * ]\n"
"    //    |\n"
"    // Aligned placement\n"
"\n"
"    // Compute the region\'s minimum dimension in pixels.\n"
"    float fMinRegionDim = min(f2AtlasDim.x * f4UVRegion.x, f2AtlasDim.y * f4UVRegion.y);\n"
"    // If the smallest valid level dimension is N, we should avoid the maximum gradient\n"
"    // becoming larger than fMinRegionDim/N.\n"
"    // This will guarantee that we will not sample levels above the smallest valid one.\n"
"    // Clamp the smallest valid level dimension to 2.0 so that we fully fade-out to mean\n"
"    // color when the gradient becomes equal the min dimension.\n"
"    float fSmallestValidLevelDim = max(Attribs.fSmallestValidLevelDim, 2.0);\n"
"    float fMaxGradLimit = fMinRegionDim / fSmallestValidLevelDim;\n"
"\n"
"    // Smoothly fade-out to mean color when the gradient is in the range [fMaxGradLimit, fMaxGradLimit * 2.0]\n"
"    float fMeanColorFadeoutFactor = Attribs.IsNonFilterable ? 0.0 : saturate((fMaxGrad - fMaxGradLimit) / fMaxGradLimit);\n"
"    float4 f4Color = float4(0.0, 0.0, 0.0, 0.0);\n"
"\n"
"    if (fMeanColorFadeoutFactor < 1.0)\n"
"    {\n"
"        // Rescale the gradients to avoid sampling above the level with the smallest valid dimension.\n"
"        float GradScale = min(1.0, fMaxGradLimit / fMaxGrad);\n"
"        f2dUV_dx *= GradScale;\n"
"        f2dUV_dy *= GradScale;\n"
"        f4Color = Atlas.SampleGrad(Atlas_sampler, float3(f2UV, Attribs.fSlice), f2dUV_dx, f2dUV_dy);\n"
"    }\n"
"\n"
"    if (fMeanColorFadeoutFactor > 0.0)\n"
"    {\n"
"        // Manually compute the mean color from the coarsest available level.\n"
"        float LastValidLOD = log2(fMaxGradLimit);\n"
"        float4 f4MeanColor = (Atlas.SampleLevel(Atlas_sampler, float3(f4UVRegion.zw + float2(0.25, 0.25) * f4UVRegion.xy, Attribs.fSlice), LastValidLOD) +\n"
"                              Atlas.SampleLevel(Atlas_sampler, float3(f4UVRegion.zw + float2(0.75, 0.25) * f4UVRegion.xy, Attribs.fSlice), LastValidLOD) +\n"
"                              Atlas.SampleLevel(Atlas_sampler, float3(f4UVRegion.zw + float2(0.25, 0.75) * f4UVRegion.xy, Attribs.fSlice), LastValidLOD) +\n"
"                              Atlas.SampleLevel(Atlas_sampler, float3(f4UVRegion.zw + float2(0.75, 0.75) * f4UVRegion.xy, Attribs.fSlice), LastValidLOD)) *\n"
"                             0.25;\n"
"\n"
"        f4Color = lerp(f4Color, f4MeanColor, fMeanColorFadeoutFactor);\n"
"    }\n"
"\n"
"    return f4Color;\n"
"}\n"
"\n"
"#endif //_ATLAS_SAMPLING_FXH_\n"