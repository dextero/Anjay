/*
 * Copyright 2017 AVSystem <avsystem@avsystem.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils_core.h"

#include <avsystem/commons/utils.h>

VISIBILITY_SOURCE_BEGIN

static int url_parse_protocol(const char **url, anjay_url_t *parsed_url) {
    const char *proto_end = strstr(*url, "://");
    size_t proto_len = 0;
    if (!proto_end) {
        anjay_log(ERROR, "could not parse protocol");
        return -1;
    }
    proto_len = (size_t) (proto_end - *url);
    if (proto_len >= sizeof(parsed_url->protocol)) {
        anjay_log(ERROR, "protocol name too long");
        return -1;
    }
    memcpy(parsed_url->protocol, *url, proto_len);
    parsed_url->protocol[proto_len] = '\0';
    *url += proto_len + 3; /* 3 for "://" */
    return 0;
}

static int url_parse_host(const char **url, anjay_url_t *parsed_url) {
    const char *raw_url = *url;
    char *host = parsed_url->host;
    char *host_limit = parsed_url->host + sizeof(parsed_url->host) - 1;

    if (*raw_url == '[') {
        ++raw_url;
        while ((host < host_limit)
                && (*raw_url != '\0')
                && (*raw_url != ']')) {
            *host++ = *raw_url++;
        }
        if ((*raw_url != '\0') && (*raw_url != ']')) {
            anjay_log(ERROR, "host address too long");
            return -1;
        }
        if (*raw_url++ != ']') {
            anjay_log(ERROR, "expected ] at the end of host address");
            return -1;
        }
    } else {
        while ((host < host_limit)
                && (*raw_url != '\0')
                && (*raw_url != '/')
                && (*raw_url != ':')) {
            if (*raw_url == '@') {
                anjay_log(ERROR, "credentials in URLs are not supported");
                return -1;
            }
            *host++ = *raw_url++;
        }
        if ((*raw_url != '\0') && (*raw_url != '/') && (*raw_url != ':')) {
            anjay_log(ERROR, "host address too long");
            return -1;
        }
    }
    if (host == parsed_url->host) {
        anjay_log(ERROR, "host part cannot be empty");
        return -1;
    }
    *host = '\0';
    *url = raw_url;
    return 0;
}

static int url_parse_port(const char **url, anjay_url_t *parsed_url) {
    const char *raw_url = *url;
    char *port = parsed_url->port;
    char *port_limit = parsed_url->port + sizeof(parsed_url->port) - 1;

    if (*raw_url == ':') {
        ++raw_url; /* move after ':' */
        while ((port < port_limit) && isdigit((unsigned char) *raw_url)) {
            *port++ = *raw_url++;
        }
        if (isdigit((unsigned char) *raw_url)) {
            anjay_log(ERROR, "port too long");
            return -1;
        }
        if (*raw_url != '\0' && *raw_url != '/') {
            anjay_log(ERROR, "port should have numeric value");
            return -1;
        }
        if (port == parsed_url->port) {
            anjay_log(ERROR, "expected at least 1 digit for port number");
            return -1;
        }
    }
    *port = '\0';

    *url = raw_url;
    return 0;
}

static int url_parsed(const char *url) {
    return (*url != '\0');
}

static int url_unescape_first(const char *data) {
    if (data[0] == '%'
            && isxdigit((unsigned char) data[1])
            && isxdigit((unsigned char) data[2])) {
        char ascii[3];
        ascii[0] = data[1];
        ascii[1] = data[2];
        ascii[2] = '\0';
        return (int) strtoul(ascii, NULL, 16);
    }
    return -1;
}

