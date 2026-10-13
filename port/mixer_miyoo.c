/*
 * mixer_miyoo.c -- the N64 audio microcode (RSP "ABI") commands that
 * src/audio/synthesis.c issues, run in C on the CPU (Miyoo Mini).
 *
 * The Dreamcast build mixed the voices on its AICA sound chip and dropped the
 * software path. This is the portable mixer of the PC ports (sm64-port's
 * mixer.c, integer arithmetic as on the RSP), adapted to this tree:
 *   - the ADPCM codebooks and loop states are still big-endian in memory
 *     (load.c does not swap them), so they are swapped here, as the DC's
 *     software mixer did;
 *   - the reverb (wet) sends are left out, as on the DC (synthesis.c does not
 *     mix the reverb back in either).
 */
#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON) && !defined(MIXER_SCALAR)
#include <arm_neon.h>
#define MIXER_NEON 1   /* ADPCM, resampler and envelope mixer 8 samples at a time (see below) */
#else
#define MIXER_NEON 0
#endif

#include <PR/ultratypes.h>
#include "audio/mixer.h"

#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~0xf)

#define DMEM_BUF_SIZE 0x17D0
#define BUF_U8(a) (rspa.buf.as_u8 + (a))
#define BUF_S16(a) (rspa.buf.as_s16 + (a) / sizeof(int16_t))

static struct {
    uint16_t in;
    uint16_t out;
    uint16_t nbytes;

    uint16_t vol[2];
    uint16_t rate[2];

    int16_t adpcm_loop_state[16];
    int16_t adpcm_table[8][2][8];

    union {
        int16_t as_s16[DMEM_BUF_SIZE / sizeof(int16_t)];
        uint8_t as_u8[DMEM_BUF_SIZE];
    } buf __attribute__((aligned(16)));
} rspa;

static const int16_t resample_table[64][4] = {
    { 0x0c39, 0x66ad, 0x0d46, 0xffdf }, { 0x0b39, 0x6696, 0x0e5f, 0xffd8 }, { 0x0a44, 0x6669, 0x0f83, 0xffd0 },
    { 0x095a, 0x6626, 0x10b4, 0xffc8 }, { 0x087d, 0x65cd, 0x11f0, 0xffbf }, { 0x07ab, 0x655e, 0x1338, 0xffb6 },
    { 0x06e4, 0x64d9, 0x148c, 0xffac }, { 0x0628, 0x643f, 0x15eb, 0xffa1 }, { 0x0577, 0x638f, 0x1756, 0xff96 },
    { 0x04d1, 0x62cb, 0x18cb, 0xff8a }, { 0x0435, 0x61f3, 0x1a4c, 0xff7e }, { 0x03a4, 0x6106, 0x1bd7, 0xff71 },
    { 0x031c, 0x6007, 0x1d6c, 0xff64 }, { 0x029f, 0x5ef5, 0x1f0b, 0xff56 }, { 0x022a, 0x5dd0, 0x20b3, 0xff48 },
    { 0x01be, 0x5c9a, 0x2264, 0xff3a }, { 0x015b, 0x5b53, 0x241e, 0xff2c }, { 0x0101, 0x59fc, 0x25e0, 0xff1e },
    { 0x00ae, 0x5896, 0x27a9, 0xff10 }, { 0x0063, 0x5720, 0x297a, 0xff02 }, { 0x001f, 0x559d, 0x2b50, 0xfef4 },
    { 0xffe2, 0x540d, 0x2d2c, 0xfee8 }, { 0xffac, 0x5270, 0x2f0d, 0xfedb }, { 0xff7c, 0x50c7, 0x30f3, 0xfed0 },
    { 0xff53, 0x4f14, 0x32dc, 0xfec6 }, { 0xff2e, 0x4d57, 0x34c8, 0xfebd }, { 0xff0f, 0x4b91, 0x36b6, 0xfeb6 },
    { 0xfef5, 0x49c2, 0x38a5, 0xfeb0 }, { 0xfedf, 0x47ed, 0x3a95, 0xfeac }, { 0xfece, 0x4611, 0x3c85, 0xfeab },
    { 0xfec0, 0x4430, 0x3e74, 0xfeac }, { 0xfeb6, 0x424a, 0x4060, 0xfeaf }, { 0xfeaf, 0x4060, 0x424a, 0xfeb6 },
    { 0xfeac, 0x3e74, 0x4430, 0xfec0 }, { 0xfeab, 0x3c85, 0x4611, 0xfece }, { 0xfeac, 0x3a95, 0x47ed, 0xfedf },
    { 0xfeb0, 0x38a5, 0x49c2, 0xfef5 }, { 0xfeb6, 0x36b6, 0x4b91, 0xff0f }, { 0xfebd, 0x34c8, 0x4d57, 0xff2e },
    { 0xfec6, 0x32dc, 0x4f14, 0xff53 }, { 0xfed0, 0x30f3, 0x50c7, 0xff7c }, { 0xfedb, 0x2f0d, 0x5270, 0xffac },
    { 0xfee8, 0x2d2c, 0x540d, 0xffe2 }, { 0xfef4, 0x2b50, 0x559d, 0x001f }, { 0xff02, 0x297a, 0x5720, 0x0063 },
    { 0xff10, 0x27a9, 0x5896, 0x00ae }, { 0xff1e, 0x25e0, 0x59fc, 0x0101 }, { 0xff2c, 0x241e, 0x5b53, 0x015b },
    { 0xff3a, 0x2264, 0x5c9a, 0x01be }, { 0xff48, 0x20b3, 0x5dd0, 0x022a }, { 0xff56, 0x1f0b, 0x5ef5, 0x029f },
    { 0xff64, 0x1d6c, 0x6007, 0x031c }, { 0xff71, 0x1bd7, 0x6106, 0x03a4 }, { 0xff7e, 0x1a4c, 0x61f3, 0x0435 },
    { 0xff8a, 0x18cb, 0x62cb, 0x04d1 }, { 0xff96, 0x1756, 0x638f, 0x0577 }, { 0xffa1, 0x15eb, 0x643f, 0x0628 },
    { 0xffac, 0x148c, 0x64d9, 0x06e4 }, { 0xffb6, 0x1338, 0x655e, 0x07ab }, { 0xffbf, 0x11f0, 0x65cd, 0x087d },
    { 0xffc8, 0x10b4, 0x6626, 0x095a }, { 0xffd0, 0x0f83, 0x6669, 0x0a44 }, { 0xffd8, 0x0e5f, 0x6696, 0x0b39 },
    { 0xffdf, 0x0d46, 0x66ad, 0x0c39 }
};

