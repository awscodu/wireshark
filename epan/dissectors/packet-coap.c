/* packet-coap.c
 * Routines for CoAP packet disassembly
 * draft-ietf-core-coap-14.txt
 * draft-ietf-core-block-10.txt
 * draft-ietf-core-observe-16.txt
 * draft-ietf-core-link-format-06.txt
 * Shoichi Sakane <sakane@tanu.org>
 *
 * Changes for draft-ietf-core-coap-17.txt
 * Hauke Mehrtens <hauke@hauke-m.de>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"


#include <epan/conversation.h>
#include <epan/packet.h>
#include <epan/proto_data.h>
#include <epan/expert.h>
#include <epan/wmem/wmem.h>
#include "packet-dtls.h"
#include "packet-coap.h"

void proto_register_coap(void);

static dissector_table_t media_type_dissector_table;

static int proto_coap						= -1;

static int hf_coap_version					= -1;
static int hf_coap_ttype					= -1;
static int hf_coap_token_len					= -1;
static int hf_coap_token					= -1;
static int hf_coap_code						= -1;
static int hf_coap_mid						= -1;
static int hf_coap_payload					= -1;
static int hf_coap_payload_desc					= -1;
static int hf_coap_payload_length				= -1;
static int hf_coap_opt_name					= -1;
static int hf_coap_opt_desc					= -1;
static int hf_coap_opt_delta					= -1;
static int hf_coap_opt_delta_ext				= -1;
static int hf_coap_opt_length					= -1;
static int hf_coap_opt_length_ext				= -1;
static int hf_coap_opt_end_marker				= -1;
static int hf_coap_opt_ctype					= -1;
static int hf_coap_opt_max_age					= -1;
static int hf_coap_opt_proxy_uri				= -1;
static int hf_coap_opt_proxy_scheme				= -1;
static int hf_coap_opt_size1					= -1;
static int hf_coap_opt_etag					= -1;
static int hf_coap_opt_uri_host					= -1;
static int hf_coap_opt_location_path				= -1;
static int hf_coap_opt_uri_port					= -1;
static int hf_coap_opt_location_query				= -1;
static int hf_coap_opt_uri_path					= -1;
static int hf_coap_opt_uri_path_recon				= -1;
static int hf_coap_opt_observe					= -1;
static int hf_coap_opt_accept					= -1;
static int hf_coap_opt_if_match					= -1;
static int hf_coap_opt_block_number				= -1;
static int hf_coap_opt_block_mflag				= -1;
static int hf_coap_opt_block_size				= -1;
static int hf_coap_opt_uri_query				= -1;
static int hf_coap_opt_unknown					= -1;
static int hf_coap_opt_object_security_non_compressed		= -1;
static int hf_coap_opt_object_security_expand			= -1;
static int hf_coap_opt_object_security_signature		= -1;
static int hf_coap_opt_object_security_kid_context_present	= -1;
static int hf_coap_opt_object_security_kid_present		= -1;
static int hf_coap_opt_object_security_piv_len			= -1;
static int hf_coap_opt_object_security_piv			= -1;
static int hf_coap_opt_object_security_kid_context_len		= -1;
static int hf_coap_opt_object_security_kid_context		= -1;
static int hf_coap_opt_object_security_kid			= -1;

static int hf_coap_response_in					= -1;
static int hf_coap_response_to					= -1;
static int hf_coap_response_time				= -1;

static gint ett_coap						= -1;
static gint ett_coap_option					= -1;
static gint ett_coap_payload					= -1;

static expert_field ei_coap_invalid_option_number		= EI_INIT;
static expert_field ei_coap_invalid_option_range		= EI_INIT;
static expert_field ei_coap_option_length_bad			= EI_INIT;
static expert_field ei_coap_option_object_security_bad		= EI_INIT;

static dissector_handle_t coap_handle;

/* CoAP's IANA-assigned port (UDP only) number */
#define DEFAULT_COAP_PORT					5683
#define DEFAULT_COAPS_PORT					5684

/* indicators whether those are to be showed or not */
#define DEFAULT_COAP_CTYPE_VALUE				~0U
#define DEFAULT_COAP_BLOCK_NUMBER				~0U

/* bitmasks */
#define COAP_VERSION_MASK					0xC0
#define COAP_TYPE_MASK						0x30
#define COAP_TOKEN_LEN_MASK					0x0F
#define COAP_BLOCK_MFLAG_MASK					0x08
#define COAP_BLOCK_SIZE_MASK					0x07
#define COAP_OBJECT_SECURITY_NON_COMPRESSED_MASK		0x80
#define COAP_OBJECT_SECURITY_EXPAND_MASK			0x40
#define COAP_OBJECT_SECURITY_SIGNATURE_MASK			0x20
#define COAP_OBJECT_SECURITY_KID_CONTEXT_MASK			0x10
#define COAP_OBJECT_SECURITY_KID_MASK				0x08
#define COAP_OBJECT_SECURITY_PIVLEN_MASK			0x07

/*
 * Transaction Type
 */
static const value_string vals_ttype[] = {
	{ 0, "Confirmable" },
	{ 1, "Non-Confirmable" },
	{ 2, "Acknowledgement" },
	{ 3, "Reset" },
	{ 0, NULL },
};
static const value_string vals_ttype_short[] = {
	{ 0, "CON" },
	{ 1, "NON" },
	{ 2, "ACK" },
	{ 3, "RST" },
	{ 0, NULL },
};

/*
 * Method Code
 * Response Code
 */
static const value_string vals_code[] = {
	{ 0, "Empty Message" },

	/* method code */
	{ 1, "GET" },
	{ 2, "POST" },
	{ 3, "PUT" },
	{ 4, "DELETE" },
	{ 5, "FETCH" },		/* RFC8132 */
	{ 6, "PATCH" },		/* RFC8132 */
	{ 7, "iPATCH" },	/* RFC8132 */

	/* response code */
	{  65, "2.01 Created" },
	{  66, "2.02 Deleted" },
	{  67, "2.03 Valid" },
	{  68, "2.04 Changed" },
	{  69, "2.05 Content" },
	{  95, "2.31 Continue" },
	{ 128, "4.00 Bad Request" },
	{ 129, "4.01 Unauthorized" },
	{ 130, "4.02 Bad Option" },
	{ 131, "4.03 Forbidden" },
	{ 132, "4.04 Not Found" },
	{ 133, "4.05 Method Not Allowed" },
	{ 134, "4.06 Not Acceptable" },
	{ 136, "4.08 Request Entity Incomplete" },	/* core-block-10 */
	{ 137, "4.09 Conflict" },			/* RFC8132 */
	{ 140, "4.12 Precondition Failed" },
	{ 141, "4.13 Request Entity Too Large" },
	{ 143, "4.15 Unsupported Content-Format" },
	{ 150, "4.22 Unprocessable Entity" },		/* RFC8132 */
	{ 160, "5.00 Internal Server Error" },
	{ 161, "5.01 Not Implemented" },
	{ 162, "5.02 Bad Gateway" },
	{ 163, "5.03 Service Unavailable" },
	{ 164, "5.04 Gateway Timeout" },
	{ 165, "5.05 Proxying Not Supported" },

	{ 0, NULL },
};
static value_string_ext vals_code_ext = VALUE_STRING_EXT_INIT(vals_code);