static int url_unescape(char *data) {
    char *src = data, *dst = data;

    if (!strchr(data, '%')) {
        /* nothing to unescape */
        return 0;
    }

    while (*src) {
        if (*src == '%') {
            int unescaped = url_unescape_first(src);
            if (unescaped >= 0) {
                *dst = (char) unescaped;
                src += 3;
                dst += 1;
            } else {
                anjay_log(ERROR, "bad escape format (%%XX)");
                return -1;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';

    return 0;
}

typedef enum {
    URL_PARSE_HINT_NONE,
    URL_PARSE_HINT_SKIP_TRAILING_SEPARATOR
} url_parse_chunks_hint_t;

static bool is_valid_url_path_char(char c) {
    /* Assumes english locale. For more information see RFC3986,
       Section "3.3. Path" */
    return isalnum((unsigned char) c)
           || !!strchr("-._~"        /* unreserved */
                       "!$&'()*+,;=" /* sub-delims */
                       ":@",         /* rest of pchar grammar rule */
                       c);
}

static bool is_valid_url_query_char(char c) {
    return is_valid_url_path_char(c) || !!strchr("/?", c);
}

static bool is_valid_url_part(const char *str,
                              size_t length,
                              bool (*is_unescaped_character_valid)(char)) {
    if (!str) {
        return false;
    }

    while (*str && length) {
        if (is_unescaped_character_valid(*str)) {
            ++str;
            --length;
        } else {
            if (url_unescape_first(str) < 0) {
                return false;
            }
            const size_t escaped_len = sizeof("%xx") - 1;
            str += escaped_len;
            length -= escaped_len;
        }
    }

    return !length;
}

static int url_parse_chunks(const char **url,
                            char delimiter,
                            char parser_terminator,
                            url_parse_chunks_hint_t hint,
                            bool (*is_unescaped_character_valid)(char),
                            AVS_LIST(anjay_string_t) *out_chunks) {
    const char *ptr = *url;
    do {
        if ((*ptr == '\0' || *ptr == delimiter || *ptr == parser_terminator)
            && ptr != *url) {
            const size_t chunk_len = (size_t)(ptr - *url - 1);

            if (hint == URL_PARSE_HINT_SKIP_TRAILING_SEPARATOR
                && (*ptr == '\0' || *ptr == parser_terminator) && !chunk_len) {
                // trailing separator, ignoring
                *url = ptr;
                return 0;
            }

            if (!is_valid_url_part(*url + 1, chunk_len,
                                   is_unescaped_character_valid)) {
                return -1;
            }

            if (out_chunks) {
                AVS_LIST(anjay_string_t) chunk =
                    (AVS_LIST(anjay_string_t)) AVS_LIST_NEW_BUFFER(chunk_len + 1);
                if (!chunk) {
                    anjay_log(ERROR, "out of memory");
                    return -1;
                }
                AVS_LIST_APPEND(out_chunks, chunk);

                if (chunk_len) {
                    // skip separator
                    memcpy(chunk, *url + 1, chunk_len);

                    if (url_unescape(chunk->c_str)) {
                        return -1;
                    }
                }
            }
            *url = ptr;
        }

        if (*ptr == parser_terminator) {
            *url = ptr;
            break;
        }
        ++ptr;
    } while (**url);
    return 0;
}

int _anjay_parse_url(const char *raw_url, anjay_url_t *out_parsed_url) {
    assert(!out_parsed_url->uri_path);
    assert(!out_parsed_url->uri_query);
    if (url_parse_protocol(&raw_url, out_parsed_url)
        || url_parse_host(&raw_url, out_parsed_url)
        || url_parse_port(&raw_url, out_parsed_url)) {
        return -1;
    }

    if (*raw_url == '/'
        && url_parse_chunks(
                   &raw_url, '/', '?', URL_PARSE_HINT_SKIP_TRAILING_SEPARATOR,
                   is_valid_url_path_char, &out_parsed_url->uri_path)) {
        goto error;
    }
    if (*raw_url == '?'
        && url_parse_chunks(&raw_url, '&', '\0', URL_PARSE_HINT_NONE,
                            is_valid_url_query_char,
                            &out_parsed_url->uri_query)) {
        goto error;
    }
    if (url_parsed(raw_url)) {
        goto error;
    }
    return 0;
error:
    _anjay_url_cleanup(out_parsed_url);
    return -1;
}

void _anjay_url_cleanup(anjay_url_t *url) {
    AVS_LIST_CLEAR(&url->uri_path);
    AVS_LIST_CLEAR(&url->uri_query);
}

#ifdef ANJAY_TEST

uint32_t _anjay_rand32(anjay_rand_seed_t *seed) {
    // simple deterministic generator for testing purposes
    return (*seed = 1103515245u * *seed + 12345u);
}

#else

#if AVS_RAND_MAX >= UINT32_MAX
#define RAND32_ITERATIONS 1
#elif AVS_RAND_MAX >= UINT16_MAX
#define RAND32_ITERATIONS 2
#else
/* standard guarantees RAND_MAX to be at least 32767 */
#define RAND32_ITERATIONS 3
#endif

uint32_t _anjay_rand32(anjay_rand_seed_t *seed) {
    uint32_t result = 0;
    int i;
    for (i = 0; i < RAND32_ITERATIONS; ++i) {
        result *= (uint32_t) AVS_RAND_MAX + 1;
        result += (uint32_t) avs_rand_r(seed);
    }
    return result;
}
#endif

AVS_LIST(const anjay_string_t) _anjay_make_string_list(const char *string,
                                                       ... /* strings */) {
    va_list list;
    va_start(list, string);

    AVS_LIST(const anjay_string_t) strings_list = NULL;
    const char *str = string;

    while (str) {
        size_t len = strlen(str) + 1;
        AVS_LIST(anjay_string_t) list_elem =
                (AVS_LIST(anjay_string_t))AVS_LIST_NEW_BUFFER(len);

        if (!list_elem) {
            anjay_log(ERROR, "out of memory");
            AVS_LIST_CLEAR(&strings_list);
            break;
        }

        memcpy(list_elem->c_str, str, len);
        AVS_LIST_APPEND(&strings_list, list_elem);
        str = va_arg(list, const char*);
    }

    va_end(list);
    return strings_list;
}

static struct {
    anjay_binding_mode_t binding;
    const char *str;
} BINDING_MODE_AS_STR[] = {
    { ANJAY_BINDING_U,   "U" },
    { ANJAY_BINDING_UQ,  "UQ" },
    { ANJAY_BINDING_S,   "S" },
    { ANJAY_BINDING_SQ,  "SQ" },
    { ANJAY_BINDING_US,  "US" },
    { ANJAY_BINDING_UQS, "UQS" }
};

const char *anjay_binding_mode_as_str(anjay_binding_mode_t binding_mode) {
    for (size_t i = 0; i < AVS_ARRAY_SIZE(BINDING_MODE_AS_STR); ++i) {
        if (BINDING_MODE_AS_STR[i].binding == binding_mode) {
            return BINDING_MODE_AS_STR[i].str;
        }
    }
    return NULL;
}

anjay_binding_mode_t anjay_binding_mode_from_str(const char *str) {
    for (size_t i = 0; i < AVS_ARRAY_SIZE(BINDING_MODE_AS_STR); ++i) {
        if (strcmp(str, BINDING_MODE_AS_STR[i].str) == 0) {
            return BINDING_MODE_AS_STR[i].binding;
        }
    }
    anjay_log(WARNING, "unsupported binding mode string: %s", str);
    return ANJAY_BINDING_NONE;
}

static int append_string_query_arg(AVS_LIST(const anjay_string_t) *list,
                                   const char *name,
                                   const char *value) {
    const size_t size = strlen(name) + sizeof("=") + strlen(value);
    AVS_LIST(anjay_string_t) arg =
            (AVS_LIST(anjay_string_t)) AVS_LIST_NEW_BUFFER(size);

    if (!arg
        || avs_simple_snprintf(arg->c_str, size, "%s=%s", name, value) < 0) {
        AVS_LIST_CLEAR(&arg);
    } else {
        AVS_LIST_APPEND(list, arg);
    }

    return !arg ? -1 : 0;
}

AVS_LIST(const anjay_string_t)
_anjay_make_query_string_list(const char *version,
                              const char *endpoint_name,
                              const int64_t *lifetime,
                              anjay_binding_mode_t binding_mode,
                              const char *sms_msisdn) {
    AVS_LIST(const anjay_string_t) list = NULL;

    if (version && append_string_query_arg(&list, "lwm2m", version)) {
        goto fail;
    }

    if (endpoint_name && append_string_query_arg(&list, "ep", endpoint_name)) {
        goto fail;
    }

    if (lifetime) {
        assert(*lifetime > 0);
        size_t lt_size = sizeof("lt=") + 16;
        AVS_LIST(anjay_string_t) lt =
                (AVS_LIST(anjay_string_t)) AVS_LIST_NEW_BUFFER(lt_size);
        if (!lt || avs_simple_snprintf(lt->c_str, lt_size,
                                       "lt=%" PRId64, *lifetime) < 0) {
            goto fail;
        }
        AVS_LIST_APPEND(&list, lt);
    }

    const char *binding_mode_str = anjay_binding_mode_as_str(binding_mode);
    if (binding_mode_str
            && append_string_query_arg(&list, "b", binding_mode_str)) {
        goto fail;
    }

    if (sms_msisdn && append_string_query_arg(&list, "sms", sms_msisdn)) {
        goto fail;
    }

    return list;

fail:
    AVS_LIST_CLEAR(&list);
    return NULL;
}

avs_net_abstract_socket_t *
_anjay_create_connected_udp_socket(anjay_t *anjay,
                                   avs_net_socket_type_t type,
                                   const char *bind_port,
                                   const void *config,
                                   const anjay_url_t *uri) {
    (void) anjay;

    avs_net_abstract_socket_t *socket = NULL;

    switch (type) {
    case AVS_NET_UDP_SOCKET:
    case AVS_NET_DTLS_SOCKET:
        if (avs_net_socket_create(&socket, type, config)) {
            anjay_log(ERROR, "could not create CoAP socket");
            goto fail;
        }

        if (bind_port && *bind_port
                && avs_net_socket_bind(socket, NULL, bind_port)) {
            anjay_log(ERROR, "could not bind socket to port %s", bind_port);
            goto fail;
        }

        if (avs_net_socket_connect(socket, uri->host, uri->port)) {
            anjay_log(ERROR, "could not connect to %s:%s",
                      uri->host, uri->port);
            goto fail;
        }

        return socket;
    default:
        anjay_log(ERROR, "unsupported socket type requested: %d", type);
        return NULL;
    }

fail:
    avs_net_socket_cleanup(&socket);
    return NULL;
}

#ifdef ANJAY_TEST
#include "test/utils.c"
#endif // ANJAY_TEST
