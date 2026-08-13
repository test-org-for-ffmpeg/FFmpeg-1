/*
 * H.26L/H.264/AVC/JVT/14496-10/... encoder/decoder
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * H.264 / AVC / MPEG-4 part10 codec.
 * non-SIMD x86-specific optimizations for H.264
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#include <stddef.h>

#include "libavcodec/cabac.h"
#include "cabac.h"

#if HAVE_INLINE_ASM

#if ARCH_X86_64
#define REG64 "r"
#else
#define REG64 "m"
#endif

//FIXME use some macros to avoid duplicating get_cabac (cannot be done yet
//as that would make optimization work hard)

/* NOTE (MV-only fork): the two upstream scanners below —
 * decode_significance_x86 and decode_significance_8x8_x86 — are dead in this
 * build. They write each significant coefficient's scan position into index[],
 * which only pixel reconstruction reads, so the *_skip clones further down are
 * dispatched instead (see h264_cabac.c decode_cabac_residual_internal_skip).
 * GCC warns "defined but not used" for both; that warning is expected. They are
 * kept so the diff against upstream stays readable. */
#if HAVE_7REGS && !BROKEN_COMPILER
#define decode_significance decode_significance_x86
static int decode_significance_x86(CABACContext *c, int max_coeff,
                                   uint8_t *significant_coeff_ctx_base,
                                   int *index, x86_reg last_off){
    void *end= significant_coeff_ctx_base + max_coeff - 1;
    int minusstart= -(intptr_t)significant_coeff_ctx_base;
    int minusindex= 4-(intptr_t)index;
    int bit;
    x86_reg coeff_count;

#ifdef BROKEN_RELOCATIONS
    void *tables;

    __asm__ volatile(
        "lea   "MANGLE(ff_h264_cabac_tables)", %0      \n\t"
        : "=&r"(tables)
        : NAMED_CONSTRAINTS_ARRAY(ff_h264_cabac_tables)
    );
#endif

    __asm__ volatile(
        "3:                                     \n\t"

        BRANCHLESS_GET_CABAC("%4", "%q4", "(%1)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c11(%6)", "%c12(%6)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%13")

        "test $1, %4                            \n\t"
        " jz 4f                                 \n\t"
        "add  %10, %1                           \n\t"

        BRANCHLESS_GET_CABAC("%4", "%q4", "(%1)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c11(%6)", "%c12(%6)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%13")

        "sub  %10, %1                           \n\t"
        "mov  %2, %0                            \n\t"
        "movl %7, %%ecx                         \n\t"
        "add  %1, %%"FF_REG_c"                  \n\t"
        "movl %%ecx, (%0)                       \n\t"

        "test $1, %4                            \n\t"
        " jnz 5f                                \n\t"

        "add"FF_OPSIZE"  $4, %2                 \n\t"

        "4:                                     \n\t"
        "add  $1, %1                            \n\t"
        "cmp  %8, %1                            \n\t"
        " jb 3b                                 \n\t"
        "mov  %2, %0                            \n\t"
        "movl %7, %%ecx                         \n\t"
        "add  %1, %%"FF_REG_c"                  \n\t"
        "movl %%ecx, (%0)                       \n\t"
        "5:                                     \n\t"
        "add  %9, %k0                           \n\t"
        "shr $2, %k0                            \n\t"
        : "=&q"(coeff_count), "+r"(significant_coeff_ctx_base), "+m"(index),
          "+&r"(c->low), "=&r"(bit), "+&r"(c->range)
        : "r"(c), "m"(minusstart), "m"(end), "m"(minusindex), "m"(last_off),
          "i"(offsetof(CABACContext, bytestream)),
          "i"(offsetof(CABACContext, bytestream_end))
          TABLES_ARG
        : "%"FF_REG_c, "memory"
    );
    return coeff_count;
}