static const value_string vals_observe_options[] = {
	{ 0, "Register" },
	{ 1, "Deregister" },
	{ 0, NULL },
};

/*
 * Option Headers
 * No-Option must not be included in this structure, is handled in the function
 * of the dissector, especially.
 */
#define COAP_OPT_IF_MATCH		1
#define COAP_OPT_URI_HOST		3
#define COAP_OPT_ETAG			4
#define COAP_OPT_IF_NONE_MATCH		5
#define COAP_OPT_OBSERVE		6	/* core-observe-16 */
#define COAP_OPT_URI_PORT		7
#define COAP_OPT_LOCATION_PATH		8
#define COAP_OPT_URI_PATH		11
#define COAP_OPT_CONTENT_TYPE		12
#define COAP_OPT_MAX_AGE		14
#define COAP_OPT_URI_QUERY		15
#define COAP_OPT_ACCEPT			17
#define COAP_OPT_LOCATION_QUERY		20
#define COAP_OPT_OBJECT_SECURITY	21	/* value used in OSCORE plugtests */
#define COAP_OPT_BLOCK2			23	/* core-block-10 */
#define COAP_OPT_BLOCK_SIZE		28	/* core-block-10 */
#define COAP_OPT_BLOCK1			27	/* core-block-10 */
#define COAP_OPT_PROXY_URI		35
#define COAP_OPT_PROXY_SCHEME		39
#define COAP_OPT_SIZE1			60

static const value_string vals_opt_type[] = {
	{ COAP_OPT_IF_MATCH,       "If-Match" },
	{ COAP_OPT_URI_HOST,       "Uri-Host" },
	{ COAP_OPT_ETAG,           "Etag" },
	{ COAP_OPT_IF_NONE_MATCH,  "If-None-Match" },
	{ COAP_OPT_URI_PORT,       "Uri-Port" },
	{ COAP_OPT_LOCATION_PATH,  "Location-Path" },
	{ COAP_OPT_URI_PATH,       "Uri-Path" },
	{ COAP_OPT_CONTENT_TYPE,   "Content-Format" },
	{ COAP_OPT_MAX_AGE,        "Max-age" },
	{ COAP_OPT_URI_QUERY,      "Uri-Query" },
	{ COAP_OPT_ACCEPT,         "Accept" },
	{ COAP_OPT_LOCATION_QUERY, "Location-Query" },
	{ COAP_OPT_OBJECT_SECURITY,"Object-Security" },
	{ COAP_OPT_PROXY_URI,      "Proxy-Uri" },
	{ COAP_OPT_PROXY_SCHEME,   "Proxy-Scheme" },
	{ COAP_OPT_SIZE1,          "Size1" },
	{ COAP_OPT_OBSERVE,        "Observe" },
	{ COAP_OPT_BLOCK2,         "Block2" },
	{ COAP_OPT_BLOCK1,         "Block1" },
	{ COAP_OPT_BLOCK_SIZE,     "Block Size" },
	{ 0, NULL },
};

struct coap_option_range_t {
	guint type;
	gint min;
	gint max;
} coi[] = {
	{ COAP_OPT_IF_MATCH,        0,   8 },
	{ COAP_OPT_URI_HOST,        1, 255 },
	{ COAP_OPT_ETAG,            1,   8 },
	{ COAP_OPT_IF_NONE_MATCH,   0,   0 },
	{ COAP_OPT_URI_PORT,        0,   2 },
	{ COAP_OPT_LOCATION_PATH,   0, 255 },
	{ COAP_OPT_URI_PATH,        0, 255 },
	{ COAP_OPT_CONTENT_TYPE,    0,   2 },
	{ COAP_OPT_MAX_AGE,         0,   4 },
	{ COAP_OPT_URI_QUERY,       1, 255 },
	{ COAP_OPT_ACCEPT,          0,   2 },
	{ COAP_OPT_LOCATION_QUERY,  0, 255 },
	{ COAP_OPT_OBJECT_SECURITY, 0, 255 },
	{ COAP_OPT_PROXY_URI,       1,1034 },
	{ COAP_OPT_PROXY_SCHEME,    1, 255 },
	{ COAP_OPT_SIZE1,           0,   4 },
	{ COAP_OPT_OBSERVE,         0,   3 },
	{ COAP_OPT_BLOCK2,          0,   3 },
	{ COAP_OPT_BLOCK1,          0,   3 },
	{ COAP_OPT_BLOCK_SIZE,      0,   4 },
};

static const value_string vals_ctype[] = {
	{  0, "text/plain; charset=utf-8" },
	{ 40, "application/link-format" },
	{ 41, "application/xml" },
	{ 42, "application/octet-stream" },
	{ 47, "application/exi" },
	{ 50, "application/json" },
	{ 60, "application/cbor" },
	{ 1542, "application/vnd.oma.lwm2m+tlv" },
<<<<<<< HEAD
	{ 1543, "application/vnd.oma.lwm2m+json" },
	{ 11542, "application/vnd.oma.lwm2m+tlv" },
	{ 11543, "application/vnd.oma.lwm2m+json" },
=======
	{ 11542, "application/vnd.oma.lwm2m+tlv" },
>>>>>>> upstream/master-2.4
	{ 0, NULL },
};

static const char *nullstr = "(null)";

void proto_reg_handoff_coap(void);

static conversation_t *
find_or_create_conversation_noaddrb(packet_info *pinfo, gboolean request)
{
	conversation_t *conv=NULL;
	address *addr_a;
	address *addr_b;
	guint32 port_a;
	guint32 port_b;

	if (request) {
		addr_a = &pinfo->src;
		addr_b = &pinfo->dst;
		port_a = pinfo->srcport;
		port_b = pinfo->destport;
	} else {
		addr_a = &pinfo->dst;
		addr_b = &pinfo->src;
		port_a = pinfo->destport;
		port_b = pinfo->srcport;
	}
	/* Have we seen this conversation before? */
	if((conv = find_conversation(pinfo->num, addr_a, addr_b,
				     conversation_pt_to_endpoint_type(pinfo->ptype), port_a,
				     port_b, NO_ADDR_B|NO_PORT_B)) != NULL) {
		if (pinfo->num > conv->last_frame) {
			conv->last_frame = pinfo->num;
		}
	} else {
		/* No, this is a new conversation. */
		conv = conversation_new(pinfo->num, &pinfo->src,
					&pinfo->dst, conversation_pt_to_endpoint_type(pinfo->ptype),
					pinfo->srcport, pinfo->destport, NO_ADDR_B|NO_PORT_B);
	}
	return conv;
}

static gint
coap_get_opt_uint(tvbuff_t *tvb, gint offset, gint length)
{
	switch (length) {
	case 0:
		return 0;
	case 1:
		return (guint)tvb_get_guint8(tvb, offset);
	case 2:
		return (guint)tvb_get_ntohs(tvb, offset);
	case 3:
		return (guint)tvb_get_ntoh24(tvb, offset);
	case 4:
		return (guint)tvb_get_ntohl(tvb, offset);
	default:
		return -1;
	}
}

