// Phosphor - physically based high-res aperture grille CRT for 4K OLED.
// All lighting math is in linear light, 1.0 = SDR reference white.
// Hot-reloaded on save.
//
// Pipeline:
//   Decode   : desktop pixel -> CRT drive signal -> tube response (gamma) -> phosphor gamut
//   Resample : (source mode) area-filter the 4K capture back to the content's real
//              resolution, undoing the GPU's upscale. A CRT has no native resolution.
//   BeamH    : analog horizontal signal (zero-order hold per source pixel) blurred by
//              the beam spot. Spot = gaussian core + wide skirt, width grows per gun
//              with beam current.
//   BeamV    : each scanline is a gaussian ribbon of the same spot.
//   Down     : glow pyramid (level k ~ gaussian sigma 2^k px).
//   Halo     : faceplate halation. Light hitting the front glass beyond the critical
//              angle is totally reflected back onto the phosphor, landing on a ring
//              around its origin. Kernel computed on the CPU from glass thickness.
//   Final    : diffusion + halation (energy conserving), highlight punch, optional
//              grille, encode for SDR (dithered) or HDR scRGB.

Texture2D T0 : register(t0);
Texture2D T1 : register(t1);
Texture2D T2 : register(t2);
Texture2D T3 : register(t3);
Texture2D T4 : register(t4);
Texture2D T5 : register(t5);
Texture2D T6 : register(t6);
Texture2D T7 : register(t7);
Texture2D T8 : register(t8); // on-screen text: r = text coverage, g = panel opacity
Texture2D T9 : register(t9);  // veiling glow levels
Texture2D T10 : register(t10);
SamplerState Lin : register(s0);    // clamp
SamplerState LinBlk : register(s1); // black border (no light beyond the glass)

cbuffer Frame : register(b0)
{
    float srcW;          // content resolution the tube is scanning
    float srcH;
    float pitchX;        // output px per source pixel
    float pitchY;        // output px per scanline

    float inputMode;     // 0 sRGB 8-bit desktop, 1 scRGB fp16 desktop
    float sdrWhiteScale; // scRGB units per SDR white
    float crtGamma;
    float outputMode;    // 0 sRGB, 1 gamma 2.2, 2 scRGB HDR

    float spotMinPx;     // core sigma at zero beam current (output px)
    float spotMaxPx;     // core sigma at full current
    float spotPower;
    float spotAspect;    // horizontal / vertical

    float spotTail;      // energy fraction in the skirt
    float tailMix;       // blend between the two skirt pyramid levels
    float aaX;           // anti-moire floor (px), only when pitch > 1
    float aaY;

    float radiusH;       // gather radius in source columns
    float radiusV;       // gather radius in scanlines
    float srcOffX;       // where the content's source grid starts on screen (px):
    float srcOffY;       // non-zero for pillarboxed / letterboxed content

    float diffusion;     // energy fraction scattered short range
    float halation;      // energy fraction scattered by the faceplate
    float nearMix;       // blend between the two diffusion pyramid levels
    float haloRings;

    float maskType;
    float maskStrength;
    float maskPitch;
    float brightness;

    float peak;          // max output in SDR-white units (HDR)
    float split;
    float highlightBoost;
    float aplKnee;

    float outputBits;
    float fullW;
    float fullH;
    float framePad;

    float4 gamut0;
    float4 gamut1;
    float4 gamut2;

    float4 halo[32];     // x radius (full-res px), y weight per tap, z source level, w angle offset

    // Physical position of each colour's emitter inside a panel pixel, in px
    // from the pixel centre. The beam image is evaluated there per channel so
    // edges line up in colour instead of fringing (QD-OLED triangle layout).
    float4 subX;
    float4 subY;

    // Rolling scan (motion clarity). x = refreshes per CRT field (K),
    // y = peak light one refresh may emit (SDR-white units), z = gain,
    // w = phosphor afterglow time constant in refreshes.
    float4 motion0;
    // x = index of the refresh being drawn within the field (0..K-1),
    // y = slice-edge softness, z = mode (0 rolling scan, 1 motion-adaptive flash)
    float4 motion1;

    // Motion pre-deblur. x = strength, y = show motion vectors, z = max smear (px)
    float4 deblur0;

    // x = glass veiling glow (energy fraction), y = blend between its two
    // pyramid levels, z = extra horizontal analog softness (sigma, px)
    float4 veil0;
};

