/****************************************************************************
 * bfs                                                                      *
 * Copyright (C) 2017-2021 Tavian Barnes <tavianator@tavianator.com>        *
 *                                                                          *
 * Permission to use, copy, modify, and/or distribute this software for any *
 * purpose with or without fee is hereby granted.                           *
 *                                                                          *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES *
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF         *
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR  *
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES   *
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN    *
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF  *
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.           *
 ****************************************************************************/

#include "printf.h"
#include "bftw.h"
#include "color.h"
#include "ctx.h"
#include "diag.h"
#include "dir.h"
#include "dstring.h"
#include "mtab.h"
#include "pwcache.h"
#include "stat.h"
#include "time.h"
#include "util.h"
#include <assert.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int bfs_printf_fn(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf);

struct bfs_printf {
	/** The printing function to invoke. */
	bfs_printf_fn *fn;
	/** String data associated with this directive. */
	char *str;
	/** The stat field to print. */
	enum bfs_stat_field stat_field;
	/** Character data associated with this directive. */
	char c;
	/** Some data used by the directive. */
	const void *ptr;
	/** The next printf directive in the chain. */
	struct bfs_printf *next;
};

/** Print some text as-is. */
static int bfs_printf_literal(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	size_t len = dstrlen(directive->str);
	if (fwrite(directive->str, 1, len, cfile->file) == len) {
		return 0;
	} else {
		return -1;
	}
}

/** \c: flush */
static int bfs_printf_flush(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	return fflush(cfile->file);
}

/** Check if we can safely colorize this directive. */
static bool should_color(CFILE *cfile, const struct bfs_printf *directive) {
	return cfile->colors && strcmp(directive->str, "%s") == 0;
}

/**
 * Print a value to a temporary buffer before formatting it.
 */
#define BFS_PRINTF_BUF(buf, format, ...)				\
	char buf[256];							\
	int ret = snprintf(buf, sizeof(buf), format, __VA_ARGS__);	\
	assert(ret >= 0 && (size_t)ret < sizeof(buf));			\
	(void)ret

/** %a, %c, %t: ctime() */
static int bfs_printf_ctime(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	// Not using ctime() itself because GNU find adds nanoseconds
	static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	const struct timespec *ts = bfs_stat_time(statbuf, directive->stat_field);
	if (!ts) {
		return -1;
	}

	struct tm tm;
	if (xlocaltime(&ts->tv_sec, &tm) != 0) {
		return -1;
	}

	BFS_PRINTF_BUF(buf, "%s %s %2d %.2d:%.2d:%.2d.%09ld0 %4d",
	               days[tm.tm_wday],
	               months[tm.tm_mon],
	               tm.tm_mday,
	               tm.tm_hour,
	               tm.tm_min,
	               tm.tm_sec,
	               (long)ts->tv_nsec,
	               1900 + tm.tm_year);

	return fprintf(cfile->file, directive->str, buf);
}

