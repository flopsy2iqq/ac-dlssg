// Depth blit for the capture slots (spec 6.5): reads the typed SRV of the
// game's depth (R32_FLOAT, R24_UNORM_X8_TYPELESS, R16_UNORM or
// R32_FLOAT_X8X24_TYPELESS; the red channel is the depth either way) and
// writes it to the slot's R32_FLOAT UAV. Both textures have the same size;
// the dispatch rounds up to 8x8 groups, so threads outside it return.
// No constant buffer and no sampler: CaptureSlots saves and restores only the
// compute shader, SRV slot 0 and UAV slot 0.
// Compiled at build time by fxc (cs_5_0) into a header; never at runtime.
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    dst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    dst[id.xy] = src.Load(int3(id.xy, 0));
}