#define decode_significance_8x8 decode_significance_8x8_x86
static int decode_significance_8x8_x86(CABACContext *c,
                                       uint8_t *significant_coeff_ctx_base,
                                       int *index, uint8_t *last_coeff_ctx_base, const uint8_t *sig_off){
    int minusindex= 4-(intptr_t)index;
    int bit;
    x86_reg coeff_count;
    x86_reg last=0;
    x86_reg state;

#ifdef BROKEN_RELOCATIONS
    void *tables;

    __asm__ volatile(
        "lea    "MANGLE(ff_h264_cabac_tables)", %0      \n\t"
        : "=&r"(tables)
        : NAMED_CONSTRAINTS_ARRAY(ff_h264_cabac_tables)
    );
#endif

    __asm__ volatile(
        "mov %1, %6                             \n\t"
        "3:                                     \n\t"

        "mov %10, %0                            \n\t"
        "movzb (%0, %6), %6                     \n\t"
        "add %9, %6                             \n\t"

        BRANCHLESS_GET_CABAC("%4", "%q4", "(%6)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c12(%7)", "%c13(%7)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%15")

        "mov %1, %6                             \n\t"
        "test $1, %4                            \n\t"
        " jz 4f                                 \n\t"

#ifdef BROKEN_RELOCATIONS
        "movzb %c14(%15, %q6), %6\n\t"
#else
        "movzb "MANGLE(ff_h264_cabac_tables)"+%c14(%6), %6\n\t"
#endif
        "add %11, %6                            \n\t"

        BRANCHLESS_GET_CABAC("%4", "%q4", "(%6)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c12(%7)", "%c13(%7)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%15")

        "mov %2, %0                             \n\t"
        "mov %1, %6                             \n\t"
        "mov %k6, (%0)                          \n\t"

        "test $1, %4                            \n\t"
        " jnz 5f                                \n\t"

        "add"FF_OPSIZE"  $4, %2                 \n\t"

        "4:                                     \n\t"
        "add $1, %6                             \n\t"
        "mov %6, %1                             \n\t"
        "cmp $63, %6                            \n\t"
        " jb 3b                                 \n\t"
        "mov %2, %0                             \n\t"
        "mov %k6, (%0)                          \n\t"
        "5:                                     \n\t"
        "addl %8, %k0                           \n\t"
        "shr $2, %k0                            \n\t"
        : "=&q"(coeff_count), "+"REG64(last), "+"REG64(index), "+&r"(c->low),
          "=&r"(bit), "+&r"(c->range), "=&r"(state)
        : "r"(c), "m"(minusindex), "m"(significant_coeff_ctx_base),
          REG64(sig_off), REG64(last_coeff_ctx_base),
          "i"(offsetof(CABACContext, bytestream)),
          "i"(offsetof(CABACContext, bytestream_end)),
          "i"(H264_LAST_COEFF_FLAG_OFFSET_8x8_OFFSET) TABLES_ARG
        : "%"FF_REG_c, "memory"
    );
    return coeff_count;
}

/* Custom (MV-only fork): 4x4 significance scan, count-only.
 *
 * H.264 codes a "significance map" per transform block: for each scan position,
 * one context bin saying *is there a coefficient here?*, and if yes a second
 * saying *is this the last one?*. Up to ~30 bins per 4x4 block.
 *
 * Upstream's decode_significance_x86() (above) additionally stores each
 * significant position into index[] so the caller can place decoded
 * coefficients into block[]. Nothing reads block[] in MV-only mode, so this
 * clone drops that entirely and keeps only the count the caller still needs to
 * know how many level values to consume. Concretely, versus upstream:
 *
 *   - No index[] stores. Upstream does an address reload, an add and a store
 *     per significant coefficient, plus an `add $4` to bump the write pointer.
 *     All of it is gone.
 *   - The counter is bound "+r", not "+m" — see the "counter++ (REGISTER!)"
 *     markers below. Across a whole block the coefficient count never touches
 *     memory.
 *   - The loop is laid out hot-path-first: `test`/`jnz 6f` falls through on
 *     *not significant* (the common case at surveillance bitrates) and jumps
 *     away to a cold block for significant positions.
 *
 * Only three things change across an entire block: the context base pointer,
 * the counter, and low/range. That register residency is the point.
 *
 * 46.7 s across the profiled corpus, 8-12% of a run. ASSEMBLY_ANALYSIS.md §4.3.
 * Must stay bit-exact: every bin consumed here advances the shared CABAC state.
 */
