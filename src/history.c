/*
 * *****************************************************************************
 *
 * Copyright 2018 Gavin D. Howard
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * *****************************************************************************
 *
 * Adapted from the following:
 *
 * linenoise.c -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the original source code at:
 *   http://github.com/antirez/linenoise
 *
 * You can find the fork that this code is based on at:
 *   https://github.com/rain-1/linenoise-mob
 *
 * ------------------------------------------------------------------------
 *
 * This code is also under the following license:
 *
 * Copyright (c) 2010-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2013, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ------------------------------------------------------------------------
 *
 * Does a number of crazy assumptions that happen to be true in 99.9999% of
 * the 2010 UNIX computers around.
 *
 * References:
 * - http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
 * - http://www.3waylabs.com/nw/WWW/products/wizcon/vt220.html
 *
 * Todo list:
 * - Filter bogus Ctrl+<char> combinations.
 * - Win32 support
 *
 * Bloat:
 * - History search like Ctrl+r in readline?
 *
 * List of escape sequences used by this program, we do everything just
 * with three sequences. In order to be so cheap we may have some
 * flickering effect with some slow terminal, but the lesser sequences
 * the more compatible.
 *
 * EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line
 *
 * CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars
 *
 * CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars
 *
 * The following is used to get the terminal width if getting
 * the width with the TIOCGWINSZ ioctl fails
 *
 * DSR (Device Status Report)
 *    Sequence: ESC [ 6 n
 *    Effect: reports the current cusor position as ESC [ n ; m R
 *            where n is the row and m is the column
 *
 * When multi line mode is enabled, we also use two additional escape
 * sequences. However multi line editing is disabled by default.
 *
 * CUU (CUrsor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars.
 *
 * CUD (CUrsor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars.
 *
 * When bc_history_clearScreen() is called, two additional escape sequences
 * are used in order to clear the screen and position the cursor at home
 * position.
 *
 * CUP (CUrsor Position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left corner
 *
 * ED (Erase Display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen
 *
 * *****************************************************************************
 *
 * Code for line history.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <vector.h>
#include <history.h>
#include <read.h>
#include <vm.h>

#if BC_ENABLE_HISTORY

/**
 * Check if the code is a wide character.
 */
static bool bc_history_wchar(unsigned long cp) {

	size_t i;

	for (i = 0; i < bc_history_wchars_len; ++i) {

		// Ranges are listed in ascending order.  Therefore, once the
		// whole range is higher than the codepoint we're testing, the
		// codepoint won't be found in any remaining range => bail early.
		if (bc_history_wchars[i][0] > cp) return false;

		// Test this range.
		if (bc_history_wchars[i][0] <= cp && cp <= bc_history_wchars[i][1])
			return true;
	}

	return false;
}

/**
 * Check if the code is a combining character.
 */
static bool bc_history_comboChar(unsigned long cp) {

	size_t i;

	for (i = 0; i < bc_history_combo_chars_len; ++i) {

		// Combining chars are listed in ascending order, so once we pass
		// the codepoint of interest, we know it's not a combining char.
		if(bc_history_combo_chars[i] > cp) return false;
		if (bc_history_combo_chars[i] == cp) return true;
	}

	return false;
}

/**
 * Get length of previous UTF8 character.
 */
static size_t bc_history_prevCharLen(const char *buf, int pos) {
	int end = pos;
	for (pos -= 1; pos >= 0 && (buf[pos] & 0xC0) == 0x80; --pos);
	return end - pos;
}

/**
 * Convert UTF-8 to Unicode code point.
 */
