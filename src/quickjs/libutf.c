#include <libutf.h>

#include <assert.h>
#include <stdlib.h>

LibutfC16Type libutf_c16_type(uint_least16_t c16) {
    if (0xD800 != (0xF800 & c16)) {
        return LIBUTF_UTF16_NOT_SURROGATE;
    }
    return (c16 & 0x0400) ? LIBUTF_UTF16_SURROGATE_LOW : LIBUTF_UTF16_SURROGATE_HIGH;
}

LibutfC8Type libutf_c8_type(char c) {
    static const LibutfC8Type lookup_table[256] = {
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
           -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
           -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
           -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
           -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
            2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
            3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
            4, 4, 4, 4, 4, 4, 4, 4,-2,-2,-2,-2,-2,-2,-2,-2,
    };
    return lookup_table[(unsigned char) c];
}

bool libutf_c32_to_c8(uint_least32_t c32, int *length, char c8[4]) {
    if (!libutf_c32_is_valid(c32)) {
        *length = 3;
        c8[0] = (char) 0xEF;
        c8[1] = (char) 0xBF;
        c8[2] = (char) 0xBD;
        return false;
    }
    if (c32 <= 0x007F) {
        c8[0] = c32;
        *length = 1;
        return true;
    } else if (c32 <= 0x07FF) {
        c8[0] = (char) (0xC0 | (c32 >> 6));
        c8[1] = (char) (0x80 | (c32 & 0x3F));
        *length = 2;
        return true;
    } else if (c32 <= 0xFFFF) {
        c8[0] = (char) (0xE0 | (c32 >> 12));
        c8[1] = (char) (0x80 | ((c32 >> 6) & 0x3F));
        c8[2] = (char) (0x80 | (c32 & 0x3F));
        *length = 3;
        return true;
    }
    assert(c32 <= 0x10FFFF);
    c8[0] = (char) (0xF0 | (c32 >> 18));
    c8[1] = (char) (0x80 | ((c32 >> 12) & 0x3F));
    c8[2] = (char) (0x80 | ((c32 >> 6) & 0x3F));
    c8[3] = (char) (0x80 | (c32 & 0x3F));
    *length = 4;
    return true;
}

bool libutf_c8_to_c32(const char *c8, uint_least32_t *c32) {
    if (!c8 || !c32) {
        return false;
    }
    unsigned char c = *c8;
    *c32 = 0xFFFD;
    LibutfC8Type type = libutf_c8_type(c);
    if (type < 0) {
        return false;
    }
    int len = type;
    uint_least32_t result;
    switch (len) {
    case 1:
        *c32 = c;
        return true;
    case 2:
        result = c & 0x1F;
        break;
    case 3:
        result = c & 0x0F;
        break;
    case 4:
        result = c & 0x07;
        break;
    default:
        abort();
    }
    for (int i = 1; i < len; ++i) {
        c = *++c8;
        if ((0xC0 & c) != 0x80) {
            return false;
        }
        result = (result << 6) + (c & 0x3F);
    }
    if (!libutf_c32_is_valid(result)) {
        return false;
    }
    char c8seq[4];
    int n;
    bool ok = libutf_c32_to_c8(result, &n, c8seq);
    assert(ok);
    if (n < (int) len) {
        return false;
    }
    *c32 = result;
    return true;
}

bool libutf_c32_to_c16(uint_least32_t c32, int *length, uint_least16_t c16[2]) {
    if (!libutf_c32_is_valid(c32)) {
        c16[0] = 0xFFFD;
        c16[1] = 0;
        *length = 1;
        return false;
    }
    if (c32 < 0x10000) {
        c16[0] = c32;
        c16[1] = 0;
        *length = 1;
    } else {
        c32 -= 0x10000;
        c16[0] = 0xD800 | (c32 >> 10);
        c16[1] = 0xDC00 | (c32 & 0x3FF);
        *length = 2;
    }
    return true;
}

bool libutf_c16_to_c32(uint_least16_t c16[2], uint_least32_t *c32) {
    LibutfC16Type type = libutf_c16_type(c16[0]);
    if (LIBUTF_UTF16_NOT_SURROGATE == type) {
        *c32 = c16[0];
        return true;
    }
    if (LIBUTF_UTF16_SURROGATE_HIGH != type || LIBUTF_UTF16_SURROGATE_LOW != libutf_c16_type(c16[1])) {
        *c32 = 0xFFFD;
        return false;
    }
    *c32 = 0x10000 + ((c16[0] & 0x3FF) << 10) + (c16[1] & 0x3ff);
    return true;
}

bool libutf_c32_is_valid(uint_least32_t c32) {
    return c32 < 0xD800 || (0xDFFF < c32 && c32 < 0x110000);
}
