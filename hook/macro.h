/**
 * These are helper macros for writing hooks, all of them are optional as is including macro.h at all
 */

#include <stdint.h>
#include "hookapi.h"
#include "sfcodes.h"

#ifndef HOOKMACROS_INCLUDED
#define HOOKMACROS_INCLUDED 1

#ifdef NDEBUG
#define DEBUG 0
#else
#define DEBUG 1
#endif

#define DONEEMPTY()\
    accept(0,0,__LINE__)

#define DONEMSG(msg)\
    accept(msg, sizeof(msg),__LINE__)

#define DONE(x)\
    accept(SVAR(x),(uint32_t)__LINE__);

#define ASSERT(x)\
{\
    if (!(x))\
        rollback(0,0,__LINE__);\
}

#define NOPE(x)\
{\
    return rollback((x), sizeof(x), __LINE__);\
}

#define TRACEVAR(v) if (DEBUG) trace_num((uint32_t)(#v), (uint32_t)(sizeof(#v) - 1), (int64_t)v);
#define TRACEHEX(v) if (DEBUG) trace((uint32_t)(#v), (uint32_t)(sizeof(#v) - 1), (uint32_t)(v), (uint32_t)(sizeof(v)), 1);
#define TRACEXFL(v) if (DEBUG) trace_float((uint32_t)(#v), (uint32_t)(sizeof(#v) - 1), (int64_t)v);
#define TRACESTR(v) if (DEBUG) trace((uint32_t)(#v), (uint32_t)(sizeof(#v) - 1), (uint32_t)(v), sizeof(v), 0);

// hook developers should use this guard macro, simply GUARD(<maximum iterations>)
#define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)
#define GUARDM(maxiter, n) _g(( (1ULL << 31U) + (__LINE__ << 16) + n), (maxiter)+1)

#define SBUF(str) (uint32_t)(str), sizeof(str)
#define SVAR(x) &x, sizeof(x)

#define REQUIRE(cond, str)\
{\
    if (!(cond))\
        rollback(SBUF(str), __LINE__);\
}

// make a report buffer as a c-string
// provide a name for a buffer to declare (buf)
// provide a static string
// provide an integer to print after the string
#define RBUF(buf, out_len, str, num)\
unsigned char buf[sizeof(str) + 21];\
int out_len = 0;\
{\
    int i = 0;\
    for (; GUARDM(sizeof(str),1),i < sizeof(str); ++i)\
        (buf)[i] = str[i];\
    if ((buf)[sizeof(str)-1] == 0) i--;\
    if ((num) < 0) (buf)[i++] = '-';\
    uint64_t unsigned_num = (uint64_t)( (num) < 0 ? (num) * -1 : (num) );\
    uint64_t j = 10000000000000000000ULL;\
    int start = 1;\
    for (; GUARDM(20,2), unsigned_num > 0 && j > 0; j /= 10)\
    {\
        unsigned char digit = ( unsigned_num / j ) % 10;\
        if (digit == 0 && start)\
            continue;\
        start = 0;\
        (buf)[i++] = '0' + digit;\
    }\
    (buf)[i] = '\0';\
    out_len = i;\
}

#define RBUF2(buff, out_len, str, num, str2, num2)\
unsigned char buff[sizeof(str) + sizeof(str2) + 42];\
int out_len = 0;\
{\
    unsigned char* buf = buff;\
    int i = 0;\
    for (; GUARDM(sizeof(str),1),i < sizeof(str); ++i)\
        (buf)[i] = str[i];\
    if ((buf)[sizeof(str)-1] == 0) i--;\
    if ((num) < 0) (buf)[i++] = '-';\
    uint64_t unsigned_num = (uint64_t)( (num) < 0 ? (num) * -1 : (num) );\
    uint64_t j = 10000000000000000000ULL;\
    int start = 1;\
    for (; GUARDM(20,2), unsigned_num > 0 && j > 0; j /= 10)\
    {\
        unsigned char digit = ( unsigned_num / j ) % 10;\
        if (digit == 0 && start)\
            continue;\
        start = 0;\
        (buf)[i++] = '0' + digit;\
    }\
    buf += i;\
    out_len += i;\
    i = 0;\
    for (; GUARDM(sizeof(str2),3),i < sizeof(str2); ++i)\
        (buf)[i] = str2[i];\
    if ((buf)[sizeof(str2)-1] == 0) i--;\
    if ((num2) < 0) (buf)[i++] = '-';\
    unsigned_num = (uint64_t)( (num2) < 0 ? (num2) * -1 : (num2) );\
    j = 10000000000000000000ULL;\
    start = 1;\
    for (; GUARDM(20,4), unsigned_num > 0 && j > 0; j /= 10)\
    {\
        unsigned char digit = ( unsigned_num / j ) % 10;\
        if (digit == 0 && start)\
            continue;\
        start = 0;\
        (buf)[i++] = '0' + digit;\
    }\
    (buf)[i] = '\0';\
    out_len += i;\
}

