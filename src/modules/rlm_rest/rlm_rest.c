/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_rest.c
 * @brief Integrate FreeRADIUS with RESTfull APIs
 *
 * @copyright 2012-2016 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 */
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/token.h>
#include <freeradius-devel/rad_assert.h>

#include <ctype.h>
#include "rest.h"

/*
 *	TLS Configuration
 */
static CONF_PARSER tls_config[] = {
	{ FR_CONF_OFFSET("ca_file", FR_TYPE_FILE_INPUT, rlm_rest_section_t, tls_ca_file) },
	{ FR_CONF_OFFSET("ca_path", FR_TYPE_FILE_INPUT, rlm_rest_section_t, tls_ca_path) },
	{ FR_CONF_OFFSET("certificate_file", FR_TYPE_FILE_INPUT, rlm_rest_section_t, tls_certificate_file) },
	{ FR_CONF_OFFSET("private_key_file", FR_TYPE_FILE_INPUT, rlm_rest_section_t, tls_private_key_file) },
	{ FR_CONF_OFFSET("private_key_password", FR_TYPE_STRING | FR_TYPE_SECRET, rlm_rest_section_t, tls_private_key_password) },
	{ FR_CONF_OFFSET("random_file", FR_TYPE_STRING, rlm_rest_section_t, tls_random_file) },
	{ FR_CONF_OFFSET("check_cert", FR_TYPE_BOOL, rlm_rest_section_t, tls_check_cert), .dflt = "yes" },
	{ FR_CONF_OFFSET("check_cert_cn", FR_TYPE_BOOL, rlm_rest_section_t, tls_check_cert_cn), .dflt = "yes" },
	{ FR_CONF_OFFSET("extract_cert_attrs", FR_TYPE_BOOL, rlm_rest_section_t, tls_extract_cert_attrs), .dflt = "no" },
	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER section_config[] = {
	{ FR_CONF_OFFSET("uri", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_rest_section_t, uri), .dflt = "" },
	{ FR_CONF_OFFSET("proxy", FR_TYPE_STRING, rlm_rest_section_t, proxy) },
	{ FR_CONF_OFFSET("method", FR_TYPE_STRING, rlm_rest_section_t, method_str), .dflt = "GET" },
	{ FR_CONF_OFFSET("body", FR_TYPE_STRING, rlm_rest_section_t, body_str), .dflt = "none" },
	{ FR_CONF_OFFSET("data", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_rest_section_t, data) },
	{ FR_CONF_OFFSET("force_to", FR_TYPE_STRING, rlm_rest_section_t, force_to_str) },

	/* User authentication */
	{ FR_CONF_OFFSET("auth", FR_TYPE_STRING, rlm_rest_section_t, auth_str), .dflt = "none" },
	{ FR_CONF_OFFSET("username", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_rest_section_t, username) },
	{ FR_CONF_OFFSET("password", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_rest_section_t, password) },
	{ FR_CONF_OFFSET("require_auth", FR_TYPE_BOOL, rlm_rest_section_t, require_auth), .dflt = "no" },

	/* Transfer configuration */
	{ FR_CONF_OFFSET("timeout", FR_TYPE_TIMEVAL, rlm_rest_section_t, timeout_tv), .dflt = "4.0" },
	{ FR_CONF_OFFSET("chunk", FR_TYPE_UINT32, rlm_rest_section_t, chunk), .dflt = "0" },

	/* TLS Parameters */
	{ FR_CONF_POINTER("tls", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) tls_config },
	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER xlat_config[] = {
	{ FR_CONF_OFFSET("proxy", FR_TYPE_STRING, rlm_rest_section_t, proxy) },

	/* User authentication */
	{ FR_CONF_OFFSET("auth", FR_TYPE_STRING, rlm_rest_section_t, auth_str), .dflt = "none" },
	{ FR_CONF_OFFSET("username", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_rest_section_t, username) },
	{ FR_CONF_OFFSET("password", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_rest_section_t, password) },
	{ FR_CONF_OFFSET("require_auth", FR_TYPE_BOOL, rlm_rest_section_t, require_auth), .dflt = "no" },

	/* Transfer configuration */
	{ FR_CONF_OFFSET("timeout", FR_TYPE_TIMEVAL, rlm_rest_section_t, timeout_tv), .dflt = "4.0" },
	{ FR_CONF_OFFSET("chunk", FR_TYPE_UINT32, rlm_rest_section_t, chunk), .dflt = "0" },

	/* TLS Parameters */
	{ FR_CONF_POINTER("tls", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) tls_config },
	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER module_config[] = {
	{ FR_CONF_DEPRECATED("connect_timeout", FR_TYPE_TIMEVAL, rlm_rest_t, connect_timeout) },
	{ FR_CONF_OFFSET("connect_proxy", FR_TYPE_STRING, rlm_rest_t, connect_proxy) },
	CONF_PARSER_TERMINATOR
};

/** Update the status attribute
 *
 * @param[in] request	The current request.
 * @param[in] handle	rest handle.
 * @return
 *	- 0 if status was updated successfully.
 *	- -1 if status was not updated successfully.
 */
static int rlm_rest_status_update(REQUEST *request,  void *handle)
{
	TALLOC_CTX	*ctx;
	VALUE_PAIR	**list;
	int		code;
	fr_value_box_t	value;

	code = rest_get_handle_code(handle);

	RINDENT();
	RDEBUG2("&REST-HTTP-Status-Code := %i", code);
	REXDENT();

	value.datum.uint32 = code;

	/*
	 *	Find the reply list, and appropriate context in the
	 *	current request.
	 */
	RADIUS_LIST_AND_CTX(ctx, list, request, REQUEST_CURRENT, PAIR_LIST_REQUEST);
	if (!list || (fr_pair_update_by_num(ctx, list, 0, PW_REST_HTTP_STATUS_CODE, TAG_ANY, &value) < 0)) {
		REDEBUG("Failed updating &REST-HTTP-Status-Code");
		return -1;
	}

	return 0;
}

static int rlm_rest_perform(rlm_rest_t const *instance, rlm_rest_thread_t *thread,
			    rlm_rest_section_t const *section, void *handle,
			    REQUEST *request, char const *username, char const *password)
{
	ssize_t		uri_len;
	char		*uri = NULL;
	int		ret;

	RDEBUG("Expanding URI components");

	/*
	 *  Build xlat'd URI, this allows REST servers to be specified by
	 *  request attributes.
	 */
	uri_len = rest_uri_build(&uri, instance, request, section->uri);
	if (uri_len <= 0) return -1;

	RDEBUG("Sending HTTP %s to \"%s\"", fr_int2str(http_method_table, section->method, NULL), uri);

	/*
	 *  Configure various CURL options, and initialise the read/write
	 *  context data.
	 */
	ret = rest_request_config(instance, thread, section, request, handle, section->method, section->body,
				  uri, username, password);
	talloc_free(uri);
	if (ret < 0) return -1;

	/*
	 *  Send the CURL request, pre-parse headers, aggregate incoming
	 *  HTTP body data into a single contiguous buffer.
	 */
	ret = rest_io_request_enqueue(thread, request, handle);
	if (ret < 0) return -1;

	return 0;
}

#if 0
/*
 *	Simple xlat to read text data from a URL
 */
static ssize_t rest_xlat(UNUSED TALLOC_CTX *ctx, char **out, UNUSED size_t outlen,
			 void const *mod_inst, UNUSED void const *xlat_inst,
			 REQUEST *request, char const *fmt)
{
	rlm_rest_t const	*inst = mod_inst;
	rlm_rest_handle_t	*handle = NULL;
	ssize_t			len;
	int			ret;
	char			*uri = NULL;
	char const		*p = fmt, *q;
	http_method_t		method;
	void			*thread = NULL;

	rad_assert(*out == NULL);

	/* There are no configurable parameters other than the URI */
	rlm_rest_section_t	*section;

	/*
	 *	Section gets modified, so we need our own copy.
	 */
	MEM(section = talloc(request, rlm_rest_section_t));
	memcpy(section, &inst->xlat, sizeof(*section));

	rad_assert(fmt);

	RDEBUG("Expanding URI components");

	/*
	 *  Extract the method from the start of the format string (if there is one)
	 */
	method = fr_substr2int(http_method_table, p, HTTP_METHOD_UNKNOWN, -1);
	if (method != HTTP_METHOD_UNKNOWN) {
		section->method = method;
		p += strlen(http_method_table[method].name);
	/*
	 *  If the method is unknown, it's either a URL or a verb
	 */
	} else {
		for (q = p; (*q != ' ') && (*q != '\0') && isalpha(*q); q++);

		/*
		 *	If the first non-alpha char was a space,
		 *	then assume this is a verb.
		 */
		if ((*q == ' ') && (q != p)) {
			section->method = HTTP_METHOD_CUSTOM;
			MEM(section->method_str = talloc_bstrndup(section, p, q - p));
			p = q;
		} else {
			section->method = HTTP_METHOD_GET;
		}
	}

	/*
	 *  Trim whitespace
	 */
	while (isspace(*p) && p++);

#if 0
	handle = fr_connection_get(t->pool, request);

	if (!handle) return -1;
#endif

	/*
	 *  Unescape parts of xlat'd URI, this allows REST servers to be specified by
	 *  request attributes.
	 */
	len = rest_uri_host_unescape(&uri, mod_inst, request, handle, p);
	if (len <= 0) {
	error:
		rest_request_cleanup(mod_inst, handle);
		talloc_free(section);

		return -1;
	}

	/*
	 *  Extract freeform body data (url can't contain spaces)
	 */
	q = strchr(p, ' ');
	if (q && (*++q != '\0')) {
		section->body = HTTP_BODY_CUSTOM_LITERAL;
		section->data = q;
	}

	RDEBUG("Sending HTTP %s to \"%s\"",
	       (section->method == HTTP_METHOD_CUSTOM) ?
	       	section->method_str : fr_int2str(http_method_table, section->method, NULL),
	       uri);

	/*
	 *  Configure various CURL options, and initialise the read/write
	 *  context data.
	 *
	 *  @todo We could extract the User-Name and password from the URL string.
	 */
	ret = rest_request_config(mod_inst, thread, section, request,
				  handle, section->method, section->body, uri, NULL, NULL);
	talloc_free(uri);
	if (ret < 0) goto error;

	/*
	 *  Send the CURL request, pre-parse headers, aggregate incoming
	 *  HTTP body data into a single contiguous buffer.
	 *
	 * @fixme need to pass in thread to all xlat functions
	 */
	ret = rest_io_request_enqueue(NULL, request, handle);
	if (ret < 0) goto error;

	return 0;	/* FIXME XLAT YIELD */
}

static ssize_t rest_xlat_resume(UNUSED TALLOC_CTX *ctx, char **out, UNUSED size_t outlen,
				void const *mod_inst, UNUSED void const *xlat_inst,
				REQUEST *request, UNUSED char const *fmt)
{
	rlm_rest_t const	*inst = mod_inst;
	rlm_rest_handle_t	*handle = NULL;	/* FIXME POPULATE FROM REQUEST */
	int			hcode;
	ssize_t			len, slen = 0;
	char const		*body;
	rlm_rest_section_t	*section = NULL; /* FIXME POPULATE FROM REQUEST */

	if (section->tls_extract_cert_attrs) rest_response_certinfo(mod_inst, section, request, handle);

	if (rlm_rest_status_update(request, handle) < 0) {
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	hcode = rest_get_handle_code(handle);
	switch (hcode) {
	case 404:
	case 410:
	case 403:
	case 401:
	{
		slen = -1;
error:
		rest_response_error(request, handle);
		goto finish;
	}
	case 204:
		goto finish;

	default:
		/*
		 *	Attempt to parse content if there was any.
		 */
		if ((hcode >= 200) && (hcode < 300)) {
			break;
		} else if (hcode < 500) {
			slen = -2;
			goto error;
		} else {
			slen = -1;
			goto error;
		}
	}

	len = rest_get_handle_data(&body, handle);
	if (len > 0) {
		*out = talloc_bstrndup(request, body, len);
		slen = len;
	}

finish:
	rest_request_cleanup(mod_inst, handle);

	fr_connection_release(t->pool, request, handle);

	talloc_free(section);

	return slen;
}
#endif

static rlm_rcode_t mod_authorize_result(REQUEST *request, void *instance, void *thread, void *ctx)
{
	rlm_rest_t const		*inst = instance;
	rlm_rest_thread_t		*t = thread;
	rlm_rest_section_t const 	*section = &inst->authenticate;
	rlm_rest_handle_t		*handle = ctx;

	int				hcode;
	int				rcode = RLM_MODULE_OK;
	int				ret;

	if (section->tls_extract_cert_attrs) rest_response_certinfo(instance, section, request, handle);

	if (rlm_rest_status_update(request, handle) < 0) {
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	hcode = rest_get_handle_code(handle);
	switch (hcode) {
	case 404:
	case 410:
		rcode = RLM_MODULE_NOTFOUND;
		break;

	case 403:
		rcode = RLM_MODULE_USERLOCK;
		break;

	case 401:
		/*
		 *	Attempt to parse content if there was any.
		 */
		ret = rest_response_decode(inst, section, request, handle);
		if (ret < 0) {
			rcode = RLM_MODULE_FAIL;
			break;
		}

		rcode = RLM_MODULE_REJECT;
		break;

	case 204:
		rcode = RLM_MODULE_OK;
		break;

	default:
		/*
		 *	Attempt to parse content if there was any.
		 */
		if ((hcode >= 200) && (hcode < 300)) {
			ret = rest_response_decode(inst, section, request, handle);
			if (ret < 0) 	   rcode = RLM_MODULE_FAIL;
			else if (ret == 0) rcode = RLM_MODULE_OK;
			else		   rcode = RLM_MODULE_UPDATED;
			break;
		} else if (hcode < 500) {
			rcode = RLM_MODULE_INVALID;
		} else {
			rcode = RLM_MODULE_FAIL;
		}
	}

	switch (rcode) {
	case RLM_MODULE_INVALID:
	case RLM_MODULE_FAIL:
	case RLM_MODULE_USERLOCK:
		rest_response_error(request, handle);
		break;

	default:
		break;
	}

finish:
	rest_request_cleanup(instance, handle);

	fr_connection_release(t->pool, request, handle);

	return rcode;
}

/*
 *	Find the named user in this modules database.  Create the set
 *	of attribute-value pairs to check and reply with for this user
 *	from the database. The authentication code only needs to check
 *	the password, the rest is done here.
 */
static rlm_rcode_t CC_HINT(nonnull) mod_authorize(void *instance, void *thread, REQUEST *request)
{
	rlm_rest_t const		*inst = instance;
	rlm_rest_thread_t		*t = thread;
	rlm_rest_section_t const	*section = &inst->authorize;

	void				*handle;
	int				ret;

	if (!section->name) return RLM_MODULE_NOOP;

	handle = fr_connection_get(t->pool, request);
	if (!handle) return RLM_MODULE_FAIL;

	ret = rlm_rest_perform(instance, thread, section, handle, request, NULL, NULL);
	if (ret < 0) {
		rest_request_cleanup(instance, handle);
		fr_connection_release(t->pool, request, handle);

		return RLM_MODULE_FAIL;
	}

	return unlang_yield(request, mod_authorize_result, rest_io_action, handle);
}

static rlm_rcode_t mod_authenticate_result(REQUEST *request, void *instance, void *thread, void *ctx)
{
	rlm_rest_t const		*inst = instance;
	rlm_rest_thread_t		*t = thread;
	rlm_rest_section_t const 	*section = &inst->authenticate;
	rlm_rest_handle_t		*handle = ctx;

	int				hcode;
	int				rcode = RLM_MODULE_OK;
	int				ret;

	if (section->tls_extract_cert_attrs) rest_response_certinfo(instance, section, request, handle);

	if (rlm_rest_status_update(request, handle) < 0) {
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	hcode = rest_get_handle_code(handle);
	switch (hcode) {
	case 404:
	case 410:
		rcode = RLM_MODULE_NOTFOUND;
		break;

	case 403:
		rcode = RLM_MODULE_USERLOCK;
		break;

	case 401:
		/*
		 *	Attempt to parse content if there was any.
		 */
		ret = rest_response_decode(inst, section, request, handle);
		if (ret < 0) {
			rcode = RLM_MODULE_FAIL;
			break;
		}

		rcode = RLM_MODULE_REJECT;
		break;

	case 204:
		rcode = RLM_MODULE_OK;
		break;

	default:
		/*
		 *	Attempt to parse content if there was any.
		 */
		if ((hcode >= 200) && (hcode < 300)) {
			ret = rest_response_decode(inst, section, request, handle);
			if (ret < 0) 	   rcode = RLM_MODULE_FAIL;
			else if (ret == 0) rcode = RLM_MODULE_OK;
			else		   rcode = RLM_MODULE_UPDATED;
			break;
		} else if (hcode < 500) {
			rcode = RLM_MODULE_INVALID;
		} else {
			rcode = RLM_MODULE_FAIL;
		}
	}

	switch (rcode) {
	case RLM_MODULE_INVALID:
	case RLM_MODULE_FAIL:
	case RLM_MODULE_USERLOCK:
		rest_response_error(request, handle);
		break;

	default:
		break;
	}

finish:
	rest_request_cleanup(instance, handle);

	fr_connection_release(t->pool, request, handle);

	return rcode;
}

/*
 *	Authenticate the user with the given password.
 */
static rlm_rcode_t CC_HINT(nonnull) mod_authenticate(void *instance, void *thread, REQUEST *request)
{
	rlm_rest_t const		*inst = instance;
	rlm_rest_thread_t		*t = thread;
	rlm_rest_section_t const	*section = &inst->authenticate;
	rlm_rest_handle_t		*handle;

	int				ret;

	VALUE_PAIR const		*username;
	VALUE_PAIR const		*password;

	if (!section->name) return RLM_MODULE_NOOP;

	username = request->username;
	if (!request->username) {
		REDEBUG("Can't perform authentication, 'User-Name' attribute not found in the request");

		return RLM_MODULE_INVALID;
	}

	password = request->password;
	if (!password ||
	    (password->da->attr != PW_USER_PASSWORD)) {
		REDEBUG("You set 'Auth-Type = REST' for a request that does not contain a User-Password attribute!");
		return RLM_MODULE_INVALID;
	}

	handle = fr_connection_get(t->pool, request);
	if (!handle) return RLM_MODULE_FAIL;

	ret = rlm_rest_perform(instance, thread, section,
			       handle, request, username->vp_strvalue, password->vp_strvalue);
	if (ret < 0) {
		rest_request_cleanup(instance, handle);
		fr_connection_release(t->pool, request, handle);

		return RLM_MODULE_FAIL;
	}

	return unlang_yield(request, mod_authenticate_result, NULL, handle);
}

static rlm_rcode_t mod_accounting_result(REQUEST *request, void *instance, void *thread, void *ctx)
{
	rlm_rest_t const		*inst = instance;
	rlm_rest_thread_t		*t = thread;
	rlm_rest_section_t const 	*section = &inst->authenticate;
	rlm_rest_handle_t		*handle = ctx;

	int				hcode;
	int				rcode = RLM_MODULE_OK;
	int				ret;

	if (section->tls_extract_cert_attrs) rest_response_certinfo(instance, section, request, handle);

	if (rlm_rest_status_update(request, handle) < 0) {
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	hcode = rest_get_handle_code(handle);
	if (hcode >= 500) {
		rcode = RLM_MODULE_FAIL;
	} else if (hcode == 204) {
		rcode = RLM_MODULE_OK;
	} else if ((hcode >= 200) && (hcode < 300)) {
		ret = rest_response_decode(inst, section, request, handle);
		if (ret < 0) 	   rcode = RLM_MODULE_FAIL;
		else if (ret == 0) rcode = RLM_MODULE_OK;
		else		   rcode = RLM_MODULE_UPDATED;
	} else {
		rcode = RLM_MODULE_INVALID;
	}

	switch (rcode) {
	case RLM_MODULE_INVALID:
	case RLM_MODULE_FAIL:
		rest_response_error(request, handle);
		break;

	default:
		break;
	}

finish:
	rest_request_cleanup(instance, handle);

	fr_connection_release(t->pool, request, handle);

	return rcode;
}

/*
 *	Send accounting info to a REST API endpoint
 */
static rlm_rcode_t CC_HINT(nonnull) mod_accounting(void *instance, void *thread, REQUEST *request)
{
	rlm_rest_t const		*inst = instance;
	rlm_rest_thread_t		*t = thread;
	rlm_rest_section_t const	*section = &inst->accounting;

	void				*handle;
	int				ret;

	if (!section->name) return RLM_MODULE_NOOP;

	handle = fr_connection_get(t->pool, request);
	if (!handle) return RLM_MODULE_FAIL;

	ret = rlm_rest_perform(inst, thread, section, handle, request, NULL, NULL);
	if (ret < 0) {
		rest_request_cleanup(instance, handle);
		fr_connection_release(t->pool, request, handle);

		return RLM_MODULE_FAIL;
	}

	return unlang_yield(request, mod_accounting_result, NULL, handle);
}

static rlm_rcode_t mod_post_auth_result(REQUEST *request, void *instance, void *thread, void *ctx)
{
	rlm_rest_t const		*inst = instance;
	rlm_rest_thread_t		*t = thread;
	rlm_rest_section_t const 	*section = &inst->authenticate;
	rlm_rest_handle_t		*handle = ctx;

	int				hcode;
	int				rcode = RLM_MODULE_OK;
	int				ret;

	if (section->tls_extract_cert_attrs) rest_response_certinfo(instance, section, request, handle);

	if (rlm_rest_status_update(request, handle) < 0) {
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	hcode = rest_get_handle_code(handle);
	if (hcode >= 500) {
		rcode = RLM_MODULE_FAIL;
	} else if (hcode == 204) {
		rcode = RLM_MODULE_OK;
	} else if ((hcode >= 200) && (hcode < 300)) {
		ret = rest_response_decode(inst, section, request, handle);
		if (ret < 0) 	   rcode = RLM_MODULE_FAIL;
		else if (ret == 0) rcode = RLM_MODULE_OK;
		else		   rcode = RLM_MODULE_UPDATED;
	} else {
		rcode = RLM_MODULE_INVALID;
	}

	switch (rcode) {
	case RLM_MODULE_INVALID:
	case RLM_MODULE_FAIL:
		rest_response_error(request, handle);
		break;

	default:
		break;
	}

finish:
	rest_request_cleanup(inst, handle);

	fr_connection_release(t->pool, request, handle);

	return rcode;
}

/*
 *	Send post-auth info to a REST API endpoint
 */
static rlm_rcode_t CC_HINT(nonnull) mod_post_auth(void *instance, void *thread, REQUEST *request)
{
	rlm_rest_t const		*inst = instance;
	rlm_rest_thread_t		*t = thread;
	rlm_rest_section_t const	*section = &inst->post_auth;

	void				*handle;
	int				ret;

	if (!section->name) return RLM_MODULE_NOOP;

	handle = fr_connection_get(t->pool, request);
	if (!handle) return RLM_MODULE_FAIL;

	ret = rlm_rest_perform(inst, thread, section, handle, request, NULL, NULL);
	if (ret < 0) {
		rest_request_cleanup(instance, handle);

		fr_connection_release(t->pool, request, handle);

		return RLM_MODULE_FAIL;
	}

	return unlang_yield(request, mod_post_auth_result, NULL, handle);
}

static int parse_sub_section(rlm_rest_t *inst, CONF_SECTION *parent, CONF_PARSER const *config_items,
			     rlm_rest_section_t *config, char const *name)
{
	CONF_SECTION *cs;

	cs = cf_subsection_find(parent, name);
	if (!cs) {
		config->name = NULL;
		return 0;
	}

	if (cf_section_parse(inst, config, cs, config_items) < 0) {
		config->name = NULL;
		return -1;
	}

	/*
	 *  Add section name (Maybe add to headers later?).
	 */
	config->name = name;

	/*
	 *  Sanity check
	 */
	 if ((config->username && !config->password) || (!config->username && config->password)) {
		cf_log_err_cs(cs, "'username' and 'password' must both be set or both be absent");

		return -1;
	 }

	/*
	 *  Convert HTTP method auth and body type strings into their integer equivalents.
	 */
	config->auth = fr_str2int(http_auth_table, config->auth_str, HTTP_AUTH_UNKNOWN);
	if (config->auth == HTTP_AUTH_UNKNOWN) {
		cf_log_err_cs(cs, "Unknown HTTP auth type '%s'", config->auth_str);

		return -1;
	} else if ((config->auth != HTTP_AUTH_NONE) && !http_curl_auth[config->auth]) {
		cf_log_err_cs(cs, "Unsupported HTTP auth type \"%s\", check libcurl version, OpenSSL build "
			      "configuration, then recompile this module", config->auth_str);

		return -1;
	}

	config->method = fr_str2int(http_method_table, config->method_str, HTTP_METHOD_CUSTOM);

	/*
	 *  We don't have any custom user data, so we need to select the right encoder based
	 *  on the body type.
	 *
	 *  To make this slightly more/less confusing, we accept both canonical body_types,
	 *  and content_types.
	 */
	if (!config->data) {
		config->body = fr_str2int(http_body_type_table, config->body_str, HTTP_BODY_UNKNOWN);
		if (config->body == HTTP_BODY_UNKNOWN) {
			config->body = fr_str2int(http_content_type_table, config->body_str, HTTP_BODY_UNKNOWN);
		}

		if (config->body == HTTP_BODY_UNKNOWN) {
			cf_log_err_cs(cs, "Unknown HTTP body type '%s'", config->body_str);
			return -1;
		}

		switch (http_body_type_supported[config->body]) {
		case HTTP_BODY_UNSUPPORTED:
			cf_log_err_cs(cs, "Unsupported HTTP body type \"%s\", please submit patches",
				      config->body_str);
			return -1;

		case HTTP_BODY_INVALID:
			cf_log_err_cs(cs, "Invalid HTTP body type.  \"%s\" is not a valid web API data "
				      "markup format", config->body_str);
			return -1;

		case HTTP_BODY_UNAVAILABLE:
			cf_log_err_cs(cs, "Unavailable HTTP body type.  \"%s\" is not available in this "
				      "build", config->body_str);
			return -1;

		default:
			break;
		}
	/*
	 *  We have custom body data so we set HTTP_BODY_CUSTOM_XLAT, but also need to try and
	 *  figure out what content-type to use. So if they've used the canonical form we
	 *  need to convert it back into a proper HTTP content_type value.
	 */
	} else {
		http_body_type_t body;

		config->body = HTTP_BODY_CUSTOM_XLAT;

		body = fr_str2int(http_body_type_table, config->body_str, HTTP_BODY_UNKNOWN);
		if (body != HTTP_BODY_UNKNOWN) {
			config->body_str = fr_int2str(http_content_type_table, body, config->body_str);
		}
	}

	if (config->force_to_str) {
		config->force_to = fr_str2int(http_body_type_table, config->force_to_str, HTTP_BODY_UNKNOWN);
		if (config->force_to == HTTP_BODY_UNKNOWN) {
			config->force_to = fr_str2int(http_content_type_table, config->force_to_str, HTTP_BODY_UNKNOWN);
		}

		if (config->force_to == HTTP_BODY_UNKNOWN) {
			cf_log_err_cs(cs, "Unknown forced response body type '%s'", config->force_to_str);
			return -1;
		}

		switch (http_body_type_supported[config->force_to]) {
		case HTTP_BODY_UNSUPPORTED:
			cf_log_err_cs(cs, "Unsupported forced response body type \"%s\", please submit patches",
				      config->force_to_str);
			return -1;

		case HTTP_BODY_INVALID:
			cf_log_err_cs(cs, "Invalid HTTP forced response body type.  \"%s\" is not a valid web API data "
				      "markup format", config->force_to_str);
			return -1;

		default:
			break;
		}
	}

	return 0;
}

/** Create a thread specific multihandle
 *
 * Easy handles representing requests are added to the curl multihandle
 * with the multihandle used for mux/demux.
 *
 * @param[in] conf	section containing the configuration of this module instance.
 * @param[in] instance	of rlm_rest_t.
 * @param[in] thread	specific data.
 * @param[in] el	The event list serviced by this thread.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_thread_instantiate(CONF_SECTION const *conf, void *instance, fr_event_list_t *el, void *thread)
{
	rlm_rest_t		*inst = instance;
	rlm_rest_thread_t	*t = thread;
	CONF_SECTION		*my_conf;

	t->el = el;
	t->inst = instance;

	/*
	 *	Temporary hack to make config parsing
	 *	thread safe.
	 */
	my_conf = cf_section_dup(NULL, conf, cf_section_name1(conf), cf_section_name2(conf), true);
	t->pool = fr_connection_pool_init(NULL, my_conf, instance, mod_conn_create, NULL, inst->xlat_name);
	talloc_free(my_conf);

	if (!t->pool) {
		ERROR("Pool instantiation failed");
		return -1;
	}

	return rest_io_init(t);
}

/** Cleanup all outstanding requests associated with this thread
 *
 * Destroys all curl easy handles, and then the multihandle associated
 * with this thread.
 *
 * @param[in] thread	specific data to destroy.
 * @return 0
 */
static int mod_thread_detach(void *thread)
{
	rlm_rest_thread_t	*t = thread;

	curl_multi_cleanup(t->mandle);
	fr_connection_pool_free(t->pool);

	return 0;
}

/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 */
static int mod_instantiate(CONF_SECTION *conf, void *instance)
{
	rlm_rest_t *inst = instance;

	inst->xlat.method_str = "GET";
	inst->xlat.body = HTTP_BODY_NONE;
	inst->xlat.body_str = "application/x-www-form-urlencoded";
	inst->xlat.force_to_str = "plain";

	/*
	 *	Parse sub-section configs.
	 */
	if (
		(parse_sub_section(inst, conf, xlat_config, &inst->xlat, "xlat") < 0) ||
		(parse_sub_section(inst, conf, section_config, &inst->authorize,
				   section_type_value[MOD_AUTHORIZE].section) < 0) ||
		(parse_sub_section(inst, conf, section_config, &inst->authenticate,
				   section_type_value[MOD_AUTHENTICATE].section) < 0) ||
		(parse_sub_section(inst, conf, section_config, &inst->accounting,
				   section_type_value[MOD_ACCOUNTING].section) < 0) ||

/* @todo add behaviour for checksimul */
/*		(parse_sub_section(conf, section_config, &inst->checksimul,
				   section_type_value[MOD_SESSION].section) < 0) || */
		(parse_sub_section(inst, conf, section_config, &inst->post_auth,
				   section_type_value[MOD_POST_AUTH].section) < 0))
	{
		return -1;
	}

	return 0;
}

static int mod_bootstrap(CONF_SECTION *conf, void *instance)
{
	rlm_rest_t *inst = instance;

	inst->xlat_name = cf_section_name2(conf);
	if (!inst->xlat_name) inst->xlat_name = cf_section_name1(conf);

#if 0
	/*
	 *	Register the rest xlat function
	 */
	xlat_register(inst, inst->xlat_name, rest_xlat, rest_uri_escape, NULL, 0, 0);
#endif

	return 0;
}

/** Initialises libcurl.
 *
 * Allocates global variables and memory required for libcurl to function.
 * MUST only be called once per module instance.
 *
 * mod_unload must not be called if mod_load fails.
 *
 * @see mod_unload
 *
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_load(void)
{
	CURLcode ret;

	curl_version_info_data *curlversion;

	/* developer sanity */
	rad_assert((sizeof(http_body_type_supported) / sizeof(*http_body_type_supported)) == HTTP_BODY_NUM_ENTRIES);

	ret = curl_global_init(CURL_GLOBAL_ALL);
	if (ret != CURLE_OK) {
		ERROR("rlm_curl - CURL init returned error: %i - %s", ret, curl_easy_strerror(ret));
		return -1;
	}

	curlversion = curl_version_info(CURLVERSION_NOW);
	if (strcmp(LIBCURL_VERSION, curlversion->version) != 0) {
		WARN("rlm_curl - libcurl version changed since the server was built");
		WARN("rlm_curl - linked: %s built: %s", curlversion->version, LIBCURL_VERSION);
	}

	INFO("rlm_curl - libcurl version: %s", curl_version());

#ifdef HAVE_JSON
	fr_json_version_print();
#endif

	return 0;
}

/** Called to free resources held by libcurl
 *
 * @see mod_load
 */
static void mod_unload(void)
{
	curl_global_cleanup();
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
extern rad_module_t rlm_rest;
rad_module_t rlm_rest = {
	.magic			= RLM_MODULE_INIT,
	.name			= "rest",
	.type			= RLM_TYPE_THREAD_SAFE,
	.inst_size		= sizeof(rlm_rest_t),
	.thread_inst_size	= sizeof(rlm_rest_thread_t),
	.config			= module_config,
	.load			= mod_load,
	.unload			= mod_unload,
	.bootstrap		= mod_bootstrap,
	.instantiate		= mod_instantiate,
	.thread_instantiate	= mod_thread_instantiate,
	.thread_detach		= mod_thread_detach,
	.methods = {
		[MOD_AUTHENTICATE]	= mod_authenticate,
		[MOD_AUTHORIZE]		= mod_authorize,
		[MOD_ACCOUNTING]	= mod_accounting,
		[MOD_POST_AUTH]		= mod_post_auth
	},
};
