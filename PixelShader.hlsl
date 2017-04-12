
cbuffer ColorConstants : register (b0)
{
	float4 color;
}

Texture2D tex : register(t0);
SamplerState smplr : register(s0);

struct V2P
{
	float4 position	: SV_POSITION;
	float3 normal	: NORMAL;
	float3 tangent	: TANGENT;
	float2 uv		: TEXCOORD;
};

float4 main(V2P input) : SV_TARGET
{
	return color * tex.Sample(smplr, input.uv);
}