#define decode_significance_skip decode_significance_skip_x86
static av_always_inline int decode_significance_skip_x86(CABACContext *c, int max_coeff,
                                   uint8_t *significant_coeff_ctx_base,
                                   x86_reg last_off){
    void *end= significant_coeff_ctx_base + max_coeff - 1;
    int bit;
    x86_reg coeff_count;
    x86_reg counter = 0;

#ifdef BROKEN_RELOCATIONS
    void *tables;
    __asm__ volatile(
        "lea    " MANGLE(ff_h264_cabac_tables) ", %0      \n\t"
        : "=&r"(tables)
        : NAMED_CONSTRAINTS_ARRAY(ff_h264_cabac_tables)
    );
#endif

    __asm__ volatile(
        "3:                                     \n\t"

        /* Decode significance flag for current coefficient position */
        BRANCHLESS_GET_CABAC("%4", "%q4", "(%1)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c9(%6)", "%c10(%6)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%11")

        "test $1, %4                            \n\t"
        " jnz 6f                                \n\t"

        /* HOT PATH: not significant — fall through */
        "4:                                     \n\t"
        "add  $1, %1                            \n\t"
        "cmp  %7, %1                            \n\t"
        " jb 3b                                 \n\t"

        /* End: implicit significant at last position */
        "add  $1, %2                            \n\t"  /* counter++ (REGISTER!) */
        "jmp 5f                                 \n\t"

        /* COLD: significant coefficient */
        "6:                                     \n\t"
        "add  %8, %1                            \n\t"

        BRANCHLESS_GET_CABAC("%4", "%q4", "(%1)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c9(%6)", "%c10(%6)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%11")

        "add  $1, %2                            \n\t"  /* counter++ (REGISTER!) */

        "test $1, %4                            \n\t"
        " jnz 5f                                \n\t"  /* exit: %1 dead, no restore */

        "sub  %8, %1                            \n\t"  /* continue: restore base */
        "jmp 4b                                 \n\t"

        "5:                                     \n\t"
        "mov  %k2, %k0                           \n\t"
 
        : "=&q"(coeff_count), "+r"(significant_coeff_ctx_base),
          "+r"(counter),                                /* "+r" not "+m"! */
          "+&r"(c->low), "=&r"(bit), "+&r"(c->range)
        : "r"(c), "r"(end), "r"(last_off),
          "i"(offsetof(CABACContext, bytestream)),
          "i"(offsetof(CABACContext, bytestream_end))
          TABLES_ARG
        : "%"FF_REG_c, "memory"
    );
    return coeff_count;
}

/* Custom (MV-only fork): 8x8 significance scan, count-only. Same job and same
 * two modifications as decode_significance_skip_x86 above (no index[] stores,
 * register-resident counter), over 64 scan positions instead of 16.
 *
 * The extra complication: for 4x4 blocks the context index *is* the scan
 * position, but for 8x8 the standard maps 64 positions onto a much smaller
 * context set, so each iteration does two table indirections the 4x4 version
 * does not — sig_off[position] for the significance bin, and the 8x8 last-flag
 * mapping for the last bin. The latter lives at H264_LAST_COEFF_FLAG_OFFSET_8x8
 * inside the SAME ff_h264_cabac_tables blob already held in a register, so it
 * costs one movzb rather than a separate table pointer.
 *
 * Register pressure is at the limit of what GCC can allocate here (HAVE_7REGS):
 * %6 is reused as both loop counter and scratch context pointer, with %1 as its
 * backing store — that is what the repeated "mov %1,%6" / "mov %6,%1" pairs are
 * doing, and why the early exits are written to leave dead registers unrestored.
 *
 * 29.3 s across the profiled corpus, 4.5-16% of a run. ASSEMBLY_ANALYSIS.md §4.4.
 * Must stay bit-exact: every bin consumed here advances the shared CABAC state.
 */
#define decode_significance_skip_8x8 decode_significance_8x8_skip_x86
static av_always_inline int decode_significance_8x8_skip_x86(CABACContext *c,
                                           uint8_t *significant_coeff_ctx_base,
                                           uint8_t *last_coeff_ctx_base,
                                           const uint8_t *sig_off){
    int bit;
    x86_reg coeff_count;
    x86_reg last=0;
    x86_reg state;
    x86_reg counter = 0;

#ifdef BROKEN_RELOCATIONS
    void *tables;

    __asm__ volatile(
        "lea    "MANGLE(ff_h264_cabac_tables)", %0      \n\t"
        : "=&r"(tables)
        : NAMED_CONSTRAINTS_ARRAY(ff_h264_cabac_tables)
    );
#endif

    __asm__ volatile(
        "mov %1, %6                             \n\t"
        "3:                                     \n\t"

        /* Look up context offset for this 8x8 position from sig_off table */
        "mov %9, %0                             \n\t"
        "movzb (%0, %6), %6                     \n\t"
        "add %8, %6                             \n\t"

        /* Decode significance flag */
        BRANCHLESS_GET_CABAC("%4", "%q4", "(%6)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c11(%7)", "%c12(%7)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%14")

        "mov %1, %6                             \n\t"
        "test $1, %4                            \n\t"
        " jz 4f                                 \n\t"

#ifdef BROKEN_RELOCATIONS
        "movzb %c13(%14, %q6), %6\n\t"
#else
        "movzb "MANGLE(ff_h264_cabac_tables)"+%c13(%6), %6\n\t"
#endif
        "add %10, %6                            \n\t"

        BRANCHLESS_GET_CABAC("%4", "%q4", "(%6)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c11(%7)", "%c12(%7)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%14")

        "add $1, %2                             \n\t"

        "test $1, %4                            \n\t"
        " jnz 5f                                \n\t"  /* exit: %6 dead, no restore */

        "mov %1, %6                             \n\t"  /* continue: restore state */

        "4:                                     \n\t"
        "add $1, %6                             \n\t"
        "mov %6, %1                             \n\t"
        "cmp $63, %6                            \n\t"
        " jb 3b                                 \n\t"

        /* Fell through: position 63 implicitly significant */
        "add $1, %2                             \n\t"

        "5:                                     \n\t"
        "mov %k2, %k0                           \n\t"
        : "=&q"(coeff_count), "+"REG64(last), "+r"(counter), "+&r"(c->low),
          "=&r"(bit), "+&r"(c->range), "=&r"(state)
        : "r"(c), "m"(significant_coeff_ctx_base),
          REG64(sig_off), REG64(last_coeff_ctx_base),
          "i"(offsetof(CABACContext, bytestream)),
          "i"(offsetof(CABACContext, bytestream_end)),
          "i"(H264_LAST_COEFF_FLAG_OFFSET_8x8_OFFSET) TABLES_ARG
        : "%"FF_REG_c, "memory"
    );

    return coeff_count;
}