static inline int16_t clamp16(int32_t v) {
    if (v < -0x8000)
        return -0x8000;
    if (v > 0x7fff)
        return 0x7fff;
    return (int16_t) v;
}

void aClearBufferImpl(uint16_t addr, int nbytes) {
    memset(BUF_U8(addr), 0, ROUND_UP_16(nbytes));
}

void aLoadBufferImpl(const void *source_addr, uint16_t dest_addr, uint16_t nbytes) {
    memcpy(BUF_U8(dest_addr), source_addr, ROUND_DOWN_16(nbytes));
}

void aSaveBufferImpl(uint16_t source_addr, int16_t *dest_addr, uint16_t nbytes) {
    memcpy(dest_addr, BUF_S16(source_addr), ROUND_DOWN_16(nbytes));
}

/* the codebook is big-endian in memory */
void aLoadADPCMImpl(int num_entries_times_16, const int16_t *book_source_addr) {
    int16_t *t = &rspa.adpcm_table[0][0][0];
    int n = num_entries_times_16 / 2;
    if (n > 8 * 2 * 8)
        n = 8 * 2 * 8;
    for (int i = 0; i < n; i++)
        t[i] = (int16_t) __builtin_bswap16((uint16_t) book_source_addr[i]);
}

void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    (void) flags;
    rspa.in = in;
    rspa.out = out;
    rspa.nbytes = nbytes;
}

void aInterleaveImpl(uint16_t left, uint16_t right) {
    int count = ROUND_UP_16(rspa.nbytes) / sizeof(int16_t);
    const int16_t *l = BUF_S16(left);
    const int16_t *r = BUF_S16(right);
    int16_t *d = BUF_S16(rspa.out);
    for (int i = 0; i < count; i++) {
        *d++ = l[i];
        *d++ = r[i];
    }
}

void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
    memmove(BUF_U8(out_addr), BUF_U8(in_addr), ROUND_UP_16(nbytes));
}