#define CLEARBUF(b)\
{\
    for (int x = 0; GUARD(sizeof(b)), x < sizeof(b); ++x)\
        b[x] = 0;\
}

// returns an in64_t, negative if error, non-negative if valid drops
#define AMOUNT_TO_DROPS(amount_buffer)\
     (((amount_buffer)[0] >> 7) ? -2 : (\
     ((((uint64_t)((amount_buffer)[0])) & 0xb00111111) << 56) +\
      (((uint64_t)((amount_buffer)[1])) << 48) +\
      (((uint64_t)((amount_buffer)[2])) << 40) +\
      (((uint64_t)((amount_buffer)[3])) << 32) +\
      (((uint64_t)((amount_buffer)[4])) << 24) +\
      (((uint64_t)((amount_buffer)[5])) << 16) +\
      (((uint64_t)((amount_buffer)[6])) <<  8) +\
      (((uint64_t)((amount_buffer)[7])))))

#define SUB_OFFSET(x) ((int32_t)(x >> 32))
#define SUB_LENGTH(x) ((int32_t)(x & 0xFFFFFFFFULL))

#define BUFFER_EQUAL_20(buf1, buf2)\
    (\
        *(((uint64_t*)(buf1)) + 0) == *(((uint64_t*)(buf2)) + 0) &&\
        *(((uint64_t*)(buf1)) + 1) == *(((uint64_t*)(buf2)) + 1) &&\
        *(((uint32_t*)(buf1)) + 4) == *(((uint32_t*)(buf2)) + 4))

#define BUFFER_EQUAL_32(buf1, buf2)\
    (\
        *(((uint64_t*)(buf1)) + 0) == *(((uint64_t*)(buf2)) + 0) &&\
        *(((uint64_t*)(buf1)) + 1) == *(((uint64_t*)(buf2)) + 1) &&\
        *(((uint64_t*)(buf1)) + 2) == *(((uint64_t*)(buf2)) + 2) &&\
        *(((uint64_t*)(buf1)) + 3) == *(((uint64_t*)(buf2)) + 3))

#define BUFFER_EQUAL_64(buf1, buf2) \
    ( \
        (*((uint64_t*)(buf1) + 0) == *((uint64_t*)(buf2) + 0)) && \
        (*((uint64_t*)(buf1) + 1) == *((uint64_t*)(buf2) + 1)) && \
        (*((uint64_t*)(buf1) + 2) == *((uint64_t*)(buf2) + 2)) && \
        (*((uint64_t*)(buf1) + 3) == *((uint64_t*)(buf2) + 3)) && \
        (*((uint64_t*)(buf1) + 4) == *((uint64_t*)(buf2) + 4)) && \
        (*((uint64_t*)(buf1) + 5) == *((uint64_t*)(buf2) + 5)) && \
        (*((uint64_t*)(buf1) + 6) == *((uint64_t*)(buf2) + 6)) && \
        (*((uint64_t*)(buf1) + 7) == *((uint64_t*)(buf2) + 7)) \
    )

// when using this macro buf1len may be dynamic but buf2len must be static
// provide n >= 1 to indicate how many times the macro will be hit on the line of code
// e.g. if it is in a loop that loops 10 times n = 10

#define BUFFER_EQUAL_GUARD(output, buf1, buf1len, buf2, buf2len, n)\
{\
    output = ((buf1len) == (buf2len) ? 1 : 0);\
    for (int x = 0; GUARDM( (buf2len) * (n), 1 ), output && x < (buf2len);\
         ++x)\
        output = *(((uint8_t*)(buf1)) + x) == *(((uint8_t*)(buf2)) + x);\
}

#define BUFFER_SWAP(x,y)\
{\
    uint8_t* z = x;\
    x = y;\
    y = z;\
}