static size_t bc_history_codePoint(const char *s, size_t len, unsigned int *cp)
{
	if (len) {

		unsigned char byte = s[0];

		if ((byte & 0x80) == 0) {
			*cp = byte;
			return 1;
		}
		else if ((byte & 0xE0) == 0xC0) {

			if (len >= 2) {
				*cp = (((unsigned long) (s[0] & 0x1F)) << 6) |
					   ((unsigned long) (s[1] & 0x3F));
				return 2;
			}
		}
		else if ((byte & 0xF0) == 0xE0) {

			if (len >= 3) {
				*cp = (((unsigned long) (s[0] & 0x0F)) << 12) |
					  (((unsigned long) (s[1] & 0x3F)) << 6) |
					   ((unsigned long) (s[2] & 0x3F));
				return 3;
			}
		}
		else if ((byte & 0xF8) == 0xF0) {

			if (len >= 4) {
				*cp = (((unsigned long) (s[0] & 0x07)) << 18) |
					  (((unsigned long) (s[1] & 0x3F)) << 12) |
					  (((unsigned long) (s[2] & 0x3F)) << 6) |
					   ((unsigned long) (s[3] & 0x3F));
				return 4;
			}
		}
		else {
			*cp = 0xFFFD;
			return 1;
		}
	}

	*cp = 0;

	return 1;
}

/**
 * Get length of next grapheme.
 */
size_t bc_history_nextLen(const char *buf, size_t buf_len,
                          size_t pos, size_t *col_len)
{
	unsigned int cp;
	size_t beg = pos;
	size_t len = bc_history_codePoint(buf + pos, buf_len - pos, &cp);

	if (bc_history_comboChar(cp)) {
		// Currently unreachable?
		return 0;
	}

	if (col_len != NULL) *col_len = bc_history_wchar(cp) ? 2 : 1;

	pos += len;

	while (pos < buf_len) {

		len = bc_history_codePoint(buf + pos, buf_len - pos, &cp);

		if (!bc_history_comboChar(cp)) return pos - beg;

		pos += len;
	}

	return pos - beg;
}

/**
 * Get length of previous grapheme.
 */
size_t bc_history_prevLen(const char* buf, size_t pos, size_t *col_len)
{
	size_t end = pos;

	while (pos > 0) {

		unsigned int cp;
		size_t len = bc_history_prevCharLen(buf, pos);

		pos -= len;
		bc_history_codePoint(buf + pos, len, &cp);

		if (!bc_history_comboChar(cp)) {
			if (col_len != NULL) *col_len = bc_history_wchar(cp) ? 2 : 1;
			return end - pos;
		}
	}

	// Currently unreachable?
	return 0;
}

/**
 * Read a Unicode code point from a file.
 */
size_t bc_history_readCode(int fd, char *buf, size_t buf_len, unsigned int *cp)
{
	size_t nread;

	if (buf_len < 1) return -1;
	nread = read(fd, buf, 1);
	if (nread <= 0) return nread;

	unsigned char byte = buf[0];

	if ((byte & 0x80) != 0) {

		if ((byte & 0xE0) == 0xC0) {

			if (buf_len < 2) return -1;
			nread = read(fd, buf + 1, 1);
			if (nread <= 0) return nread;
		}
		else if ((byte & 0xF0) == 0xE0) {

			if (buf_len < 3) return -1;
			nread = read(fd, buf + 1, 2);
			if (nread <= 0) return nread;
		}
		else if ((byte & 0xF8) == 0xF0) {

			if (buf_len < 3) return -1;
			nread = read(fd, buf + 1, 3);
			if (nread <= 0) return nread;
		}
		else {
			return -1;
		}
	}

	return bc_history_codePoint(buf, buf_len, cp);
}

/**
 * Get column length from begining of buffer to current byte position.
 */
static size_t bc_history_colPos(const char *buf, size_t buf_len, size_t pos) {

	size_t ret = 0, off = 0;

	while (off < pos) {

		size_t col_len, len;

		len = bc_history_nextLen(buf, buf_len, off, &col_len);

		off += len;
		ret += col_len;
	}

	return ret;
}

/**
 * Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences.
 */
static bool bc_history_isBadTerm() {

	int i;
	char *term = getenv("TERM");

	if (term == NULL) return false;

	for (i = 0; bc_history_bad_terms[i]; ++i) {
		if (!strcasecmp(term, bc_history_bad_terms[i])) return true;
	}

	return false;
}