/* the loop state is big-endian in memory */
void aSetLoopImpl(ADPCM_STATE *adpcm_loop_state) {
    const int16_t *s = (const int16_t *) adpcm_loop_state;
    for (int i = 0; i < 16; i++)
        rspa.adpcm_loop_state[i] = (int16_t) __builtin_bswap16((uint16_t) s[i]);
}

void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) {
    const uint8_t *in = BUF_U8(rspa.in);
    int16_t *out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_32(rspa.nbytes);
    if (flags & A_INIT)
        memset(out, 0, 16 * sizeof(int16_t));
    else if (flags & A_LOOP)
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
    else
        memcpy(out, state, 16 * sizeof(int16_t));
    out += 16;

#if MIXER_NEON
    /* NEON (sm64-port's, as in mr-finster/miyoo64's MK64 port, MIT): the same integer
       arithmetic as the loop below, result for result (checked against it under qemu) */
    {
        static const int8_t pos0_data[16] = { -1, 0, -1, 0, -1, 1, -1, 1, -1, 2, -1, 2, -1, 3, -1, 3 };
        static const int8_t pos1_data[16] = { -1, 4, -1, 4, -1, 5, -1, 5, -1, 6, -1, 6, -1, 7, -1, 7 };
        static const int16_t mult_data[8] = { 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10 };
        static const int16_t table_prefix_data[8] = { 0, 0, 0, 0, 0, 0, 0, 1 << 11 };
        const int8x16_t pos0 = vld1q_s8(pos0_data);
        const int8x16_t pos1 = vld1q_s8(pos1_data);
        const int16x8_t mult = vld1q_s16(mult_data);
        const int16x8_t mask = vdupq_n_s16((int16_t) 0xf000);
        const int16x8_t table_prefix = vld1q_s16(table_prefix_data);
        int16x8_t result = vld1q_s16(out - 8);
        while (nbytes > 0) {
            const int shift = *in >> 4;
            const int16_t (*tbl)[8] = rspa.adpcm_table[*in++ & 7];
            const int8x8_t inv = vld1_s8((const int8_t *) in);
            const int16x8_t tblvec[2] = { vld1q_s16(tbl[0]), vld1q_s16(tbl[1]) };
            int16x8_t invec[2] = {
                vreinterpretq_s16_s8(vcombine_s8(vtbl1_s8(inv, vget_low_s8(pos0)), vtbl1_s8(inv, vget_high_s8(pos0)))),
                vreinterpretq_s16_s8(vcombine_s8(vtbl1_s8(inv, vget_low_s8(pos1)), vtbl1_s8(inv, vget_high_s8(pos1))))
            };
            const int16x8_t shiftcount = vdupq_n_s16((int16_t) (shift - 12));   /* (negative: right) */
            int16x8_t tv1[8];
            in += 8;
            tv1[0] = vextq_s16(table_prefix, tblvec[1], 7);
            tv1[1] = vextq_s16(table_prefix, tblvec[1], 6);
            tv1[2] = vextq_s16(table_prefix, tblvec[1], 5);
            tv1[3] = vextq_s16(table_prefix, tblvec[1], 4);
            tv1[4] = vextq_s16(table_prefix, tblvec[1], 3);
            tv1[5] = vextq_s16(table_prefix, tblvec[1], 2);
            tv1[6] = vextq_s16(table_prefix, tblvec[1], 1);
            tv1[7] = table_prefix;
            invec[0] = vshlq_s16(vandq_s16(vmulq_s16(invec[0], mult), mask), shiftcount);
            invec[1] = vshlq_s16(vandq_s16(vmulq_s16(invec[1], mult), mask), shiftcount);
            for (int i = 0; i < 2; i++) {
                int32x4_t acc0, acc1;
                acc1 = vmull_lane_s16(vget_high_s16(tblvec[0]), vget_high_s16(result), 2);
                acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec[1]), vget_high_s16(result), 3);
                acc0 = vmull_lane_s16(vget_low_s16(tblvec[0]), vget_high_s16(result), 2);
                acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec[1]), vget_high_s16(result), 3);
                acc0 = vmlal_lane_s16(acc0, vget_low_s16(tv1[0]), vget_low_s16(invec[i]), 0);
                acc0 = vmlal_lane_s16(acc0, vget_low_s16(tv1[1]), vget_low_s16(invec[i]), 1);
                acc0 = vmlal_lane_s16(acc0, vget_low_s16(tv1[2]), vget_low_s16(invec[i]), 2);
                acc0 = vmlal_lane_s16(acc0, vget_low_s16(tv1[3]), vget_low_s16(invec[i]), 3);
                acc1 = vmlal_lane_s16(acc1, vget_high_s16(tv1[0]), vget_low_s16(invec[i]), 0);
                acc1 = vmlal_lane_s16(acc1, vget_high_s16(tv1[1]), vget_low_s16(invec[i]), 1);
                acc1 = vmlal_lane_s16(acc1, vget_high_s16(tv1[2]), vget_low_s16(invec[i]), 2);
                acc1 = vmlal_lane_s16(acc1, vget_high_s16(tv1[3]), vget_low_s16(invec[i]), 3);
                acc1 = vmlal_lane_s16(acc1, vget_high_s16(tv1[4]), vget_high_s16(invec[i]), 0);
                acc1 = vmlal_lane_s16(acc1, vget_high_s16(tv1[5]), vget_high_s16(invec[i]), 1);
                acc1 = vmlal_lane_s16(acc1, vget_high_s16(tv1[6]), vget_high_s16(invec[i]), 2);
                acc1 = vmlal_lane_s16(acc1, vget_high_s16(tv1[7]), vget_high_s16(invec[i]), 3);
                result = vcombine_s16(vqshrn_n_s32(acc0, 11), vqshrn_n_s32(acc1, 11));
                vst1q_s16(out, result);
                out += 8;
            }
            nbytes -= 16 * sizeof(int16_t);
        }
    }