/** %A, %B/%W, %C, %T: strftime() */
static int bfs_printf_strftime(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	const struct timespec *ts = bfs_stat_time(statbuf, directive->stat_field);
	if (!ts) {
		return -1;
	}

	struct tm tm;
	if (xlocaltime(&ts->tv_sec, &tm) != 0) {
		return -1;
	}

	int ret;
	char buf[256];
	char format[] = "% ";
	switch (directive->c) {
	// Non-POSIX strftime() features
	case '@':
		ret = snprintf(buf, sizeof(buf), "%lld.%09ld0", (long long)ts->tv_sec, (long)ts->tv_nsec);
		break;
	case '+':
		ret = snprintf(buf, sizeof(buf), "%4d-%.2d-%.2d+%.2d:%.2d:%.2d.%09ld0",
		               1900 + tm.tm_year,
		               tm.tm_mon + 1,
		               tm.tm_mday,
		               tm.tm_hour,
		               tm.tm_min,
		               tm.tm_sec,
		               (long)ts->tv_nsec);
		break;
	case 'k':
		ret = snprintf(buf, sizeof(buf), "%2d", tm.tm_hour);
		break;
	case 'l':
		ret = snprintf(buf, sizeof(buf), "%2d", (tm.tm_hour + 11)%12 + 1);
		break;
	case 's':
		ret = snprintf(buf, sizeof(buf), "%lld", (long long)ts->tv_sec);
		break;
	case 'S':
		ret = snprintf(buf, sizeof(buf), "%.2d.%09ld0", tm.tm_sec, (long)ts->tv_nsec);
		break;
	case 'T':
		ret = snprintf(buf, sizeof(buf), "%.2d:%.2d:%.2d.%09ld0",
			       tm.tm_hour,
			       tm.tm_min,
			       tm.tm_sec,
			       (long)ts->tv_nsec);
		break;

	// POSIX strftime() features
	default:
		format[1] = directive->c;
		ret = strftime(buf, sizeof(buf), format, &tm);
		break;
	}

	assert(ret >= 0 && (size_t)ret < sizeof(buf));
	(void)ret;

	return fprintf(cfile->file, directive->str, buf);
}

/** %b: blocks */
static int bfs_printf_b(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	uintmax_t blocks = ((uintmax_t)statbuf->blocks*BFS_STAT_BLKSIZE + 511)/512;
	BFS_PRINTF_BUF(buf, "%ju", blocks);
	return fprintf(cfile->file, directive->str, buf);
}

/** %d: depth */
static int bfs_printf_d(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	return fprintf(cfile->file, directive->str, (intmax_t)ftwbuf->depth);
}

/** %D: device */
static int bfs_printf_D(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)statbuf->dev);
	return fprintf(cfile->file, directive->str, buf);
}

/** %f: file name */
static int bfs_printf_f(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	if (should_color(cfile, directive)) {
		return cfprintf(cfile, "%pF", ftwbuf);
	} else {
		return fprintf(cfile->file, directive->str, ftwbuf->path + ftwbuf->nameoff);
	}
}

/** %F: file system type */
static int bfs_printf_F(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	const char *type = bfs_fstype(directive->ptr, statbuf);
	return fprintf(cfile->file, directive->str, type);
}

/** %G: gid */
static int bfs_printf_G(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)statbuf->gid);
	return fprintf(cfile->file, directive->str, buf);
}

/** %g: group name */
static int bfs_printf_g(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	const struct bfs_groups *groups = directive->ptr;
	const struct group *grp = groups ? bfs_getgrgid(groups, statbuf->gid) : NULL;
	if (!grp) {
		return bfs_printf_G(cfile, directive, ftwbuf);
	}

	return fprintf(cfile->file, directive->str, grp->gr_name);
}

/** %h: leading directories */
static int bfs_printf_h(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	char *copy = NULL;
	const char *buf;

	if (ftwbuf->nameoff > 0) {
		size_t len = ftwbuf->nameoff;
		if (len > 1) {
			--len;
		}

		buf = copy = strndup(ftwbuf->path, len);
	} else if (ftwbuf->path[0] == '/') {
		buf = "/";
	} else {
		buf = ".";
	}

	if (!buf) {
		return -1;
	}

	int ret;
	if (should_color(cfile, directive)) {
		ret = cfprintf(cfile, "${di}%s${rs}", buf);
	} else {
		ret = fprintf(cfile->file, directive->str, buf);
	}

	free(copy);
	return ret;
}

/** %H: current root */
static int bfs_printf_H(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	if (should_color(cfile, directive)) {
		if (ftwbuf->depth == 0) {
			return cfprintf(cfile, "%pP", ftwbuf);
		} else {
			return cfprintf(cfile, "${di}%s${rs}", ftwbuf->root);
		}
	} else {
		return fprintf(cfile->file, directive->str, ftwbuf->root);
	}
}