/**
 * Raw mode: 1960's black magic.
 */
static BcStatus bc_history_enableRaw(BcHistory *h) {

	struct termios raw;

	if (h->rawMode) return BC_STATUS_SUCCESS;

	if (!isatty(STDIN_FILENO)) return bc_vm_err(BC_ERROR_VM_IO_ERR);
	if (tcgetattr(h->ifd, &h->orig_termios) == -1)
		return bc_vm_err(BC_ERROR_VM_IO_ERR);

	// Modify the original mode.
	raw = h->orig_termios;

	// Input modes: no break, no CR to NL, no parity check, no strip char,
	// no start/stop output control.
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

	// Control modes - set 8 bit chars.
	raw.c_cflag |= (CS8);

	// Local modes - choing off, canonical off, no extended functions,
	// no signal chars (^Z,^C).
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

	// Control chars - set return condition: min number of bytes and timer.
	// We want read to give every single byte, w/o timeout (1 byte, no timer).
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;

	// Put terminal in raw mode after flushing.
	if (tcsetattr(h->ifd, TCSAFLUSH, &raw) < 0)
		return bc_vm_err(BC_ERROR_VM_IO_ERR);

	h->rawMode = true;

	return BC_STATUS_SUCCESS;
}

static void bc_history_disableRaw(BcHistory *h) {

	// Don't even check the return value as it's too late.
	if (h->rawMode && tcsetattr(h->ifd, TCSAFLUSH, &h->orig_termios) != -1)
		h->rawMode = false;
}

/**
 * Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor.
 */
static int bc_history_cursorPos(BcHistory *h) {

	char buf[32];
	int cols, rows;
	unsigned int i;

	// Report cursor location.
	if (write(h->ofd, "\x1b[6n", 4) != 4) return -1;

	// Read the response: ESC [ rows ; cols R.
	for (i = 0; i < sizeof(buf) - 1; ++i) {
		if (read(h->ifd, buf + i, 1) != 1 || buf[i] == 'R') break;
	}

	buf[i] = '\0';

	// Parse it.
	if (buf[0] != BC_ACTION_ESC || buf[1] != '[') return -1;
	if (sscanf(buf + 2, "%d;%d", &rows, &cols) != 2) return -1;

	return cols;
}

/**
 * Try to get the number of columns in the current terminal, or assume 80
 * if it fails.
 */
static int bc_history_columns(BcHistory *h) {

	struct winsize ws;

	if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {

		// Calling ioctl() failed. Try to query the terminal itself.
		int start, cols;

		// Get the initial position so we can restore it later.
		start = bc_history_cursorPos(h);
		if (start == -1) return BC_HISTORY_DEF_COLS;

		// Go to right margin and get position.
		if (write(h->ofd,"\x1b[999C",6) != 6) return BC_HISTORY_DEF_COLS;
		cols = bc_history_cursorPos(h);
		if (cols == -1) return BC_HISTORY_DEF_COLS;

		// Restore position.
		if (cols > start) {

			char seq[32];

			snprintf(seq, 32, "\x1b[%dD", cols - start);

			if (write(h->ofd, seq, strlen(seq)) == -1)
				bc_vm_exit(BC_ERROR_VM_IO_ERR);
		}

		return cols;
	}

	return ws.ws_col;
}

/**
 * Check if text is an ANSI escape sequence.
 */
static int bc_history_ansiEscape(const char *buf, size_t buf_len, size_t *len) {

	if (buf_len > 2 && !memcmp("\033[", buf, 2)) {

		size_t off = 2;

		while (off < buf_len) {

			char c = buf[off++];

			if ((c >= 'A' && c <= 'K' && c != 'I') ||
			    c == 'S' || c == 'T' || c == 'f' || c == 'm')
			{
				*len = off;
				return 1;
			}
		}
	}
	return 0;
}

