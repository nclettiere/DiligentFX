"#include \"HnAxesStructures.fxh\"\n"
"#include \"BasicStructures.fxh\"\n"
"\n"
"struct PSInput\n"
"{\n"
"    float4 Pos   : SV_POSITION;\n"
"    float4 Color : COLOR;\n"
"};\n"
"\n"
"cbuffer cbCameraAttribs\n"
"{\n"
"    CameraAttribs g_CameraAttribs;\n"
"}\n"
"\n"
"cbuffer cbConstants\n"
"{\n"
"    AxesConstants g_Constants;\n"
"}\n"
"\n"
"void main(in  uint    VertID : SV_VertexID,\n"
"          out PSInput PSIn)\n"
"{\n"
"    //float3 Pos[12];\n"
"    //Pos[0] = float3(-1.0, 0.0, 0.0);\n"
"    //Pos[1] = float3( 0.0, 0.0, 0.0);\n"
"    //Pos[2] = float3( 0.0, 0.0, 0.0);\n"
"    //Pos[3] = float3(+1.0, 0.0, 0.0);\n"
"\n"
"    //Pos[4] = float3(0.0, -1.0, 0.0);\n"
"    //Pos[5] = float3(0.0,  0.0, 0.0);\n"
"    //Pos[6] = float3(0.0,  0.0, 0.0);\n"
"    //Pos[7] = float3(0.0, +1.0, 0.0);\n"
"\n"
"    //Pos[ 8] = float3(0.0, 0.0, -1.0);\n"
"    //Pos[ 9] = float3(0.0, 0.0,  0.0);\n"
"    //Pos[10] = float3(0.0, 0.0,  0.0);\n"
"    //Pos[11] = float3(0.0, 0.0, +1.0);\n"
"    float3 Pos;\n"
"    Pos.x = (VertID == 0) ? -1.0 : ((VertID ==  3) ? +1.0 : 0.0);\n"
"    Pos.y = (VertID == 4) ? -1.0 : ((VertID ==  7) ? +1.0 : 0.0);\n"
"    Pos.z = (VertID == 8) ? -1.0 : ((VertID == 11) ? +1.0 : 0.0);\n"
"\n"
"    PSIn.Pos   = mul(mul(float4(Pos, 1.0), g_Constants.Transform), g_CameraAttribs.mViewProj);\n"
"    PSIn.Color = g_Constants.AxesColors[VertID/2];\n"
"    PSIn.Color.a *= 1 - length(Pos);\n"
"}\n"