#include <stdio.h>
#include "base64.h"

#define FAKE_ENCODE 0
#if NDEBUG
#if FAKE_ENCODE
#error  CANNOT SET  FAKE_ENCODE in release
#endif
#endif

#define BASE64_PAD      '='
#define BASE64DE_FIRST  '+'
#define BASE64DE_LAST   'z'
/* The Base 64 encoding with an URL and filename safe base64en, RFC 4648
   section 5 */
static const char base64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char base64en[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
#if 1
//TODO
int hv_base64_encode_lb(const void* parm1, unsigned insz, unsigned brsz, char* out)
{
    int nSize, nCR;
#if FAKE_ENCODE
    memcpy(out, parm1, insz);
    return insz;
#else
    size_t i, o, count = brsz;
    const unsigned char* in = (const unsigned char*)parm1;

    if (insz == 0) {
        return 0;
    }

    // nSize = BASE64_ENCODE_OUT_SIZE(insz);
    // nCR = nSize / brsz; // count of CRs
    // nSize += nCR * 2;

    for (i = 0, o = 0; i < insz; i += 3) {
        int tmp;
        tmp = in[i] << 16;
        if (i + 1 < insz)
            tmp += in[i + 1] << 8;
        if (i + 2 < insz)
            tmp += in[i + 2];
        out[o] = base64en[(tmp >> 18) & 0x3f];
        out[o + 1] = base64en[(tmp >> 12) & 0x3f];
        out[o + 2] = base64en[(tmp >> 6) & 0x3f];
        out[o + 3] = base64en[tmp & 0x3f];
        if (i + 2 > insz)
            out[o + 2] = '=';
        if (i + 3 > insz)
            out[o + 3] = '=';
        o += 4;
        if ((count -= 4) == 0) {
            out[o++] = '\r';
            out[o++] = '\n';
            count = brsz;
        }
    }
    return o;
#endif
    return nSize;
}

int hv_base64_encode(const unsigned char* parm1, unsigned insz, char* out)
{
#if FAKE_ENCODE
    memcpy(out, parm1, insz);
    return insz;
#else
    size_t i, o;
    const unsigned char* in = (const unsigned char*)parm1;

    if (insz == 0) {
        return 0;
    }

    for (i = 0, o = 0; i < insz; i += 3, o += 4) {
        int tmp;
        tmp = in[i] << 16;
        if (i + 1 < insz)
            tmp += in[i + 1] << 8;
        if (i + 2 < insz)
            tmp += in[i + 2];
        out[o] = base64en[(tmp >> 18) & 0x3f];
        out[o + 1] = base64en[(tmp >> 12) & 0x3f];
        out[o + 2] = base64en[(tmp >> 6) & 0x3f];
        out[o + 3] = base64en[tmp & 0x3f];
        if (i + 2 > insz)
            out[o + 2] = '=';
        if (i + 3 > insz)
            out[o + 3] = '=';
    }
    return o;
#endif
}

int hv_base64_decode(const char *in, unsigned int insz, unsigned char *out)
{
#if FAKE_ENCODE
    memcpy(out, in, insz);
    return insz;
#else
    static const int revalphabet[] = {
        -1, -1, -1, -1, -1, -1, -1, -1,   /*   0 ..   7 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /*   8 ..  15 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /*  16 ..  23 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /*  24 ..  31 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /*  32 ..  39 */
        -1, -1, -1, 62, -1, -1, -1, 63,   /*  40 ..  47 */
        52, 53, 54, 55, 56, 57, 58, 59,   /*  48  .. 55 */
        60, 61, -1, -1, -1, -1, -1, -1,   /*  56 ..  63 */
        -1,  0,  1,  2,  3,  4,  5,  6,   /*  64 ..  71 */
        7,  8,  9, 10, 11, 12, 13, 14,   /*  72 ..  79 */
        15, 16, 17, 18, 19, 20, 21, 22,   /*  80 ..  87 */
        23, 24, 25, -1, -1, -1, -1, -1,   /*  88 ..  95 */
        -1, 26, 27, 28, 29, 30, 31, 32,   /*  96 .. 103 */
        33, 34, 35, 36, 37, 38, 39, 40,   /* 104 .. 111 */
        41, 42, 43, 44, 45, 46, 47, 48,   /* 112 .. 119 */
        49, 50, 51, -1, -1, -1, -1, -1,   /* 120 .. 127 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 128 .. 135 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 136 .. 143 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 144 .. 151 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 152 .. 159 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 160 .. 167 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 168 .. 175 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 176 .. 183 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 184 .. 191 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 192 .. 199 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 200 .. 207 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 208 .. 215 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 216 .. 223 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 224 .. 231 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 232 .. 239 */
        -1, -1, -1, -1, -1, -1, -1, -1,   /* 240 .. 247 */
        -1, -1, -1, -1, -1, -1, -1, -1 }; /* 248 .. 255 */
        size_t i, o, olen;
        int tmp, digit;

        if (insz % 4){
            return -1;
        }
        if (insz == 0) {
            return 0;
        }

        if (in[insz - 2] == '=')
            olen = ((insz / 4) * 3) - 2;
        else if (in[insz - 1] == '=')
            olen = ((insz / 4) * 3) - 1;
        else
            olen = (insz / 4) * 3;

        for (i = 0, o = 0; i < insz; i += 4, o += 3) {
            if ((digit = revalphabet[(unsigned char)in[i    ]]) == -1)
                goto leave;
            tmp  = digit << 18;
            if ((digit = revalphabet[(unsigned char)in[i + 1]]) == -1)
                goto leave;
            tmp += digit << 12;
            if ((digit = revalphabet[(unsigned char)in[i + 2]]) == -1) {
                if ((i == insz - 4) && (in[i + 2] == '='))
                    digit = 0;
                else
                    goto leave;
            }
            tmp += digit <<  6;
            if ((digit = revalphabet[(unsigned char)in[i + 3]]) == -1) {
                if ((i == insz - 4) && (in[i + 3] == '='))
                    digit = 0;
                else
                    goto leave;
            }
            tmp += digit;
            out[o    ] = (tmp >> 16) & 0xff;
            if (o + 1 < olen)
                out[o + 1] = (tmp >>  8) & 0xff;
            if (o + 2 < olen)
                out[o + 2] =  tmp        & 0xff;
        }
        return olen;
leave:
        return -1;
#endif
}

#else
/* ASCII order for BASE 64 decode, -1 in unused character */
static const signed char base64de[] = {
    /* '+', ',', '-', '.', '/', '0', '1', '2', */
        62,  -1,  -1,  -1,  63,  52,  53,  54,

    /* '3', '4', '5', '6', '7', '8', '9', ':', */
        55,  56,  57,  58,  59,  60,  61,  -1,

    /* ';', '<', '=', '>', '?', '@', 'A', 'B', */
        -1,  -1,  -1,  -1,  -1,  -1,   0,   1,

    /* 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', */
         2,   3,   4,   5,   6,   7,   8,   9,

    /* 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', */
        10,  11,  12,  13,  14,  15,  16,  17,

    /* 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', */
        18,  19,  20,  21,  22,  23,  24,  25,

    /* '[', '\', ']', '^', '_', '`', 'a', 'b', */
        -1,  -1,  -1,  -1,  -1,  -1,  26,  27,

    /* 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', */
        28,  29,  30,  31,  32,  33,  34,  35,

    /* 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', */
        36,  37,  38,  39,  40,  41,  42,  43,

    /* 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', */
        44,  45,  46,  47,  48,  49,  50,  51,
};

int hv_base64_encode(const unsigned char *in, unsigned int inlen, char *out) {
    unsigned int i = 0, j = 0;

    for (; i < inlen; i++) {
        int s = i % 3;

        switch (s) {
        case 0:
            out[j++] = base64en[(in[i] >> 2) & 0x3F];
            continue;
        case 1:
            out[j++] = base64en[((in[i-1] & 0x3) << 4) + ((in[i] >> 4) & 0xF)];
            continue;
        case 2:
            out[j++] = base64en[((in[i-1] & 0xF) << 2) + ((in[i] >> 6) & 0x3)];
            out[j++] = base64en[in[i] & 0x3F];
        }
    }

    /* move back */
    i -= 1;

    /* check the last and add padding */
    if ((i % 3) == 0) {
        out[j++] = base64en[(in[i] & 0x3) << 4];
        out[j++] = BASE64_PAD;
        out[j++] = BASE64_PAD;
    } else if ((i % 3) == 1) {
        out[j++] = base64en[(in[i] & 0xF) << 2];
        out[j++] = BASE64_PAD;
    }

    return j;
}

int hv_base64_decode(const char *in, unsigned int inlen, unsigned char *out) {
    unsigned int i = 0, j = 0;

    for (; i < inlen; i++) {
        int c;
        int s = i % 4;

        if (in[i] == '=')
            return j;

        if (in[i] < BASE64DE_FIRST || in[i] > BASE64DE_LAST ||
            (c = base64de[in[i] - BASE64DE_FIRST]) == -1)
            return -1;

        switch (s) {
        case 0:
            out[j] = ((unsigned int)c << 2) & 0xFF;
            continue;
        case 1:
            out[j++] += ((unsigned int)c >> 4) & 0x3;

            /* if not last char with padding */
            if (i < (inlen - 3) || in[inlen - 2] != '=')
                out[j] = ((unsigned int)c & 0xF) << 4;
            continue;
        case 2:
            out[j++] += ((unsigned int)c >> 2) & 0xF;

            /* if not last char with padding */
            if (i < (inlen - 2) || in[inlen - 1] != '=')
                out[j] =  ((unsigned int)c & 0x3) << 6;
            continue;
        case 3:
            out[j++] += (unsigned char)c;
        }
    }

    return j;
}
#endif
