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
#define decode_significance decode_significance_x86
static int decode_significance_x86(CABACContext *c, int max_coeff,
                                   uint8_t *significant_coeff_ctx_base,
                                   int *index, x86_reg last_off, int motion_vectors_only){
    void *end= significant_coeff_ctx_base + max_coeff - 1;
    int minusstart= -(intptr_t)significant_coeff_ctx_base;
    int minusindex= -(intptr_t)index;
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
        "cmpl $0, %13                            \n\t"
        "jne 6f                                  \n\t"

        // Normal path: parse and store coefficients
        "3:                                      \n\t"
        BRANCHLESS_GET_CABAC("%4", "%q4", "(%1)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c11(%6)", "%c12(%6)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%14")

        "test $1, %4                             \n\t"
        " jz 4f                                  \n\t"
        "add  %10, %1                            \n\t"

        BRANCHLESS_GET_CABAC("%4", "%q4", "(%1)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c11(%6)", "%c12(%6)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%14")

        "sub  %10, %1                            \n\t"
        "mov  %2, %0                             \n\t"
        "movl %7, %%ecx                          \n\t"
        "add  %1, %%"FF_REG_c"                   \n\t"
        "movl %%ecx, (%0)                        \n\t"

        "test $1, %4                             \n\t"
        " jnz 5f                                 \n\t"

        "add"FF_OPSIZE"  $4, %2                  \n\t"

        "4:                                      \n\t"
        "add  $1, %1                             \n\t"
        "cmp  %8, %1                             \n\t"
        " jb 3b                                  \n\t"
        "mov  %2, %0                             \n\t"
        "movl %7, %%ecx                          \n\t"
        "add  %1, %%"FF_REG_c"                   \n\t"
        "movl %%ecx, (%0)                        \n\t"
        "jmp 5f                                  \n\t"

        // MV-only path: parse coefficients but don't store them
        "6:                                      \n\t"
        BRANCHLESS_GET_CABAC("%4", "%q4", "(%1)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c11(%6)", "%c12(%6)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%14")

        "test $1, %4                             \n\t"
        " jz 7f                                  \n\t"
        "add  %10, %1                            \n\t"

        BRANCHLESS_GET_CABAC("%4", "%q4", "(%1)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "%c11(%6)", "%c12(%6)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%14")

        "sub  %10, %1                            \n\t"
        "test $1, %4                             \n\t"
        " jnz 5f                                 \n\t"

        "7:                                      \n\t"
        "add  $1, %1                             \n\t"
        "cmp  %8, %1                             \n\t"
        " jb 6b                                  \n\t"
        
        "5:                                      \n\t"
        "add  %9, %k0                            \n\t"
        "shr $2, %k0                             \n\t"
        : "=&q"(coeff_count), "+r"(significant_coeff_ctx_base), "+m"(index),
          "+&r"(c->low), "=&r"(bit), "+&r"(c->range)
        : "r"(c), "m"(minusstart), "m"(end), "m"(minusindex), "m"(last_off),
          "i"(offsetof(CABACContext, bytestream)),
          "i"(offsetof(CABACContext, bytestream_end)),
          "m"(motion_vectors_only)
          TABLES_ARG
        : "%"FF_REG_c, "memory"
    );
    return coeff_count;
}