static gint
coap_opt_check(packet_info *pinfo, proto_tree *subtree, guint opt_num, gint opt_length)
{
	int i;

	for (i = 0; i < (int)(array_length(coi)); i++) {
		if (coi[i].type == opt_num)
			break;
	}
	if (i == (int)(array_length(coi))) {
		expert_add_info_format(pinfo, subtree, &ei_coap_invalid_option_number,
			"Invalid Option Number %u", opt_num);
		return -1;
	}
	if (opt_length < coi[i].min || opt_length > coi[i].max) {
		expert_add_info_format(pinfo, subtree, &ei_coap_invalid_option_range,
			"Invalid Option Range: %d (%d < x < %d)", opt_length, coi[i].min, coi[i].max);
	}

	return 0;
}

static void
dissect_coap_opt_hex_string(tvbuff_t *tvb, proto_item *item, proto_tree *subtree, gint offset, gint opt_length, int hf)
{
	const guint8 *str;

	if (opt_length == 0)
		str = nullstr;
	else
		str = tvb_bytes_to_str_punct(wmem_packet_scope(), tvb, offset, opt_length, ' ');

	proto_tree_add_item(subtree, hf, tvb, offset, opt_length, ENC_NA);

	/* add info to the head of the packet detail */
	proto_item_append_text(item, ": %s", str);
}

static void
dissect_coap_opt_uint(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length, int hf)
{
	guint i = 0;

	if (opt_length != 0) {
		i = coap_get_opt_uint(tvb, offset, opt_length);
	}

	proto_tree_add_uint(subtree, hf, tvb, offset, opt_length, i);

	/* add info to the head of the packet detail */
	proto_item_append_text(head_item, ": %u", i);
}

static void
dissect_coap_opt_uri_host(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length, coap_info *coinfo)
{
	const guint8 *str;

	proto_tree_add_item_ret_string(subtree, hf_coap_opt_uri_host, tvb, offset, opt_length, ENC_ASCII, wmem_packet_scope(), &str);

	/* add info to the head of the packet detail */
	proto_item_append_text(head_item, ": %s", str);

	/* forming a uri-string
	 *   If the 'uri host' looks an IPv6 address, assuming that the address has
	 *   to be enclosed by brackets.
	 */
	if (strchr(str, ':') == NULL) {
		wmem_strbuf_append_printf(coinfo->uri_str_strbuf, "coap://%s", str);
	} else {
		wmem_strbuf_append_printf(coinfo->uri_str_strbuf, "coap://[%s]", str);
	}
}

static void
dissect_coap_opt_uri_path(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length, coap_info *coinfo)
{
	const guint8 *str = NULL;

	wmem_strbuf_append_c(coinfo->uri_str_strbuf, '/');

	if (opt_length == 0) {
		str = nullstr;
	} else {
		str = tvb_get_string_enc(wmem_packet_scope(), tvb, offset, opt_length, ENC_ASCII);
		wmem_strbuf_append(coinfo->uri_str_strbuf, str);
	}

	proto_tree_add_string(subtree, hf_coap_opt_uri_path, tvb, offset, opt_length, str);

	/* add info to the head of the packet detail */
	proto_item_append_text(head_item, ": %s", str);
}

static void
dissect_coap_opt_uri_query(tvbuff_t *tvb, proto_item *head_item,proto_tree *subtree, gint offset, gint opt_length, coap_info *coinfo)
{
	const guint8 *str = NULL;

	wmem_strbuf_append_c(coinfo->uri_query_strbuf,
			     (wmem_strbuf_get_len(coinfo->uri_query_strbuf) == 0) ? '?' : '&');

	if (opt_length == 0) {
		str = nullstr;
	} else {
		str = tvb_get_string_enc(wmem_packet_scope(), tvb, offset, opt_length, ENC_ASCII);
		wmem_strbuf_append(coinfo->uri_query_strbuf, str);
	}

	proto_tree_add_string(subtree, hf_coap_opt_uri_query, tvb, offset, opt_length, str);

	/* add info to the head of the packet detail */
	proto_item_append_text(head_item, ": %s", str);
}

static void
dissect_coap_opt_location_path(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length)
{
	const guint8 *str = NULL;

	if (opt_length == 0) {
		str = nullstr;
	} else {
		str = tvb_get_string_enc(wmem_packet_scope(), tvb, offset, opt_length, ENC_ASCII);
	}

	proto_tree_add_string(subtree, hf_coap_opt_location_path, tvb, offset, opt_length, str);

	/* add info to the head of the packet detail */
	proto_item_append_text(head_item, ": %s", str);
}

static void
dissect_coap_opt_location_query(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length)
{
	const guint8 *str = NULL;

	if (opt_length == 0) {
		str = nullstr;
	} else {
		str = tvb_get_string_enc(wmem_packet_scope(), tvb, offset, opt_length, ENC_ASCII);
	}

	proto_tree_add_string(subtree, hf_coap_opt_location_query, tvb, offset, opt_length, str);

	/* add info to the head of the packet detail */
	proto_item_append_text(head_item, ": %s", str);
}

/* draft-ietf-core-object-security-07 */
static void
dissect_coap_opt_object_security(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length, packet_info *pinfo, coap_info *coinfo)
{
	const guint8 *kid = NULL;
	const guint8 *kid_context = NULL;
	const guint8 *piv = NULL;
	gboolean non_compressed = FALSE;
	gboolean expand = FALSE;
	gboolean signature_present = FALSE;
	gboolean kid_context_present = FALSE;
	guint8 kid_context_len = 0;
	gboolean kid_present = FALSE;
	guint8 piv_len = 0;
	guint8 flag_byte = 0;
	gint8 kid_len = 0;

	coinfo->object_security = TRUE;

	if (opt_length == 0) { /* option length is zero, means flag byte is 0x00*/
		/* add info to the head of the packet detail */
		proto_item_append_text(head_item, ": 00 (no Flag Byte)");
	} else {
		flag_byte = tvb_get_guint8(tvb, offset);

		proto_tree_add_item(subtree, hf_coap_opt_object_security_non_compressed, tvb, offset, 1, ENC_BIG_ENDIAN);
		non_compressed = flag_byte & COAP_OBJECT_SECURITY_NON_COMPRESSED_MASK;

		proto_tree_add_item(subtree, hf_coap_opt_object_security_expand, tvb, offset, 1, ENC_BIG_ENDIAN);
		expand = flag_byte & COAP_OBJECT_SECURITY_EXPAND_MASK;

		proto_tree_add_item(subtree, hf_coap_opt_object_security_signature, tvb, offset, 1, ENC_BIG_ENDIAN);
		signature_present = flag_byte & COAP_OBJECT_SECURITY_SIGNATURE_MASK;

		proto_tree_add_item(subtree, hf_coap_opt_object_security_kid_context_present, tvb, offset, 1, ENC_BIG_ENDIAN);
		kid_context_present = flag_byte & COAP_OBJECT_SECURITY_KID_CONTEXT_MASK;

		proto_tree_add_item(subtree, hf_coap_opt_object_security_kid_present, tvb, offset, 1, ENC_BIG_ENDIAN);
		kid_present = flag_byte & COAP_OBJECT_SECURITY_KID_MASK;

		proto_tree_add_item(subtree, hf_coap_opt_object_security_piv_len, tvb, offset, 1, ENC_BIG_ENDIAN);
		piv_len = (flag_byte & COAP_OBJECT_SECURITY_PIVLEN_MASK) >> 0;

		/* kid_len is what remains in the option after all other fields are parsed
		we calculate kid_len by subtracting from option length as we parse individual fields */
		kid_len = opt_length;

		offset += 1;
		kid_len -= 1;

		if (non_compressed || expand || signature_present) {
			/* how these bits are handled is not yet specified */
			expert_add_info_format(pinfo, subtree, &ei_coap_option_object_security_bad, "Unsupported format");
		}

		if (piv_len > 0) {
			proto_tree_add_item(subtree, hf_coap_opt_object_security_piv, tvb, offset, piv_len, ENC_NA);
			piv = tvb_bytes_to_str_punct(wmem_packet_scope(), tvb, offset, piv_len, ' ');

			offset += piv_len;
			kid_len -= piv_len;
		} else {
			piv = nullstr;
		}

		if (kid_context_present) {
			proto_tree_add_item(subtree, hf_coap_opt_object_security_kid_context_len, tvb, offset, 1, ENC_BIG_ENDIAN);
			kid_context_len = tvb_get_guint8(tvb, offset);

			offset += 1;
			kid_len -= 1;

			proto_tree_add_item(subtree, hf_coap_opt_object_security_kid_context, tvb, offset, kid_context_len, ENC_NA);
			kid_context = tvb_bytes_to_str_punct(wmem_packet_scope(), tvb, offset, kid_context_len, ' ');

			offset += kid_context_len;
			kid_len -= kid_context_len;
		} else {
			kid_context = nullstr;
		}

		if (kid_present) {
			if(kid_len > 0) {
				proto_tree_add_item(subtree, hf_coap_opt_object_security_kid, tvb, offset, kid_len, ENC_NA);
				kid = tvb_bytes_to_str_punct(wmem_packet_scope(), tvb, offset, kid_len, ' ');

			} else {
				expert_add_info_format(pinfo, subtree, &ei_coap_option_object_security_bad, "Key ID flag is set but there are no remaining bytes to be processed");
				kid = nullstr;
			}
		} else {
			kid = nullstr;
		}

		proto_item_append_text(head_item, ": Key ID:%s, Key ID Context:%s, Partial IV:%s", kid, kid_context, piv);
	}
}

