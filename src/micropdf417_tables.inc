/*
    libzint - the open source barcode library
    Copyright (C) 2008-2026 Robin Stuart <rstuart114@gmail.com>

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
    3. Neither the name of the project nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
    OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
    HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
    OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
    SUCH DAMAGE.
 */
/* SPDX-License-Identifier: BSD-3-Clause */

// ISO/IEC 24728 variant, Reed-Solomon and row-address tables.
// Source: https://github.com/zint/zint/blob/master/backend/pdf417_tabs.h
// Retrieved 2026-10-07; only BSD-3-Clause tables are adapted here.
inline constexpr int RAPTable[4][34] = {
     { 1,  8, 36, 19,  9, 25, 1,  1,  8, 36, 19,  9, 27,  1,  7, 15,  25,  37,   1,   1,  21,  15,   1, 47,  1,  7, 15,  25,  37,   1,   1,  21,  15,   1 },
     { 0,  0,  0,  0,  0,  0, 0,  0,  0,  0,  0,  0,  0,  1,  7, 15,  25,  37,  17,   9,  29,  31,  25, 19,  1,  7, 15,  25,  37,  17,   9,  29,  31,  25 },
     { 9,  8, 36, 19, 17, 33, 1,  9,  8, 36, 19, 17, 35,  1,  7, 15,  25,  37,  33,  17,  37,  47,  49, 43,  1,  7, 15,  25,  37,  33,  17,  37,  47,  49 },
     { 0,  1,  2,  0,  2,  0, 0,  0,  1,  2,  0,  2,  2,  0,  0,  2,   0,   0,   0,   0,   2,   2,   0,  1,  0,  0,  2,   0,   0,   0,   0,   2,   2,   0 }
};

inline constexpr int rap_side[52] = {
    0x322, 0x3A2, 0x3B2, 0x332, 0x372, 0x37A, 0x33A, 0x3BA, 0x39A, 0x3DA,
    0x3CA, 0x38A, 0x30A, 0x31A, 0x312, 0x392, 0x3D2, 0x3D6, 0x3D4, 0x394,
    0x3B4, 0x3A4, 0x3A6, 0x3AE, 0x3AC, 0x3A8, 0x328, 0x32C, 0x32E, 0x326,
    0x336, 0x3B6, 0x396, 0x316, 0x314, 0x334, 0x374, 0x364, 0x366, 0x36E,
    0x36C, 0x368, 0x348, 0x358, 0x35C, 0x35E, 0x34E, 0x34C, 0x344, 0x346,
    0x342, 0x362
};

inline constexpr int rap_centre[52] = {
    0x2CE, 0x24E, 0x26E, 0x22E, 0x226, 0x236, 0x216, 0x212, 0x21A, 0x23A,
    0x232, 0x222, 0x262, 0x272, 0x27A, 0x2FA, 0x2F2, 0x2F6, 0x276, 0x274,
    0x264, 0x266, 0x246, 0x242, 0x2C2, 0x2E2, 0x2E6, 0x2E4, 0x2EC, 0x26C,
    0x22C, 0x228, 0x268, 0x2E8, 0x2C8, 0x2CC, 0x2C4, 0x2C6, 0x286, 0x28E,
    0x28C, 0x29C, 0x298, 0x2B8, 0x2B0, 0x290, 0x2D0, 0x250, 0x258, 0x25C,
    0x2DC, 0x2DE
};

