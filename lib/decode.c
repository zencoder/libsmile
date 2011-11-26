/*
 * Copyright 2011 Pierre-Alexandre Meyer
 *
 * Pierre-Alexandre Meyer licenses this file to you under the Apache License,
 * version 2.0 (the "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at:
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * State machine model inspired from zlib implementation: http://zlib.net/
 */

#include "decode.h"

#ifdef DEBUG
#define DEBUG_STREAM_BYTES(n) \
    do { \
        debug_bits = 0; \
        while (debug_bits < n) { \
            printf("DEBUG: [%d] %c (0x%lx) (%d|%d)\n", state->mode, \
                   (char) (debug_bits == 0 ? hold : (hold >> (8 * debug_bits))) & 0xff, (hold >> 8*n) & 0xff, have + (n - debug_bits + 1), left); \
            debug_bits++; \
        }; \
    } while (0)

#define DEBUG_OUTPUT() \
    do { \
        nb_out = 0; \
        while (nb_out < strm->state->total) { \
          putchar(*(strm->next_out + nb_out)); \
          nb_out++; \
        } \
    } while(0);

#define DEBUG_HEADER() \
    do { \
        if (strm->state->hdr.valid) { \
            fputs("DEBUG: valid header, ", stdout); \
        } else { \
            fputs("DEBUG: invalid header, ", stdout); \
        } \
        printf("version [0x%u], ", strm->state->hdr.version); \
        if (strm->state->hdr.raw_binary) { \
            fputs("raw binary values present, ", stdout); \
        } else { \
            fputs("raw binary values absent, ", stdout); \
        } \
        if (strm->state->hdr.shared_key_names) { \
            fputs("shared key names, ", stdout); \
        } else { \
            fputs("non-shared key names, ", stdout); \
        } \
        if (strm->state->hdr.shared_value_names) { \
            fputs("shared values\n", stdout); \
        } else { \
            fputs("non-shared values\n", stdout); \
        } \
    } while (0)
#else
#define DEBUG_OUTPUT()
#define DEBUG_STREAM(n)
#define DEBUG_HEADER()
#endif

/* Load registers with state for speed */
#define LOAD() \
    do { \
        put = strm->next_out; \
        left = strm->avail_out; \
        next = strm->next_in; \
        have = strm->avail_in; \
        hold = state->hold; \
        bits = state->bits; \
    } while (0)

/* Restore state from registers */
#define RESTORE() \
    do { \
        strm->next_out = put; \
        strm->avail_out = left; \
        strm->next_in = next; \
        strm->avail_in = have; \
        state->hold = hold; \
        state->bits = bits; \
    } while (0)

/* Clear the input bit accumulator */
#define INITBITS() \
    do { \
        hold = 0; \
        bits = 0; \
    } while (0)

/* Get a byte of input into the bit accumulator, or return
   if there is no input available. */
#define PULLBYTE() \
    do { \
        if (have == 0) goto out; \
        have--; \
        hold += (unsigned long)(*next++) << bits; \
        bits += 8; \
    } while (0)

#define NEEDBYTE()      NEEDBITS(8)
#define NEEDBYTES(b)    NEEDBITS((b << 3))

/* Assure that there are at least n bits in the bit accumulator.  If there is
   not enough available input to do that, then return */
#define NEEDBITS(n) \
    do { \
        while (bits < (unsigned)(n)) \
            PULLBYTE(); \
    } while (0)

#define BYTE()  BITS(8)

/* Return the low n bits of the bit accumulator (n < 16) */
#define BITS(n) \
    ((unsigned)hold & ((1U << (n)) - 1))

#define DROPBYTE()  DROPBITS(8)

/* Remove n bits from the bit accumulator */
#define DROPBITS(n) \
    do { \
        hold >>= (n); \
        bits -= (unsigned)(n); \
    } while (0)

/* Remove zero to seven bits as needed to go to a byte boundary */
#define BYTEBITS() \
    do { \
        hold >>= bits & 7; \
        bits -= bits & 7; \
    } while (0)

/* Reverse the bytes in a 32-bit value */
#define REVERSE(q) \
    ((((q) >> 24) & 0xff) + (((q) >> 8) & 0xff00) + \
     (((q) & 0xff00) << 8) + (((q) & 0xff) << 24))

// TODO make it re-entrant (check space left)
#define COPY(s) \
    do { \
        strncpy(put, s, strlen(s)); \
        state->total += strlen(s); \
        put += strlen(s); \
        left -= strlen(s); \
    } while (0)