/**
 * Get column length of prompt text.
 */
static size_t bc_history_promptColLen(const char *prompt, size_t plen) {

	char buf[BC_HISTORY_MAX_LINE + 1];
	size_t buf_len = 0, off = 0;

	while (off < plen) {

		size_t len;

		if (bc_history_ansiEscape(prompt + off, plen - off, &len)) {
			off += len;
			continue;
		}

		buf[buf_len++] = prompt[off++];
	}

	return bc_history_colPos(buf,buf_len,buf_len);
}

/**
 * Rewrites the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal.
 */
static void bc_history_refresh(BcHistory *h) {

	char seq[64];
	int colpos;
	char* buf = h->buf.v;
	size_t len = BC_HISTORY_BUF_LEN(h), pos = h->pos, pcollen;
	BcVec vec;

	pcollen = bc_history_promptColLen(h->prompt, strlen(h->prompt));

	while(pcollen + bc_history_colPos(buf, len, pos) >= h->cols) {

		int chlen = bc_history_nextLen(buf, len, 0, NULL);

		buf += chlen;
		len -= chlen;
		pos -= chlen;
	}

	while (pcollen + bc_history_colPos(buf, len, len) > h->cols)
		len -= bc_history_prevLen(buf, len, NULL);

	bc_vec_init(&vec, sizeof(char), NULL);

	// Cursor to left edge.
	snprintf(seq,64,"\r");
	bc_vec_string(&vec, strlen(seq), seq);

	// Write the prompt and the current buffer content.
	bc_vec_concat(&vec, h->prompt);
	bc_vec_concat(&vec, buf);

	// Erase to right.
	snprintf(seq,64,"\x1b[0K");
	bc_vec_concat(&vec, seq);

	// Move cursor to original position.
	colpos = (int) (bc_history_colPos(buf, len, pos) + pcollen);
	snprintf(seq, 64, "\r\x1b[%dC", colpos);
	bc_vec_concat(&vec, seq);

	if (write(h->ofd, vec.v, vec.len - 1) == -1) bc_vm_exit(BC_ERROR_VM_IO_ERR);

	bc_vec_free(&vec);
}

/**
 * Insert the character 'c' at cursor current position.
 */
BcStatus bc_history_edit_insert(BcHistory *h, const char *cbuf, size_t clen) {

	bc_vec_expand(&h->buf, h->buf.len + clen);

	if (h->pos == BC_HISTORY_BUF_LEN(h)) {

		size_t colpos;
		size_t len;

		memcpy(bc_vec_item(&h->buf, h->pos), cbuf, clen);

		h->pos += clen;
		h->buf.len += clen - 1;
		bc_vec_pushByte(&h->buf, '\0');

		len = BC_HISTORY_BUF_LEN(h);
		colpos = !bc_history_promptColLen(h->prompt, h->plen);
		colpos += bc_history_colPos(h->buf.v, len, len);

		if (colpos < h->cols) {
			// Avoid a full update of the line in the trivial case.
			if (write(h->ofd, cbuf, clen) == -1)
				return bc_vm_err(BC_ERROR_VM_IO_ERR);
		}
		else bc_history_refresh(h);
	}
	else {

		size_t amt = BC_HISTORY_BUF_LEN(h) - h->pos;

		memmove(h->buf.v + h->pos + clen, h->buf.v + h->pos, amt);
		memcpy(h->buf.v + h->pos, cbuf, clen);

		h->pos += clen;
		h->buf.len += clen;
		h->buf.v[BC_HISTORY_BUF_LEN(h)] = '\0';

		bc_history_refresh(h);
	}

	return BC_STATUS_SUCCESS;
}

/**
 * Move cursor to the left.
 */
void bc_history_edit_left(BcHistory *h) {

	if (h->pos <= 0) return;

	h->pos -= bc_history_prevLen(h->buf.v, h->pos, NULL);
	bc_history_refresh(h);
}

