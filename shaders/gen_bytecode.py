#!/usr/bin/env python3
"""Extract/rewrite SDVX HLSL, compile with fxc, emit shaders_bytecode.h

Matched at runtime by (src_len, profile, entry) when Wine D3DX fails.
REWRITE entries compile alternate register-based HLSL for effect-style
sources that use illegal sampler_state in D3DXCompileShader.

Sources are pulled by marker+length from versioned soundvoltex-*.dll dumps
(not stale PE VAs).
"""
from __future__ import annotations

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FXC = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
OUT_H = os.path.join(ROOT, "shaders_bytecode.h")

DLL_LATE = os.path.join(ROOT, "soundvoltex-2025120900.dll")  # Exceed/Nabla EXTRACT + Cubism Normal 760
DLL_MID = os.path.join(ROOT, "soundvoltex-2021121400.dll")  # Exceed-era full Cubism FX (4157)
DLL_EARLY = os.path.join(ROOT, "soundvoltex-2020011500.dll")  # Early full Cubism FX (3130)

# (name, src_len, profile, entry, marker, must_all, must_not)
# must_all / must_not are tuples of bytes substrings.
EXTRACT = [
    ("p3d_ps_mrt", 506, "ps_3_0", "PixelShaderEntryPoint", b"COLOR1", (b"samplerTexture",), ()),
    ("p3d_ps_tex", 440, "ps_3_0", "PixelShaderEntryPoint", b"tex2D(samplerTexture", (b"PixelShaderEntryPoint",), (b"COLOR1",)),
    ("p3d_ps_color", 364, "ps_3_0", "PixelShaderEntryPoint", b"PixelShaderOutput", (b"PixelShaderEntryPoint",), (b"sampler",)),
    ("p3d_ps_passthru", 323, "ps_3_0", "PixelShaderEntryPoint", b"output.color = input.color", (b"samplerTexture",), ()),
    ("p3d_vs", 824, "vs_3_0", "VertexShaderEntryPoint", b"screenOffset", (b"VertexShaderEntryPoint",), ()),
    ("afp_tone_ps", 2091, "ps_2_b", "main", b"sampler2D input", (b"main",), ()),
    ("afp_blur_vert_ps", 1585, "ps_2_b", "main", b"colorTex", (b"blurStrength", b"PSOUTPUT"), ()),
    ("afp_blur_comb_ps", 1635, "ps_2_b", "main", b"verticalBlurTexture", (b"blurStrength",), ()),
    ("afp_alphablur_ps", 1361, "ps_2_b", "AlphaBlurShader", b"AlphaBlurShader", (), ()),
    ("afp_hsv_ps", 3848, "ps_2_b", "HSVPixelShader", b"HSVPixelShader", (b"register",), ()),
    ("csm_setupmask_vs", 713, "vs_3_0", "VertexShaderEntryPoint", b"clipPosition = output.position", (), ()),
    ("csm_setupmask_ps", 905, "ps_3_0", "PixelShaderEntryPoint", b"channelFlag", (b"step(baseColor.x",), ()),
    ("csm_normal_vs", 621, "vs_3_0", "VertexShaderEntryPoint", b"VertexShaderEntryPoint", (b"row_major float4x4 mtx",), (b"clipPosition",)),
    ("csm_masked_vs", 813, "vs_3_0", "VertexShaderEntryPoint", b"clipMtx", (), ()),
    ("csm_normal_ps", 798, "ps_3_0", "PixelShaderEntryPoint", b"multiplyColor", (b"color.xyz *= color.w",), (b"maskTexture",)),
    ("csm_premult_ps", 760, "ps_3_0", "PixelShaderEntryPoint", b"multiplyColor", (), (b"maskTexture", b"color.xyz *= color.w")),
    ("csm_masked_ps", 1191, "ps_3_0", "PixelShaderEntryPoint", b"maskTexture", (b"color = color * maskVal", b"color.xyz *= color.w"), ()),
    ("csm_maskinv_ps", 1200, "ps_3_0", "PixelShaderEntryPoint", b"maskTexture", (b"color * (1.0f - maskVal)", b"color.xyz *= color.w"), ()),
    ("csm_maskpre_ps", 1153, "ps_3_0", "PixelShaderEntryPoint", b"maskTexture", (b"color = color * maskVal",), (b"color.xyz *= color.w",)),
    ("csm_maskinvpre_ps", 1162, "ps_3_0", "PixelShaderEntryPoint", b"maskTexture", (b"color * (1.0f - maskVal)",), (b"color.xyz *= color.w",)),
]

