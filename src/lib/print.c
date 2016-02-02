/*
 * print.c	Routines to print stuff.
 *
 * Version:	$Id$
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 */

RCSID("$Id$")

#include	<freeradius-devel/libradius.h>

#include	<ctype.h>

/** Checks for utf-8, taken from http://www.w3.org/International/questions/qa-forms-utf-8
 *
 * @param str input string.
 * @param inlen length of input string.  May be -1 if str is \0 terminated.
 */
int fr_utf8_char(uint8_t const *str, ssize_t inlen)
{
	if (inlen == 0) return 0;

	if (inlen < 0) inlen = 4;	/* longest char */

	if (*str < 0x20) return 0;

	if (*str <= 0x7e) return 1;	/* 1 */

	if (*str <= 0xc1) return 0;

	if (inlen < 2) return 0;

	if ((str[0] >= 0xc2) &&		/* 2 */
	    (str[0] <= 0xdf) &&
	    (str[1] >= 0x80) &&
	    (str[1] <= 0xbf)) {
		return 2;
	}

	if (inlen < 3) return 0;

	if ((str[0] == 0xe0) &&		/* 3 */
	    (str[1] >= 0xa0) &&
	    (str[1] <= 0xbf) &&
	    (str[2] >= 0x80) &&
	    (str[2] <= 0xbf)) {
		return 3;
	}

	if ((str[0] >= 0xe1) &&		/* 4a */
	    (str[0] <= 0xec) &&
	    (str[1] >= 0x80) &&
	    (str[1] <= 0xbf) &&
	    (str[2] >= 0x80) &&
	    (str[2] <= 0xbf)) {
		return 3;
	}

	if ((str[0] >= 0xee) &&		/* 4b */
	    (str[0] <= 0xef) &&
	    (str[1] >= 0x80) &&
	    (str[1] <= 0xbf) &&
	    (str[2] >= 0x80) &&
	    (str[2] <= 0xbf)) {
		return 3;
	}

	if ((str[0] == 0xed) &&		/* 5 */
	    (str[1] >= 0x80) &&
	    (str[1] <= 0x9f) &&
	    (str[2] >= 0x80) &&
	    (str[2] <= 0xbf)) {
		return 3;
	}

	if (inlen < 4) return 0;

	if ((str[0] == 0xf0) &&		/* 6 */
	    (str[1] >= 0x90) &&
	    (str[1] <= 0xbf) &&
	    (str[2] >= 0x80) &&
	    (str[2] <= 0xbf) &&
	    (str[3] >= 0x80) &&
	    (str[3] <= 0xbf)) {
		return 4;
	}

	if ((str[0] >= 0xf1) &&		/* 6 */
	    (str[1] <= 0xf3) &&
	    (str[1] >= 0x80) &&
	    (str[1] <= 0xbf) &&
	    (str[2] >= 0x80) &&
	    (str[2] <= 0xbf) &&
	    (str[3] >= 0x80) &&
	    (str[3] <= 0xbf)) {
		return 4;
	}


	if ((str[0] == 0xf4) &&		/* 7 */
	    (str[1] >= 0x80) &&
	    (str[1] <= 0x8f) &&
	    (str[2] >= 0x80) &&
	    (str[2] <= 0xbf) &&
	    (str[3] >= 0x80) &&
	    (str[3] <= 0xbf)) {
		return 4;
	}

	/*
	 *	Invalid UTF-8 Character
	 */
	return 0;
}

/** Return a pointer to the first UTF8 char in a string.
 *
 * @param[out] chr_len Where to write the length of the multibyte char passed in chr (may be NULL).
 * @param[in] str Haystack.
 * @param[in] chr Multibyte needle.
 * @return
 *	- Position of chr in str.
 *	- NULL if not found.
 */
char const *fr_utf8_strchr(int *chr_len, char const *str, char const *chr)
{
	int cchr;

	cchr = fr_utf8_char((uint8_t const *)chr, -1);
	if (cchr == 0) cchr = 1;
	if (chr_len) *chr_len = cchr;

	while (*str) {
		int schr;

		schr = fr_utf8_char((uint8_t const *) str, -1);
		if (schr == 0) schr = 1;
		if (schr != cchr) goto next;

		if (memcmp(str, chr, schr) == 0) {
			return (char const *) str;
		}
	next:
		str += schr;
	}

	return NULL;
}

/** Escape any non printable or non-UTF8 characters in the input string
 *
 * @note Return value should be checked with is_truncated
 * @note Will always \0 terminate unless outlen == 0.
 *
 * @param[in] in string to escape.
 * @param[in] inlen length of string to escape (lets us deal with embedded NULs)
 * @param[out] out where to write the escaped string.
 * @param[out] outlen the length of the buffer pointed to by out.
 * @param[in] quote the quotation character
 * @return
 *	- The number of bytes written to the out buffer.
 *	- A number >= outlen if truncation has occurred.
 */