/**
 * Move cursor on the right.
*/
void bc_history_edit_right(BcHistory *h) {

	if (h->pos == BC_HISTORY_BUF_LEN(h)) return;

	h->pos += bc_history_nextLen(h->buf.v, BC_HISTORY_BUF_LEN(h), h->pos, NULL);
	bc_history_refresh(h);
}

/**
 * Move cursor to the end of the current word.
 */
void bc_history_edit_wordEnd(BcHistory *h) {

	size_t len = BC_HISTORY_BUF_LEN(h);

	if (len == 0 || h->pos >= len) return;

	if (h->buf.v[h->pos] == ' ') {
		while (h->pos < len && h->buf.v[h->pos] == ' ') ++h->pos;
	}

	while (h->pos < len && h->buf.v[h->pos] != ' ') ++h->pos;

	bc_history_refresh(h);
}

/**
 * Move cursor to the start of the current word.
 */
void bc_history_edit_wordStart(BcHistory *h) {

	size_t len = BC_HISTORY_BUF_LEN(h);

	if (len == 0) return;

	if (h->pos != 0 && h->buf.v[h->pos - 1] == ' ') --h->pos;

	if (h->buf.v[h->pos] == ' ') {
		while (h->pos < len && h->buf.v[h->pos] == ' ') --h->pos;
	}

	while (h->pos < len && h->buf.v[h->pos - 1] != ' ') --h->pos;

	bc_history_refresh(h);
}

/**
 * Move cursor to the start of the line.
 */
void bc_history_edit_home(BcHistory *h) {

	if (h->pos == 0) return;

	h->pos = 0;
	bc_history_refresh(h);
}

/**
 * Move cursor to the end of the line.
 */
void bc_history_edit_end(BcHistory *h) {

	if (h->pos == BC_HISTORY_BUF_LEN(h)) return;

	h->pos = BC_HISTORY_BUF_LEN(h);
	bc_history_refresh(h);
}

/**
 * Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir' (direction).
 */
void bc_history_edit_next(BcHistory *h, int dir) {

	if (h->history.len <= 1) return;

	size_t idx = h->history.len - 1 - h->idx;
	char* dup = bc_vm_strdup(h->buf.v);

	// Update the current history entry before
	// overwriting it with the next one.
	bc_vec_replaceAt(&h->history, idx, &dup);

	// Show the new entry.
	h->idx += (dir == BC_HISTORY_PREV) ? 1 : -1;

	if (h->idx == SIZE_MAX) {
		h->idx = 0;
		return;
	}
	else if (h->idx >= h->history.len) {
		h->idx = h->history.len - 1;
		return;
	}

	// Recalculate.
	idx = h->history.len - 1 - h->idx;

	char* str = *((char**) bc_vec_item(&h->history, idx));
	bc_vec_string(&h->buf, strlen(str), str);

	h->pos = BC_HISTORY_BUF_LEN(h);

	bc_history_refresh(h);
}

/**
 * Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key.
 */
void bc_history_edit_delete(BcHistory *h) {

	size_t chlen, len = BC_HISTORY_BUF_LEN(h);

	if (len == 0 || h->pos >= len) return;

	chlen = bc_history_nextLen(h->buf.v, len, h->pos, NULL);

	memmove(h->buf.v + h->pos, h->buf.v + h->pos + chlen, len - h->pos - chlen);

	h->buf.len -= chlen;
	h->buf.v[BC_HISTORY_BUF_LEN(h)] = '\0';

	bc_history_refresh(h);
}

void bc_history_edit_backspace(BcHistory *h) {

	size_t chlen, len = BC_HISTORY_BUF_LEN(h);

	if (h->pos == 0 || len == 0) return;

	chlen = bc_history_prevLen(h->buf.v, h->pos, NULL);

	memmove(h->buf.v + h->pos - chlen, h->buf.v + h->pos, len - h->pos);

	h->pos -= chlen;
	h->buf.len -= chlen;
	h->buf.v[BC_HISTORY_BUF_LEN(h)] = '\0';

	bc_history_refresh(h);
}