#define ACCOUNT_COMPARE(compare_result, buf1, buf2)\
{\
    compare_result = 0;\
    for (int i = 0; GUARD(20), i < 20; ++i)\
    {\
        if (buf1[i] > buf2[i])\
        {\
            compare_result = 1;\
            break;\
        }\
        else if (buf1[i] < buf2[i])\
        {\
            compare_result = -1;\
            break;\
        }\
    }\
}

#define BUFFER_EQUAL_STR_GUARD(output, buf1, buf1len, str, n)\
    BUFFER_EQUAL_GUARD(output, buf1, buf1len, str, (sizeof(str)-1), n)

#define BUFFER_EQUAL_STR(output, buf1, buf1len, str)\
    BUFFER_EQUAL_GUARD(output, buf1, buf1len, str, (sizeof(str)-1), 1)

#define BUFFER_EQUAL(output, buf1, buf2, compare_len)\
    BUFFER_EQUAL_GUARD(output, buf1, compare_len, buf2, compare_len, 1)


#define UINT8_TO_BUF(buf_raw, i)\
{\
    unsigned char* buf = (unsigned char*)buf_raw;\
    buf[0] = (((uint8_t)i) >> 0) & 0xFFUL;\
    if (i < 0) buf[0] |= 0x80U;\
}

#define UINT8_FROM_BUF(buf)\
    (((uint8_t)((buf)[0]) <<  0))


#define UINT16_TO_BUF(buf_raw, i)\
{\
    unsigned char* buf = (unsigned char*)buf_raw;\
    buf[0] = (((uint64_t)i) >> 8) & 0xFFUL;\
    buf[1] = (((uint64_t)i) >> 0) & 0xFFUL;\
}

#define UINT16_FROM_BUF(buf)\
    (((uint64_t)((buf)[0]) <<  8) +\
     ((uint64_t)((buf)[1]) <<  0))

#define UINT32_TO_BUF(buf_raw, i)\
{\
    unsigned char* buf = (unsigned char*)buf_raw;\
    buf[0] = (((uint64_t)i) >> 24) & 0xFFUL;\
    buf[1] = (((uint64_t)i) >> 16) & 0xFFUL;\
    buf[2] = (((uint64_t)i) >>  8) & 0xFFUL;\
    buf[3] = (((uint64_t)i) >>  0) & 0xFFUL;\
}

#define UINT32_FROM_BUF(buf)\
    (((uint64_t)((buf)[0]) << 24) +\
     ((uint64_t)((buf)[1]) << 16) +\
     ((uint64_t)((buf)[2]) <<  8) +\
     ((uint64_t)((buf)[3]) <<  0))

#define UINT64_TO_BUF(buf_raw, i)\
{\
    unsigned char* buf = (unsigned char*)buf_raw;\
    buf[0] = (((uint64_t)i) >> 56) & 0xFFUL;\
    buf[1] = (((uint64_t)i) >> 48) & 0xFFUL;\
    buf[2] = (((uint64_t)i) >> 40) & 0xFFUL;\
    buf[3] = (((uint64_t)i) >> 32) & 0xFFUL;\
    buf[4] = (((uint64_t)i) >> 24) & 0xFFUL;\
    buf[5] = (((uint64_t)i) >> 16) & 0xFFUL;\
    buf[6] = (((uint64_t)i) >>  8) & 0xFFUL;\
    buf[7] = (((uint64_t)i) >>  0) & 0xFFUL;\
}

#define UINT64_FROM_BUF(buf)\
    (((uint64_t)((buf)[0]) << 56) +\
     ((uint64_t)((buf)[1]) << 48) +\
     ((uint64_t)((buf)[2]) << 40) +\
     ((uint64_t)((buf)[3]) << 32) +\
     ((uint64_t)((buf)[4]) << 24) +\
     ((uint64_t)((buf)[5]) << 16) +\
     ((uint64_t)((buf)[6]) <<  8) +\
     ((uint64_t)((buf)[7]) <<  0))

#define INT64_TO_BUF(buf_raw, i)\
{\
    unsigned char* buf = (unsigned char*)buf_raw;\
    buf[0] = (((uint64_t)i) >> 56) & 0x7FUL;\
    buf[1] = (((uint64_t)i) >> 48) & 0xFFUL;\
    buf[2] = (((uint64_t)i) >> 40) & 0xFFUL;\
    buf[3] = (((uint64_t)i) >> 32) & 0xFFUL;\
    buf[4] = (((uint64_t)i) >> 24) & 0xFFUL;\
    buf[5] = (((uint64_t)i) >> 16) & 0xFFUL;\
    buf[6] = (((uint64_t)i) >>  8) & 0xFFUL;\
    buf[7] = (((uint64_t)i) >>  0) & 0xFFUL;\
    if (i < 0) buf[0] |= 0x80U;\
}