static void
dissect_coap_opt_proxy_uri(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length)
{
	const guint8 *str = NULL;

	if (opt_length == 0) {
		str = nullstr;
	} else {
		str = tvb_get_string_enc(wmem_packet_scope(), tvb, offset, opt_length, ENC_ASCII);
	}

	proto_tree_add_string(subtree, hf_coap_opt_proxy_uri, tvb, offset, opt_length, str);

	/* add info to the head of the packet detail */
	proto_item_append_text(head_item, ": %s", str);
}

static void
dissect_coap_opt_proxy_scheme(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length)
{
	const guint8 *str = NULL;

	if (opt_length == 0) {
		str = nullstr;
	} else {
		str = tvb_get_string_enc(wmem_packet_scope(), tvb, offset, opt_length, ENC_ASCII);
	}

	proto_tree_add_string(subtree, hf_coap_opt_proxy_scheme, tvb, offset, opt_length, str);

	/* add info to the head of the packet detail */
	proto_item_append_text(head_item, ": %s", str);
}

static void
dissect_coap_opt_ctype(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length, int hf, coap_info *coinfo)
{
	if (opt_length == 0) {
		coinfo->ctype_value = 0;
	} else {
		coinfo->ctype_value = coap_get_opt_uint(tvb, offset, opt_length);
	}

	coinfo->ctype_str = val_to_str(coinfo->ctype_value, vals_ctype, "Unknown Type %u");

	proto_tree_add_string(subtree, hf, tvb, offset, opt_length, coinfo->ctype_str);

	/* add info to the head of the packet detail */
	proto_item_append_text(head_item, ": %s", coinfo->ctype_str);
}

static void
dissect_coap_opt_block(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length, coap_info *coinfo)
{
	guint8      val = 0;
	guint       encoded_block_size;
	guint       block_esize;

	if (opt_length == 0) {
		coinfo->block_number = 0;
		val = 0;
	} else {
		coinfo->block_number = coap_get_opt_uint(tvb, offset, opt_length) >> 4;
		val = tvb_get_guint8(tvb, offset + opt_length - 1) & 0x0f;
	}

	proto_tree_add_uint(subtree, hf_coap_opt_block_number,
	    tvb, offset, opt_length, coinfo->block_number);

	/* More flag in the end of the option */
	coinfo->block_mflag = (val & COAP_BLOCK_MFLAG_MASK) >> 3;
	proto_tree_add_uint(subtree, hf_coap_opt_block_mflag,
	    tvb, offset + opt_length - 1, 1, coinfo->block_mflag);

	/* block size */
	encoded_block_size = val & COAP_BLOCK_SIZE_MASK;
	block_esize = 1 << (encoded_block_size + 4);
	proto_tree_add_uint_format(subtree, hf_coap_opt_block_size,
	    tvb, offset + opt_length - 1, 1, encoded_block_size, "Block Size: %u (%u encoded)", block_esize, encoded_block_size);

	/* add info to the head of the packet detail */
	proto_item_append_text(head_item, ": NUM:%u, M:%u, SZX:%u",
	    coinfo->block_number, coinfo->block_mflag, block_esize);
}

static void
dissect_coap_opt_uri_port(tvbuff_t *tvb, proto_item *head_item, proto_tree *subtree, gint offset, gint opt_length, coap_info *coinfo)
{
	guint port = 0;

	if (opt_length != 0) {
		port = coap_get_opt_uint(tvb, offset, opt_length);
	}

	proto_tree_add_uint(subtree, hf_coap_opt_uri_port, tvb, offset, opt_length, port);

	proto_item_append_text(head_item, ": %u", port);

	/* forming a uri-string */
	wmem_strbuf_append_printf(coinfo->uri_str_strbuf, ":%u", port);
}

/*
 * dissector for each option of CoAP.
 * return the total length of the option including the header (e.g. delta and length).
 */