/**
 * Delete the previous word, maintaining the cursor at the start of the
 * current word.
 */
void bc_history_edit_deletePrevWord(BcHistory *h) {

	size_t diff, old_pos = h->pos;

	while (h->pos > 0 && h->buf.v[h->pos - 1] == ' ') --h->pos;
	while (h->pos > 0 && h->buf.v[h->pos - 1] != ' ') --h->pos;

	diff = old_pos - h->pos;
	memmove(h->buf.v + h->pos, h->buf.v + old_pos, BC_HISTORY_BUF_LEN(h) - old_pos + 1);
	h->buf.len -= diff;

	bc_history_refresh(h);
}

/**
 * Delete the next word, maintaining the cursor at the same position.
 */
void bc_history_deleteNextWord(BcHistory *h) {

	size_t next_end = h->pos, len = BC_HISTORY_BUF_LEN(h);

	while (next_end < len && h->buf.v[next_end] == ' ') ++next_end;
	while (next_end < len && h->buf.v[next_end] != ' ') ++next_end;

	memmove(h->buf.v + h->pos, h->buf.v + next_end, len - next_end);

	h->buf.len -= next_end - h->pos;

	bc_history_refresh(h);
}

void bc_history_swap(BcHistory *h) {

	size_t pcl, ncl;
	char auxb[5];

	pcl = bc_history_prevLen(h->buf.v, h->pos, NULL);
	ncl = bc_history_nextLen(h->buf.v, BC_HISTORY_BUF_LEN(h), h->pos, NULL);

	// To perform a swap we need:
	// * nonzero char length to the left
	// * not at the end of the line
	if (pcl != 0 && h->pos != BC_HISTORY_BUF_LEN(h) && pcl < 5 && ncl < 5) {

		memcpy(auxb, h->buf.v + h->pos - pcl, pcl);
		memcpy(h->buf.v + h->pos - pcl, h->buf.v + h->pos, ncl);
		memcpy(h->buf.v + h->pos - pcl + ncl, auxb, pcl);

		h->pos += -pcl + ncl;

		bc_history_refresh(h);
	}
}

/**
 * Handle escape sequences.
 */
static void bc_history_escape(BcHistory *h) {

	char seq[3];

	if (read(h->ifd, seq, 1) == -1) return;

	// ESC ? sequences.
	if (seq[0] != '[' && seq[0] != '0') {

		switch (seq[0]) {

			case 'f':
			{
				bc_history_edit_wordEnd(h);
				break;
			}

			case 'b':
			{
				bc_history_edit_wordStart(h);
				break;
			}

			case 'd':
			{
				bc_history_deleteNextWord(h);
				break;
			}
		}
	}
	else {

		if (read(h->ifd, seq + 1, 1) == -1) return;

		// ESC [ sequences.
		if (seq[0] == '[') {

			if (seq[1] >= '0' && seq[1] <= '9') {

				// Extended escape, read additional byte.
				if (read(h->ifd, seq + 2, 1) == -1) return;
				if (seq[2] == '~') {

					switch(seq[1]) {

						// Delete key.
						case '3':
						{
							bc_history_edit_delete(h);
							break;
						}
					}
				}
			}
			else {

				switch(seq[1]) {

					// Up.
					case 'A':
					{
						bc_history_edit_next(h, BC_HISTORY_PREV);
						break;
					}

					// Down.
					case 'B':
					{
						bc_history_edit_next(h, BC_HISTORY_NEXT);
						break;
					}

					// Right.
					case 'C':
					{
						bc_history_edit_right(h);
						break;
					}

					// Left.
					case 'D':
					{
						bc_history_edit_left(h);
						break;
					}

					// Home.
					case 'H':
					case '1':
					{
						bc_history_edit_home(h);
						break;
					}

					// End.
					case 'F':
					case '4':
					{
						bc_history_edit_end(h);
						break;
					}

					case 'd':
					{
						bc_history_deleteNextWord(h);
						break;
					}
				}
			}
		}
		// ESC O sequences.
		else if (seq[0] == 'O') {

			switch(seq[1]) {

				case 'H':
				{
					bc_history_edit_home(h);
					break;
				}

				case 'F':
				{
					bc_history_edit_end(h);
					break;
				}
			}
		}
	}
}