#define COPY_BUFFER(l) \
    do { \
        strncpy(put, next, l); \
        state->total += l; \
        next += l; \
        put += l; \
        left -= l; \
    } while (0)

#define COPY_VARIABLE_LENGTH_STRING() \
    do { \
        LOOK_FOR_STRING_LENGTH(); \
        COPY("\""); \
        COPY_BUFFER(smile_value_length); \
        COPY("\""); \
        /* Drop end-of-string marker */ \
        next++; \
        left--; \
    } while(0)

#define LOOK_FOR_STRING_LENGTH() \
    do { \
        smile_value_length = 0; \
        while (*(next + smile_value_length) != 0xFC) { \
            smile_value_length++; \
        } \
    } while(0)

#define ERROR_REPORT(prefix, msg, error) \
    do { \
        /* TODO Should go in strm->msg */ \
        fprintf(stderr, "%s: %s\n", prefix, msg); \
        state->mode = BAD; \
        ret = error; \
        goto out; \
    } while (0)

#define ERROR(s)            ERROR_REPORT("ERROR", s, -30)
#define RESERVED(s)         ERROR_REPORT("RESERVED", s, -20)
#define NOT_IMPLEMENTED(s)  ERROR_REPORT("NOT IMPLEMENTED", s, -10)

#define LOOKUP_SHORT_SHARED_KEY() state->keys_tables[BITS(8) - 0x40]
#define LOOKUP_SHORT_SHARED_VALUE() state->values_tables[BITS(10) - 1]

#define SAVE_KEY_STRING(l) \
    do { \
        state->max_keys_ref_value++; \
        strncpy(state->keys_tables[state->max_keys_ref_value], next, l); \
        state->keys_tables[state->max_keys_ref_value][l] = '\0'; \
    } while(0)

#define SAVE_AND_COPY_KEY_STRING() \
    do { \
        SAVE_KEY_STRING(smile_key_length); \
        COPY("\""); \
        COPY_BUFFER(smile_key_length); \
        COPY("\":"); \
    } while(0)

#define SAVE_VALUE_STRING(l) \
    do { \
        state->max_values_ref_value++; \
        strncpy(state->values_tables[state->max_values_ref_value], next, l); \
        state->values_tables[state->max_values_ref_value][l] = '\0'; \
    } while(0)

#define SAVE_AND_COPY_VALUE_STRING() \
    do { \
        SAVE_VALUE_STRING(smile_value_length); \
        COPY("\""); \
        COPY_BUFFER(smile_value_length); \
        COPY("\""); \
    } while(0)