static int
dissect_coap_options_main(tvbuff_t *tvb, packet_info *pinfo, proto_tree *coap_tree, gint offset, guint8 opt_count, guint *opt_num, gint coap_length, coap_info *coinfo)
{
	guint8      opt_jump;
	gint        opt_length, opt_length_ext, opt_delta, opt_delta_ext;
	gint        opt_length_ext_off = 0;
	gint8       opt_length_ext_len = 0;
	gint        opt_delta_ext_off  = 0;
	gint8       opt_delta_ext_len  = 0;
	gint        orig_offset	       = offset;
	proto_tree *subtree;
	proto_item *item;
	char	    strbuf[56];

	opt_jump = tvb_get_guint8(tvb, offset);
	if (0xff == opt_jump)
		return offset;
	offset += 1;

	/*
	 * section 3.1 in coap-17:
	 * Option Delta:  4-bit unsigned integer.  A value between 0 and 12
	 * indicates the Option Delta.  Three values are reserved for special
	 * constructs:
	 *
	 * 13:  An 8-bit unsigned integer follows the initial byte and
	 *      indicates the Option Delta minus 13.
	 *
	 * 14:  A 16-bit unsigned integer in network byte order follows the
	 *      initial byte and indicates the Option Delta minus 269.
	 *
	 * 15:  Reserved for the Payload Marker.  If the field is set to this
	 *      value but the entire byte is not the payload marker, this MUST
	 *      be processed as a message format error.
	 */
	switch (opt_jump & 0xf0) {
	case 0xd0:
		opt_delta_ext = tvb_get_guint8(tvb, offset);
		opt_delta_ext_off = offset;
		opt_delta_ext_len = 1;
		offset += 1;

		opt_delta = 13;
		opt_delta += opt_delta_ext;
		break;
	case 0xe0:
		opt_delta_ext = coap_get_opt_uint(tvb, offset, 2);
		opt_delta_ext_off = offset;
		opt_delta_ext_len = 2;
		offset += 2;

		opt_delta = 269;
		opt_delta += opt_delta_ext;
		break;
	case 0xf0:
		expert_add_info_format(pinfo, coap_tree, &ei_coap_option_length_bad,
				"end-of-options marker found, but option length isn't 15");
		return -1;
	default:
		opt_delta = ((opt_jump & 0xf0) >> 4);
		break;
	}
	*opt_num += opt_delta;

	/*
	 * section 3.1 in coap-17:
	 * Option Length:  4-bit unsigned integer.  A value between 0 and 12
	 * indicates the length of the Option Value, in bytes.  Three values
	 * are reserved for special constructs:
	 *
	 * 13:  An 8-bit unsigned integer precedes the Option Value and
	 *      indicates the Option Length minus 13.
	 *
	 * 14:  A 16-bit unsigned integer in network byte order precedes the
	 *      Option Value and indicates the Option Length minus 269.
	 *
	 * 15:  Reserved for future use.  If the field is set to this value,
	 *      it MUST be processed as a message format error.
	 */
	switch (opt_jump & 0x0f) {
	case 0x0d:
		opt_length_ext = tvb_get_guint8(tvb, offset);
		opt_length_ext_off = offset;
		opt_length_ext_len = 1;
		offset += 1;

		opt_length  = 13;
		opt_length += opt_length_ext;
		break;
	case 0x0e:
		opt_length_ext = coap_get_opt_uint(tvb, offset, 2);
		opt_length_ext_off = offset;
		opt_length_ext_len = 2;
		offset += 2;

		opt_length  = 269;
		opt_length += opt_length_ext;
		break;
	case 0x0f:
		expert_add_info_format(pinfo, coap_tree, &ei_coap_option_length_bad,
			"end-of-options marker found, but option delta isn't 15");
		return -1;
	default:
		opt_length = (opt_jump & 0x0f);
		break;
	}
	if (offset + opt_length > coap_length) {
		expert_add_info_format(pinfo, coap_tree, &ei_coap_option_length_bad,
			"option longer than the package");
		return -1;
	}

	coap_opt_check(pinfo, coap_tree, *opt_num, opt_length);

	g_snprintf(strbuf, sizeof(strbuf),
	    "#%u: %s", opt_count, val_to_str_const(*opt_num, vals_opt_type,
	    *opt_num % 14 == 0 ? "No-Op" : "Unknown Option"));
	item = proto_tree_add_string(coap_tree, hf_coap_opt_name,
	    tvb, orig_offset, offset - orig_offset + opt_length, strbuf);
	subtree = proto_item_add_subtree(item, ett_coap_option);

	g_snprintf(strbuf, sizeof(strbuf),
	    "Type %u, %s, %s%s", *opt_num,
	    (*opt_num & 1) ? "Critical" : "Elective",
	    (*opt_num & 2) ? "Unsafe" : "Safe",
	    ((*opt_num & 0x1e) == 0x1c) ? ", NoCacheKey" : "");
	proto_tree_add_string(subtree, hf_coap_opt_desc,
	    tvb, orig_offset, offset - orig_offset + opt_length, strbuf);

	proto_tree_add_item(subtree, hf_coap_opt_delta,  tvb, orig_offset, 1, ENC_BIG_ENDIAN);
	proto_tree_add_item(subtree, hf_coap_opt_length, tvb, orig_offset, 1, ENC_BIG_ENDIAN);

	if (opt_delta_ext_off && opt_delta_ext_len)
		proto_tree_add_item(subtree, hf_coap_opt_delta_ext, tvb, opt_delta_ext_off, opt_delta_ext_len, ENC_BIG_ENDIAN);

	if (opt_length_ext_off && opt_length_ext_len)
		proto_tree_add_item(subtree, hf_coap_opt_length_ext, tvb, opt_length_ext_off, opt_length_ext_len, ENC_BIG_ENDIAN);

	/* offset points the next to its option header */
	switch (*opt_num) {
	case COAP_OPT_CONTENT_TYPE:
		dissect_coap_opt_ctype(tvb, item, subtree, offset,
		    opt_length, hf_coap_opt_ctype, coinfo);
		break;
	case COAP_OPT_MAX_AGE:
		dissect_coap_opt_uint(tvb, item, subtree, offset,
		    opt_length, hf_coap_opt_max_age);
		break;
	case COAP_OPT_PROXY_URI:
		dissect_coap_opt_proxy_uri(tvb, item, subtree, offset,
		    opt_length);
		break;
	case COAP_OPT_PROXY_SCHEME:
		dissect_coap_opt_proxy_scheme(tvb, item, subtree, offset,
		    opt_length);
		break;
	case COAP_OPT_SIZE1:
		dissect_coap_opt_uint(tvb, item, subtree, offset,
		    opt_length, hf_coap_opt_size1);
		break;
	case COAP_OPT_ETAG:
		dissect_coap_opt_hex_string(tvb, item, subtree, offset,
		    opt_length, hf_coap_opt_etag);
		break;
	case COAP_OPT_URI_HOST:
		dissect_coap_opt_uri_host(tvb, item, subtree, offset,
		    opt_length, coinfo);
		break;
	case COAP_OPT_LOCATION_PATH:
		dissect_coap_opt_location_path(tvb, item, subtree, offset,
		    opt_length);
		break;
	case COAP_OPT_URI_PORT:
		dissect_coap_opt_uri_port(tvb, item, subtree, offset,
		    opt_length, coinfo);
		break;
	case COAP_OPT_LOCATION_QUERY:
		dissect_coap_opt_location_query(tvb, item, subtree, offset,
		    opt_length);
		break;
	case COAP_OPT_OBJECT_SECURITY:
		dissect_coap_opt_object_security(tvb, item, subtree, offset,
		    opt_length, pinfo, coinfo);
		break;
	case COAP_OPT_URI_PATH:
		dissect_coap_opt_uri_path(tvb, item, subtree, offset,
		    opt_length, coinfo);
		break;
	case COAP_OPT_OBSERVE:
		dissect_coap_opt_uint(tvb, item, subtree, offset,
		    opt_length, hf_coap_opt_observe);
		break;
	case COAP_OPT_ACCEPT:
		dissect_coap_opt_ctype(tvb, item, subtree, offset,
		    opt_length, hf_coap_opt_accept, coinfo);
		break;
	case COAP_OPT_IF_MATCH:
		dissect_coap_opt_hex_string(tvb, item, subtree, offset,
		    opt_length, hf_coap_opt_if_match);
		break;
	case COAP_OPT_URI_QUERY:
		dissect_coap_opt_uri_query(tvb, item, subtree, offset,
		    opt_length, coinfo);
		break;
	case COAP_OPT_BLOCK2:
		dissect_coap_opt_block(tvb, item, subtree, offset,
		    opt_length, coinfo);
		break;
	case COAP_OPT_BLOCK1:
		dissect_coap_opt_block(tvb, item, subtree, offset,
		    opt_length, coinfo);
		break;
	case COAP_OPT_IF_NONE_MATCH:
		break;
	case COAP_OPT_BLOCK_SIZE:
		dissect_coap_opt_uint(tvb, item, subtree, offset,
		    opt_length, hf_coap_opt_block_size);
		break;
	default:
		dissect_coap_opt_hex_string(tvb, item, subtree, offset,
		    opt_length, hf_coap_opt_unknown);
		break;
	}

	return offset + opt_length;
}