/**
 * This function is the core of the line editing capability of bc history.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 */
static BcStatus bc_history_edit(BcHistory *h, const char *prompt) {

	BcStatus s = BC_STATUS_SUCCESS;

	// Populate the history state.
	h->prompt = prompt;
	h->plen = strlen(prompt);
	h->oldcolpos = h->pos = 0;
	h->cols = bc_history_columns(h);
	h->idx = 0;

	// Buffer starts empty.
	bc_vec_npop(&h->buf, h->buf.len);
	bc_vec_pushByte(&h->buf, '\0');

	// The latest history entry is always our current buffer, that
	// initially is just an empty string.
	bc_history_add(h, strdup(""));

	if (write(h->ofd, prompt, h->plen) == -1) return BC_STATUS_SUCCESS;

	while (!s) {

		unsigned int c;
		char cbuf[32]; // large enough for any encoding?
		int nread;

	// Continue reading if interrupted by a signal.
// TODO
//	do {
//		  nread = read(l.ifd,&c,1);
//		} while((nread == -1) && (errno == EINTR));
		nread = bc_history_readCode(h->ifd, cbuf, sizeof(cbuf), &c);
		if (nread <= 0) return bc_vm_err(BC_ERROR_VM_IO_ERR);

		switch(c) {

			case BC_ACTION_LINE_FEED:
			case BC_ACTION_ENTER:
			{
				bc_vec_pop(&h->history);
				return BC_STATUS_SUCCESS;
			}

			case BC_ACTION_CTRL_C:
			{
				errno = EAGAIN;
				return BC_STATUS_SUCCESS;
			}

			case BC_ACTION_BACKSPACE:
			case BC_ACTION_CTRL_H:
			{
				bc_history_edit_backspace(h);
				break;
			}

			// Remove char at right of cursor, or if the
			// line is empty, act as end-of-file.
			case BC_ACTION_CTRL_D:
			{
				if (BC_HISTORY_BUF_LEN(h) > 0) {
					bc_history_edit_delete(h);
				}
				else {
					bc_vec_pop(&h->history);
					return bc_vm_err(BC_ERROR_VM_IO_ERR);
				}

				break;
			}

			// Swaps current character with previous.
			case BC_ACTION_CTRL_T:
			{
				bc_history_swap(h);
				break;
			}

			case BC_ACTION_CTRL_B:
			{
				bc_history_edit_left(h);
				break;
			}

			case BC_ACTION_CTRL_F:
			{
				bc_history_edit_right(h);
				break;
			}

			case BC_ACTION_CTRL_P:
			{
				bc_history_edit_next(h, BC_HISTORY_PREV);
				break;
			}

			case BC_ACTION_CTRL_N:
			{
				bc_history_edit_next(h, BC_HISTORY_NEXT);
				break;
			}

			case BC_ACTION_ESC:
			{
				bc_history_escape(h);
				break;
			}

			// Delete the whole line.
			case BC_ACTION_CTRL_U:
			{
				bc_vec_string(&h->buf, 0, "");
				h->pos = 0;

				bc_history_refresh(h);

				break;
			}

			// Delete from current to end of line.
			case BC_ACTION_CTRL_K:
			{
				bc_vec_npop(&h->buf, h->buf.len - h->pos);
				bc_vec_pushByte(&h->buf, '\0');

				bc_history_refresh(h);

				break;
			}

			// Go to the start of the line.
			case BC_ACTION_CTRL_A:
			{
				bc_history_edit_home(h);
				break;
			}

			// Go to the end of the line.
			case BC_ACTION_CTRL_E:
			{
				bc_history_edit_end(h);
				break;
			}

			// Clear screen.
			case BC_ACTION_CTRL_L:
			{
				if (write(h->ofd, "\x1b[H\x1b[2J", 7) <= 0)
					s = bc_vm_err(BC_ERROR_VM_IO_ERR);
				bc_history_refresh(h);
				break;
			}

			// Delete previous word.
			case BC_ACTION_CTRL_W:
			{
				bc_history_edit_deletePrevWord(h);
				break;
			}

			default:
			{
				s = bc_history_edit_insert(h, cbuf, nread);
				break;
			}
		}
	}

	return s;
}