/** %i: inode */
static int bfs_printf_i(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)statbuf->ino);
	return fprintf(cfile->file, directive->str, buf);
}

/** %k: 1K blocks */
static int bfs_printf_k(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	uintmax_t blocks = ((uintmax_t)statbuf->blocks*BFS_STAT_BLKSIZE + 1023)/1024;
	BFS_PRINTF_BUF(buf, "%ju", blocks);
	return fprintf(cfile->file, directive->str, buf);
}

/** %l: link target */
static int bfs_printf_l(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	char *buf = NULL;
	const char *target = "";

	if (ftwbuf->type == BFS_LNK) {
		if (should_color(cfile, directive)) {
			return cfprintf(cfile, "%pL", ftwbuf);
		}

		const struct bfs_stat *statbuf = bftw_cached_stat(ftwbuf, BFS_STAT_NOFOLLOW);
		size_t len = statbuf ? statbuf->size : 0;

		target = buf = xreadlinkat(ftwbuf->at_fd, ftwbuf->at_path, len);
		if (!target) {
			return -1;
		}
	}

	int ret = fprintf(cfile->file, directive->str, target);
	free(buf);
	return ret;
}

/** %m: mode */
static int bfs_printf_m(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	return fprintf(cfile->file, directive->str, (unsigned int)(statbuf->mode & 07777));
}

/** %M: symbolic mode */
static int bfs_printf_M(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	char buf[11];
	xstrmode(statbuf->mode, buf);
	return fprintf(cfile->file, directive->str, buf);
}

/** %n: link count */
static int bfs_printf_n(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)statbuf->nlink);
	return fprintf(cfile->file, directive->str, buf);
}

/** %p: full path */
static int bfs_printf_p(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	if (should_color(cfile, directive)) {
		return cfprintf(cfile, "%pP", ftwbuf);
	} else {
		return fprintf(cfile->file, directive->str, ftwbuf->path);
	}
}

/** %P: path after root */
static int bfs_printf_P(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	size_t offset = strlen(ftwbuf->root);
	if (ftwbuf->path[offset] == '/') {
		++offset;
	}

	if (should_color(cfile, directive)) {
		if (ftwbuf->depth == 0) {
			return 0;
		}

		struct BFTW copybuf = *ftwbuf;
		copybuf.path += offset;
		copybuf.nameoff -= offset;
		return cfprintf(cfile, "%pP", &copybuf);
	} else {
		return fprintf(cfile->file, directive->str, ftwbuf->path + offset);
	}
}

/** %s: size */
static int bfs_printf_s(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)statbuf->size);
	return fprintf(cfile->file, directive->str, buf);
}

/** %S: sparseness */
static int bfs_printf_S(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	double sparsity;
	if (statbuf->size == 0 && statbuf->blocks == 0) {
		sparsity = 1.0;
	} else {
		sparsity = (double)BFS_STAT_BLKSIZE*statbuf->blocks/statbuf->size;
	}
	return fprintf(cfile->file, directive->str, sparsity);
}

/** %U: uid */
static int bfs_printf_U(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)statbuf->uid);
	return fprintf(cfile->file, directive->str, buf);
}

/** %u: user name */
static int bfs_printf_u(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, ftwbuf->stat_flags);
	if (!statbuf) {
		return -1;
	}

	const struct bfs_users *users = directive->ptr;
	const struct passwd *pwd = users ? bfs_getpwuid(users, statbuf->uid) : NULL;
	if (!pwd) {
		return bfs_printf_U(cfile, directive, ftwbuf);
	}

	return fprintf(cfile->file, directive->str, pwd->pw_name);
}

static const char *bfs_printf_type(enum bfs_type type) {
	switch (type) {
	case BFS_BLK:
		return "b";
	case BFS_CHR:
		return "c";
	case BFS_DIR:
		return "d";
	case BFS_DOOR:
		return "D";
	case BFS_FIFO:
		return "p";
	case BFS_LNK:
		return "l";
	case BFS_REG:
		return "f";
	case BFS_SOCK:
		return "s";
	default:
		return "U";
	}
}

