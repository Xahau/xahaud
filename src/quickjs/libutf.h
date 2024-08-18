/* LIBrary of UTF-(8,16,32) helper utility functions.
 *
 * Terminology:
 * code point = number of a Unicode character
 * code unit
 *   = 1 byte  for UTF-8
 *   = 2 bytes for UTF-16
 *   = 4 bytes for UTF-32
 * sequence = sequence of code units encoding SINGLE Unicode character
 *
 * Unicode has code points 0x000000..0x10FFFF except for the codes 0xD800..0xDFFF which are
 * reserved for UTF-16 surrogate pairs.
 *
 * UTF-32 is the simplest Unicode encoding. It encodes Unicode code points as is. Thus we may use
 * UTF-32 as a synonym for Unicode code point.
 * + O(1) indexing
 * - the text is 2-4 times bigger than in other encodings
 *
 * UTF-16 is variable width encoding. Each Unicode code point is represented as either single
 * code unit (two-byte number) or two code units which are called surrogate pairs.
 * - ASCII-only texts will be twice as big as in ASCII
 * - O(n) indexing
 *
 * UTF-8 is a variable width encoding. Each Unicode code point is represented as a sequence
 * of up to 4 bytes. The first byte determines the length of the sequence.
 * + is a superset of ASCII
 * + ASCII-only texts will have the same size
 * - O(n) indexing
 * - single Unicode code point can have different representations in
 *   UTF-8, only the shortest one is considered valid
 */

#ifndef LIBUTF_H
#define LIBUTF_H

#include <stdbool.h>
#include <stdint.h>

// UTF-8 code unit type
typedef enum {
    LIBUTF_UTF8_OVERLONG   = -2,
    LIBUTF_UTF8_TRAILING   = -1,
    LIBUTF_UTF8_ONE_BYTE   = 1,
    LIBUTF_UTF8_TWO_BYTE   = 2,
    LIBUTF_UTF8_THREE_BYTE = 3,
    LIBUTF_UTF8_FOUR_BYTE  = 4,
} LibutfC8Type;

// UTF-16 code unit type
typedef enum {
    LIBUTF_UTF16_SURROGATE_LOW  = -1,
    LIBUTF_UTF16_NOT_SURROGATE  = 1,
    LIBUTF_UTF16_SURROGATE_HIGH = 2,
} LibutfC16Type;

/* Determine type of UTF-16 code unit. type = length of UTF-16 sequence starting with the specified code unit
 * or negative value if sequence cannot start with this code unit. */
LibutfC16Type libutf_c16_type(uint_least16_t c16);

/* Determine type of UTF-8 sequence based on the first byte. type = length of UTF-8 sequence starting
 * with this byte or negative value if the byte cannot be first. UTF-8 may be up to 4 bytes long.
 * Common idiom for using this function:
 * >>> LibutfC8Type type = libutf_c8_type(c);
 * >>> if (type < 0) {
 * >>>     return ERROR_CODE;
 * >>> }
 * >>> int length = type;
 * */
LibutfC8Type libutf_c8_type(char c);

/* Convert Unicode code point into UTF-8 sequence. If c32 is not a valid
 * Unicode code point c8 will be filled with UTF-8 representation of special
 * replacement character U+FFFD, *length will be set to its length and false will be returned.
 * c32    -- UTF-32 Unicode code point
 * length -- where to put length of the UTF-8 sequence.
 * c8     -- where to put UTF-8 sequence. Make sure string has enough space.
 * result -- true if c32 is a valid Unicode code point or false otherwise. */
bool libutf_c32_to_c8(uint_least32_t c32, int *length, char c8[4]);

/* Convert UTF-8 sequence into a UTF-32 Unicode code point. If c8 does not
 * point to a valid UTF-8 sequence c32 will be filled with special replacement
 * character U+FFFD and false will be returned.
 * c8     -- pointer to UTF-8 sequence.
 * c32    -- where to save UTF-32 Unicode code point.
 * result -- true if c8 points to a valid UTF-8 sequence or false otherwise. */
bool libutf_c8_to_c32(const char *c8, uint_least32_t *c32);

/* Convert UTF-32 Unicode code point into UTF-16 sequence. If c32 is not a valid
 * Unicode code point c16 will be filled with UTF-16 representation of special
 * replacement character U+FFFD and false will be returned.
 * c32    -- UTF-32 Unicode code point.
 * length -- were to put length of UTF-16 sequence.
 * c16    -- where to put UTF-16 sequence (c16[0] -- high surrogate, c16[1] -- low surrogate)
 * result -- true if c32 is a valid Unicode code point or false otherwise. */
bool libutf_c32_to_c16(uint_least32_t c32, int *length, uint_least16_t c16[2]);

/* Construct UTF-32 Unicode code point from UTF-16 surrogate pair. high
 * must be high surrogate and low must be low surrogate otherwise *c32 will
 * be filled with special replacement character U+FFFD and false will be returned.
 * c16    -- where to put UTF-16 sequence (c32[0] -- high surrogate, c32[1] -- low surrogate)
 * result -- true if c16 points to a valid UTF-16 sequence (single not surrogate
 * or valid surrogate pair) or false otherwise. */
bool libutf_c16_to_c32(uint_least16_t c16[2], uint_least32_t *c32);

/* Check whether given value is a valid Unicode code point i.e. below 0x110000 and is not
 * a UTF-16 surrogate. */
bool libutf_c32_is_valid(uint_least32_t c32);

#endif