/**
 * This function calls the line editing function bc_history_edit()
 * using the STDIN file descriptor set in raw mode.
 */
static BcStatus bc_history_raw(BcHistory *h, const char *prompt) {

	BcStatus s;

	s = bc_history_enableRaw(h);
	if (s) return s;

	s = bc_history_edit(h, prompt);
	bc_history_disableRaw(h);
	bc_vm_puts("\n", stderr);

	return s;
}

BcStatus bc_history_line(BcHistory *h, BcVec *vec, const char *prompt) {

	BcStatus s;
	char* line;

	if (vm->ttyin && !vm->history.badTerm) {

		s = bc_history_raw(h, prompt);
		if (s) return s;

		bc_vec_string(vec, BC_HISTORY_BUF_LEN(h), h->buf.v);
	}
	else {
		s = bc_read_chars(vec, prompt);
		if (s) return s;
	}

	line = bc_vm_strdup(vec->v);
	bc_history_add(h, line);

	return s;
}

void bc_history_add(BcHistory *h, char *line) {

	// Don't add duplicated lines.
	if (h->history.len) {
		char* str = *((char**) bc_vec_item_rev(&h->history, 0));
		if (!strcmp(str, line)) {
			free(line);
			return;
		}
	}

	if (h->history.len == BC_HISTORY_MAX_LEN) bc_vec_popAt(&h->history, 0);

	bc_vec_push(&h->history, &line);
}

void bc_history_init(BcHistory *h) {

	h->rawMode = false;
	h->ifd = STDIN_FILENO;
	h->ofd = STDERR_FILENO;
	h->badTerm = bc_history_isBadTerm();

	bc_vec_init(&h->buf, sizeof(char), NULL);
	bc_vec_init(&h->history, sizeof(char*), bc_string_free);
}

void bc_history_free(BcHistory *h) {

	bc_history_disableRaw(h);

#ifndef NDEBUG
	bc_vec_free(&h->buf);
	bc_vec_free(&h->history);
#endif // NDEBUG
}

/**
 * This special mode is used by bc history in order to print scan codes
 * on screen for debugging / development purposes.
 */
#ifndef NDEBUG
BcStatus bc_history_printKeyCodes(BcHistory *h) {

	BcStatus s;
	char quit[4];

	bc_vm_printf("Linenoise key codes debugging mode.\n"
	             "Press keys to see scan codes. "
	             "Type 'quit' at any time to exit.\n");

	s = bc_history_enableRaw(h);
	if (s) return s;
	memset(quit, ' ', 4);

	while(true) {

		char c;
		int nread;

		nread = read(STDIN_FILENO,&c,1);
		if (nread <= 0) continue;

		// Shift string to left.
		memmove(quit, quit + 1, sizeof(quit) - 1);

		// Insert current char on the right.
		quit[sizeof(quit) - 1] = c;
		if (memcmp(quit, "quit", sizeof(quit)) == 0) break;

		printf("'%c' %02x (%d) (type quit to exit)\n",
		       isprint((int) c) ? c : '?', (int) c, (int) c);

		// Go left edge manually, we are in raw mode.
		bc_vm_putchar('\r');
		fflush(stdout);
	}

	bc_history_disableRaw(h);

	return s;
}
#endif // NDEBUG

#endif // BC_ENABLE_HISTORY