#define decode_significance_8x8 decode_significance_8x8_x86
static int decode_significance_8x8_x86(CABACContext *c,
                                       uint8_t *significant_coeff_ctx_base,
                                       int *index, uint8_t *last_coeff_ctx_base, 
                                       const uint8_t *sig_off, int motion_vectors_only){
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
        "cmpl $0, %15                            \n\t"  
        "jne 6f                                  \n\t"  
        
        "mov %1, %6                              \n\t"
        "3:                                      \n\t"
        "mov %9, %0                              \n\t"        // Load sig_off base
        "movzb (%0, %6), %0                      \n\t"        // Load sig_off[i]
        "add %10, %0                             \n\t"        // Add significant_coeff_ctx_base
        "mov %0, %6                              \n\t"        // Store back to %6
        BRANCHLESS_GET_CABAC("%4", "%q4", "(%6)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "12(%7)", "13(%7)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%16")
        "mov %1, %6                              \n\t"
        "test $1, %4                             \n\t"
        " jz 4f                                  \n\t"
        "mov %9, %0                              \n\t"        // Load sig_off base  
        "movzb (%0, %6), %0                      \n\t"        // Load sig_off[i]
        "mov %11, %6                             \n\t"        // Load last_coeff_ctx_base
        "add %0, %6                              \n\t"        // Add offset
        BRANCHLESS_GET_CABAC("%4", "%q4", "(%6)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "12(%7)", "13(%7)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%16")
        "mov %2, %0                              \n\t"
        "mov %1, %6                              \n\t"
        "movl %k6, (%0)                          \n\t"        // Fixed: use movl for 32-bit
        "test $1, %4                             \n\t"
        " jnz 5f                                 \n\t"
        "add"FF_OPSIZE"  $4, %2                  \n\t"
        "4:                                      \n\t"
        "add $1, %6                              \n\t"
        "mov %6, %1                              \n\t"
        "cmp $63, %6                             \n\t"
        " jb 3b                                  \n\t"
        "mov %2, %0                              \n\t"
        "movl %k6, (%0)                          \n\t"        // Fixed: use movl for 32-bit
        "jmp 5f                                  \n\t"
        
        // MV-only path - simplified
        "6:                                      \n\t"
        "mov %1, %6                              \n\t"
        "7:                                      \n\t"
        "mov %9, %0                              \n\t"
        "movzb (%0, %6), %0                      \n\t"
        "add %10, %0                             \n\t"
        "mov %0, %6                              \n\t"
        BRANCHLESS_GET_CABAC("%4", "%q4", "(%6)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "12(%7)", "13(%7)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%16")
        "mov %1, %6                              \n\t"
        "test $1, %4                             \n\t"
        " jz 8f                                  \n\t"
        "mov %9, %0                              \n\t"
        "movzb (%0, %6), %0                      \n\t"
        "mov %11, %6                             \n\t"
        "add %0, %6                              \n\t"
        BRANCHLESS_GET_CABAC("%4", "%q4", "(%6)", "%3", "%w3",
                             "%5", "%q5", "%k0", "%b0",
                             "12(%7)", "13(%7)",
                             AV_STRINGIFY(H264_NORM_SHIFT_OFFSET),
                             AV_STRINGIFY(H264_LPS_RANGE_OFFSET),
                             AV_STRINGIFY(H264_MLPS_STATE_OFFSET),
                             "%16")
        "test $1, %4                             \n\t"
        " jnz 5f                                 \n\t"
        "8:                                      \n\t"
        "add $1, %6                              \n\t"
        "mov %6, %1                              \n\t"
        "cmp $63, %6                             \n\t"
        " jb 7b                                  \n\t"
        
        "5:                                      \n\t"
        "addl %8, %k0                            \n\t"        // Fixed: use proper register size
        "shr $2, %k0                             \n\t"
        : "=&q"(coeff_count), "+r"(last), "+m"(index), "+&r"(c->low),
          "=&r"(bit), "+&r"(c->range), "=&r"(state)
        : "r"(c), "r"(minusindex), "r"(sig_off), "r"(significant_coeff_ctx_base),
          "r"(last_coeff_ctx_base),
          "i"(offsetof(CABACContext, bytestream)),
          "i"(offsetof(CABACContext, bytestream_end)),
          "i"(276), "m"(motion_vectors_only) TABLES_ARG
        : "%"FF_REG_c, "memory"
    );
    return coeff_count;
}

#endif /* HAVE_7REGS && BROKEN_COMPILER */

#endif /* HAVE_INLINE_ASM */