int smile_decode(s_stream *strm)
{
    unsigned char *curr;
    struct decode_state *state;
    const unsigned char *next;  /* next input */
    unsigned char *put;         /* next output */
    unsigned int have, left;    /* available input and output */
    unsigned long hold;         /* bit buffer */
    unsigned int bits;          /* bits in bit buffer */
    unsigned int bytes;
#ifdef DEBUG
    unsigned int debug_bits;
    int nb_out;
#endif
    unsigned int in, out;       /* save starting available input and output */
    unsigned int copy;          /* number of stored or match bytes to copy */
    unsigned char *from;        /* where to copy match bytes from */
    unsigned int len;           /* length to copy for repeats, bits to drop */
    int ret = 0;                /* return code */

    int smile_key_length;
    int smile_value_length;
    short smile_value_lookup;

    if (strm == NULL || strm->state == NULL ||
        (strm->next_in == NULL && strm->avail_in != 0)) {
        return -1;
    }

    state = (struct decode_state*)strm->state;
    LOAD();
    in = have;
    out = left;
    ret = 0;

    for (;;) {
        switch(state->mode) {
        case HEAD:
            NEEDBITS(32);

            if ((hold & 0xff) != ':' ||
                ((hold >> 8) & 0xff) != ')'||
                ((hold >> 16) & 0xff) != '\n') {
                strm->msg = (char *)"incorrect header";
                state->mode = BAD;
                break;
            }
            state->mode = ROOT;

            // Header verified
            state->hdr.valid = true;
            // 0x00 for current version
            state->hdr.version = (BYTE() & 0xF0);
            // Whether raw binary (unescaped 8-bit) values may be present in content
            state->hdr.raw_binary = (BYTE() & 0x04) >> 2;
            // Shared String key
            state->hdr.shared_key_names = (BYTE() & 0x01);
            // Shared String value
            state->hdr.shared_value_names = (BYTE() & 0x02) >> 1;
            DEBUG_HEADER();

            INITBITS();
            break;
        // Value is the default mode for tokens for main-level ("root") output context and JSON Array context.
        // It is also used between JSON Object property name tokens.
        case ROOT:
        case ARRAY:
        case VALUE:
            // Tokens are divided in 8 classes, class defined by 3 MSB of the first byte:
            NEEDBYTE();

            // TODO Kludge for now
            if (state->in_array) {
                if (state->first_array_element) {
                    state->first_array_element = false;
                } else if (BYTE() != 0xF9) {
                    COPY(",");
                }
            }

            if (BYTE() >= 0x01 && BYTE() <= 0x1f) {
                // 10 bit lookup
                NEEDBITS(2);

                COPY("\"");
                COPY(LOOKUP_SHORT_SHARED_VALUE());
                COPY("\"");
            } else if (BYTE() >= 0x20 && BYTE() <= 0x23) {
                // Simple literals, numbers
                if (BYTE() == 0x20) {
                    // Empty String
                    COPY("\"\"");
                } else if (BYTE() == 0x21) {
                    // null
                } else if (BYTE() == 0x22) {
                    // false
                    COPY("false");
                } else if (BYTE() == 0x23) {
                    // true
                    COPY("true");
                }
            } else if (BYTE() >= 0x24 && BYTE() <= 0x27) {
                // Integral numbers
                NOT_IMPLEMENTED("value integral numbers");
                smile_value_length = (BYTE() & 0x03);

                if (smile_value_length == 0) {
                    // 32-bit
                } else if (smile_value_length == 1) {
                    // 64-bit
                } else if (smile_value_length == 2) {
                    // BigInteger
                } else {
                    // Reserved for future use
                    RESERVED("integral numbers with length >= 3");
                }
            } else if (BYTE() >= 0x28 && BYTE() <= 0x2B) {
                // Floating point numbers
                NOT_IMPLEMENTED("value fp");
            } else if (BYTE() >= 0x2C && BYTE() <= 0x3F) {
                // Reserved for future use
                RESERVED("0x2C <= value <= 0x3F");
            } else if (BYTE() >= 0x40 && BYTE() <= 0x5F) {
                // Tiny ASCII
                // 5 LSB used to indicate lengths from 2 to 32 (bytes == chars)
                smile_value_length = (BYTE() & 0x1F) + 1;
                SAVE_AND_COPY_VALUE_STRING();
            } else if (BYTE() >= 0x60 && BYTE() <= 0x7F) {
                // Small ASCII
                // 5 LSB used to indicate lengths from 33 to 64 (bytes == chars)
                smile_value_length = (BYTE() & 0x1F) + 33;
                SAVE_AND_COPY_VALUE_STRING();
            } else if (BYTE() >= 0x80 && BYTE() <= 0x9F) {
                // Tiny Unicode
                // 5 LSB used to indicate _byte_ lengths from 2 to 33
                smile_value_length = (BYTE() & 0x1F) + 1;
                SAVE_AND_COPY_VALUE_STRING();
            } else if (BYTE() >= 0xA0 && BYTE() <= 0xBF) {
                // Small Unicode
                NOT_IMPLEMENTED("value small unicode");
                // 5 LSB used to indicate _byte_ lengths from 34 to 65
                smile_value_length = (BYTE() & 0x1F) + 33;
                SAVE_AND_COPY_VALUE_STRING();
            } else if (BYTE() >= 0xC0 && BYTE() <= 0xDF) {
                // Small integers
                NOT_IMPLEMENTED("value small int");
            } else {
                // Misc; binary / text / structure markers
                if (BYTE() >= 0xE0 && BYTE() < 0xE4) {
                    // Long (variable length) ASCII text
                    COPY_VARIABLE_LENGTH_STRING();
                } else if (BYTE() >= 0xE4 && BYTE() < 0xE8) {
                    NOT_IMPLEMENTED("value long unicode");
                } else if (BYTE() >= 0xE8 && BYTE() < 0xEC) {
                    NOT_IMPLEMENTED("value long shared string reference");
                } else if (BYTE() >= 0xEC && BYTE() < 0xF8) {
                    // Binary, 7-bit encoded
                    NOT_IMPLEMENTED("value binary");
                } else if (BYTE() == 0xF8) {
                    // START_ARRAY
                    COPY("[");
                    state->in_array = true;
                } else if (BYTE() == 0xF9) {
                    // END_ARRAY
                    COPY("]");
                    state->in_array = false;
                } else if (BYTE() == 0xFA) {
                    // START_OBJECT
                    COPY("{");
                    state->first_key = true;
                } else if (BYTE() == 0xFB) {
                    // Reserved
                    RESERVED("value == 0xFB");
                } else {
                    NOT_IMPLEMENTED("value >= 0xFC");
                }
            }
            if (!state->in_array) {
                state->mode = KEY;
            }
            INITBITS();
            break;
        case KEY:
            NEEDBYTE();

            // TODO Kludge for now
            if (state->first_key) {
                state->first_key = false;
            } else if (BYTE() != 0xFB) {
                COPY(",");
            }

            // Byte ranges are divided in 4 main sections (64 byte values each)
            if (BYTE() >= 0x01 && BYTE() <= 0x1F) {
                // Reserved for future use
                RESERVED("0x01 <= key <= 0x1F");
            } else if (BYTE() == 0x20) {
                // Empty String
                COPY("\"\"");
            } else if (BYTE() >= 0x21 && BYTE() <= 0x2F) {
                // Reserved for future use
                RESERVED("0x21 <= key <= 0x2F");
            } else if (BYTE() >= 0x30 && BYTE() <= 0x33) {
                // "Long" shared key name reference
                NOT_IMPLEMENTED("long shared key name reference");
            } else if (BYTE() == 0x32) {
                // Long (not-yet-shared) Unicode name, 64 bytes or more
                NOT_IMPLEMENTED("long key name");
            } else if (BYTE() >= 0x35 && BYTE() <= 0x39) {
                // Reserved for future use
                RESERVED("0x35 <= key <= 0x39");
            } else if (BYTE() == 0x3A) {
                // Error
                ERROR("Error decoding key, 0x3A NOT allowed in Key mode");
            } else if (BYTE() >= 0x3B && BYTE() <= 0x3F) {
                // Reserved for future use
                RESERVED("0x3B <= key <= 0x3F");
            } else if (BYTE() >= 0x40 && BYTE() <= 0x7F) {
                // "Short" shared key name reference (1 byte lookup)
                COPY("\"");
                COPY(LOOKUP_SHORT_SHARED_KEY());
                COPY("\":");
            } else if (BYTE() >= 0x80 && BYTE() <= 0xBF) {
                // Short Ascii names
                // 5 LSB used to indicate lengths from 2 to 32 (bytes == chars)
                smile_key_length = (BYTE() & 0x1F) + 1;
                SAVE_AND_COPY_KEY_STRING();
            } else if (BYTE() >= 0xC0 && BYTE() <= 0xF7) {
                // Short Unicode names
                // 5 LSB used to indicate lengths from 2 to 57
                NOT_IMPLEMENTED("key short unicode");
                smile_key_length = (BYTE() - 0xC0) + 2;
                SAVE_AND_COPY_KEY_STRING();
            } else if (BYTE() >= 0xF8 && BYTE() <= 0xFA) {
                // Reserved
                RESERVED("0xF8 <= key <= 0xFA");
            } else if (BYTE() == 0xFB) {
                // END_OBJECT marker
                COPY("}");
            } else if (BYTE() >= 0xFC) {
                // Reserved
                RESERVED("key >= 0xFC");
            }
            state->mode = VALUE;
            INITBITS();
            break;
        case BAD:
            ret = -1;
            goto out;
        DONE:
            goto out;
        default:
            strm->msg = (char *)"shouldn't get here";
            state->mode = BAD;
            goto out;
        }
    }

out:
    RESTORE();

    in -= strm->avail_in;
    out -= strm->avail_out;
    strm->total_in += in;
    strm->total_out += out;
    state->total += out;

    return ret;
}

void smile_decode_init(s_stream *strm)
{
    strm->state = &WS(strm)->decode_state;

    // Default Smile header: shared keys are enabled, but not values
    strm->state->hdr.raw_binary = true;
    strm->state->hdr.shared_key_names = true;

    // Initialize lookup
    strm->state->max_keys_ref_value = -1;
    strm->state->max_values_ref_value = -1;

    strm->state->in_array = false;

    // Printing
    strm->state->first_key = true;
    strm->state->first_array_element = true;
}