/** %y: type */
static int bfs_printf_y(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	const char *type = bfs_printf_type(ftwbuf->type);
	return fprintf(cfile->file, directive->str, type);
}

/** %Y: target type */
static int bfs_printf_Y(CFILE *cfile, const struct bfs_printf *directive, const struct BFTW *ftwbuf) {
	int error = 0;

	if (ftwbuf->type != BFS_LNK) {
		return bfs_printf_y(cfile, directive, ftwbuf);
	}

	const char *type = "U";

	const struct bfs_stat *statbuf = bftw_stat(ftwbuf, BFS_STAT_FOLLOW);
	if (statbuf) {
		type = bfs_printf_type(bfs_mode_to_type(statbuf->mode));
	} else {
		switch (errno) {
		case ELOOP:
			type = "L";
			break;
		case ENOENT:
		case ENOTDIR:
			type = "N";
			break;
		default:
			type = "?";
			error = errno;
			break;
		}
	}

	int ret = fprintf(cfile->file, directive->str, type);
	if (error != 0) {
		ret = -1;
		errno = error;
	}
	return ret;
}

/**
 * Free a printf directive.
 */
static void free_directive(struct bfs_printf *directive) {
	if (directive) {
		dstrfree(directive->str);
		free(directive);
	}
}

/**
 * Create a new printf directive.
 */
static struct bfs_printf *new_directive(const struct bfs_ctx *ctx, bfs_printf_fn *fn) {
	struct bfs_printf *directive = malloc(sizeof(*directive));
	if (!directive) {
		bfs_perror(ctx, "malloc()");
		goto error;
	}

	directive->fn = fn;
	directive->str = dstralloc(2);
	if (!directive->str) {
		bfs_perror(ctx, "dstralloc()");
		goto error;
	}
	directive->stat_field = 0;
	directive->c = 0;
	directive->ptr = NULL;
	directive->next = NULL;
	return directive;

error:
	free_directive(directive);
	return NULL;
}

/**
 * Append a printf directive to the chain.
 */
static struct bfs_printf **append_directive(struct bfs_printf **tail, struct bfs_printf *directive) {
	assert(directive);
	*tail = directive;
	return &directive->next;
}

/**
 * Append a literal string to the chain.
 */
static struct bfs_printf **append_literal(struct bfs_printf **tail, struct bfs_printf **literal) {
	struct bfs_printf *directive = *literal;
	if (directive && dstrlen(directive->str) > 0) {
		*literal = NULL;
		return append_directive(tail, directive);
	} else {
		return tail;
	}
}

struct bfs_printf *bfs_printf_parse(const struct bfs_ctx *ctx, const char *format) {
	struct bfs_printf *head = NULL;
	struct bfs_printf **tail = &head;

	struct bfs_printf *literal = new_directive(ctx, bfs_printf_literal);
	if (!literal) {
		goto error;
	}