/*
 * options dissector.
 * return offset pointing the next of options. (i.e. the top of the paylaod
 * or the end of the data.
 */
static int
dissect_coap_options(tvbuff_t *tvb, packet_info *pinfo, proto_tree *coap_tree, gint offset, gint coap_length, coap_info *coinfo)
{
	guint  opt_num = 0;
	int    i;
	guint8 endmarker;

	/* loop for dissecting options */
	for (i = 1; offset < coap_length; i++) {
		offset = dissect_coap_options_main(tvb, pinfo, coap_tree,
		    offset, i, &opt_num, coap_length, coinfo);
		if (offset == -1)
			return -1;
		if (offset >= coap_length)
			break;
		endmarker = tvb_get_guint8(tvb, offset);
		if (endmarker == 0xff) {
			proto_tree_add_item(coap_tree, hf_coap_opt_end_marker, tvb, offset, 1, ENC_BIG_ENDIAN);
			offset += 1;
			break;
		}
	}

	return offset;
}

static int
dissect_coap(tvbuff_t *tvb, packet_info *pinfo, proto_tree *parent_tree, void* data _U_)
{
	gint              offset = 0;
	proto_item       *coap_root;
	proto_item       *pi;
	proto_tree       *coap_tree;
	guint8            ttype;
	guint8            token_len;
	guint8            code;
	guint8            code_class;
	guint16           mid;
	gint              coap_length;
	gchar            *coap_token_str;
	coap_info        *coinfo;
	conversation_t   *conversation;
	coap_conv_info   *ccinfo;
	coap_transaction *coap_trans = NULL;

	/* Allocate information for upper layers */
	if (!PINFO_FD_VISITED(pinfo)) {
		coinfo = wmem_new0(wmem_file_scope(), coap_info);
		p_add_proto_data(wmem_file_scope(), pinfo, proto_coap, 0, coinfo);
	} else {
		coinfo = (coap_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_coap, 0);
	}

	/* initialize the CoAP length and the content-Format */
	/*
	 * the length of CoAP message is not specified in the CoAP header.
	 * It has to be from the lower layer.
	 * Currently, the length is just copied from the reported length of the tvbuffer.
	 */
	coap_length = tvb_reported_length(tvb);
	coinfo->ctype_str = "";
	coinfo->ctype_value = DEFAULT_COAP_CTYPE_VALUE;

	col_set_str(pinfo->cinfo, COL_PROTOCOL, "CoAP");
	col_clear(pinfo->cinfo, COL_INFO);

	coap_root = proto_tree_add_item(parent_tree, proto_coap, tvb, offset, -1, ENC_NA);
	coap_tree = proto_item_add_subtree(coap_root, ett_coap);

	proto_tree_add_item(coap_tree, hf_coap_version, tvb, offset, 1, ENC_BIG_ENDIAN);

	proto_tree_add_item(coap_tree, hf_coap_ttype, tvb, offset, 1, ENC_BIG_ENDIAN);
	ttype = (tvb_get_guint8(tvb, offset) & COAP_TYPE_MASK) >> 4;

	proto_tree_add_item(coap_tree, hf_coap_token_len, tvb, offset, 1, ENC_BIG_ENDIAN);
	token_len = tvb_get_guint8(tvb, offset) & COAP_TOKEN_LEN_MASK;

	offset += 1;

	proto_tree_add_item(coap_tree, hf_coap_code, tvb, offset, 1, ENC_BIG_ENDIAN);
	code = tvb_get_guint8(tvb, offset);
	code_class = code >> 5;
	offset += 1;

	proto_tree_add_item(coap_tree, hf_coap_mid, tvb, offset, 2, ENC_BIG_ENDIAN);
	mid = tvb_get_ntohs(tvb, offset);

	col_add_fstr(pinfo->cinfo, COL_INFO,
		     "%s, MID:%u, %s",
		     val_to_str(ttype, vals_ttype_short, "Unknown %u"),
		     mid,
		     val_to_str_ext(code, &vals_code_ext, "Unknown %u"));

	/* append the header information */
	proto_item_append_text(coap_root,
			       ", %s, %s, MID:%u",
			       val_to_str(ttype, vals_ttype, "Unknown %u"),
			       val_to_str_ext(code, &vals_code_ext, "Unknown %u"),
			       mid);

	offset += 2;

	/* initialize the external value */
	coinfo->block_number = DEFAULT_COAP_BLOCK_NUMBER;
	coinfo->block_mflag  = 0;
	coinfo->uri_str_strbuf   = wmem_strbuf_sized_new(wmem_packet_scope(), 0, 1024);
	coinfo->uri_query_strbuf = wmem_strbuf_sized_new(wmem_packet_scope(), 0, 1024);
	coinfo->object_security = FALSE;
	coap_token_str = NULL;
	if (token_len > 0)
	{
		/* This has to be file scope as the token string is stored in the map
		* for conversation lookup */
		coap_token_str = tvb_bytes_to_str_punct(wmem_file_scope(), tvb, offset, token_len, ' ');
		proto_tree_add_item(coap_tree, hf_coap_token,
				    tvb, offset, token_len, ENC_NA);
		offset += token_len;
	}

	/* process options */
	offset = dissect_coap_options(tvb, pinfo, coap_tree, offset, coap_length, coinfo);
	if (offset == -1)
		return tvb_captured_length(tvb);

	/* Use conversations to track state for request/response */
	conversation = find_or_create_conversation_noaddrb(pinfo, (code_class == 0));

	/* Retrieve or create state structure for this conversation */
	ccinfo = (coap_conv_info *)conversation_get_proto_data(conversation, proto_coap);
	if (!ccinfo) {
		/* No state structure - create it */
		ccinfo = wmem_new(wmem_file_scope(), coap_conv_info);
		ccinfo->messages = wmem_map_new(wmem_file_scope(), g_str_hash, g_str_equal);
		conversation_add_proto_data(conversation, proto_coap, ccinfo);
	}

	/* Everything based on tokens */
	if (coap_token_str != NULL) {
		/* Process request/response in conversation */
		if (code != 0) { /* Ignore empty messages */
			/* Try and look up a matching token. If it's the first
			* sight of a request, there shouldn't be one */
			coap_trans = (coap_transaction *)wmem_map_lookup(ccinfo->messages, coap_token_str);
			if (!coap_trans) {
				if ((!PINFO_FD_VISITED(pinfo)) && (code_class == 0)) {
					/* New request - log it */
					coap_trans = wmem_new(wmem_file_scope(), coap_transaction);
					coap_trans->req_frame = pinfo->num;
					coap_trans->rsp_frame = 0;
					coap_trans->req_time = pinfo->fd->abs_ts;
					if (coinfo->uri_str_strbuf) {
						/* Store the URI into CoAP transaction info */
						coap_trans->uri_str_strbuf = wmem_strbuf_new(wmem_file_scope(), wmem_strbuf_get_str(coinfo->uri_str_strbuf));
					}
					wmem_map_insert(ccinfo->messages, coap_token_str, (void *)coap_trans);
				}
			} else {
				if ((code_class >= 2) && (code_class <= 5)) {
					if (!PINFO_FD_VISITED(pinfo)) {
						/* Log the first matching response frame */
						coap_trans->rsp_frame = pinfo->num;
					}
					if (coap_trans->uri_str_strbuf) {
						/* Copy the URI stored in matching transaction info into CoAP packet info */
						coinfo->uri_str_strbuf = wmem_strbuf_new(wmem_packet_scope(), wmem_strbuf_get_str(coap_trans->uri_str_strbuf));
					}
				}
			}
		}
	}

	if (coap_trans != NULL) {
		/* Print state tracking in the tree */
		if (code_class == 0) {
			/* This is a request */
			if (coap_trans->rsp_frame) {
				proto_item *it;

				it = proto_tree_add_uint(coap_tree, hf_coap_response_in,
						tvb, 0, 0, coap_trans->rsp_frame);
				PROTO_ITEM_SET_GENERATED(it);
			}
		} else if ((code_class >= 2) && (code_class <= 5)) {
			/* This is a reply */
			if (coap_trans->req_frame) {
				proto_item *it;
				nstime_t ns;

				it = proto_tree_add_uint(coap_tree, hf_coap_response_to,
						tvb, 0, 0, coap_trans->req_frame);
				PROTO_ITEM_SET_GENERATED(it);

				nstime_delta(&ns, &pinfo->fd->abs_ts, &coap_trans->req_time);
				it = proto_tree_add_time(coap_tree, hf_coap_response_time, tvb, 0, 0, &ns);
				PROTO_ITEM_SET_GENERATED(it);
			}
		}
	}

	/* add informations to the packet list */
	if (coap_token_str != NULL)
		col_append_fstr(pinfo->cinfo, COL_INFO, ", TKN:%s", coap_token_str);
	if (coinfo->block_number != DEFAULT_COAP_BLOCK_NUMBER)
		col_append_fstr(pinfo->cinfo, COL_INFO, ", %sBlock #%u",
				coinfo->block_mflag ? "" : "End of ", coinfo->block_number);
	if (wmem_strbuf_get_len(coinfo->uri_str_strbuf) > 0) {
		col_append_fstr(pinfo->cinfo, COL_INFO, ", %s", wmem_strbuf_get_str(coinfo->uri_str_strbuf));
		/* Add a generated protocol item as well */
		pi = proto_tree_add_string(coap_tree, hf_coap_opt_uri_path_recon, tvb, 0, 0, wmem_strbuf_get_str(coinfo->uri_str_strbuf));
		PROTO_ITEM_SET_GENERATED(pi);
	}
	if (wmem_strbuf_get_len(coinfo->uri_query_strbuf) > 0)
		col_append_str(pinfo->cinfo, COL_INFO, wmem_strbuf_get_str(coinfo->uri_query_strbuf));

	/* dissect the payload */
	if (coap_length > offset) {
		proto_tree *payload_tree;
		proto_item *payload_item, *length_item;
		tvbuff_t   *payload_tvb;
		guint	    payload_length = coap_length - offset;
		const char *coap_ctype_str_dis;
		char	    str_payload[80];

		/* coinfo->ctype_value == DEFAULT_COAP_CTYPE_VALUE: No Content-Format option present */
		/* coinfo->ctype_value == 0: Content-Format option present with length 0 */
		if (coinfo->ctype_value == DEFAULT_COAP_CTYPE_VALUE || coinfo->ctype_value == 0) {
			/*
			* 5.5.2.  Diagnostic Payload
			*
			* If no Content-Format option is given, the payload of responses
			* indicating a client or server error is a brief human-readable
			* diagnostic message, explaining the error situation. This diagnostic
			* message MUST be encoded using UTF-8 [RFC3629], more specifically
			* using Net-Unicode form [RFC5198].
			*/
			if ((code_class >= 4) && (code_class <= 5)) {
				coinfo->ctype_str = "text/plain; charset=utf-8";
				coap_ctype_str_dis = "text/plain";
			} else {
				/* Assume no Content-Format is opaque octet stream */
				coinfo->ctype_str = "application/octet-stream";
				coap_ctype_str_dis = coinfo->ctype_str;
			}
		} else {
			coap_ctype_str_dis = coinfo->ctype_str;
		}
		if (coinfo->object_security) {
			g_snprintf(str_payload, sizeof(str_payload), "Encrypted OSCORE Data");
		} else {
			g_snprintf(str_payload, sizeof(str_payload),
				"Payload Content-Format: %s%s, Length: %u",
				coinfo->ctype_str, coinfo->ctype_value == DEFAULT_COAP_CTYPE_VALUE ?
				" (no Content-Format)" : "", payload_length);
		}

		payload_item = proto_tree_add_string(coap_tree, hf_coap_payload,
						     tvb, offset, payload_length,
						     str_payload);
		payload_tree = proto_item_add_subtree(payload_item, ett_coap_payload);

		proto_tree_add_string(payload_tree, hf_coap_payload_desc, tvb, offset, 0, coinfo->ctype_str);
		length_item = proto_tree_add_uint(payload_tree, hf_coap_payload_length, tvb, offset, 0, payload_length);
		PROTO_ITEM_SET_GENERATED(length_item);
		payload_tvb = tvb_new_subset_length(tvb, offset, payload_length);

		dissector_try_string(media_type_dissector_table, coap_ctype_str_dis,
				     payload_tvb, pinfo, parent_tree, NULL);
	}

	return tvb_captured_length(tvb);
}

