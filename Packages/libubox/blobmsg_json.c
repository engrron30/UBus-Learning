/*
 * Copyright (C) 2010-2012 Felix Fietkau <nbd@openwrt.org>
 *
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
#include <inttypes.h>
#include "blobmsg.h"
#include "blobmsg_json.h"

#include <json.h>

bool blobmsg_add_object(struct blob_buf *b, json_object *obj)
{
	json_object_object_foreach(obj, key, val) {
		if (!blobmsg_add_json_element(b, key, val))
			return false;
	}
	return true;
}

static bool blobmsg_add_array(struct blob_buf *b, struct array_list *a)
{
	int i, len;

	for (i = 0, len = array_list_length(a); i < len; i++) {
		if (!blobmsg_add_json_element(b, NULL, array_list_get_idx(a, i)))
			return false;
	}

	return true;
}

bool blobmsg_add_json_element(struct blob_buf *b, const char *name, json_object *obj)
{
	bool ret = true;
	void *c;

	switch (json_object_get_type(obj)) {
	case json_type_object:
		c = blobmsg_open_table(b, name);
		ret = blobmsg_add_object(b, obj);
		blobmsg_close_table(b, c);
		break;
	case json_type_array:
		c = blobmsg_open_array(b, name);
		ret = blobmsg_add_array(b, json_object_get_array(obj));
		blobmsg_close_array(b, c);
		break;
	case json_type_string:
		blobmsg_add_string(b, name, json_object_get_string(obj));
		break;
	case json_type_boolean:
		blobmsg_add_u8(b, name, json_object_get_boolean(obj));
		break;
	case json_type_int: {
		int64_t i64 = json_object_get_int64(obj);
		if (i64 >= INT32_MIN && i64 <= INT32_MAX) {
			blobmsg_add_u32(b, name, (uint32_t)i64);
		} else {
			blobmsg_add_u64(b, name, (uint64_t)i64);
		}
		break;
	}
	case json_type_double:
		blobmsg_add_double(b, name, json_object_get_double(obj));
		break;
	case json_type_null:
		blobmsg_add_field(b, BLOBMSG_TYPE_UNSPEC, name, NULL, 0);
		break;
	default:
		return false;
	}
	return ret;
}

static bool __blobmsg_add_json(struct blob_buf *b, json_object *obj)
{
	bool ret = false;

	if (!obj)
		return false;

	if (json_object_get_type(obj) != json_type_object)
		goto out;

	ret = blobmsg_add_object(b, obj);

out:
	json_object_put(obj);
	return ret;
}

bool blobmsg_add_json_from_file(struct blob_buf *b, const char *file)
{
	return __blobmsg_add_json(b, json_object_from_file(file));
}

bool blobmsg_add_json_from_string(struct blob_buf *b, const char *str)
{
	return __blobmsg_add_json(b, json_tokener_parse(str));
}


struct strbuf {
	int len;
	int pos;
	char *buf;

	blobmsg_json_format_t custom_format;
	void *priv;
	bool indent;
	int indent_level;
};

static bool blobmsg_puts(struct strbuf *s, const char *c, int len)
{
	size_t new_len;
	char *new_buf;

	if (len <= 0)
		return true;

	if (s->pos + len >= s->len) {
		new_len = s->len + 16 + len;
		new_buf = realloc(s->buf, new_len);
		if (!new_buf)
			return false;

		s->len = new_len;
		s->buf = new_buf;
	}

	memcpy(s->buf + s->pos, c, len);
	s->pos += len;
	return true;
}

static void add_separator(struct strbuf *s)
{
	const char indent_chars[] = "\n\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t";
	size_t len;

	if (!s->indent)
		return;

	len = s->indent_level + 1;
	if (len > sizeof(indent_chars) - 1)
		len = sizeof(indent_chars) - 1;

	blobmsg_puts(s, indent_chars, len);
}


static void blobmsg_format_string(struct strbuf *s, const char *str)
{
	const unsigned char *p, *last, *end;
	char buf[8] = "\\u00";

	end = (unsigned char *) str + strlen(str);
	blobmsg_puts(s, "\"", 1);
	for (p = (unsigned char *) str, last = p; *p; p++) {
		char escape = '\0';
		int len;

		switch(*p) {
		case '\b':
			escape = 'b';
			break;
		case '\n':
			escape = 'n';
			break;
		case '\t':
			escape = 't';
			break;
		case '\r':
			escape = 'r';
			break;
		case '"':
		case '\\':
			escape = *p;
			break;
		default:
			if (*p < ' ')
				escape = 'u';
			break;
		}

		if (!escape)
			continue;

		if (p > last)
			blobmsg_puts(s, (char *) last, p - last);
		last = p + 1;
		buf[1] = escape;

		if (escape == 'u') {
			snprintf(buf + 4, sizeof(buf) - 4, "%02x", (unsigned char) *p);
			len = 6;
		} else {
			len = 2;
		}
		blobmsg_puts(s, buf, len);
	}

	blobmsg_puts(s, (char *) last, end - last);
	blobmsg_puts(s, "\"", 1);
}

static void blobmsg_format_json_list(struct strbuf *s, struct blob_attr *attr, int len, bool array);

static void blobmsg_format_element(struct strbuf *s, struct blob_attr *attr, bool without_name, bool head)
{
	const char *data_str;
	char buf[317];
	void *data;
	int len;

	if (!blobmsg_check_attr(attr, false))
		return;

	if (!without_name && blobmsg_name(attr)[0]) {
		blobmsg_format_string(s, blobmsg_name(attr));
		blobmsg_puts(s, ": ", s->indent ? 2 : 1);
	}

	data = blobmsg_data(attr);
	len = blobmsg_data_len(attr);

	if (!head && s->custom_format) {
		data_str = s->custom_format(s->priv, attr);
		if (data_str)
			goto out;
	}

	data_str = buf;
	switch(blob_id(attr)) {
	case BLOBMSG_TYPE_UNSPEC:
		snprintf(buf, sizeof(buf), "null");
		break;
	case BLOBMSG_TYPE_BOOL:
		snprintf(buf, sizeof(buf), "%s", *(uint8_t *)data ? "true" : "false");
		break;
	case BLOBMSG_TYPE_INT16:
		snprintf(buf, sizeof(buf), "%" PRId16, (int16_t) be16_to_cpu(*(uint16_t *)data));
		break;
	case BLOBMSG_TYPE_INT32:
		snprintf(buf, sizeof(buf), "%" PRId32, (int32_t) be32_to_cpu(*(uint32_t *)data));
		break;
	case BLOBMSG_TYPE_INT64:
		snprintf(buf, sizeof(buf), "%" PRId64, (int64_t) be64_to_cpu(*(uint64_t *)data));
		break;
	case BLOBMSG_TYPE_DOUBLE:
		snprintf(buf, sizeof(buf), "%lf", blobmsg_get_double(attr));
		break;
	case BLOBMSG_TYPE_STRING:
		blobmsg_format_string(s, data);
		return;
	case BLOBMSG_TYPE_ARRAY:
		blobmsg_format_json_list(s, data, len, true);
		return;
	case BLOBMSG_TYPE_TABLE:
		blobmsg_format_json_list(s, data, len, false);
		return;
	}

out:
	blobmsg_puts(s, data_str, strlen(data_str));
}

static void blobmsg_format_json_list(struct strbuf *s, struct blob_attr *attr, int len, bool array)
{
	struct blob_attr *pos;
	bool first = true;
	size_t rem = len;

	blobmsg_puts(s, (array ? "[" : "{" ), 1);
	s->indent_level++;
	add_separator(s);
	__blob_for_each_attr(pos, attr, rem) {
		if (!first) {
			blobmsg_puts(s, ",", 1);
			add_separator(s);
		}

		blobmsg_format_element(s, pos, array, false);
		first = false;
	}
	s->indent_level--;
	add_separator(s);
	blobmsg_puts(s, (array ? "]" : "}"), 1);
}

static void setup_strbuf(struct strbuf *s, struct blob_attr *attr, blobmsg_json_format_t cb, void *priv, int indent)
{
	s->len = blob_len(attr);
	s->buf = malloc(s->len);
	s->pos = 0;
	s->custom_format = cb;
	s->priv = priv;
	s->indent = false;

	if (indent >= 0) {
		s->indent = true;
		s->indent_level = indent;
	}
}

char *blobmsg_format_json_with_cb(struct blob_attr *attr, bool list, blobmsg_json_format_t cb, void *priv, int indent)
{
	struct strbuf s = {0};
	bool array;
	char *ret;

	setup_strbuf(&s, attr, cb, priv, indent);
	if (!s.buf)
		return NULL;

	array = blob_is_extended(attr) &&
		blobmsg_type(attr) == BLOBMSG_TYPE_ARRAY;

	if (list)
		blobmsg_format_json_list(&s, blobmsg_data(attr), blobmsg_data_len(attr), array);
	else
		blobmsg_format_element(&s, attr, false, false);

	if (!s.len) {
		free(s.buf);
		return NULL;
	}

	ret = realloc(s.buf, s.pos + 1);
	if (!ret) {
		free(s.buf);
		return NULL;
	}

	ret[s.pos] = 0;

	return ret;
}

char *blobmsg_format_json_value_with_cb(struct blob_attr *attr, blobmsg_json_format_t cb, void *priv, int indent)
{
	struct strbuf s = {0};
	char *ret;

	setup_strbuf(&s, attr, cb, priv, indent);
	if (!s.buf)
		return NULL;

	blobmsg_format_element(&s, attr, true, false);

	if (!s.len) {
		free(s.buf);
		return NULL;
	}

	ret = realloc(s.buf, s.pos + 1);
	if (!ret) {
		free(s.buf);
		return NULL;
	}

	ret[s.pos] = 0;

	return ret;
}