	for (const char *i = format; *i; ++i) {
		char c = *i;

		if (c == '\\') {
			c = *++i;

			if (c >= '0' && c < '8') {
				c = 0;
				for (int j = 0; j < 3 && *i >= '0' && *i < '8'; ++i, ++j) {
					c *= 8;
					c += *i - '0';
				}
				--i;
				goto one_char;
			}

			switch (c) {
			case 'a':  c = '\a'; break;
			case 'b':  c = '\b'; break;
			case 'f':  c = '\f'; break;
			case 'n':  c = '\n'; break;
			case 'r':  c = '\r'; break;
			case 't':  c = '\t'; break;
			case 'v':  c = '\v'; break;
			case '\\': c = '\\'; break;

			case 'c':
				tail = append_literal(tail, &literal);
				struct bfs_printf *directive = new_directive(ctx, bfs_printf_flush);
				if (!directive) {
					goto error;
				}
				tail = append_directive(tail, directive);
				goto done;

			case '\0':
				bfs_error(ctx, "'%s': Incomplete escape sequence '\\'.\n", format);
				goto error;

			default:
				bfs_error(ctx, "'%s': Unrecognized escape sequence '\\%c'.\n", format, c);
				goto error;
			}
		} else if (c == '%') {
			if (i[1] == '%') {
				c = *++i;
				goto one_char;
			}

			struct bfs_printf *directive = new_directive(ctx, NULL);
			if (!directive) {
				goto directive_error;
			}
			if (dstrapp(&directive->str, c) != 0) {
				bfs_perror(ctx, "dstrapp()");
				goto directive_error;
			}

			const char *specifier = "s";

			// Parse any flags
			bool must_be_numeric = false;
			while (true) {
				c = *++i;

				switch (c) {
				case '#':
				case '0':
				case '+':
					must_be_numeric = true;
					BFS_FALLTHROUGH;
				case ' ':
				case '-':
					if (strchr(directive->str, c)) {
						bfs_error(ctx, "'%s': Duplicate flag '%c'.\n", format, c);
						goto directive_error;
					}
					if (dstrapp(&directive->str, c) != 0) {
						bfs_perror(ctx, "dstrapp()");
						goto directive_error;
					}
					continue;
				}

				break;
			}

			// Parse the field width
			while (c >= '0' && c <= '9') {
				if (dstrapp(&directive->str, c) != 0) {
					bfs_perror(ctx, "dstrapp()");
					goto directive_error;
				}
				c = *++i;
			}

			// Parse the precision
			if (c == '.') {
				do {
					if (dstrapp(&directive->str, c) != 0) {
						bfs_perror(ctx, "dstrapp()");
						goto directive_error;
					}
					c = *++i;
				} while (c >= '0' && c <= '9');
			}

			switch (c) {
			case 'a':
				directive->fn = bfs_printf_ctime;
				directive->stat_field = BFS_STAT_ATIME;
				break;
			case 'b':
				directive->fn = bfs_printf_b;
				break;
			case 'c':
				directive->fn = bfs_printf_ctime;
				directive->stat_field = BFS_STAT_CTIME;
				break;
			case 'd':
				directive->fn = bfs_printf_d;
				specifier = "jd";
				break;
			case 'D':
				directive->fn = bfs_printf_D;
				break;
			case 'f':
				directive->fn = bfs_printf_f;
				break;
			case 'F':
				directive->ptr = bfs_ctx_mtab(ctx);
				if (!directive->ptr) {
					bfs_error(ctx, "Couldn't parse the mount table: %m.\n");
					goto directive_error;
				}
				directive->fn = bfs_printf_F;
				break;
			case 'g':
				directive->ptr = bfs_ctx_groups(ctx);
				if (!directive->ptr) {
					bfs_error(ctx, "Couldn't parse the group table: %m.\n");
					goto directive_error;
				}
				directive->fn = bfs_printf_g;
				break;
			case 'G':
				directive->fn = bfs_printf_G;
				break;
			case 'h':
				directive->fn = bfs_printf_h;
				break;
			case 'H':
				directive->fn = bfs_printf_H;
				break;
			case 'i':
				directive->fn = bfs_printf_i;
				break;
			case 'k':
				directive->fn = bfs_printf_k;
				break;
			case 'l':
				directive->fn = bfs_printf_l;
				break;
			case 'm':
				directive->fn = bfs_printf_m;
				specifier = "o";
				break;
			case 'M':
				directive->fn = bfs_printf_M;
				break;
			case 'n':
				directive->fn = bfs_printf_n;
				break;
			case 'p':
				directive->fn = bfs_printf_p;
				break;
			case 'P':
				directive->fn = bfs_printf_P;
				break;
			case 's':
				directive->fn = bfs_printf_s;
				break;
			case 'S':
				directive->fn = bfs_printf_S;
				specifier = "g";
				break;
			case 't':
				directive->fn = bfs_printf_ctime;
				directive->stat_field = BFS_STAT_MTIME;
				break;
			case 'u':
				directive->ptr = bfs_ctx_users(ctx);
				if (!directive->ptr) {
					bfs_error(ctx, "Couldn't parse the user table: %m.\n");
					goto directive_error;
				}
				directive->fn = bfs_printf_u;
				break;
			case 'U':
				directive->fn = bfs_printf_U;
				break;
			case 'w':
				directive->fn = bfs_printf_ctime;
				directive->stat_field = BFS_STAT_BTIME;
				break;
			case 'y':
				directive->fn = bfs_printf_y;
				break;
			case 'Y':
				directive->fn = bfs_printf_Y;
				break;

			case 'A':
				directive->stat_field = BFS_STAT_ATIME;
				goto directive_strftime;
			case 'B':
			case 'W':
				directive->stat_field = BFS_STAT_BTIME;
				goto directive_strftime;
			case 'C':
				directive->stat_field = BFS_STAT_CTIME;
				goto directive_strftime;
			case 'T':
				directive->stat_field = BFS_STAT_MTIME;
				goto directive_strftime;

			directive_strftime:
				directive->fn = bfs_printf_strftime;
				c = *++i;
				if (!c) {
					bfs_error(ctx, "'%s': Incomplete time specifier '%s%c'.\n", format, directive->str, i[-1]);
					goto directive_error;
				} else if (strchr("%+@aAbBcCdDeFgGhHIjklmMnprRsStTuUVwWxXyYzZ", c)) {
					directive->c = c;
				} else {
					bfs_error(ctx, "'%s': Unrecognized time specifier '%%%c%c'.\n", format, i[-1], c);
					goto directive_error;
				}
				break;

			case '\0':
				bfs_error(ctx, "'%s': Incomplete format specifier '%s'.\n", format, directive->str);
				goto directive_error;

			default:
				bfs_error(ctx, "'%s': Unrecognized format specifier '%%%c'.\n", format, c);
				goto directive_error;
			}

			if (must_be_numeric && strcmp(specifier, "s") == 0) {
				bfs_error(ctx, "'%s': Invalid flags '%s' for string format '%%%c'.\n", format, directive->str + 1, c);
				goto directive_error;
			}

			if (dstrcat(&directive->str, specifier) != 0) {
				bfs_perror(ctx, "dstrcat()");
				goto directive_error;
			}

			tail = append_literal(tail, &literal);
			tail = append_directive(tail, directive);

			if (!literal) {
				literal = new_directive(ctx, bfs_printf_literal);
				if (!literal) {
					goto error;
				}
			}

			continue;

		directive_error:
			free_directive(directive);
			goto error;
		}

	one_char:
		if (dstrapp(&literal->str, c) != 0) {
			bfs_perror(ctx, "dstrapp()");
			goto error;
		}
	}

done:
	tail = append_literal(tail, &literal);
	if (head) {
		free_directive(literal);
		return head;
	} else {
		return literal;
	}

error:
	free_directive(literal);
	bfs_printf_free(head);
	return NULL;
}

int bfs_printf(CFILE *cfile, const struct bfs_printf *format, const struct BFTW *ftwbuf) {
	int ret = 0, error = 0;

	for (const struct bfs_printf *directive = format; directive; directive = directive->next) {
		if (directive->fn(cfile, directive, ftwbuf) < 0) {
			ret = -1;
			error = errno;
		}
	}

	errno = error;
	return ret;
}

void bfs_printf_free(struct bfs_printf *format) {
	while (format) {
		struct bfs_printf *next = format->next;
		free_directive(format);
		format = next;
	}
}