#else
    while (nbytes > 0) {
        const int shift = *in >> 4;          /* 0..12 */
        const int table_index = *in++ & 0xf; /* 0..7 */
        const int16_t (*tbl)[8] = rspa.adpcm_table[table_index & 7];
        for (int i = 0; i < 2; i++) {
            int16_t ins[8];
            const int16_t prev1 = out[-1];
            const int16_t prev2 = out[-2];
            for (int j = 0; j < 4; j++) {
                ins[j * 2] = (int16_t) ((((*in >> 4) << 28) >> 28) << shift);
                ins[j * 2 + 1] = (int16_t) ((((*in++ & 0xf) << 28) >> 28) << shift);
            }
            for (int j = 0; j < 8; j++) {
                int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);
                for (int k = 0; k < j; k++)
                    acc += tbl[1][(j - k) - 1] * ins[k];
                acc >>= 11;
                *out++ = clamp16(acc);
            }
        }
        nbytes -= 16 * sizeof(int16_t);
    }
#endif
    memcpy(state, out - 16, 16 * sizeof(int16_t));
}

void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) {
    int16_t tmp[32];
    int16_t *in_initial = BUF_S16(rspa.in);
    int16_t *in = in_initial;
    int16_t *out = BUF_S16(rspa.out);
    int nbytes = ROUND_UP_16(rspa.nbytes);
    uint32_t pitch_accumulator;
    int i;

    if (flags & A_INIT)
        memset(tmp, 0, 5 * sizeof(int16_t));
    else
        memcpy(tmp, state, 16 * sizeof(int16_t));
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
        in -= tmp[5] / (int) sizeof(int16_t);
    }
    in -= 4;
    pitch_accumulator = (uint16_t) tmp[4];
    memcpy(in, tmp, 4 * sizeof(int16_t));

