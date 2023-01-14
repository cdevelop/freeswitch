/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 *
 *
 * sofia_media.c -- SOFIA SIP Endpoint (sofia media code)
 *
 */
#include "mod_sofia.h"




uint8_t sofia_media_negotiate_sdp(switch_core_session_t *session, const char *r_sdp, switch_sdp_type_t type)
{
	uint8_t t, p = 0;
	private_object_t *tech_pvt = switch_core_session_get_private(session);

	if ((t = switch_core_media_negotiate_sdp(session, r_sdp, &p, type))) {
		sofia_set_flag_locked(tech_pvt, TFLAG_SDP);
	}

	if (!p) {
		sofia_set_flag(tech_pvt, TFLAG_NOREPLY);
	}

	return t;
}

switch_status_t sofia_media_activate_rtp(private_object_t *tech_pvt)
{
	switch_status_t status;

	switch_mutex_lock(tech_pvt->sofia_mutex);
	status = switch_core_media_activate_rtp(tech_pvt->session);
	switch_mutex_unlock(tech_pvt->sofia_mutex);


	if (status == SWITCH_STATUS_SUCCESS) {
		sofia_set_flag(tech_pvt, TFLAG_RTP);
		sofia_set_flag(tech_pvt, TFLAG_IO);
	}

	return status;
}


static char *do_sdp_replace(const char *sdp, const char *val)
{
	const char *string ;
	size_t string_len;
	const char *search = val;
	const char *replace;
	size_t search_len;
	size_t replace_len;
	size_t i, n;
	size_t dest_len = 0;
	char *dest, *tmp;

	if(zstr(sdp) || zstr(val)){
		return NULL;
	}
	string = sdp;
	string_len = strlen(string);
	replace = strchr(val, '|');
	if(zstr(replace)){
		return NULL;
	}
	search_len = replace - search;
	replace += 1;
	if(zstr(replace)){
		return NULL;
	}
	replace_len = strlen(replace);
	dest = (char *)malloc(sizeof(char));
	switch_assert(dest);

	for (i = 0; i < string_len; i++) {
		if (switch_string_match(string + i, string_len - i, search, search_len) == SWITCH_STATUS_SUCCESS) {
			for (n = 0; n < replace_len; n++) {
				dest[dest_len] = replace[n];
				dest_len++;
				tmp = (char *)realloc(dest, sizeof(char) * (dest_len + 1));
				switch_assert(tmp);
				dest = tmp;
			}
			i += search_len - 1;
		} else {
			dest[dest_len] = string[i];
			dest_len++;
			tmp = (char *)realloc(dest, sizeof(char) * (dest_len + 1));
			switch_assert(tmp);
			dest = tmp;
		}
	}

	dest[dest_len] = 0;

	return dest;
}

// sdp_replace_ip=192.168.31.57|192.168.31.1 替换SDP中的IP
static const char *sofia_sdp_replace(switch_channel_t *channel, const char *sdp)
{
	switch_event_header_t *hp;
	switch_event_t *event;
	const char *replace = sdp;
	switch_channel_get_variables(channel, &event);

	for (hp = event->headers; hp; hp = hp->next) {
		char *var = hp->name;
		char *val = hp->value;

		if (!strncasecmp(var, "sdp_replace", 11)) {
			if (hp->idx) {
				int i;
				for (i = 0; i < hp->idx; i++) {
					char *tmp = do_sdp_replace(replace, hp->array[i]);
					if (tmp) {
						if (replace != sdp) { free((char *)replace); }
						replace = tmp;
					}
				}
			} else {
				char *tmp = do_sdp_replace(replace, val);
				if (tmp) {
					if (replace != sdp) { free((char *)replace); }
					replace = tmp;
				}
			}
		}
	}

	switch_event_destroy(&event);

	if (replace != sdp) {
		switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG,
						  "Remote SDP:\n%s\nAfter Replace:\n%s\n", sdp, replace);
		sdp = switch_core_session_strdup(switch_channel_get_session(channel), replace);
		free((char *)replace);
	}

	return sdp;
}



switch_status_t sofia_media_tech_media(private_object_t *tech_pvt, const char *r_sdp, switch_sdp_type_t type)
{
	switch_assert(tech_pvt != NULL);
	switch_assert(r_sdp != NULL);

	if (zstr(r_sdp)) {
		return SWITCH_STATUS_FALSE;
	}

    //需要替换SDP打开这个代码
	// r_sdp = sofia_sdp_replace(tech_pvt->channel, r_sdp);

	if (sofia_media_negotiate_sdp(tech_pvt->session, r_sdp, type)) {
		if (switch_core_media_choose_port(tech_pvt->session, SWITCH_MEDIA_TYPE_AUDIO, 0) != SWITCH_STATUS_SUCCESS) {
			return SWITCH_STATUS_FALSE;
		}
		if (sofia_media_activate_rtp(tech_pvt) != SWITCH_STATUS_SUCCESS) {
			return SWITCH_STATUS_FALSE;
		}
		switch_channel_set_variable(tech_pvt->channel, SWITCH_ENDPOINT_DISPOSITION_VARIABLE, "EARLY MEDIA");
		sofia_set_flag_locked(tech_pvt, TFLAG_EARLY_MEDIA);
		switch_channel_mark_pre_answered(tech_pvt->channel);
		return SWITCH_STATUS_SUCCESS;
	}


	return SWITCH_STATUS_FALSE;
}

static void process_mp(switch_core_session_t *session, switch_stream_handle_t *stream, const char *boundary, const char *str) {
	char *dname = switch_core_session_strdup(session, str);
	char *dval;

	if ((dval = strchr(dname, ':'))) {
		*dval++ = '\0';
		if (*dval == '~') {
			stream->write_function(stream, "--%s\r\nContent-Type: %s\r\nContent-Length: %d\r\n%s\r\n", boundary, dname, strlen(dval), dval + 1);
		} else {
			stream->write_function(stream, "--%s\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n%s\r\n", boundary, dname, strlen(dval) + 1, dval);
		}
	}
}

char *sofia_media_get_multipart(switch_core_session_t *session, const char *prefix, const char *sdp, char **mp_type)
{
	char *extra_headers = NULL;
	switch_stream_handle_t stream = { 0 };
	switch_event_header_t *hi = NULL;
	int x = 0;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *boundary = switch_core_session_get_uuid(session);

	SWITCH_STANDARD_STREAM(stream);
	if ((hi = switch_channel_variable_first(channel))) {
		for (; hi; hi = hi->next) {
			const char *name = (char *) hi->name;
			char *value = (char *) hi->value;

			if (!strcasecmp(name, prefix)) {
				if (hi->idx > 0) {
					int i = 0;

					for(i = 0; i < hi->idx; i++) {
						process_mp(session, &stream, boundary, hi->array[i]);
						x++;
					}
				} else {
					process_mp(session, &stream, boundary, value);
					x++;
				}
			}
		}
		switch_channel_variable_last(channel);
	}

	if (x) {
		*mp_type = switch_core_session_sprintf(session, "multipart/mixed; boundary=%s", boundary);
		if (sdp) {
			stream.write_function(&stream, "--%s\r\nContent-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s\r\n", boundary, strlen(sdp) + 1, sdp);
		}
		stream.write_function(&stream, "--%s--\r\n", boundary);
	}

	if (!zstr((char *) stream.data)) {
		extra_headers = stream.data;
	} else {
		switch_safe_free(stream.data);
	}

	return extra_headers;
}





/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */

