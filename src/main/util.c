/*
 * util.c	Various utility functions.
 *
 * Version:     $Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 */

RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/rad_assert.h>

#include <ctype.h>

#include <sys/stat.h>
#include <fcntl.h>

/*
 *	The signal() function in Solaris 2.5.1 sets SA_NODEFER in
 *	sa_flags, which causes grief if signal() is called in the
 *	handler before the cause of the signal has been cleared.
 *	(Infinite recursion).
 *
 *	The same problem appears on HPUX, so we avoid it, if we can.
 *
 *	Using sigaction() to reset the signal handler fixes the problem,
 *	so where available, we prefer that solution.
 */

void (*reset_signal(int signo, void (*func)(int)))(int)
{
#ifdef HAVE_SIGACTION
	struct sigaction act, oact;

	memset(&act, 0, sizeof(act));
	act.sa_handler = func;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
#ifdef  SA_INTERRUPT		/* SunOS */
	act.sa_flags |= SA_INTERRUPT;
#endif
	if (sigaction(signo, &act, &oact) < 0)
		return SIG_ERR;
	return oact.sa_handler;
#else

	/*
	 *	re-set by calling the 'signal' function, which
	 *	may cause infinite recursion and core dumps due to
	 *	stack growth.
	 *
	 *	However, the system is too dumb to implement sigaction(),
	 *	so we don't have a choice.
	 */
	signal(signo, func);

	return NULL;
#endif
}

/*
 *	Per-request data, added by modules...
 */
struct request_data_t {
	request_data_t	*next;

	void		*unique_ptr;
	int		unique_int;
	void		*opaque;
	bool		free_opaque;
};

/*
 *	Add opaque data (with a "free" function) to a REQUEST.
 *
 *	The unique ptr is meant to be a module configuration,
 *	and the unique integer allows the caller to have multiple
 *	opaque data associated with a REQUEST.
 */
int request_data_add(REQUEST *request, void *unique_ptr, int unique_int, void *opaque, bool free_opaque)
{
	request_data_t *this, **last, *next;

	/*
	 *	Some simple sanity checks.
	 */
	if (!request || !opaque) return -1;

	this = next = NULL;
	for (last = &(request->data);
	     *last != NULL;
	     last = &((*last)->next)) {
		if (((*last)->unique_ptr == unique_ptr) &&
		    ((*last)->unique_int == unique_int)) {
			this = *last;
			next = this->next;

			/*
			 *	If caller requires custom behaviour on free
			 *	they must set a destructor.
			 */
			if (this->opaque && this->free_opaque) talloc_free(this->opaque);

			break;	/* replace the existing entry */
		}
	}

	/*
	 *	Only alloc new memory if we're not replacing
	 *	an existing entry.
	 */
	if (!this) this = talloc_zero(request, request_data_t);
	if (!this) return -1;

	this->next = next;
	this->unique_ptr = unique_ptr;
	this->unique_int = unique_int;
	this->opaque = opaque;
	this->free_opaque = free_opaque;

	*last = this;

	return 0;
}

/*
 *	Get opaque data from a request.
 */
void *request_data_get(REQUEST *request, void *unique_ptr, int unique_int)
{
	request_data_t **last;

	if (!request) return NULL;

	for (last = &(request->data);
	     *last != NULL;
	     last = &((*last)->next)) {
		if (((*last)->unique_ptr == unique_ptr) &&
		    ((*last)->unique_int == unique_int)) {
			request_data_t *this;
			void *ptr;

			this = *last;
			ptr = this->opaque;

			/*
			 *	Remove the entry from the list, and free it.
			 */
			*last = this->next;
			talloc_free(this);

			return ptr; 		/* don't free it, the caller does that */
		}
	}

	return NULL;		/* wasn't found, too bad... */
}

/*
 *	Get opaque data from a request without removing it.
 */
void *request_data_reference(REQUEST *request, void *unique_ptr, int unique_int)
{
	request_data_t **last;

	for (last = &(request->data);
	     *last != NULL;
	     last = &((*last)->next)) {
		if (((*last)->unique_ptr == unique_ptr) &&
		    ((*last)->unique_int == unique_int)) {
			return (*last)->opaque;
		}
	}

	return NULL;		/* wasn't found, too bad... */
}

/*
 *	Create possibly many directories.
 *
 *	Note that the input directory name is NOT a constant!
 *	This is so that IF an error is returned, the 'directory' ptr
 *	points to the name of the file which caused the error.
 */
int rad_mkdir(char *directory, mode_t mode)
{
	int rcode;
	char *p;

	/*
	 *	Try to make the directory.  If it exists, chmod it.
	 *	If a path doesn't exist, that's OK.  Otherwise
	 *	return with an error.
	 */
	rcode = mkdir(directory, mode & 0777);
	if (rcode < 0) {
		if (errno == EEXIST) {
			return 0; /* don't change permissions */
		}

		if (errno != ENOENT) {
			return rcode;
		}

		/*
		 *	A component in the directory path doesn't
		 *	exist.  Look for the LAST directory name.  Try
		 *	to create that.  If there's an error, we leave
		 *	the directory path as the one at which the
		 *	error occured.
		 */
		p = strrchr(directory, FR_DIR_SEP);
		if (!p || (p == directory)) return -1;

		*p = '\0';
		rcode = rad_mkdir(directory, mode);
		if (rcode < 0) return rcode;

		/*
		 *	Reset the directory path, and try again to
		 *	make the directory.
		 */
		*p = FR_DIR_SEP;
		rcode = mkdir(directory, mode & 0777);
		if (rcode < 0) return rcode;
	} /* else we successfully created the directory */

	/*
	 *	Set the permissions on the created directory.
	 */
	return chmod(directory, mode);
}

/** Escapes the raw string such that it should be safe to use as part of a file path
 *
 * This function is designed to produce a string that's still readable but portable
 * across the majority of file systems.
 *
 * For security reasons it cannot remove characters from the name, and must not allow
 * collisions to occur between different strings.
 *
 * With that in mind '-' has been chosen as the escape character, and will be double
 * escaped '-' -> '--' to avoid collisions.
 *
 * Escaping should be reversible if the original string needs to be extracted.
 *
 * @note function takes additional arguments so that it may be used as an xlat escape
 *	function but it's fine to call it directly.
 *
 * @note OSX/Unix/NTFS/VFAT have a max filename size of 255 bytes.
 *
 * @param request Current request (may be NULL).
 * @param out Output buffer.
 * @param outlen Size of the output buffer.
 * @param in string to escape.
 * @param arg Context arguments (unused, should be NULL).
 */
size_t rad_filename_escape(UNUSED REQUEST *request, char *out, size_t outlen, char const *in, UNUSED void *arg)
{
	size_t freespace = outlen;

	while (*in != '\0') {
		size_t utf8_len;

		/*
		 *	Encode multibyte UTF8 chars
		 */
		utf8_len = fr_utf8_char((uint8_t const *) in);
		if (utf8_len > 1) {
			if (freespace <= (utf8_len * 3)) break;

			switch (utf8_len) {
			case 2:
				snprintf(out, freespace, "-%x-%x", in[0], in[1]);
				break;

			case 3:
				snprintf(out, freespace, "-%x-%x-%x", in[0], in[1], in[2]);
				break;

			case 4:
				snprintf(out, freespace, "-%x-%x-%x-%x", in[0], in[1], in[2], in[3]);
				break;
			}

			freespace -= (utf8_len * 3);
			out += (utf8_len * 3);
			in += utf8_len;

			continue;
		}

		/*
		 *	Safe chars
		 */
		if (((*in >= 'A') && (*in <= 'Z')) ||
		    ((*in >= 'a') && (*in <= 'z')) ||
		    ((*in >= '0') && (*in <= '9')) ||
		    (*in == '_') || (*in == '.')) {
		    	if (freespace <= 1) break;

		 	*out++ = *in++;
		 	freespace--;
		 	continue;
		}

		if (freespace <= 2) break;

		/*
		 *	Double escape '-' (like \\)
		 */
		if (*in == '-') {
			*out++ = '-';
			*out++ = '-';

			freespace -= 2;
			in++;
			continue;
		}

		/*
		 *	Unsafe chars
		 */
		*out++ = '-';
		fr_bin2hex(out, (uint8_t const *)in++, 1);
		out += 2;
		freespace -= 3;
	}
	*out = '\0';

	return outlen - freespace;
}

/** Converts data stored in a file name back to its original form
 *
 * @param out Where to write the unescaped string (may be the same as in).
 * @param outlen Length of the output buffer.
 * @param in Input filename.
 * @param inlen Length of input.
 * @return number of bytes written to output buffer, or offset where parse error
 *	occurred on failure.
 */
ssize_t rad_filename_unescape(char *out, size_t outlen, char const *in, size_t inlen)
{
	char const *p, *end = in + inlen;
	size_t freespace = outlen;

	for (p = in; p < end; p++) {
		if (freespace <= 1) break;

		if (((*p >= 'A') && (*p <= 'Z')) ||
		    ((*p >= 'a') && (*p <= 'z')) ||
		    ((*p >= '0') && (*p <= '9')) ||
		    (*p == '_') || (*p == '.')) {
		 	*out++ = *p;
		 	freespace--;
		 	continue;
		}

		if (p[0] == '-') {
			/*
			 *	End of input, '-' needs at least one extra char after
			 *	it to be valid.
			 */
			if ((end - p) < 2) return in - p;
			if (p[1] == '-') {
				p++;
				*out++ = '-';
				freespace--;
				continue;
			}

			/*
			 *	End of input, '-' must be followed by <hex><hex>
			 *	but there aren't enough chars left
			 */
			if ((end - p) < 3) return in - p;

			/*
			 *	If hex2bin returns 0 the next two chars weren't hexits.
			 */
			if (fr_hex2bin((uint8_t *) out, 1, in, 1) == 0) return in - (p + 1);
			in += 2;
			out++;
			freespace--;
		}

		return in - p; /* offset we found the bad char at */
	}
	*out = '\0';

	return outlen - freespace;	/* how many bytes were written */
}

/*
 *	Allocate memory, or exit.
 *
 *	This call ALWAYS succeeds!
 */
void *rad_malloc(size_t size)
{
	void *ptr = malloc(size);

	if (ptr == NULL) {
		ERROR("no memory");
		fr_exit(1);
	}

	return ptr;
}


void rad_const_free(void const *ptr)
{
	void *tmp;
	if (!ptr) return;

	memcpy(&tmp, &ptr, sizeof(tmp));
	talloc_free(tmp);
}

char *rad_ajoin(TALLOC_CTX *ctx, char const **array, char c)
{
	char const **array_p;
	char *buff, *p;
	size_t len = 0, left, wrote;

	if (!*array) {
		goto null;
	}

	for (array_p = array; *array_p; array_p++) {
		len += (strlen(*array_p) + 1);
	}

	if (!len) {
		null:
		return talloc_zero_array(ctx, char, 1);
	}

	left = len + 1;
	buff = p = talloc_zero_array(ctx, char, left);
	for (array_p = array; *array_p; array_p++) {
		wrote = snprintf(p, left, "%s%c", *array_p, c);
		left -= wrote;
		p += wrote;
	}
	buff[len - 1] = '\0';

	return buff;
}

/*
 *	Logs an error message and aborts the program
 *
 */

void NEVER_RETURNS rad_assert_fail(char const *file, unsigned int line, char const *expr)
{
	ERROR("ASSERT FAILED %s[%u]: %s", file, line, expr);
	fr_fault(SIGABRT);
	fr_exit_now(1);
}

/*
 *	Free a REQUEST struct.
 */
static int _request_free(REQUEST *request)
{
	rad_assert(!request->in_request_hash);
#ifdef WITH_PROXY
	rad_assert(!request->in_proxy_hash);
#endif
	rad_assert(!request->ev);

#ifdef WITH_COA
	if (request->coa) {
		request->coa->parent = NULL;
	}

	if (request->parent && (request->parent->coa == request)) {
		request->parent->coa = NULL;
	}
#endif

#ifndef NDEBUG
	request->magic = 0x01020304;	/* set the request to be nonsense */
#endif
	request->client = NULL;
#ifdef WITH_PROXY
	request->home_server = NULL;
#endif

	return 0;
}

/*
 *	Create a new REQUEST data structure.
 */
REQUEST *request_alloc(TALLOC_CTX *ctx)
{
	REQUEST *request;

	request = talloc_zero(ctx, REQUEST);
	if (!request) return NULL;
	talloc_set_destructor(request, _request_free);
#ifndef NDEBUG
	request->magic = REQUEST_MAGIC;
#endif
#ifdef WITH_PROXY
	request->proxy = NULL;
#endif
	request->reply = NULL;
#ifdef WITH_PROXY
	request->proxy_reply = NULL;
#endif
	request->config_items = NULL;
	request->username = NULL;
	request->password = NULL;
	request->timestamp = time(NULL);
	request->log.lvl = debug_flag; /* Default to global debug level */

	request->module = "";
	request->component = "<core>";
	request->log.func = vradlog_request;

	return request;
}


/*
 *	Create a new REQUEST, based on an old one.
 *
 *	This function allows modules to inject fake requests
 *	into the server, for tunneled protocols like TTLS & PEAP.
 */
REQUEST *request_alloc_fake(REQUEST *request)
{
	REQUEST *fake;

	fake = request_alloc(request);
	if (!fake) return NULL;

	fake->number = request->number;
#ifdef HAVE_PTHREAD_H
	fake->child_pid = request->child_pid;
#endif
	fake->parent = request;
	fake->root = request->root;
	fake->client = request->client;

	/*
	 *	For new server support.
	 *
	 *	FIXME: Key instead off of a "virtual server" data structure.
	 *
	 *	FIXME: Permit different servers for inner && outer sessions?
	 */
	fake->server = request->server;

	fake->packet = rad_alloc(fake, true);
	if (!fake->packet) {
		talloc_free(fake);
		return NULL;
	}

	fake->reply = rad_alloc(fake, false);
	if (!fake->reply) {
		talloc_free(fake);
		return NULL;
	}

	fake->master_state = REQUEST_ACTIVE;
	fake->child_state = REQUEST_RUNNING;

	/*
	 *	Fill in the fake request.
	 */
	fake->packet->sockfd = -1;
	fake->packet->src_ipaddr = request->packet->src_ipaddr;
	fake->packet->src_port = request->packet->src_port;
	fake->packet->dst_ipaddr = request->packet->dst_ipaddr;
	fake->packet->dst_port = 0;

	/*
	 *	This isn't STRICTLY required, as the fake request MUST NEVER
	 *	be put into the request list.  However, it's still reasonable
	 *	practice.
	 */
	fake->packet->id = fake->number & 0xff;
	fake->packet->code = request->packet->code;
	fake->timestamp = request->timestamp;
	fake->packet->timestamp = request->packet->timestamp;

	/*
	 *	Required for new identity support
	 */
	fake->listener = request->listener;

	/*
	 *	Fill in the fake reply, based on the fake request.
	 */
	fake->reply->sockfd = fake->packet->sockfd;
	fake->reply->src_ipaddr = fake->packet->dst_ipaddr;
	fake->reply->src_port = fake->packet->dst_port;
	fake->reply->dst_ipaddr = fake->packet->src_ipaddr;
	fake->reply->dst_port = fake->packet->src_port;
	fake->reply->id = fake->packet->id;
	fake->reply->code = 0; /* UNKNOWN code */

	/*
	 *	Copy debug information.
	 */
	memcpy(&(fake->log), &(request->log), sizeof(fake->log));
	fake->log.indent = 0;	/* Apart from the indent which we reset */

	return fake;
}

#ifdef WITH_COA
REQUEST *request_alloc_coa(REQUEST *request)
{
	if (!request || request->coa) return NULL;

	/*
	 *	Originate CoA requests only when necessary.
	 */
	if ((request->packet->code != PW_CODE_ACCESS_REQUEST) &&
	    (request->packet->code != PW_CODE_ACCOUNTING_REQUEST)) return NULL;

	request->coa = request_alloc_fake(request);
	if (!request->coa) return NULL;

	request->coa->packet->code = 0; /* unknown, as of yet */
	request->coa->child_state = REQUEST_RUNNING;
	request->coa->proxy = rad_alloc(request->coa, false);
	if (!request->coa->proxy) {
		TALLOC_FREE(request->coa);
		return NULL;
	}

	return request->coa;
}
#endif

/*
 *	Copy a quoted string.
 */
int rad_copy_string(char *to, char const *from)
{
	int length = 0;
	char quote = *from;

	do {
		if (*from == '\\') {
			*(to++) = *(from++);
			length++;
		}
		*(to++) = *(from++);
		length++;
	} while (*from && (*from != quote));

	if (*from != quote) return -1; /* not properly quoted */

	*(to++) = quote;
	length++;
	*to = '\0';

	return length;
}

/*
 *	Copy a quoted string but without the quotes. The length
 *	returned is the number of chars written; the number of
 *	characters consumed is 2 more than this.
 */
int rad_copy_string_bare(char *to, char const *from)
{
	int length = 0;
	char quote = *from;

	from++;
	while (*from && (*from != quote)) {
		if (*from == '\\') {
			*(to++) = *(from++);
			length++;
		}
		*(to++) = *(from++);
		length++;
	}

	if (*from != quote) return -1; /* not properly quoted */

	*to = '\0';

	return length;
}


/*
 *	Copy a %{} string.
 */
int rad_copy_variable(char *to, char const *from)
{
	int length = 0;
	int sublen;

	*(to++) = *(from++);
	length++;

	while (*from) {
		switch (*from) {
		case '"':
		case '\'':
			sublen = rad_copy_string(to, from);
			if (sublen < 0) return sublen;
			from += sublen;
			to += sublen;
			length += sublen;
			break;

		case '}':	/* end of variable expansion */
			*(to++) = *(from++);
			*to = '\0';
			length++;
			return length; /* proper end of variable */

		case '\\':
			*(to++) = *(from++);
			*(to++) = *(from++);
			length += 2;
			break;

		case '%':	/* start of variable expansion */
			if (from[1] == '{') {
				*(to++) = *(from++);
				length++;

				sublen = rad_copy_variable(to, from);
				if (sublen < 0) return sublen;
				from += sublen;
				to += sublen;
				length += sublen;
				break;
			} /* else FIXME: catch %%{ ?*/

			/* FALL-THROUGH */
		default:
			*(to++) = *(from++);
			length++;
			break;
		}
	} /* loop over the input string */

	/*
	 *	We ended the string before a trailing '}'
	 */

	return -1;
}

#ifndef USEC
#define USEC 1000000
#endif

uint32_t rad_pps(uint32_t *past, uint32_t *present, time_t *then, struct timeval *now)
{
	uint32_t pps;

	if (*then != now->tv_sec) {
		*then = now->tv_sec;
		*past = *present;
		*present = 0;
	}

	/*
	 *	Bootstrap PPS by looking at a percentage of
	 *	the previous PPS.  This lets us take a moving
	 *	count, without doing a moving average.  If
	 *	we're a fraction "f" (0..1) into the current
	 *	second, we can get a good guess for PPS by
	 *	doing:
	 *
	 *	PPS = pps_now + pps_old * (1 - f)
	 *
	 *	It's an instantaneous measurement, rather than
	 *	a moving average.  This will hopefully let it
	 *	respond better to sudden spikes.
	 *
	 *	Doing the calculations by thousands allows us
	 *	to not overflow 2^32, AND to not underflow
	 *	when we divide by USEC.
	 */
	pps = USEC - now->tv_usec; /* useconds left in previous second */
	pps /= 1000;		   /* scale to milliseconds */
	pps *= *past;		   /* multiply by past count to get fraction */
	pps /= 1000;		   /* scale to usec again */
	pps += *present;	   /* add in current count */

	return pps;
}

/** Split string into words and expand each one
 *
 * @param request Current request.
 * @param cmd string to split.
 * @param max_argc the maximum number of arguments to split into.
 * @param argv Where to write the pointers into argv_buf.
 * @param can_fail If false, stop processing if any of the xlat expansions fail.
 * @param argv_buflen size of argv_buf.
 * @param argv_buf temporary buffer we used to mangle/expand cmd.
 *	Pointers to offsets of this buffer will be written to argv.
 * @return argc or -1 on failure.
 */

int rad_expand_xlat(REQUEST *request, char const *cmd,
		    int max_argc, char *argv[], bool can_fail,
		    size_t argv_buflen, char *argv_buf)
{
	char const *from;
	char *to;
	int argc = -1;
	int i;
	int left;

	if (strlen(cmd) > (argv_buflen - 1)) {
		ERROR("rad_expand_xlat: Command line is too long");
		return -1;
	}

	/*
	 *	Check for bad escapes.
	 */
	if (cmd[strlen(cmd) - 1] == '\\') {
		ERROR("rad_expand_xlat: Command line has final backslash, without a following character");
		return -1;
	}

	strlcpy(argv_buf, cmd, argv_buflen);

	/*
	 *	Split the string into argv's BEFORE doing radius_xlat...
	 */
	from = cmd;
	to = argv_buf;
	argc = 0;
	while (*from) {
		int length;

		/*
		 *	Skip spaces.
		 */
		if ((*from == ' ') || (*from == '\t')) {
			from++;
			continue;
		}

		argv[argc] = to;
		argc++;

		if (argc >= (max_argc - 1)) break;

		/*
		 *	Copy the argv over to our buffer.
		 */
		while (*from && (*from != ' ') && (*from != '\t')) {
			if (to >= argv_buf + argv_buflen - 1) {
				ERROR("rad_expand_xlat: Ran out of space in command line");
				return -1;
			}

			switch (*from) {
			case '"':
			case '\'':
				length = rad_copy_string_bare(to, from);
				if (length < 0) {
					ERROR("rad_expand_xlat: Invalid string passed as argument");
					return -1;
				}
				from += length+2;
				to += length;
				break;

			case '%':
				if (from[1] == '{') {
					*(to++) = *(from++);

					length = rad_copy_variable(to, from);
					if (length < 0) {
						ERROR("rad_expand_xlat: Invalid variable expansion passed as argument");
						return -1;
					}
					from += length;
					to += length;
				} else { /* FIXME: catch %%{ ? */
					*(to++) = *(from++);
				}
				break;

			case '\\':
				if (from[1] == ' ') from++;
				/* FALL-THROUGH */

			default:
				*(to++) = *(from++);
			}
		} /* end of string, or found a space */

		*(to++) = '\0';	/* terminate the string */
	}

	/*
	 *	We have to have SOMETHING, at least.
	 */
	if (argc <= 0) {
		ERROR("rad_expand_xlat: Empty command line");
		return -1;
	}

	/*
	 *	Expand each string, as appropriate.
	 */
	left = argv_buf + argv_buflen - to;
	for (i = 0; i < argc; i++) {
		int sublen;

		/*
		 *	Don't touch argv's which won't be translated.
		 */
		if (strchr(argv[i], '%') == NULL) continue;

		if (!request) continue;

		sublen = radius_xlat(to, left - 1, request, argv[i], NULL, NULL);
		if (sublen <= 0) {
			if (can_fail) {
				/*
				 *	Fail to be backwards compatible.
				 *
				 *	It's yucky, but it won't break anything,
				 *	and it won't cause security problems.
				 */
				sublen = 0;
			} else {
				ERROR("rad_expand_xlat: xlat failed");
				return -1;
			}
		}

		argv[i] = to;
		to += sublen;
		*(to++) = '\0';
		left -= sublen;
		left--;

		if (left <= 0) {
			ERROR("rad_expand_xlat: Ran out of space while expanding arguments");
			return -1;
		}
	}
	argv[argc] = NULL;

	return argc;
}

/** Return the default log dir
 *
 * This is set at build time from --prefix
 * @return the value of LOGDIR
 */
char const *rad_default_log_dir(void)
{
	return LOGDIR;
}

/** Return the default lib dir
 *
 * This is set at build time from --prefix
 * @return the value of LIBDIR
 */
char const *rad_default_lib_dir(void)
{
	return LIBDIR;
}

/** Return the default raddb dir
 *
 * This is set at build time from --prefix
 * @return the value of RADDBDIR
 */
char const *rad_default_raddb_dir(void)
{
	return RADDBDIR;
}

/** Return the default run dir
 *
 * This is set at build time from --prefix
 * @return the value of RUNDIR
 */
char const *rad_default_run_dir(void)
{
	return RUNDIR;
}

/** Return the default sbin dir
 *
 * This is set at build time from --prefix
 * @return the value of SBINDIR
 */
char const *rad_default_sbin_dir(void)
{
	return SBINDIR;
}

/** Return the default radacct dir
 *
 * This is set at build time from --prefix
 * @return the value of RADIR
 */
char const *rad_radacct_dir(void)
{
	return RADIR;
}

#ifndef NDEBUG
/*
 *	Verify a packet.
 */
static void verify_packet(char const *file, int line, REQUEST *request, RADIUS_PACKET *packet, char const *type)
{
	TALLOC_CTX *parent;

	if (!packet) {
		fprintf(stderr, "CONSISTENCY CHECK FAILED %s[%i]: RADIUS_PACKET %s pointer was NULL", file, line, type);
		fr_assert(0);
		fr_exit_now(0);
	}

	parent = talloc_parent(packet);
	if (parent != request) {
		ERROR("CONSISTENCY CHECK FAILED %s[%i]: Expected RADIUS_PACKET %s to be parented by %p (%s), "
		      "but parented by %p (%s)", file, line, type, request, talloc_get_name(request),
		      parent, parent ? talloc_get_name(parent) : "NULL");

		fr_log_talloc_report(packet);
		if (parent) fr_log_talloc_report(parent);

		rad_assert(0);
	}

	VERIFY_PACKET(packet);

	if (!packet->vps) return;

#ifdef WITH_VERIFY_PTR
	fr_verify_list(file, line, packet, packet->vps);
#endif
}
/*
 *	Catch horrible talloc errors.
 */
void verify_request(char const *file, int line, REQUEST *request)
{
	if (!request) {
		fprintf(stderr, "CONSISTENCY CHECK FAILED %s[%i]: REQUEST pointer was NULL", file, line);
		fr_assert(0);
		fr_exit_now(0);
	}

	(void) talloc_get_type_abort(request, REQUEST);

#ifdef WITH_VERIFY_PTR
	fr_verify_list(file, line, request, request->config_items);
	fr_verify_list(file, line, request, request->state);
#endif

	if (request->packet) verify_packet(file, line, request, request->packet, "request");
	if (request->reply) verify_packet(file, line, request, request->reply, "reply");
#ifdef WITH_PROXY
	if (request->proxy) verify_packet(file, line, request, request->proxy, "proxy-request");
	if (request->proxy_reply) verify_packet(file, line, request, request->proxy_reply, "proxy-reply");
#endif

#ifdef WITH_COA
	if (request->coa) {
		void *parent;

		(void) talloc_get_type_abort(request->coa, REQUEST);
		parent = talloc_parent(request->coa);

		rad_assert(parent == request);

		verify_request(file, line, request->coa);
	}
#endif
}
#endif

#ifdef HAVE_GRP_H
#ifndef HAVE_GETGRNAM_R
bool fr_getgid(char const *name, gid_t *gid)
{
	struct group *grp;

	grp =  getgrnam(name);
	if (!grp) return false;

	*gid = grp->gr_gid;

	return true;
}
#else  /* getgrnam_r() exists */

bool fr_getgid(char const *name, gid_t *gid)
{
	struct group	*grp, my_group;
	char		*group_buffer;
	size_t		group_size = 1024;

	grp = NULL;
	group_buffer = talloc_array(NULL, char, group_size);
	while (group_buffer) {
		int err;

		err = getgrnam_r(name, &my_group, group_buffer, group_size, &grp);
		if (err == ERANGE) {
			group_size *= 2;
			group_buffer = talloc_realloc(NULL, group_buffer, char, group_size);
			continue;
		}

		if (err) errno = err; /* so the caller knows what went wrong */

		break;
	}

	talloc_free(group_buffer);

	if (!grp) return false;

	*gid = grp->gr_gid;
	return true;
}
#endif	/* HAVE_GETGRNAM_R */
#endif	/* HAVE_GRP_H */