cbuffer PassData : register(b1)
{
    float2 srcSize;
    float2 srcTexel;
    float2 dstSize;
    float2 dstTexel;
    float4 passParams; // per-pass extras (motion search)
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// ---------------------------------------------------------------- helpers

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float3 SrgbToLinear(float3 c)
{
    c = max(c, 0);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 LinearToSrgb(float3 c)
{
    c = max(c, 0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// erf approximation (Winitzki), max abs error ~1e-4
float3 Erf3(float3 x)
{
    float3 x2 = x * x;
    const float a = 0.147;
    float3 t = -x2 * (1.27323954 + a * x2) / (1.0 + a * x2);
    return sign(x) * sqrt(1.0 - exp(t));
}

// Box of half-width hw convolved with a gaussian, evaluated at d.
// Sums to exactly 1 across a row of boxes: brightness is preserved.
// Gaussian CDF as a smooth odd polynomial (max error 0.0017), reaching 0 and 1
// at +-3 sigma with zero slope: a few multiply-adds, no exp.
float3 Phi(float3 x)
{
    x = clamp(x, -3.0, 3.0);
    float3 x2 = x * x;
    return 0.5 + x * (0.3968262987 + x2 * (-0.06202805046 + x2 * (0.007276986876 + x2 * (-0.0004683397891 +
                                                                                            x2 * 1.220493696e-05))));
}

// A box (one source pixel held for hw*2) blurred by the beam spot, at d.
// Across a row of boxes the terms telescope (Phi(b) - Phi(a), then Phi(c) -
// Phi(b), ...), so brightness is conserved exactly whatever CDF is used.
float3 BoxGaussK(float3 d, float hw, float3 invSigma)
{
    return Phi((d + hw) * invSigma) - Phi((d - hw) * invSigma);
}

float3 BoxGauss(float3 d, float hw, float3 sigma)
{
    return BoxGaussK(d, hw, 1.0 / sigma);
}

// Spot core width per gun from its beam current.
float3 SpotSigma(float3 c)
{
    float3 k = saturate(c);
    if (spotPower != 1.0)
        k = pow(k, spotPower);
    return lerp(spotMinPx.xxx, spotMaxPx.xxx, k);
}

float3 DecodeRaw(float3 s)
{
    return inputMode < 0.5 ? SrgbToLinear(s) : s / sdrWhiteScale;
}

// 3x3 tent [1 2 1]^2 / 16 from four bilinear taps at half-texel offsets.
float3 Tent(Texture2D t, float2 uv)
{
    uint w, h;
    t.GetDimensions(w, h);
    float2 o = 0.5 / float2(w, h);
    float3 c = t.Sample(Lin, uv + float2(-o.x, -o.y)).rgb;
    c += t.Sample(Lin, uv + float2( o.x, -o.y)).rgb;
    c += t.Sample(Lin, uv + float2(-o.x,  o.y)).rgb;
    c += t.Sample(Lin, uv + float2( o.x,  o.y)).rgb;
    return c * 0.25;
}

float Hash(float2 p, float seed)
{
    p += seed * float2(47.0, 17.0);
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// ---------------------------------------------------------------- passes

// Desktop code value = tube drive voltage. The tube answers with a pure power
// law (true black, no sRGB toe). HDR highlights above SDR white pass through.
// Desktop code value = tube drive voltage -> what the tube emits (gamut applied).
float3 DecodePixel(float3 raw)
{
    float3 crt;
    if (inputMode < 0.5)
        crt = pow(saturate(raw), crtGamma); // 8-bit desktop: the code value is the drive
    else
    {
        // HDR desktop: re-encode to the code value, then the tube curve. Both
        // steps are baked into a lookup table (T1, indexed by sqrt for precision
        // near black); HDR highlights above SDR white pass through.
        float3 lin = max(raw / sdrWhiteScale, 0);
        float3 base = min(lin, 1.0);
        float3 u = sqrt(base) * (1023.0 / 1024.0) + 0.5 / 1024.0;
        crt = float3(T1.SampleLevel(Lin, float2(u.r, 0.5), 0).r, T1.SampleLevel(Lin, float2(u.g, 0.5), 0).r,
                     T1.SampleLevel(Lin, float2(u.b, 0.5), 0).r) +
              (lin - base);
    }
    return float3(dot(gamut0.xyz, crt), dot(gamut1.xyz, crt), dot(gamut2.xyz, crt));
}

float4 PS_Decode(VSOut i) : SV_Target
{
    return float4(DecodePixel(T0.Load(int3(i.pos.xy, 0)).rgb), 1);
}

// Area filter W x H -> srcW x srcH. Inverts the GPU's upscale of lower-res content.
float4 PS_Resample(VSOut i) : SV_Target
{
    float2 p = floor(i.pos.xy);
    float x0 = srcOffX + p.x * pitchX, x1 = x0 + pitchX;
    float y0 = srcOffY + p.y * pitchY, y1 = y0 + pitchY;
    int c0 = int(floor(x0)), c1 = min(int(ceil(x1)), int(srcSize.x));
    int r0 = int(floor(y0)), r1 = min(int(ceil(y1)), int(srcSize.y));
    float3 acc = 0;
    float wsum = 0;
    [loop] for (int r = r0; r < r1; r++)
    {
        float wy = min(r + 1.0, y1) - max(float(r), y0);
        [loop] for (int c = c0; c < c1; c++)
        {
            float w = wy * (min(c + 1.0, x1) - max(float(c), x0));
            acc += T0.Load(int3(c, r, 0)).rgb * w;
            wsum += w;
        }
    }
    return float4(acc / max(wsum, 1e-6), 1);
}

// Horizontal: the DAC holds each source pixel for pitchX, the beam spot core
// blurs it. (The spot's wide skirt is added in Final from the glow pyramid.)
float4 PS_BeamH(VSOut i) : SV_Target
{
    int y = int(i.pos.y);
    float x = i.pos.x;
    int jc = int(floor((x - srcOffX) / pitchX));
    int R = int(radiusH);
    int w = int(srcSize.x);
    float3 acc = 0;
    [loop] for (int j = max(jc - R, 0); j <= min(jc + R, w - 1); j++)
    {
        float3 c = max(T0.Load(int3(j, y, 0)).rgb, 0);
        float3 sc = SpotSigma(c) * spotAspect;
        // Analog video bandwidth: extra horizontal softness, same for every
        // brightness, added to the spot (and the anti-moire floor) in quadrature.
        float extra = aaX * aaX + veil0.z * veil0.z;
        if (extra > 0)
            sc = sqrt(sc * sc + extra);
        float3 d = x + subX.xyz - (srcOffX + (j + 0.5) * pitchX);
        acc += c * BoxGauss(d, 0.5 * pitchX, sc);
    }
    return float4(acc, 1);
}

// Vertical: each scanline is a gaussian ribbon. Energy per line = value * pitchY,
// so average brightness is kept; thin bright lines peak above 1 (HDR headroom).
float4 PS_BeamV(VSOut i) : SV_Target
{
    int x = int(i.pos.x);
    float yc = i.pos.y;
    int kc = int(floor((yc - srcOffY) / pitchY));
    int R = int(radiusV);
    int n = int(srcSize.y);
    float3 acc = 0;
    [loop] for (int k = max(kc - R, 0); k <= min(kc + R, n - 1); k++)
    {
        float3 c = max(T0.Load(int3(x, k, 0)).rgb, 0);
        float3 sc = SpotSigma(c);
        if (aaY > 0)
            sc = sqrt(sc * sc + aaY * aaY);
        float3 d = yc + subY.xyz - (srcOffY + (k + 0.5) * pitchY);
        acc += c * BoxGauss(d, 0.5, sc) * pitchY;
    }
    return float4(acc, 1);
}

// 13-tap downsample (energy preserving).
float4 PS_Down(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float2 t = srcTexel;
    float3 a = T0.Sample(Lin, uv + t * float2(-2, -2)).rgb;
    float3 b = T0.Sample(Lin, uv + t * float2( 0, -2)).rgb;
    float3 c = T0.Sample(Lin, uv + t * float2( 2, -2)).rgb;
    float3 d = T0.Sample(Lin, uv + t * float2(-1, -1)).rgb;
    float3 e = T0.Sample(Lin, uv + t * float2( 1, -1)).rgb;
    float3 f = T0.Sample(Lin, uv + t * float2(-2,  0)).rgb;
    float3 g = T0.Sample(Lin, uv).rgb;
    float3 h = T0.Sample(Lin, uv + t * float2( 2,  0)).rgb;
    float3 j = T0.Sample(Lin, uv + t * float2(-1,  1)).rgb;
    float3 k = T0.Sample(Lin, uv + t * float2( 1,  1)).rgb;
    float3 l = T0.Sample(Lin, uv + t * float2(-2,  2)).rgb;
    float3 m = T0.Sample(Lin, uv + t * float2( 0,  2)).rgb;
    float3 n = T0.Sample(Lin, uv + t * float2( 2,  2)).rgb;
    float3 r = (d + e + j + k) * 0.125;
    r += (a + b + f + g) * 0.03125;
    r += (b + c + g + h) * 0.03125;
    r += (f + g + l + m) * 0.03125;
    r += (g + h + m + n) * 0.03125;
    return float4(r, 1);
}

// Faceplate halation: ring kernel, each ring sampled from a pyramid level
// blurred enough for its angular tap spacing. T0..T2 = levels 4..6.
float4 PS_Halo(VSOut i) : SV_Target
{
    float2 fullTexel = 1.0 / float2(fullW, fullH);
    int n = int(haloRings);
    float3 acc = 0;
    [loop] for (int r = 0; r < n; r++)
    {
        float4 h = halo[r];
        float3 ring = 0;
        [loop] for (int a = 0; a < 24; a++)
        {
            float ang = h.w + a * 0.26179939;
            float2 uv = i.uv + float2(cos(ang), sin(ang)) * h.x * fullTexel;
            if (h.z < 0.5)
                ring += T0.SampleLevel(LinBlk, uv, 0).rgb;
            else if (h.z < 1.5)
                ring += T1.SampleLevel(LinBlk, uv, 0).rgb;
            else
                ring += T2.SampleLevel(LinBlk, uv, 0).rgb;
        }
        acc += ring * h.y;
    }
    return float4(acc, 1);
}

// Aperture grille: vertical RGB phosphor stripes, mean 1.
float3 Grille(float x)
{
    float ph = x / maskPitch - 1.0 / 6.0;
    float3 m = 0.5 + 0.5 * cos(6.2831853 * (ph - float3(0, 1, 2) / 3.0));
    return m * 2.0;
}

// SMPTE ST 2084 (PQ): the curve Windows encodes HDR output with.
static const float kPqM1 = 0.1593017578125;
static const float kPqM2 = 78.84375;
static const float kPqC1 = 0.8359375;
static const float kPqC2 = 18.8515625;
static const float kPqC3 = 18.6875;

float3 NitsToPq(float3 nits)
{
    float3 y = pow(saturate(nits / 10000.0), kPqM1);
    return pow((kPqC1 + kPqC2 * y) / (1.0 + kPqC3 * y), kPqM2);
}

float3 PqToNits(float3 pq)
{
    float3 p = pow(saturate(pq), 1.0 / kPqM2);
    return pow(max(p - kPqC1, 0.0) / (kPqC2 - kPqC3 * p), 1.0 / kPqM1) * 10000.0;
}

// Triangular dither: the analog tube had no banding, neither should we.
float4 Encode(float3 c, float2 p)
{
    float n = Hash(p, 0.0) - Hash(p, 1.0);
    if (outputMode > 1.5)
    {
        // Windows quantises scRGB to 10-bit PQ for the panel: dither one PQ
        // code step so dark gradients stay smooth after the tube curve. The
        // step size in nits is a fit to the exact PQ curve (within ~10% from
        // 0.1 to 1000 nits), far cheaper than converting to PQ and back.
        // Sign kept: scRGB goes negative for phosphor colours beyond sRGB.
        c = min(c, peak);
        float3 nits = abs(c) * sdrWhiteScale * 80.0;
        float3 step = nits * (0.0085 + 0.011 * pow(max(nits, 1e-4), -0.35));
        // Fade out at true black so pixels that should be off stay off.
        nits = max(nits + n * step * saturate(nits / 0.005), 0.0);
        return float4(sign(c) * nits / 80.0, 1);
    }
    float3 v = outputMode < 0.5 ? LinearToSrgb(saturate(c)) : pow(saturate(c), 1.0 / 2.2);
    v += n / (exp2(outputBits) - 1.0);
    return float4(v, 1);
}

// T0 beam core, T1/T2 diffusion levels, T3 halation, T4 local average (APL),
// T5 raw source, T6/T7 spot skirt levels
float4 PS_Final(VSOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);
    float3 beam = T0.Load(int3(p, 0)).rgb;
    // Fine glow levels are already smooth: plain bilinear is enough there.
    float3 skirt = lerp(T6.Sample(Lin, i.uv).rgb, T7.Sample(Lin, i.uv).rgb, tailMix);
    beam = beam * (1.0 - spotTail) + skirt * spotTail;
    float3 nearGlow = lerp(T1.Sample(Lin, i.uv).rgb, T2.Sample(Lin, i.uv).rgb, nearMix);
    float3 haloGlow = Tent(T3, i.uv);
    // Glass veiling glow: light scattered very widely in the faceplate. A soft
    // atmospheric haze around bright areas; energy neutral like the others.
    float veil = veil0.x;
    float3 veilGlow = veil > 0 ? lerp(Tent(T9, i.uv), Tent(T10, i.uv), veil0.y) : 0;
    float3 col = beam * (1.0 - diffusion - halation - veil) + nearGlow * diffusion + haloGlow * halation +
                 veilGlow * veil;

    // CRTs drive small bright areas far above their full-field white (ABL only
    // limits large areas). Keep that pop, in HDR only; based on a wide local
    // average so it never pumps with content changes elsewhere on screen.
    if (outputMode > 1.5 && highlightBoost > 1.0)
    {
        float apl = dot(Tent(T4, i.uv), kLuma);
        col *= lerp(highlightBoost, 1.0, saturate(apl / aplKnee));
    }

    if (maskType > 0.5)
        col *= lerp(1.0.xxx, Grille(i.pos.x), maskStrength);

    col *= brightness;

    if (split > 0)
    {
        float sx = split * dstSize.x;
        if (i.pos.x < sx)
            col = max(DecodeRaw(T5.Load(int3(p, 0)).rgb), 0);
        if (abs(i.pos.x - sx) < 1.0)
            col = 0.25;
    }

    // On-screen text sits on top of the tube, untouched by it. Unbound = zero.
    float2 osd = T8.Load(int3(p, 0)).rg;
    col = lerp(col * (1.0 - 0.75 * osd.g), 1.0.xxx, osd.r);
    // Linear output for a later pass to encode: outputMode 3 = rolling-scan
    // field (PS_Scan), deblur0.w = motion pre-deblur (PS_Deblur).
    if (outputMode > 2.5 || deblur0.w > 0.5)
        return float4(col, 1);
    return Encode(col, i.pos.xy);
}

// ---------------------------------------------------------------- rolling scan
// Each CRT field lasts K panel refreshes. The beam reaches a row at time
// a = row position * K (in refreshes). From then the phosphor emits the
// row's whole field energy (value * K) at up to `peak` per refresh, so bright
// pixels burn longer (as on a real tube), then fades with the afterglow time
// constant. A refresh shows the light emitted inside its own time window, so
// light averaged over the field equals the input exactly.

// Integral of (1 - exp(-t / tau)) from 0 to x: cumulative light of a burst
// that starts at 0, after phosphor rise/decay smoothing.
float3 Afterglow(float3 x)
{
    x = max(x, 0.0);
    float tau = motion0.w;
    if (tau < 1e-3)
        return x;
    return x - tau * (1.0 - exp(-x / tau));
}

// Light in window [s, s + 1) from a burst of value c starting at a.
float3 Emit(float3 c, float a, float s)
{
    float k = motion0.x, peakRate = motion0.y;
    float3 len = min(max(c, 0.0) * k * motion0.z / peakRate, k);
    return peakRate * (Afterglow(s + 1.0 - a) - Afterglow(s - a) - Afterglow(s + 1.0 - a - len) +
                       Afterglow(s - a - len));
}

// T0 = current field (linear), T1 = previous field, T8 = on-screen text.
float4 PS_Scan(VSOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);
    float3 cur = T0.Load(int3(p, 0)).rgb;
    float2 osd = T8.Load(int3(p, 0)).rg;
    // On-screen text and the raw half of the split stay steady.
    bool hold = osd.g > 0.0 || (split > 0 && i.pos.x < split * dstSize.x + 1.0);
    float3 col = cur;
    if (!hold && motion1.z > 0.5)
    {
        // Motion-adaptive flash: everything that is changing flashes in the
        // same refresh (no slices, so no slice-boundary bands); static areas,
        // per the change mask (T2), stay steadily lit and never flicker.
        float m = T2.SampleLevel(Lin, i.uv, 0).r;
        if (m > 0.0)
        {
            float k = motion0.x, s = motion1.x;
            float3 prev = T1.Load(int3(p, 0)).rgb;
            float3 flash = Emit(cur, 0.0, s) + Emit(prev, -k, s);
            col = lerp(cur, flash, m);
        }
    }
    else if (!hold)
    {
        float k = motion0.x, s = motion1.x;
        float a = i.pos.y / dstSize.y * k; // scan runs top to bottom
        float3 prev = T1.Load(int3(p, 0)).rgb;
        // Soft slice edges: each row's flash time is spread over a window of
        // `softness` refreshes, so rows near a slice boundary share their light
        // with the neighbouring refresh instead of switching abruptly (the
        // abrupt switch shows as dark bands when the eye moves).
        float soft = motion1.y;
        col = 0;
        [unroll] for (int t = 0; t < 4; t++)
        {
            float o = (t + 0.5) / 4.0 - 0.5;
            float at = a + o * soft;
            col += Emit(cur, at, s) + Emit(prev, at - k, s);
        }
        col *= 0.25;
    }
    return Encode(col, i.pos.xy);
}

// ---------------------------------------------------------------- motion pre-deblur
// On a sample-and-hold panel the eye, tracking a moving object, smears each
// held frame along the motion by exactly the distance the object moves per
// content frame (a box blur). Measure that motion, then pre-sharpen along it
// so the eye's smear lands closer to the sharp image. Overshoots above white
// use the HDR headroom.

// Perceptual luma of the raw desktop, at half resolution (2x2 average).
float4 PS_Luma(VSOut i) : SV_Target
{
    float3 c = max(DecodeRaw(T0.Sample(Lin, i.uv).rgb), 0.0);
    return float4(pow(saturate(dot(c, kLuma)), 1.0 / 2.2), 0, 0, 1);
}

float4 PS_LumaDown(VSOut i) : SV_Target
{
    return float4(T0.Sample(Lin, i.uv).r, 0, 0, 1);
}

// Mean absolute difference between a block of the current frame and the
// previous frame displaced by d (cur(x) ~ prev(x - d)), sampled on an n x n
// grid spread over a b x b footprint.
float BlockSad(int2 bo, int2 d, int b, int n, int2 lim)
{
    int step = max(b / n, 1);
    float s = 0;
    [loop] for (int y = 0; y < n; y++)
        [loop] for (int x = 0; x < n; x++)
        {
            int2 q = bo + int2(x, y) * step;
            float a = T0.Load(int3(clamp(q, 0, lim), 0)).r;
            float p = T1.Load(int3(clamp(q - d, 0, lim), 0)).r;
            s += abs(a - p);
        }
    return s / float(n * n);
}

// Search a window around the coarser level's vector. Scores add a small cost
// per pixel of displacement: among equally good matches the shortest vector
// wins, which on a straight edge is the motion across it (the only part that
// smears it), and "no motion" wins unless motion clearly matches better.
// passParams: x = block footprint, y = search radius, z = full-res px per
// level px, w = samples per side (+100 = sub-pixel refinement).
// T2 = coarser field (unbound on the coarsest level).
float4 PS_MotionSearch(VSOut i) : SV_Target
{
    int b = int(passParams.x), r = int(passParams.y);
    float scale = passParams.z;
    int n = int(passParams.w) % 100;
    bool subpixel = passParams.w >= 100.0;
    uint lw, lh;
    T0.GetDimensions(lw, lh);
    int2 lim = int2(lw, lh) - 1;
    int2 bo = int2(floor(i.pos.xy)) * b;

    int2 guess = 0;
    uint gw, gh;
    T2.GetDimensions(gw, gh);
    if (gw > 0)
    {
        int2 gi = int2(floor(i.pos.xy * float2(gw, gh) / dstSize));
        guess = int2(round(T2.Load(int3(gi, 0)).xy / scale));
    }

    const float kStill = 0.002, kPerPx = 0.00005;
    float sad0 = BlockSad(bo, int2(0, 0), b, n, lim);
    float bestScore = sad0, best = sad0;
    int2 bd = 0;
    [loop] for (int dy = -r; dy <= r; dy++)
        [loop] for (int dx = -r; dx <= r; dx++)
        {
            int2 d = guess + int2(dx, dy);
            if (all(d == 0))
                continue;
            float s = BlockSad(bo, d, b, n, lim);
            float score = s + kStill + kPerPx * length(float2(d) * scale);
            if (score < bestScore)
            {
                bestScore = score;
                best = s;
                bd = d;
            }
        }

    float2 v = float2(bd);
    if (subpixel && any(bd != 0))
    {
        // Parabola through the neighbours for sub-pixel precision.
        float sx0 = BlockSad(bo, bd - int2(1, 0), b, n, lim), sx1 = BlockSad(bo, bd + int2(1, 0), b, n, lim);
        float sy0 = BlockSad(bo, bd - int2(0, 1), b, n, lim), sy1 = BlockSad(bo, bd + int2(0, 1), b, n, lim);
        float denx = sx0 - 2.0 * best + sx1, deny = sy0 - 2.0 * best + sy1;
        if (denx > 1e-5)
            v.x += clamp(0.5 * (sx0 - sx1) / denx, -0.5, 0.5);
        if (deny > 1e-5)
            v.y += clamp(0.5 * (sy0 - sy1) / deny, -0.5, 0.5);
    }
    // Confidence: how much better the motion explains the block than "still".
    float conf = any(bd != 0) ? saturate((sad0 - best) / max(sad0, 1e-4) * 2.0) : 0.0;
    return float4(v * scale, conf, best);
}

// Colour wheel for the motion debug view: hue = direction, strength = speed.
float3 MotionColour(float2 v)
{
    float a = atan2(v.y, v.x) / 6.2831853 + 0.5;
    float3 h = saturate(abs(frac(a + float3(0, 2, 1) / 3.0) * 6.0 - 3.0) - 1.0);
    return h;
}

// T0 = composed linear image, T1 = motion field (full-res px per content frame,
// z = confidence), T8 = on-screen text (left untouched).
float4 PS_Deblur(VSOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);
    float3 c = T0.Load(int3(p, 0)).rgb;
    float2 osd = T8.Load(int3(p, 0)).rg;
    float4 m = T1.Sample(Lin, i.uv);
    float2 v = m.xy;
    float len = length(v);
    bool raw = split > 0 && i.pos.x < split * dstSize.x + 1.0;
    float3 col = c;
    if (len > 0.5 && osd.g <= 0.0 && !raw)
    {
        len = min(len, deblur0.z);
        float2 u = v / length(v) * dstTexel;
        int n = clamp(int(ceil(len)), 2, 32);
        float3 smear = 0;
        [loop] for (int k = 0; k < n; k++)
        {
            float t = ((k + 0.5) / n - 0.5) * len;
            smear += T0.SampleLevel(Lin, i.uv + u * t, 0).rgb;
        }
        smear /= n;
        // First-order inverse of the eye's box smear: c + (c - smear).
        col = clamp(c + deblur0.x * m.z * (c - smear), 0.0, peak);
    }
    if (deblur0.y > 0.5 && len > 0.5)
        col = lerp(col, MotionColour(v), 0.45 * saturate(len / 16.0));
    return Encode(col, i.pos.xy);
}

// Plain copy (debug readback).
float4 PS_Copy(VSOut i) : SV_Target
{
    return T0.Load(int3(i.pos.xy, 0));
}

// Change mask for the motion-adaptive flash, at 1/8 resolution. T0/T1 = new and
// previous luma (equal when nothing new arrived), T2 = previous mask.
// A region counts as moving when it (or a neighbour, so borders get margin)
// changed by more than passParams.x; the mask then decays by passParams.y per
// field instead of switching off at once, so regions don't pop.
float4 PS_ChangeMask(VSOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);
    uint lw, lh;
    T0.GetDimensions(lw, lh);
    int2 lim = int2(lw, lh) - 1;
    float change = 0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++)
        {
            int2 q = clamp(p + int2(x, y), 0, lim);
            change = max(change, abs(T0.Load(int3(q, 0)).r - T1.Load(int3(q, 0)).r));
        }
    float now = smoothstep(passParams.x, passParams.x * 3.0, change);
    float before = T2.Load(int3(p, 0)).r * passParams.y;
    return float4(max(now, before), 0, 0, 1);
}

