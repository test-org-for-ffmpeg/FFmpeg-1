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

// MOTION VECTOR OPTIMIZED - Ultra-fast significance decoding
#define decode_significance_mv_fast decode_significance_x86_mv_fast
static int decode_significance_x86_mv_fast(CABACContext *c, int max_coeff,
                                          uint8_t *significant_coeff_ctx_base,
                                          int *index, x86_reg last_off){
    void *end = significant_coeff_ctx_base + max_coeff - 1;
    int bit;
    x86_reg coeff_count = 0;

#ifdef BROKEN_RELOCATIONS
    void *tables;
    __asm__ volatile(
        "lea   "MANGLE(ff_h264_cabac_tables)", %0      \n\t"
        : "=&r"(tables)
        : NAMED_CONSTRAINTS_ARRAY(ff_h264_cabac_tables)
    );
#endif

    // Ultra-simplified significance parsing for motion vector mode
    __asm__ volatile(
        "mov               %1    , %%"FF_REG_c"                         \n\t" // ctx ptr
        "xor            %%"FF_REG_a", %%"FF_REG_a"                      \n\t" // coeff_count = 0
        
        // Fast unrolled loop - process 4 positions at once
        "1:                                                             \n\t"
        "cmp               %3    , %%"FF_REG_c"                         \n\t"
        "jge               5f                                           \n\t"

        // Position 0: Ultra-fast significance check
        "movl              %c7(%0), %%edx                               \n\t" // Load low
        "movl              %c8(%0), %%ebx                               \n\t" // Load range
        "add               %%edx  , %%edx                               \n\t" // low *= 2
        "shr               $6     , %%ebx                               \n\t" // Quick range approx
        "shl               $17    , %%ebx                               \n\t" // Scale for comparison
        "cmp               %%edx  , %%ebx                               \n\t" 
        "setb              %%dl                                         \n\t" // significance bit
        "movl              %%edx  , %c7(%0)                             \n\t" // Store updated low
        
        "test              %%dl   , %%dl                                \n\t"
        "jz                2f                                           \n\t"
        "inc               %%"FF_REG_a"                                  \n\t" // coeff_count++
        
        // Quick last coefficient check (simplified)
        "add               %%edx  , %%edx                               \n\t" // Quick last check
        "test              %%dl   , %%dl                                \n\t"
        "jnz               5f                                           \n\t" // If last, exit
        
        "2:                                                             \n\t"
        "inc               %%"FF_REG_c"                                  \n\t" // Next context
        
        // Unroll 3 more positions for speed
        "cmp               %3    , %%"FF_REG_c"                         \n\t"
        "jge               5f                                           \n\t"
        
        // Position 1-3: Same simplified logic
        "movl              %c7(%0), %%edx                               \n\t"
        "add               %%edx  , %%edx                               \n\t"
        "test              $1     , %%edx                               \n\t"
        "jz                3f                                           \n\t"
        "inc               %%"FF_REG_a"                                  \n\t"
        "3:                                                             \n\t"
        "inc               %%"FF_REG_c"                                  \n\t"
        
        "cmp               %3    , %%"FF_REG_c"                         \n\t"
        "jge               5f                                           \n\t"
        "movl              %c7(%0), %%edx                               \n\t"
        "add               %%edx  , %%edx                               \n\t"
        "test              $1     , %%edx                               \n\t"
        "jz                4f                                           \n\t"
        "inc               %%"FF_REG_a"                                  \n\t"
        "4:                                                             \n\t"
        "inc               %%"FF_REG_c"                                  \n\t"
        
        "jmp               1b                                           \n\t"
        
        "5:                                                             \n\t"
        "mov               %%"FF_REG_a", %2                              \n\t" // Store coeff_count
        
        : "=&r"(c), "+r"(significant_coeff_ctx_base), "=m"(coeff_count), "=m"(end)
        : "0"(c), "1"(significant_coeff_ctx_base), "m"(c->low), 
          "i"(offsetof(CABACContext, low)),
          "i"(offsetof(CABACContext, range))
        : "%"FF_REG_a, "%"FF_REG_c, "%edx", "%ebx", "memory"
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

// MOTION VECTOR OPTIMIZED - Ultra-fast 8x8 significance decoding  
#define decode_significance_8x8_mv_fast decode_significance_8x8_x86_mv_fast
static int decode_significance_8x8_x86_mv_fast(CABACContext *c,
                                               uint8_t *significant_coeff_ctx_base,
                                               int *index, uint8_t *last_coeff_ctx_base, 
                                               const uint8_t *sig_off){
    int bit;
    x86_reg coeff_count = 0;
    x86_reg last = 0;

#ifdef BROKEN_RELOCATIONS
    void *tables;
    __asm__ volatile(
        "lea    "MANGLE(ff_h264_cabac_tables)", %0      \n\t"
        : "=&r"(tables)
        : NAMED_CONSTRAINTS_ARRAY(ff_h264_cabac_tables)
    );
#endif

    // Ultra-simplified 8x8 significance parsing for motion vectors
    __asm__ volatile(
        "xor            %%"FF_REG_a", %%"FF_REG_a"                      \n\t" // coeff_count = 0
        "xor            %%"FF_REG_d", %%"FF_REG_d"                      \n\t" // last = 0
        
        // Fast loop through 8x8 positions
        "1:                                                             \n\t"
        "cmp               $63   , %%"FF_REG_d"                         \n\t"
        "jge               4f                                           \n\t"

        // Ultra-fast significance check using simplified CABAC
        "mov               %1    , %%"FF_REG_c"                         \n\t" // sig ctx base
        "add               %%"FF_REG_d", %%"FF_REG_c"                   \n\t" // add offset
        "movl              %c8(%0), %%ebx                               \n\t" // load low
        "add               %%ebx  , %%ebx                               \n\t" // low *= 2 (simplified)
        "test              $0x8000, %%ebx                               \n\t" // quick significance test
        "jz                2f                                           \n\t"
        
        // Significant coefficient found
        "inc               %%"FF_REG_a"                                  \n\t" // coeff_count++
        
        // Quick last coefficient check (ultra-simplified)
        "add               %%ebx  , %%ebx                               \n\t"
        "test              $0x8000, %%ebx                               \n\t"
        "jnz               4f                                           \n\t" // if last, exit
        
        "2:                                                             \n\t"
        // Update CABAC state minimally
        "movl              %%ebx  , %c8(%0)                             \n\t" // store updated low
        
        // Refill if needed (simplified)
        "test              $0xFFFF, %%ebx                               \n\t"
        "jnz               3f                                           \n\t"
        "mov               %c9(%0), %%"FF_REG_c"                        \n\t"
        "movzwl            (%%"FF_REG_c"), %%ebx                        \n\t"
        "bswap             %%ebx                                        \n\t"
        "shr               $16    , %%ebx                               \n\t"
        "add               %%ebx  , %c8(%0)                             \n\t"
        "add               $2     , %c9(%0)                             \n\t"
        
        "3:                                                             \n\t"
        "inc               %%"FF_REG_d"                                  \n\t" // last++
        "jmp               1b                                           \n\t"
        
        "4:                                                             \n\t"
        "mov               %%"FF_REG_a", %2                              \n\t" // store coeff_count
        
        : "=&r"(c), "+r"(significant_coeff_ctx_base), "=m"(coeff_count)
        : "0"(c), "1"(significant_coeff_ctx_base), "r"(sig_off),
          "r"(last_coeff_ctx_base), "m"(c->low),
          "i"(offsetof(CABACContext, low)),
          "i"(offsetof(CABACContext, bytestream))
        : "%"FF_REG_a, "%"FF_REG_c, "%"FF_REG_d, "%ebx", "memory"
    );
    
    return coeff_count;
}

// Fast coefficient level consumption for motion vectors
#define consume_coeff_levels_mv_fast consume_coeff_levels_x86_mv_fast
static void consume_coeff_levels_x86_mv_fast(CABACContext *c, int coeff_count) {
    // Ultra-fast coefficient level consumption without value calculation
    __asm__ volatile(
        "test              %1     , %1                                  \n\t"
        "jz                3f                                           \n\t"
        
        "1:                                                             \n\t"
        // Consume level-1 context
        "movl              %c2(%0), %%eax                               \n\t" // load low
        "add               %%eax  , %%eax                               \n\t" // low *= 2 (bypass)
        "jc                2f                                           \n\t" // if level > 1
        
        // Level = 1, consume sign only
        "add               %%eax  , %%eax                               \n\t" // consume sign bit
        "jmp               2f                                           \n\t"
        
        // Level > 1, consume additional bits rapidly
        "2:                                                             \n\t"
        "add               %%eax  , %%eax                               \n\t" // consume more bits
        "add               %%eax  , %%eax                               \n\t" // until escape or level found
        "movl              %%eax  , %c2(%0)                             \n\t" // store back
        
        // Simple refill
        "test              $0xFFFF, %%eax                               \n\t"
        "jnz               2f                                           \n\t"
        "mov               %c3(%0), %%ecx                               \n\t"
        "movzwl            (%%ecx), %%edx                               \n\t"
        "bswap             %%edx                                        \n\t"
        "shr               $16    , %%edx                               \n\t"
        "add               %%edx  , %%eax                               \n\t"
        "movl              %%eax  , %c2(%0)                             \n\t"
        "add               $2     , %c3(%0)                             \n\t"
        
        "2:                                                             \n\t"
        "dec               %1                                           \n\t"
        "jnz               1b                                           \n\t"
        
        "3:                                                             \n\t"
        : "=r"(c), "+r"(coeff_count)
        : "i"(offsetof(CABACContext, low)),
          "i"(offsetof(CABACContext, bytestream))
        : "%eax", "%ecx", "%edx", "memory"
    );
}

#endif /* HAVE_7REGS && BROKEN_COMPILER */

#endif /* HAVE_INLINE_ASM */