#if MIXER_NEON
    /* NEON: 8 outputs a step, each tap (in * tbl + 0x4000) >> 15 and the four summed in
       32 bits with one clamp, as in the loop below (positions from a 32-bit accumulator:
       its high half is how far in has moved) */
    {
        static const uint16_t multiples_data[8] = { 0, 2, 4, 6, 8, 10, 12, 14 };
        const uint16x8_t multiples = vld1q_u16(multiples_data);
        const uint32x4_t step8 = vdupq_n_u32((uint32_t) (pitch << 1) * 8);
        const uint32x4_t acclo = vdupq_n_u32((uint16_t) pitch_accumulator);
        uint32x4_t acc_a = vmlal_n_u16(acclo, vget_low_u16(multiples), pitch);
        uint32x4_t acc_b = vmlal_n_u16(acclo, vget_high_u16(multiples), pitch);
        do {
            const uint16x8x2_t uz = vuzpq_u16(vreinterpretq_u16_u32(acc_a), vreinterpretq_u16_u32(acc_b));
            const uint16x8_t tp = vshrq_n_u16(uz.val[0], 10);
            const uint16x8_t ip = uz.val[1];
            int32x4_t sums[2];
            for (int k = 0; k < 4; k++) {
                int16x8_t t, x;
                switch (k) {   /* (lane numbers must be constants) */
                case 0:
                    t = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tp, 0)]), vld1_s16(resample_table[vgetq_lane_u16(tp, 1)]));
                    x = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(ip, 0)]), vld1_s16(&in[vgetq_lane_u16(ip, 1)]));
                    break;
                case 1:
                    t = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tp, 2)]), vld1_s16(resample_table[vgetq_lane_u16(tp, 3)]));
                    x = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(ip, 2)]), vld1_s16(&in[vgetq_lane_u16(ip, 3)]));
                    break;
                case 2:
                    t = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tp, 4)]), vld1_s16(resample_table[vgetq_lane_u16(tp, 5)]));
                    x = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(ip, 4)]), vld1_s16(&in[vgetq_lane_u16(ip, 5)]));
                    break;
                default:
                    t = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tp, 6)]), vld1_s16(resample_table[vgetq_lane_u16(tp, 7)]));
                    x = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(ip, 6)]), vld1_s16(&in[vgetq_lane_u16(ip, 7)]));
                    break;
                }
                const int32x4_t lo = vrshrq_n_s32(vmull_s16(vget_low_s16(x), vget_low_s16(t)), 15);
                const int32x4_t hi = vrshrq_n_s32(vmull_s16(vget_high_s16(x), vget_high_s16(t)), 15);
                const int32x2_t pair = vpadd_s32(vpadd_s32(vget_low_s32(lo), vget_high_s32(lo)),
                                                 vpadd_s32(vget_low_s32(hi), vget_high_s32(hi)));
                if (k & 1)
                    sums[k >> 1] = vcombine_s32(vget_low_s32(sums[k >> 1]), pair);
                else
                    sums[k >> 1] = vcombine_s32(pair, pair);
            }
            vst1q_s16(out, vcombine_s16(vqmovn_s32(sums[0]), vqmovn_s32(sums[1])));
            acc_a = vaddq_u32(acc_a, step8);
            acc_b = vaddq_u32(acc_b, step8);
            out += 8;
            nbytes -= 8 * sizeof(int16_t);
        } while (nbytes > 0);
        in += vgetq_lane_u16(vreinterpretq_u16_u32(acc_a), 1);
        pitch_accumulator = vgetq_lane_u16(vreinterpretq_u16_u32(acc_a), 0);
    }
#else
    do {
        for (i = 0; i < 8; i++) {
            const int16_t *tbl = resample_table[pitch_accumulator * 64 >> 16];
            const int32_t sample = ((in[0] * tbl[0] + 0x4000) >> 15) + ((in[1] * tbl[1] + 0x4000) >> 15) +
                                   ((in[2] * tbl[2] + 0x4000) >> 15) + ((in[3] * tbl[3] + 0x4000) >> 15);
            *out++ = clamp16(sample);
            pitch_accumulator += (uint32_t) (pitch << 1);
            in += pitch_accumulator >> 16;
            pitch_accumulator %= 0x10000;
        }
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
#endif

    state[4] = (int16_t) pitch_accumulator;
    memcpy(state, in, 4 * sizeof(int16_t));
    i = (int) (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0)
        i = -8 - i;
    state[5] = (int16_t) i;
    memcpy(state + 8, in, 8 * sizeof(int16_t));
}

void aEnvSetup1Impl(uint8_t initial_vol_wet, uint16_t rate_wet, uint16_t rate_left, uint16_t rate_right) {
    (void) initial_vol_wet;
    (void) rate_wet;
    rspa.rate[0] = rate_left;
    rspa.rate[1] = rate_right;
}

void aEnvSetup2Impl(uint16_t initial_vol_left, uint16_t initial_vol_right) {
    rspa.vol[0] = initial_vol_left;
    rspa.vol[1] = initial_vol_right;
}