// Resolution probe: 32 evenly spaced rows (v < 32) and 32 columns (v >= 32,
// stored transposed) of the raw desktop, for the CPU
// resolution detector. T0 = raw desktop.
float4 PS_ProbeLines(VSOut i) : SV_Target
{
    uint w, h;
    T0.GetDimensions(w, h);
    int x = int(i.pos.x), v = int(i.pos.y);
    int2 q;
    if (v < 32)
        q = int2(x, int((v + 0.5) * h / 32.0));
    else
    {
        if (x >= int(h))
            return 0;
        q = int2(int((v - 32 + 0.5) * w / 32.0), x);
    }
    // The encoded green value: what scalers actually interpolate (keeps the
    // detector's model linear). HDR desktops are re-encoded from scRGB.
    float3 raw = T0.Load(int3(q, 0)).rgb;
    float g = inputMode < 0.5 ? raw.g : LinearToSrgb(max(raw / sdrWhiteScale, 0.0)).g;
    return float4(g, 0, 0, 1);
}

// ---------------------------------------------------------------- fused beam
// Native resolution (one panel row per scanline) is the common case: decode,
// horizontal beam and vertical beam run in one compute pass. Each 16x16 tile
// loads its pixels (plus an apron) once into on-chip memory, so the two
// full-screen intermediates of the pixel-shader path are never written.
// Same maths as PS_Decode + PS_BeamH + PS_BeamV with pitch 1 and no offset.
#ifdef PHOSPHOR_CS
#define TILE 16 // tile width
#ifndef TILEY
#define TILEY 16
#endif
#ifndef MAXR
#define MAXR 3 // gather radius the apron covers (set per variant by the renderer)
#endif
#define APRON (TILE + 2 * MAXR)
#define APRONY (TILEY + 2 * MAXR)
groupshared float3 sDec[APRONY][APRON];
groupshared float3 sDecK[APRONY][APRON]; // 1 / horizontal spot sigma, per source pixel
groupshared float3 sH[APRONY][TILE];
groupshared float3 sHK[APRONY][TILE];    // 1 / vertical spot sigma, per scanline sample
RWTexture2D<float4> OutBeam : register(u0);