REWRITES = [
    (
        "afp_gray_ps",
        870,
        "ps_2_b",
        "RenderGrayPixelShader",
        r"""
float4 g_pixelColor : register(c0);
sampler2D SrcSampler : register(s0);

struct VS_OUTPUT
{
	float4 Pos		: POSITION;
	float4 Color	: COLOR;
	float2 Tex		: TEXCOORD0;
};

float4 RenderGrayPixelShader( VS_OUTPUT In ) : COLOR
{
	float4 OutColor = tex2D(SrcSampler, In.Tex);
	const float3 RGB2Y = {0.29900, 0.58700, 0.11400};
	OutColor.rgb = dot(OutColor.rgb, RGB2Y);
	OutColor = (OutColor * In.Color) + g_pixelColor;
	return OutColor;
}
""",
    ),
    (
        "afp_sepia_ps",
        1170,
        "ps_2_b",
        "RenderSepiaPixelShader",
        r"""
float4 g_pixelColor : register(c0);
sampler2D SrcSampler : register(s0);

struct VS_OUTPUT
{
	float4 Pos		: POSITION;
	float4 Color	: COLOR;
	float2 Tex		: TEXCOORD0;
};

float4 RenderSepiaPixelShader( VS_OUTPUT In ) : COLOR
{
	float4 OutColor = tex2D(SrcSampler, In.Tex);
	const float3 RGB2Y = {0.29900, 0.58700, 0.11400};
	float3 YCbCr;
	float3x3 YCbCr2RGB =
	{
		{1.0f, 0.00000f, 1.40200f},
		{1.0f,-0.34414f,-0.71414f},
		{1.0f, 1.77200f, 0.00000f},
	};
	YCbCr.x = dot(OutColor.rgb, RGB2Y);
	YCbCr.y = -0.2f;
	YCbCr.z = 0.1;
	OutColor.rgb = mul(YCbCr2RGB, YCbCr);
	OutColor = (OutColor * In.Color) + g_pixelColor;
	return OutColor;
}
""",
    ),
    (
        "afp_raster_ps",
        1265,
        "ps_2_b",
        "RasterScrollShader",
        r"""
float4 g_pixelColor : register(c0);
float g_period : register(c1);
float g_amplitude : register(c2);
float g_offset : register(c3);
sampler2D SrcSampler : register(s0);

struct VS_OUTPUT
{
	float4 Pos		: POSITION;
	float4 Color	: COLOR;
	float2 Tex		: TEXCOORD0;
};

float4 RasterScrollShader( VS_OUTPUT In ) : COLOR
{
	In.Tex.x += sin( ( In.Tex.y + g_offset ) * g_period ) * g_amplitude;
	if( In.Tex.x < 0.0f )
	{
		int num = 1 + g_amplitude;
		In.Tex.x += 1.0f * num;
	}
	const float border = 0.5f;
	if( In.Tex.x > border )
	{
		int num = In.Tex.x / border;
		In.Tex.x -= border * num;
	}
	return tex2D( SrcSampler, In.Tex ) * In.Color + g_pixelColor;
}
""",
    ),
    # Pre-20251209 AFP HSV (effect-style src_len=5852). Original omits g_mul decl.
    (
        "afp_hsv_legacy_ps",
        5852,
        "ps_2_b",
        "HSVPixelShader",
        r"""
float4 g_pixelColor : register(c0);
float g_hue : register(c1);
float g_saturation : register(c2);
float g_value : register(c3);
float g_mul : register(c4);
sampler2D SrcSampler : register(s0);

struct VS_OUTPUT
{
	float4 Pos		: POSITION;
	float4 Color	: COLOR;
	float2 Tex		: TEXCOORD0;
};

float3 func_rgb_to_hls(float R, float G, float B)
{
	float H = 0;
	float L = 0.5f;
	float S = 0.5f;

	float cmax = max(R, max(G, B));
	float cmin = min(R, min(G, B));
	L=(cmax+cmin)*0.5f;

	if( (cmax-cmin) < (1.0f/256.0f))
	{
		S=0;
		H=0;
	}
	else
	{
		if( L<=0.5 )
		{
			S=(cmax-cmin)/(cmax+cmin);
		}
		else if( L>0.5 )
		{
			S=(cmax-cmin)/(2-(cmax+cmin));
		}

		float Cr = (cmax - R) / (cmax-cmin);
		float Cg = (cmax - G) / (cmax-cmin);
		float Cb = (cmax - B) / (cmax-cmin);
		if( R==cmax )
		{
			H=60*(Cb - Cg);
		}
		else if( G==cmax )
		{
			H=60*(2+(Cr - Cb));
		}
		else if( B==cmax )
		{
			H=60*(4+(Cg - Cr));
		}
	}

	if( H < 0 )
	{
		H += 360;
	}

	return float3( H,L,S );
}

float func_calc_hls_to_rgb_elem(float H, float cmin, float cmax)
{
	float ret = 0.0f;
	if( H < 0.0f )
	{
		H += 360.0f;
	}
	if( H >= 360.0f )
	{
		H -= 360.0f;
	}

	if( H<60.0f )
	{
		ret = cmin+(cmax-cmin)*H/60.0f;
	}
	else if(H>=60.0f && H<180.0f )
	{
		ret = cmax;
	}
	else if(H>=180.0f && H<240.0f )
	{
		ret = cmin+(cmax-cmin)*(240.0f-H)/60.0f;
	}
	else if(H>=240.0f)
	{
		ret = cmin;
	}

	ret = clamp(ret, 0.0f, 1.0f);
	return ret;
}

float3 func_hls_to_rgb(float H, float L, float S)
{
	float3 ret = float3(0,0,0);
	float MAX, MIN = 0;

	if( L <= 0.5 )
	{
		MAX = L * (1 +S);
	}
	else if( L > 0.5 )
	{
		MAX = L * (1 - S) + S;
	}

	MIN = 2 * L -MAX;

	{
		ret.r = func_calc_hls_to_rgb_elem(H + 120, MIN, MAX);
		ret.g = func_calc_hls_to_rgb_elem(H , MIN, MAX);
		ret.b = func_calc_hls_to_rgb_elem(H - 120, MIN, MAX);
	}

	return ret;
}

float4 HSVPixelShader( VS_OUTPUT In ) : COLOR
{
	float4 color = tex2D( SrcSampler, In.Tex );

	float3 baseHLS = func_rgb_to_hls(color.r, color.g, color.b);

	float H = g_hue * 360.0f;
	float L = g_value;
	float S = g_saturation;
	if( S > 0.0f )
	{
		baseHLS.z = lerp(baseHLS.z, 1.0f, pow(S,2.2f));
	}
	else
	{
		baseHLS.z = baseHLS.z * (S+1.0f);
	}

	if( L > 0.0f )
	{
		baseHLS.y = lerp(baseHLS.y, 1.0f, pow(L,2.2f));
	}
	else
	{
		baseHLS.y = baseHLS.y * (L+1.0f);
	}
	baseHLS.x = baseHLS.x * g_mul + H;
	if(baseHLS.x>=360.0f){ baseHLS.x -= 360.0f; }
	baseHLS.y = clamp(baseHLS.y, 0, 1.0);
	baseHLS.z = baseHLS.z*g_mul + (S*(1.0f-g_mul));
	baseHLS.z = clamp(baseHLS.z, 0, 1.0);

	color.rgb = func_hls_to_rgb(baseHLS.x, baseHLS.y, baseHLS.z);

	return color * In.Color + g_pixelColor;
}
""",
    ),
]


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def iter_cstrings(data: bytes, marker: bytes):
    start = 0
    while True:
        i = data.find(marker, start)
        if i < 0:
            return
        b = i
        while b > 0 and data[b - 1] != 0:
            b -= 1
        e = i
        while e < len(data) and data[e] != 0:
            e += 1
        yield data[b:e]
        start = i + 1