#endif /* HAVE_7REGS && BROKEN_COMPILER */

#if !BROKEN_COMPILER
/* Custom (MV-only fork): decode a BYPASS bin and throw the value away.
 *
 * Bypass bins are the cheap kind — no context model, no probability lookup;
 * the bin is literally the next binary digit of low/range. H.264 uses them for
 * coefficient sign bits and for the suffix of large-magnitude escape codes.
 *
 * The observation this is built on: in MV-only mode those *values* are never
 * used (a sign bit only matters if you are going to store a coefficient, and
 * nothing stores coefficients here) — but the bin still has to be decoded,
 * because consuming it is what advances low and the bytestream to where the
 * next bin the decoder *does* care about begins.
 *
 * So this is a byte-for-byte copy of get_cabac_bypass_x86 with exactly one
 * instruction removed: the "inc %edx" that turns the 0/-1 mask into a 0/1
 * return value. Saves one µop per call (~1 cycle on Intel). The state update
 * (low, range, bytestream refill) is bit-identical to the stock primitive, so
 * correctness is unaffected.
 *
 * `cdq` is the whole algorithm: after the subtraction, eax's sign bit IS the
 * decoded bin, and cdq broadcasts it into edx as an all-ones/all-zeros mask
 * that undoes the subtraction on a 0-bin (and, in the stock version, becomes
 * the return value).
 *
 * Note where SKIP_BLOCK does NOT use this: the escape-prefix loop
 * `while (get_cabac_bypass(CC) && ...)` genuinely reads its return value as the
 * loop condition, so it still calls the stock primitive. Only the sign bit and
 * the escape *suffix* bits — pure state advancement — come here.
 *
 * 9.4 s across the profiled corpus, 1.3-3.8% of a run. ASSEMBLY_ANALYSIS.md §4.5. */
#define consume_cabac_bypass consume_cabac_bypass_x86
static av_always_inline void consume_cabac_bypass_x86(CABACContext *c)
{
    x86_reg tmp;
    __asm__ volatile(
        "movl        %c5(%1), %k0       \n\t"
        "movl        %c2(%1), %%eax     \n\t"
        "shl             $17, %k0       \n\t"
        "add           %%eax, %%eax     \n\t"
        "sub             %k0, %%eax     \n\t"
        "cdq                            \n\t"
        "and           %%edx, %k0       \n\t"
        "add             %k0, %%eax     \n\t"
        /* No "inc %edx" — return value is discarded by caller. */
        "test           %%ax, %%ax      \n\t"
        "jnz              1f            \n\t"
        "mov         %c3(%1), %0        \n\t"
        "subl        $0xFFFF, %%eax     \n\t"
        "movzwl         (%0), %%ecx     \n\t"
        "bswap         %%ecx            \n\t"
        "shrl            $15, %%ecx     \n\t"
        "addl          %%ecx, %%eax     \n\t"
        "cmp         %c4(%1), %0        \n\t"
        "jge              1f            \n\t"
        "add"FF_OPSIZE"   $2, %c3(%1)   \n\t"
        "1:                             \n\t"
        "movl          %%eax, %c2(%1)   \n\t"

        : "=&r"(tmp)
        : "r"(c),
          "i"(offsetof(CABACContext, low)),
          "i"(offsetof(CABACContext, bytestream)),
          "i"(offsetof(CABACContext, bytestream_end)),
          "i"(offsetof(CABACContext, range))
        : "%eax", "%ecx", "%edx", "memory"
    );
}
#endif /* !BROKEN_COMPILER */

#endif /* HAVE_INLINE_ASM */