/* dry mix only (no reverb send, as on the DC) */
void aEnvMixerImpl(uint16_t in_addr, uint16_t n_samples, int swap_reverb, int neg_left, int neg_right,
                   uint16_t dry_left_addr, uint16_t dry_right_addr, uint16_t wet_left_addr, uint16_t wet_right_addr) {
    (void) swap_reverb;
    (void) wet_left_addr;
    (void) wet_right_addr;
    const int16_t *in = BUF_S16(in_addr);
    int16_t *dl = BUF_S16(dry_left_addr), *dr = BUF_S16(dry_right_addr);
    const int16_t nl = neg_left ? -1 : 0, nr = neg_right ? -1 : 0;
    int n = ROUND_UP_16(n_samples);
    uint16_t vl = rspa.vol[0], vr = rspa.vol[1];
    const uint16_t rl = rspa.rate[0], rr = rspa.rate[1];
#if MIXER_NEON
    /* NEON, 8 samples a step (the volumes step once per 8 there too): the product's bits
       16..31, XOR the sign, then a saturating add. Buffers that overlap only partly stay
       on the loop below (sample order would matter there). */
    {
        const intptr_t a = (intptr_t) in, b = (intptr_t) dl, c = (intptr_t) dr, len = n * 2;
#define MX_PART(p, q) ((p) != (q) && (p) < (q) + len && (q) < (p) + len)
        if (!MX_PART(a, b) && !MX_PART(a, c) && !MX_PART(b, c)) {
            const int16x8_t vnl = vdupq_n_s16(nl), vnr = vdupq_n_s16(nr);
            do {
                const int16x8_t x = vld1q_s16(in);
                const int32x4_t xl = vmovl_s16(vget_low_s16(x)), xh = vmovl_s16(vget_high_s16(x));
                const int16x8_t l = veorq_s16(vcombine_s16(vshrn_n_s32(vmulq_n_s32(xl, vl), 16),
                                                           vshrn_n_s32(vmulq_n_s32(xh, vl), 16)), vnl);
                vst1q_s16(dl, vqaddq_s16(vld1q_s16(dl), l));
                const int16x8_t r = veorq_s16(vcombine_s16(vshrn_n_s32(vmulq_n_s32(xl, vr), 16),
                                                           vshrn_n_s32(vmulq_n_s32(xh, vr), 16)), vnr);
                vst1q_s16(dr, vqaddq_s16(vld1q_s16(dr), r));
                in += 8; dl += 8; dr += 8;
                vl += rl;
                vr += rr;
                n -= 8;
            } while (n > 0);
            return;
        }
#undef MX_PART
    }
#endif
    do {
        for (int i = 0; i < 8; i++) {
            const int32_t s = *in++;
            *dl = clamp16(*dl + (int16_t) (((s * vl) >> 16) ^ nl));
            dl++;
            *dr = clamp16(*dr + (int16_t) (((s * vr) >> 16) ^ nr));
            dr++;
        }
        vl += rl;
        vr += rr;
        n -= 8;
    } while (n > 0);
}

void aSMixImpl(uint16_t in_addr, uint16_t out_addr, uint16_t count) {
    (void) in_addr; (void) out_addr; (void) count;   /* (reverb only: not issued) */
}

void aMixImpl(int16_t gain, uint16_t in_addr, uint16_t out_addr, uint16_t count) {
    (void) gain; (void) in_addr; (void) out_addr; (void) count;   /* (reverb only: not issued) */
}

/* the first count bytes at in, copied t + 1 times one after another from out (a short
   synthetic wave looped to fill the samples a note needs) */
void aDMEMMove2Impl(uint8_t t, uint16_t in_addr, uint16_t out_addr, uint16_t count) {
    const uint8_t *in = BUF_U8(in_addr);
    uint8_t *out = BUF_U8(out_addr);
    const int nbytes = ROUND_UP_32(count);
    do {
        if (out + nbytes > rspa.buf.as_u8 + DMEM_BUF_SIZE)
            break;
        memmove(out, in, nbytes);
        out += nbytes;
    } while (t-- > 0);
}

/* every other sample */
void aDownsampleHalfImpl(uint16_t n_samples, uint16_t in_addr, uint16_t out_addr) {
    const int16_t *in = BUF_S16(in_addr);
    int16_t *out = BUF_S16(out_addr);
    int n = ROUND_UP_8(n_samples);
    do {
        *out++ = in[0];
        *out++ = in[2];
        *out++ = in[4];
        *out++ = in[6];
        *out++ = in[8];
        *out++ = in[10];
        *out++ = in[12];
        *out++ = in[14];
        in += 16;
        n -= 8;
    } while (n > 0);
}