/*
 * Protocol initialization
 */
void
proto_register_coap(void)
{
	static hf_register_info hf[] = {
		{ &hf_coap_version,
		  { "Version", "coap.version",
		    FT_UINT8, BASE_DEC, NULL, COAP_VERSION_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_ttype,
		  { "Type", "coap.type",
		    FT_UINT8, BASE_DEC, VALS(vals_ttype), COAP_TYPE_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_token_len,
		  { "Token Length", "coap.token_len",
		    FT_UINT8, BASE_DEC, NULL, COAP_TOKEN_LEN_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_token,
		  { "Token", "coap.token",
		    FT_BYTES, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_code,
		  { "Code", "coap.code",
		    FT_UINT8, BASE_DEC | BASE_EXT_STRING, &vals_code_ext, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_mid,
		  { "Message ID", "coap.mid",
		    FT_UINT16, BASE_DEC, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_payload,
		  { "Payload", "coap.payload",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_payload_desc,
		  { "Payload Desc", "coap.payload_desc",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_payload_length,
		    { "Payload Length", "coap.payload_length",
		    FT_UINT32, BASE_DEC, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_name,
		  { "Opt Name", "coap.opt.name",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_desc,
		  { "Opt Desc", "coap.opt.desc",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_delta,
		  { "Opt Delta", "coap.opt.delta",
		    FT_UINT8, BASE_DEC, NULL, 0xf0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_delta_ext,
		  { "Opt Delta extended", "coap.opt.delta_ext",
		    FT_UINT16, BASE_DEC, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_length,
		  { "Opt Length", "coap.opt.length",
		    FT_UINT8, BASE_DEC, NULL, 0x0f,
		    "CoAP Option Length", HFILL }
		},
		{ &hf_coap_opt_length_ext,
		  { "Opt Length extended", "coap.opt.length_ext",
		    FT_UINT16, BASE_DEC, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_end_marker,
		  { "End of options marker", "coap.opt.end_marker",
		    FT_UINT8, BASE_DEC, NULL, 0x00,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_ctype,
		  { "Content-type", "coap.opt.ctype",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_max_age,
		  { "Max-age", "coap.opt.max_age",
		    FT_UINT32, BASE_DEC, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_proxy_uri,
		  { "Proxy-Uri", "coap.opt.proxy_uri",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_proxy_scheme,
		  { "Proxy-Scheme", "coap.opt.proxy_scheme",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_size1,
		  { "Size1", "coap.opt.size1",
		    FT_UINT32, BASE_DEC, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_etag,
		  { "Etag", "coap.opt.etag",
		    FT_BYTES, BASE_NONE, NULL, 0x0,
		    "CoAP Option Etag", HFILL }
		},
		{ &hf_coap_opt_uri_host,
		  { "Uri-Host", "coap.opt.uri_host",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_location_path,
		  { "Location-Path", "coap.opt.location_path",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_uri_port,
		  { "Uri-Port", "coap.opt.uri_port",
		    FT_UINT16, BASE_DEC, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_location_query,
		  { "Location-Query", "coap.opt.location_query",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_object_security_non_compressed,
		  { "Non-compressed COSE message", "coap.opt.object_security_non_compressed",
		    FT_BOOLEAN, 8, NULL, COAP_OBJECT_SECURITY_NON_COMPRESSED_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_object_security_expand,
		  { "Expanded Flag Byte", "coap.opt.object_security_expand",
		    FT_BOOLEAN, 8, NULL, COAP_OBJECT_SECURITY_EXPAND_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_object_security_signature,
		  { "Signature Present", "coap.opt.object_security_signature",
		    FT_BOOLEAN, 8, NULL, COAP_OBJECT_SECURITY_SIGNATURE_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_object_security_kid_context_present,
		  { "Key ID Context Present", "coap.opt.object_security_kid_context_present",
		    FT_BOOLEAN, 8, NULL, COAP_OBJECT_SECURITY_KID_CONTEXT_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_object_security_kid_present,
		  { "Key ID Present", "coap.opt.object_security_kid",
		    FT_BOOLEAN, 8, NULL, COAP_OBJECT_SECURITY_KID_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_object_security_piv_len,
		  { "Partial IV Length", "coap.opt.object_security_piv_len",
		    FT_UINT8, BASE_DEC, NULL, COAP_OBJECT_SECURITY_PIVLEN_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_object_security_piv,
		  { "Partial IV", "coap.opt.object_security_piv",
		    FT_BYTES, BASE_NONE, NULL, 0x00,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_object_security_kid_context_len,
		  { "Key ID Context Length", "coap.opt.object_security_kid_context_len",
		    FT_UINT8, BASE_DEC, NULL, 0x00,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_object_security_kid_context,
		  { "Partial IV", "coap.opt.object_security_kid_context",
		    FT_BYTES, BASE_NONE, NULL, 0x00,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_object_security_kid,
		  { "Key ID", "coap.opt.object_security_kid",
		    FT_BYTES, BASE_NONE, NULL, 0x00,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_uri_path,
		  { "Uri-Path", "coap.opt.uri_path",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_uri_path_recon,
		  { "Uri-Path", "coap.opt.uri_path_recon",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_observe,
		  { "Observe", "coap.opt.observe",
		    FT_UINT32, BASE_DEC, VALS(vals_observe_options), 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_accept,
		  { "Accept", "coap.opt.accept",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_if_match,
		  { "If-Match", "coap.opt.if_match",
		    FT_BYTES, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_block_number,
		  { "Block Number", "coap.opt.block_number",
		    FT_UINT32, BASE_DEC, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_block_mflag,
		  { "More Flag", "coap.opt.block_mflag",
		    FT_UINT8, BASE_DEC, NULL, COAP_BLOCK_MFLAG_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_block_size,
		  { "Encoded Block Size", "coap.opt.block_size",
		    FT_UINT8, BASE_DEC, NULL, COAP_BLOCK_SIZE_MASK,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_uri_query,
		  { "Uri-Query", "coap.opt.uri_query",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_opt_unknown,
		  { "Unknown", "coap.opt.unknown",
		    FT_BYTES, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }
		},
		{ &hf_coap_response_in,
		  { "Response In", "coap.response_in",
		    FT_FRAMENUM, BASE_NONE, NULL, 0x0,
		    "The response to this CoAP request is in this frame", HFILL }
		},
		{ &hf_coap_response_to,
		  { "Request In", "coap.response_to",
		    FT_FRAMENUM, BASE_NONE, NULL, 0x0,
		    "This is a response to the CoAP request in this frame", HFILL }
		},
		{ &hf_coap_response_time,
		  { "Response Time", "coap.response_time",
		    FT_RELATIVE_TIME, BASE_NONE, NULL, 0x0,
		    "The time between the Call and the Reply", HFILL }
		},
	};

	static gint *ett[] = {
		&ett_coap,
		&ett_coap_option,
		&ett_coap_payload,
	};

	static ei_register_info ei[] = {
		{ &ei_coap_invalid_option_number,
		  { "coap.invalid_option_number", PI_MALFORMED, PI_WARN, "Invalid Option Number", EXPFILL }},
		{ &ei_coap_invalid_option_range,
		  { "coap.invalid_option_range", PI_MALFORMED, PI_WARN, "Invalid Option Range", EXPFILL }},
		{ &ei_coap_option_length_bad,
		  { "coap.option_length_bad", PI_MALFORMED, PI_WARN, "Option length bad", EXPFILL }},
		{ &ei_coap_option_object_security_bad,
		  { "coap.option.object_security", PI_MALFORMED, PI_WARN, "Invalid Object-Security Option Format", EXPFILL }},

	};

	expert_module_t *expert_coap;

	proto_coap = proto_register_protocol("Constrained Application Protocol", "CoAP", "coap");
	proto_register_field_array(proto_coap, hf, array_length(hf));
	proto_register_subtree_array(ett, array_length(ett));
	expert_coap = expert_register_protocol(proto_coap);
	expert_register_field_array(expert_coap, ei, array_length(ei));

	coap_handle = register_dissector("coap", dissect_coap, proto_coap);
}

void
proto_reg_handoff_coap(void)
{
	media_type_dissector_table = find_dissector_table("media_type");
	dissector_add_uint_with_preference("udp.port", DEFAULT_COAP_PORT, coap_handle);
	dtls_dissector_add(DEFAULT_COAPS_PORT, coap_handle);
}

/*
 * Editor modelines  -  http://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 noexpandtab:
 * :indentSize=8:tabSize=8:noTabs=false:
 */