def normalize_hlsl(raw: bytes, src_len: int) -> bytes | None:
    if len(raw) == src_len:
        return raw
    if len(raw) > src_len:
        for prefix in (b"float4", b"float2", b"float ", b"sampler", b"struct", b"\n", b"\t"):
            j = raw.find(prefix)
            if j >= 0 and len(raw) - j == src_len:
                return raw[j:]
            # leading newline/tab then float
            if prefix in (b"\n", b"\t"):
                continue
        # try first float/sampler after junk
        for prefix in (b"float4", b"float2", b"sampler2D", b"row_major", b"struct"):
            j = raw.find(prefix)
            if j >= 0 and len(raw) - j == src_len:
                return raw[j:]
    return None


def find_shader(
    data: bytes,
    marker: bytes,
    src_len: int,
    must_all: tuple[bytes, ...] = (),
    must_not: tuple[bytes, ...] = (),
) -> bytes:
    for raw in iter_cstrings(data, marker):
        body = normalize_hlsl(raw, src_len)
        if body is None:
            continue
        if any(m not in body for m in must_all):
            continue
        if any(m in body for m in must_not):
            continue
        return body
    raise ValueError(
        f"no match len={src_len} marker={marker!r} must={must_all!r} not={must_not!r}"
    )


def compile_one(name: str, src: bytes, profile: str, entry: str, src_len: int):
    hlsl_path = os.path.join(HERE, name + ".hlsl")
    cso_path = os.path.join(HERE, name + ".cso")
    with open(hlsl_path, "wb") as f:
        f.write(src)
    cmd = [FXC, "/nologo", "/T", profile, "/E", entry, "/Fo", cso_path, hlsl_path]
    print(" ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise SystemExit(r.returncode)
    with open(cso_path, "rb") as f:
        cso = f.read()
    key = fnv1a64(src + b"\0" + entry.encode() + b"\0" + profile.encode())
    print(f"  {name}: match_src_len={src_len} cso={len(cso)}")
    return (name, profile, entry, src_len, key, cso)


def compile_fx(name: str, src: bytes, src_len: int) -> tuple[str, int, bytes]:
    fx_path = os.path.join(HERE, name + ".fx")
    fxo_path = os.path.join(HERE, name + ".fxo")
    with open(fx_path, "wb") as f:
        f.write(src)
    cmd = [FXC, "/nologo", "/T", "fx_2_0", "/Fo", fxo_path, fx_path]
    print(" ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise SystemExit(r.returncode)
    with open(fxo_path, "rb") as f:
        fxo = f.read()
    print(f"  {name}: match_src_len={src_len} fxo={len(fxo)}")
    return (name, src_len, fxo)


def emit_bytes_array(lines: list[str], symbol: str, blob: bytes) -> None:
    lines.append(f"static const uint8_t {symbol}[{len(blob)}] = {{")
    for i in range(0, len(blob), 16):
        chunk = blob[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")


def main() -> int:
    if not os.path.isfile(FXC):
        print("fxc not found:", FXC, file=sys.stderr)
        return 1
    for path in (DLL_LATE, DLL_MID, DLL_EARLY):
        if not os.path.isfile(path):
            print("DLL not found:", path, file=sys.stderr)
            return 1

    late = open(DLL_LATE, "rb").read()
    mid = open(DLL_MID, "rb").read()
    early = open(DLL_EARLY, "rb").read()
    entries = []

    for name, src_len, profile, entry, marker, must_all, must_not in EXTRACT:
        src = find_shader(late, marker, src_len, must_all, must_not)
        entries.append(compile_one(name, src, profile, entry, src_len))

    for name, src_len, profile, entry, rewrite in REWRITES:
        src = rewrite.encode("ascii")
        entries.append(compile_one(name, src, profile, entry, src_len))

    fx_late = find_shader(late, b"ShaderNames_Normal", 760, (b"PixelNormal",), ())
    fx_full_early = find_shader(
        early, b"ShaderNames_SetupMask", 3130, (b"ShaderNames_Normal",), ()
    )
    fx_full_mid = find_shader(
        mid, b"ShaderNames_SetupMask", 4157, (b"ShaderNames_NormalMaskedInverted",), ()
    )
    _, _, fxo_late = compile_fx("cubism_normal", fx_late, 760)
    _, _, fxo_full_early = compile_fx("cubism_full_3130", fx_full_early, 3130)
    _, _, fxo_full_mid = compile_fx("cubism_full_4157", fx_full_mid, 4157)

    lines = [
        "/* Auto-generated by shaders/gen_bytecode.py — do not edit */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "typedef struct SdvxShaderBlob {",
        "    uint64_t key;",
        "    uint32_t src_len;",
        "    const char *profile;",
        "    const char *entry;",
        "    const char *name;",
        "    const uint8_t *data;",
        "    uint32_t size;",
        "} SdvxShaderBlob;",
        "",
    ]

    for name, profile, entry, src_len, key, cso in entries:
        emit_bytes_array(lines, f"k_{name}_cso", cso)

    emit_bytes_array(lines, "k_cubism_normal_fxo", fxo_late)
    emit_bytes_array(lines, "k_cubism_full_3130_fxo", fxo_full_early)
    emit_bytes_array(lines, "k_cubism_full_4157_fxo", fxo_full_mid)
    lines.append("#define CUBISM_NORMAL_FX_SRC_LEN 760u")
    lines.append("#define CUBISM_FULL_FX_SRC_LEN_3130 3130u")
    lines.append("#define CUBISM_FULL_FX_SRC_LEN_4157 4157u")
    lines.append(f"#define CUBISM_NORMAL_FXO_SIZE {len(fxo_late)}u")
    lines.append(f"#define CUBISM_FULL_FXO_3130_SIZE {len(fxo_full_early)}u")
    lines.append(f"#define CUBISM_FULL_FXO_4157_SIZE {len(fxo_full_mid)}u")
    lines.append("")

    lines.append(f"#define SDVX_SHADER_BLOB_COUNT {len(entries)}")
    lines.append("static const SdvxShaderBlob g_sdvx_shader_blobs[SDVX_SHADER_BLOB_COUNT] = {")
    for name, profile, entry, src_len, key, cso in entries:
        lines.append(
            f'    {{ 0x{key:016X}ULL, {src_len}u, "{profile}", "{entry}", "{name}", '
            f"k_{name}_cso, {len(cso)}u }},"
        )
    lines.append("};")
    lines.append("")

    with open(OUT_H, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print(
        "Wrote",
        OUT_H,
        f"blobs={len(entries)} cubism_fxo={len(fxo_late)} "
        f"full={len(fxo_full_early)}/{len(fxo_full_mid)}",
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
