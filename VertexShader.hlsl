
cbuffer ConstantsPerCamera : register(b0)
{
	matrix matView;
  matrix matProj;
}

cbuffer ConstantsPerInstance : register(b1)
{
  matrix matWorld;
  matrix matWorld_IT;
}

struct Input
{
	float4 position : POSITION;
	float3 normal	: NORMAL;
	float3 tangent	: TANGENT;
	float2 uv		: TEXCOORD;
};

struct V2P
{
	float4 position	: SV_POSITION;
	float3 normal	: NORMAL;
	float3 tangent	: TANGENT;
	float2 uv		: TEXCOORD;
};

V2P main(Input v)
{
	V2P v2p;
	float4x4 matMVP = mul(mul(matWorld, matView), matProj);

	v2p.position = mul(v.position, matMVP);
	v2p.normal = mul(v.normal, (float3x3)matWorld_IT);
	v2p.tangent = mul(v.tangent, (float3x3)matWorld_IT);
	v2p.uv = v.uv;

	return v2p;
}