inline constexpr int Microcoeffs[344] = {
    /* k = 7, k-offset 0 */
     76, 925, 537, 597, 784, 691, 437,

    /* k = 8, k-offset 7 */
    237, 308, 436, 284, 646, 653, 428, 379,

    /* k = 9, k-offset 15 */
    567, 527, 622, 257, 289, 362, 501, 441, 205,

    /* k = 10, k-offset 24 */
    377, 457,  64, 244, 826, 841, 818, 691, 266, 612,

    /* k = 11, k-offset 34 */
    462,  45, 565, 708, 825, 213,  15,  68, 327, 602, 904,

    /* k = 12, k-offset 45 */
    597, 864, 757, 201, 646, 684, 347, 127, 388,   7,  69, 851,

    /* k = 13, k-offset 57 */
    764, 713, 342, 384, 606, 583, 322, 592, 678, 204, 184, 394, 692,

    /* k = 14, k-offset 70 */
    669, 677, 154, 187, 241, 286, 274, 354, 478, 915, 691, 833, 105, 215,

    /* k = 15, k-offset 84 */
    460, 829, 476, 109, 904, 664, 230,   5,  80,  74, 550, 575, 147, 868, 642,

    /* k = 16, k-offset 99 */
    274, 562, 232, 755, 599, 524, 801, 132, 295, 116, 442, 428, 295,  42, 176,  65,

    /* k = 18, k-offset 115 */
    279, 577, 315, 624,  37, 855, 275, 739, 120, 297, 312, 202, 560, 321, 233, 756,
    760, 573,

    /* k = 21, k-offset 133 */
    108, 519, 781, 534, 129, 425, 681, 553, 422, 716, 763, 693, 624, 610, 310, 691,
    347, 165, 193, 259, 568,

    /* k = 26, k-offset 154 */
    443, 284, 887, 544, 788,  93, 477, 760, 331, 608, 269, 121, 159, 830, 446, 893,
    699, 245, 441, 454, 325, 858, 131, 847, 764, 169,

    /* k = 32, k-offset 180 */
    361, 575, 922, 525, 176, 586, 640, 321, 536, 742, 677, 742, 687, 284, 193, 517,
    273, 494, 263, 147, 593, 800, 571, 320, 803, 133, 231, 390, 685, 330,  63, 410,

    /* k = 38, k-offset 212 */
    234, 228, 438, 848, 133, 703, 529, 721, 788, 322, 280, 159, 738, 586, 388, 684,
    445, 680, 245, 595, 614, 233, 812,  32, 284, 658, 745, 229,  95, 689, 920, 771,
    554, 289, 231, 125, 117, 518,

    /* k = 44, k-offset 250 */
    476,  36, 659, 848, 678,  64, 764, 840, 157, 915, 470, 876, 109,  25, 632, 405,
    417, 436, 714,  60, 376,  97, 413, 706, 446,  21,   3, 773, 569, 267, 272, 213,
     31, 560, 231, 758, 103, 271, 572, 436, 339, 730,  82, 285,

    /* k = 50, k-offset 294 */
    923, 797, 576, 875, 156, 706,  63,  81, 257, 874, 411, 416, 778,  50, 205, 303,
    188, 535, 909, 155, 637, 230, 534,  96, 575, 102, 264, 233, 919, 593, 865,  26,
    579, 623, 766, 146,  10, 739, 246, 127,  71, 244, 211, 477, 920, 876, 427, 820,
    718, 435
};

inline constexpr int MicroVariants[4][34] = {
    {  1,  1,  1,  1,  1,  1, 2,  2,  2,  2,  2,  2,  2,  3,  3,  3,   3,   3,   3,   3,   3,   3,   3,  4,  4,  4,  4,   4,   4,   4,   4,   4,   4,   4 },
    { 11, 14, 17, 20, 24, 28, 8, 11, 14, 17, 20, 23, 26,  6,  8, 10,  12,  15,  20,  26,  32,  38,  44,  4,  6,  8, 10,  12,  15,  20,  26,  32,  38,  44 },
    {  7,  7,  7,  8,  8,  8, 8,  9,  9, 10, 11, 13, 15, 12, 14, 16,  18,  21,  26,  32,  38,  44,  50,  8, 12, 14, 16,  18,  21,  26,  32,  38,  44,  50 },
    {  0,  0,  0,  7,  7,  7, 7, 15, 15, 24, 34, 57, 84, 45, 70, 99, 115, 133, 154, 180, 212, 250, 294,  7, 45, 70, 99, 115, 133, 154, 180, 212, 250, 294 }
};