[numthreads(TILE, TILEY, 1)]
void CS_BeamNative(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    int2 origin = int2(gid.xy) * int2(TILE, TILEY);
    int2 base = origin - MAXR;
    int2 size = int2(fullW, fullH);
    int lane = int(tid.y) * TILE + int(tid.x);
    float extra = veil0.z * veil0.z; // analog softness, horizontal only

    // Decode the tile + apron once; each pixel's spot width is computed once too.
    [loop] for (int i = lane; i < APRONY * APRON; i += TILE * TILEY)
    {
        int2 q = base + int2(i % APRON, i / APRON);
        float3 c = 0;
        if (all(q >= 0) && all(q < size))
            c = max(DecodePixel(T0.Load(int3(q, 0)).rgb), 0);
        float3 sc = SpotSigma(c) * spotAspect;
        sDec[i / APRON][i % APRON] = c;
        sDecK[i / APRON][i % APRON] = rsqrt(sc * sc + extra);
    }
    GroupMemoryBarrierWithGroupSync();

    int rh = int(radiusH), rv = int(radiusV);
    [loop] for (int k = lane; k < APRONY * TILE; k += TILE * TILEY)
    {
        int row = k / TILE, col = k % TILE;
        float3 acc = 0;
        [loop] for (int dx = -rh; dx <= rh; dx++)
            acc += sDec[row][col + MAXR + dx] * BoxGaussK(subX.xyz - dx, 0.5, sDecK[row][col + MAXR + dx]);
        sH[row][col] = acc;
        sHK[row][col] = 1.0 / SpotSigma(acc);
    }
    GroupMemoryBarrierWithGroupSync();

    int2 o = origin + int2(tid.xy);
    if (any(o >= size))
        return;
    float3 acc = 0;
    [loop] for (int dy = -rv; dy <= rv; dy++)
    {
        int r = int(tid.y) + MAXR + dy;
        acc += sH[r][tid.x] * BoxGaussK(subY.xyz - dy, 0.5, sHK[r][tid.x]);
    }
    OutBeam[o] = float4(acc, 1);
}
#endif // PHOSPHOR_CS
