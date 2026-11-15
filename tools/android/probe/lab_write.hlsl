// The shader lab's small shaders: one suspect each (lab_shaders.py builds it with VARIANT 0-6).
struct CB {
    uint2 offset;
    uint2 resolution;
    uint fmt;
    uint siz;
    uint ditherPattern;
    uint ditherRandomSeed;
    uint usesHDR;
};

[[vk::push_constant]] ConstantBuffer<CB> gConstants : register(b0, space0);
RWBuffer<uint> gOutput : register(u1, space0);

[numthreads(8, 8, 1)]
void CSMain(uint2 coord : SV_DispatchThreadID) {
    uint v = gConstants.fmt;
#if VARIANT == 1
    v = ((v << 8) & 0xFF00) | ((v >> 8) & 0xFF);
#elif VARIANT == 2
    v = ((v & 0xFFU) * 256U) + ((v >> 8) & 0xFFU);
#elif VARIANT == 3
    v = ((v & 0xFFU) * 256U) + ((v / 256U) & 0xFFU);
#elif VARIANT == 4
    v = gConstants.usesHDR;
#elif VARIANT == 5
    v = v & 0xFF00;
#elif VARIANT == 6
    v = (v << 8) | (v >> 8);
#endif
    gOutput[coord.x] = v;
}