#define INT64_FROM_BUF(buf)\
   ((((uint64_t)((buf)[0] & 0x7FU) << 56) +\
     ((uint64_t)((buf)[1]) << 48) +\
     ((uint64_t)((buf)[2]) << 40) +\
     ((uint64_t)((buf)[3]) << 32) +\
     ((uint64_t)((buf)[4]) << 24) +\
     ((uint64_t)((buf)[5]) << 16) +\
     ((uint64_t)((buf)[6]) <<  8) +\
     ((uint64_t)((buf)[7]) <<  0)) * (buf[0] & 0x80U ? -1 : 1))


#define BYTES20_TO_BUF(buf_raw, i)\
{\
    unsigned char* buf = (unsigned char*)buf_raw;\
    *(uint64_t*)(buf + 0) = *(uint64_t*)(i +  0);\
    *(uint64_t*)(buf + 8) = *(uint64_t*)(i +  8);\
    *(uint32_t*)(buf + 16) = *(uint32_t*)(i + 16);\
}

#define BYTES20_FROM_BUF(buf_raw, i)\
{\
    const unsigned char* buf = (const unsigned char*)buf_raw;\
    *(uint64_t*)(i +  0) = *(const uint64_t*)(buf + 0);\
    *(uint64_t*)(i +  8) = *(const uint64_t*)(buf + 8);\
    *(uint32_t*)(i + 16) = *(const uint32_t*)(buf + 16);\
}

#define BYTES32_TO_BUF(buf_raw, i)\
{\
    unsigned char* buf = (unsigned char*)buf_raw;\
    *(uint64_t*)(buf + 0) = *(uint64_t*)(i +  0);\
    *(uint64_t*)(buf + 8) = *(uint64_t*)(i +  8);\
    *(uint64_t*)(buf + 16) = *(uint64_t*)(i + 16);\
    *(uint64_t*)(buf + 24) = *(uint64_t*)(i + 24);\
}

#define BYTES32_FROM_BUF(buf_raw, i)\
{\
    const unsigned char* buf = (const unsigned char*)buf_raw;\
    *(uint64_t*)(i +  0) = *(const uint64_t*)(buf + 0);\
    *(uint64_t*)(i +  8) = *(const uint64_t*)(buf + 8);\
    *(uint64_t*)(i + 16) = *(const uint64_t*)(buf + 16);\
    *(uint64_t*)(i + 24) = *(const uint64_t*)(buf + 24);\
}

#define FLIP_ENDIAN_32(n) ((uint32_t) (((n & 0xFFU) << 24U) | \
                                   ((n & 0xFF00U) << 8U) | \
                                 ((n & 0xFF0000U) >> 8U) | \
                                ((n & 0xFF000000U) >> 24U)))

#define FLIP_ENDIAN_64(n) ((uint64_t)(((n & 0xFFULL) << 56ULL) |             \
                                      ((n & 0xFF00ULL) << 40ULL) |           \
                                      ((n & 0xFF0000ULL) << 24ULL) |         \
                                      ((n & 0xFF000000ULL) << 8ULL) |        \
                                      ((n & 0xFF00000000ULL) >> 8ULL) |      \
                                      ((n & 0xFF0000000000ULL) >> 24ULL) |   \
                                      ((n & 0xFF000000000000ULL) >> 40ULL) | \
                                      ((n & 0xFF00000000000000ULL) >> 56ULL)))

#define tfCANONICAL 0x80000000UL

#define atACCOUNT 1U
#define atOWNER 2U
#define atDESTINATION 3U
#define atISSUER 4U
#define atAUTHORIZE 5U
#define atUNAUTHORIZE 6U
#define atTARGET 7U
#define atREGULARKEY 8U
#define atPSEUDOCALLBACK 9U

#define amAMOUNT 1U
#define amBALANCE 2U
#define amLIMITAMOUNT 3U
#define amTAKERPAYS 4U
#define amTAKERGETS 5U
#define amLOWLIMIT 6U
#define amHIGHLIMIT 7U
#define amFEE 8U
#define amSENDMAX 9U
#define amDELIVERMIN 10U
#define amMINIMUMOFFER 16U
#define amRIPPLEESCROW 17U
#define amDELIVEREDAMOUNT 18U

#endif