size_t fr_snprint(char *out, size_t outlen, char const *in, ssize_t inlen, char quote)
{
	uint8_t const	*p = (uint8_t const *) in;
	size_t		utf8;
	size_t		used;
	size_t		freespace;

	/* No input, so no output... */
	if (!in) {
		if (out && outlen) *out = '\0';
		return 0;
	}

	/* Figure out the length of the input string */
	if (inlen < 0) inlen = strlen(in);

	/*
	 *	No quotation character, just use memcpy, ensuring we
	 *	don't overflow the output buffer.
	 */
	if (!quote) {
		if (!out) return inlen;

		if ((size_t)inlen >= outlen) {
			memcpy(out, in, outlen - 1);
			out[outlen - 1] = '\0';
		} else {
			memcpy(out, in, inlen);
			out[inlen] = '\0';
		}

		return inlen;
	}

	/*
	 *	Check the output buffer and length.  Zero both of them
	 *	out if either are zero.
	 */
	freespace = outlen;
	if (freespace == 0) out = NULL;
	if (!out) freespace = 0;

	used = 0;

	while (inlen > 0) {
		int sp = 0;

		/*
		 *	Always escape the quotation character.
		 */
		if (*p == quote) {
			sp = quote;
			goto do_escape;
		}

		/*
		 *	Escape the backslash ONLY for single quoted strings.
		 */
		if (quote == '\'') {
			if (*p == '\\') {
				sp = '\\';
			}
			goto do_escape;
		}

		/*
		 *	Try to convert 0x0a --> \r, etc.
		 *	Backslashes get handled specially.
		 */
		switch (*p) {
		case '\r':
			sp = 'r';
			break;

		case '\n':
			sp = 'n';
			break;

		case '\t':
			sp = 't';
			break;

		case '\\':
			sp = '\\';
			break;

		default:
			sp = '\0';
			break;
		} /* escape the character at *p */

	do_escape:
		if (sp) {
			if ((freespace > 0) && (freespace <= 2)) {
				if (out) out[used] = '\0';
				out = NULL;
				freespace = 0;

			} else if (freespace > 2) { /* room for char AND trailing zero */
				if (out) {
					out[used] = '\\';
					out[used + 1] = sp;
				}
				freespace -= 2;
			}

			used += 2;
			p++;
			inlen--;
			continue;
		}

		/*
		 *	All strings are UTF-8 clean.
		 */
		utf8 = fr_utf8_char(p, inlen);

		/*
		 *	If we have an invalid UTF-8 character, it gets
		 *	copied over as a 1-byte character for single
		 *	quoted strings.  Which means that the output
		 *	isn't strictly UTF-8, but oh well...
		 *
		 *	For double quoted strints, the invalid
		 *	characters get escaped as octal encodings.
		 */
		if (utf8 == 0) {
			if (quote == '\'') {
				utf8 = 1;

			} else {
				if ((freespace > 0) && (freespace <= 4)) {
					if (out) out[used] = '\0';
					out = NULL;
					freespace = 0;

				} else if (freespace > 4) { /* room for char AND trailing zero */
					if (out) snprintf(out + used, freespace, "\\%03o", *p);
					freespace -= 4;
				}

				used += 4;
				p++;
				inlen--;
				continue;
			}
		}

		if ((freespace > 0) && (freespace <= utf8)) {
			if (out) out[used] = '\0';
			out = NULL;
			freespace = 0;

		} else if (freespace > utf8) { /* room for char AND trailing zero */
			if (out) memcpy(out + used, p, utf8);
			freespace -= utf8;
		}

		used += utf8;
		p += utf8;
		inlen -= utf8;
	}

	/*
	 *	Ensure that the output buffer is always zero terminated.
	 */
	if (out && freespace) out[used] = '\0';

	return used;
}

/** Find the length of the buffer required to fully escape a string with fr_prints
 *
 * Were assuming here that's it's cheaper to figure out the length and do one
 * alloc than repeatedly expand the buffer when we find extra chars which need
 * to be added.
 *
 * @param in string to calculate the escaped length for.
 * @param inlen length of the input string, if < 0 strlen will be used to check the length.
 * @param[in] quote the quotation character.
 * @return the size of buffer required to hold the escaped string including the NUL byte.
 */
size_t fr_snprint_len(char const *in, ssize_t inlen, char quote)
{
	return fr_snprint(NULL, 0, in, inlen, quote) + 1;
}

/** Escape string that may contain binary data, and write it to a new buffer
 *
 * This is useful in situations where we expect printable strings as input,
 * but under some conditions may get binary data. A good example is libldap
 * and the arrays of struct berval ldap_get_values_len returns.
 *
 * @param[in] ctx To allocate new buffer in.
 * @param[in] in String to escape.
 * @param[in] inlen Length of string. Should be >= 0 if the data may contain
 *	embedded \0s. Must be >= 0 if data may not be \0 terminated.
 *	If < 0 inlen will be calculated using strlen.
 * @param[in] quote the quotation character.
 * @return new buffer holding the escaped string.
 */
char *fr_asprint(TALLOC_CTX *ctx, char const *in, ssize_t inlen, char quote)
{
	size_t len, ret;
	char *out;

	len = fr_snprint_len(in, inlen, quote);

	out = talloc_array(ctx, char, len);
	ret = fr_snprint(out, len, in, inlen, quote);
	/*
	 *	This is a fatal error, but fr_cond_assert is the strongest
	 *	assert we're allowed to use in library functions.
	 */
	if (!fr_cond_assert(ret == (len - 1))) {
		talloc_free(out);
		return NULL;
	}

	return out;
}
