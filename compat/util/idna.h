/* Copyright libuv contributors. All rights reserved.
 * commitid: 3530bcc30350d4a6ccf35d2f7b33e23292b9de70
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef UV_SRC_IDNA_H_
#define UV_SRC_IDNA_H_

/* Decode a single codepoint. Returns the codepoint or UINT32_MAX on error.
 * |p| is updated on success _and_ error, i.e., bad multi-byte sequences are
 * skipped in their entirety, not just the first bad byte.
 */
unsigned uv__utf8_decode1(const char** p, const char* pe);

/* Convert a UTF-8 domain name to IDNA 2008 / Punycode. A return value >= 0
 * is the number of bytes written to |d|, including the trailing nul byte.
 * A return value < 0 is a libuv error code. |s| and |d| can not overlap.
 */
ssize_t uv__idna_toascii(const char* s, const char* se, char* d, char* de);


/* String utilities needed internally for dealing with Windows. */
size_t uv_utf16_length_as_wtf8(const uint16_t* utf16,
                               ssize_t utf16_len);
int uv_utf16_to_wtf8(const uint16_t* utf16,
                     ssize_t utf16_len,
                     char** wtf8_ptr,
                     size_t* wtf8_len_ptr);
ssize_t uv_wtf8_length_as_utf16(const char* wtf8);
void uv_wtf8_to_utf16(const char* wtf8,
                      uint16_t* utf16,
                      size_t utf16_len);
int uv__copy_utf16_to_utf8(const uint16_t* utf16buffer, size_t utf16len, char* utf8, size_t *size);
#endif  /* UV_SRC_IDNA_H_ */
