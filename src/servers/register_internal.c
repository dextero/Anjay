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

#include <inttypes.h>

#include <anjay_modules/time_defs.h>

#include "../sched.h"
#include "../anjay_core.h"
#include "../servers.h"
#include "../interface/register.h"

#define ANJAY_SERVERS_INTERNALS

#include "activate.h"
#include "connection_info.h"
#include "register_internal.h"
#include "servers_internal.h"

VISIBILITY_SOURCE_BEGIN

/** Update messages are sent to the server every
 * LIFETIME/ANJAY_UPDATE_INTERVAL_FACTOR seconds. */
#define ANJAY_UPDATE_INTERVAL_MARGIN_FACTOR 2

/** To avoid flooding the network in case of a very small lifetime, Update
 * messages are not sent more often than every ANJAY_MIN_UPDATE_INTERVAL_S
 * seconds. */
#define ANJAY_MIN_UPDATE_INTERVAL_S 1

AVS_STATIC_ASSERT(UINTPTR_MAX > UINT16_MAX, pointer_big_enough);
static const uintptr_t SEND_UPDATE_SCHED_JOB_REFRESH_CONNECTION_FLAG =
        (1 << 16);

typedef enum {
    DONT_RECONNECT = 0,
    DO_RECONNECT
} reconnect_required_t;

static void *send_update_args_encode(anjay_ssid_t ssid,
                                     reconnect_required_t refresh) {
    uintptr_t value = ssid;
    if (refresh) {
        value = (value | SEND_UPDATE_SCHED_JOB_REFRESH_CONNECTION_FLAG);
    }
    return (void *) value;
}

static void send_update_args_decode(anjay_ssid_t *out_ssid,
                                    reconnect_required_t *out_refresh,
                                    void *value) {
    uintptr_t int_value = (uintptr_t) value;
    *out_ssid = (anjay_ssid_t) (int_value & UINT16_MAX);
    *out_refresh = (reconnect_required_t)
            !!(int_value & SEND_UPDATE_SCHED_JOB_REFRESH_CONNECTION_FLAG);
}

static int force_server_reregister_clb(anjay_t *anjay,
                                       void *server_ssid_) {
    anjay_ssid_t ssid = (anjay_ssid_t)(intptr_t)server_ssid_;
    AVS_LIST(anjay_active_server_info_t) *server_ptr =
            _anjay_servers_find_active_ptr(&anjay->servers, ssid);

    if (!server_ptr || (*server_ptr)->ssid != ssid) {
        anjay_log(DEBUG, "ignoring forced re-registration of server %u: not an "
                  "active server", ssid);
        return 0;
    }

    if (_anjay_server_register(anjay, *server_ptr)) {
        anjay_log(DEBUG, "re-registration failed");
        _anjay_server_deactivate(anjay, &anjay->servers, ssid,
                                 AVS_TIME_DURATION_ZERO);
    }

    return 0;
}

static int force_server_reregister(anjay_t *anjay,
                                   anjay_active_server_info_t *server) {
    if (_anjay_sched_now(anjay->sched, NULL, force_server_reregister_clb,
                         (void *) (intptr_t) server->ssid)) {
        anjay_log(DEBUG, "could not schedule server re-registration");
        return -1;
    }
    return 0;
}

static int send_update_sched_job(anjay_t *anjay, void *args) {
    anjay_ssid_t ssid;
    reconnect_required_t reconnect_required;
    send_update_args_decode(&ssid, &reconnect_required, args);

    assert(ssid != ANJAY_SSID_ANY);

    AVS_LIST(anjay_active_server_info_t) server =
            _anjay_servers_find_active(&anjay->servers, ssid);

    if (!server) {
        return -1;
    }

    bool is_bootstrap = (server->ssid == ANJAY_SSID_BOOTSTRAP);

    int result = _anjay_server_refresh(anjay, server, reconnect_required);
    if (!result && reconnect_required && is_bootstrap) {
        result = _anjay_bootstrap_update_reconnected(anjay);
    }

    if (!result && !is_bootstrap) {
        result = _anjay_server_update_or_reregister(anjay, server);
        if (result == AVS_COAP_CTX_ERR_NETWORK) {
            anjay_log(ERROR, "network communication error while updating "
                             "registration for SSID==%" PRIu16, server->ssid);
            // We cannot use _anjay_schedule_server_reconnect(), because it
            // would mean an endless loop without backoff if the server is down.
            // Instead, we disconnect the socket and rely on scheduler's
            // backoff. During the next call, _anjay_server_refresh() will
            // reconnect the socket.
            _anjay_connection_suspend((anjay_connection_ref_t) {
                .server = server,
                .conn_type = server->registration_info.conn_type
            });
        }
    }

    // Updates are retryable, so we only need to reschedule after success
    if (!result) {
        result = _anjay_server_reschedule_update_job(anjay, server);
    }
    return result;
}

static avs_time_duration_t
get_server_update_interval(const anjay_registration_info_t *info) {
    return avs_time_duration_div(
            avs_time_duration_from_scalar(info->last_update_params.lifetime_s,
                                          AVS_TIME_S),
            ANJAY_UPDATE_INTERVAL_MARGIN_FACTOR);
}

static int
schedule_update(anjay_t *anjay,
                anjay_sched_handle_t *out_handle,
                const anjay_active_server_info_t *server,
                avs_time_duration_t delay,
                reconnect_required_t refresh) {
    anjay_log(DEBUG, "scheduling update for SSID %u after "
                     "%" PRId64 ".%09" PRId32,
              server->ssid, delay.seconds, delay.nanoseconds);

    void *update_args = send_update_args_encode(server->ssid, refresh);

    return _anjay_sched_retryable(anjay->sched, out_handle, delay,
                                  ANJAY_SERVER_RETRYABLE_BACKOFF,
                                  send_update_sched_job, update_args);
}

static int
schedule_next_update(anjay_t *anjay,
                     anjay_sched_handle_t *out_handle,
                     const anjay_active_server_info_t *server) {
    avs_time_duration_t remaining =
            _anjay_register_time_remaining(&server->registration_info);
    avs_time_duration_t update_interval =
            get_server_update_interval(&server->registration_info);
    remaining = avs_time_duration_diff(remaining, update_interval);

    if (remaining.seconds < ANJAY_MIN_UPDATE_INTERVAL_S) {
        remaining = avs_time_duration_from_scalar(ANJAY_MIN_UPDATE_INTERVAL_S,
                                                  AVS_TIME_S);
    }

    return schedule_update(anjay, out_handle, server, remaining,
                           DONT_RECONNECT);
}

static int send_update(anjay_t *anjay,
                       anjay_active_server_info_t *server) {
    anjay_connection_ref_t connection = {
        .server = server,
        .conn_type = server->registration_info.conn_type
    };
    if (_anjay_bind_server_stream(anjay, connection)) {
        anjay_log(ERROR, "could not get stream for server %u", server->ssid);
        return -1;
    }

    int result = _anjay_update_registration(anjay);
    if (result == ANJAY_REGISTRATION_UPDATE_REJECTED) {
        anjay_log(DEBUG, "update rejected for SSID = %u; re-registering",
                  server->ssid);
        result = force_server_reregister(anjay, server);
    } else if (result != 0) {
        anjay_log(ERROR, "could not send registration update: %d", result);
    } else {
        _anjay_observe_sched_flush_current_connection(anjay);
    }

    avs_stream_reset(anjay->comm_stream);
    _anjay_release_server_stream(anjay);
    return result;
}

int _anjay_server_update_or_reregister(anjay_t *anjay,
                                       anjay_active_server_info_t *server) {
    bool needs_reregister = false;

    if (server->registration_info.conn_type == ANJAY_CONNECTION_WILDCARD
            || !_anjay_connection_is_online(
                        (anjay_connection_ref_t) {
                            .server = server,
                            .conn_type = server->registration_info.conn_type
                        })) {
        anjay_log(INFO, "No valid existing connection to Registration Interface"
                  " for SSID = %u, re-registering", server->ssid);
        needs_reregister = true;
        if (_anjay_server_setup_registration_connection(server)) {
            return -1;
        }
    }

    if (!needs_reregister) {
        avs_time_duration_t remaining =
                _anjay_register_time_remaining(&server->registration_info);
        if (avs_time_duration_less(remaining, AVS_TIME_DURATION_ZERO)) {
            anjay_log(DEBUG, "Registration Lifetime expired for SSID = %u, "
                      "forcing re-register", server->ssid);
            needs_reregister = true;
        }
    }

    if (needs_reregister) {
        return force_server_reregister(anjay, server);
    } else {
        return send_update(anjay, server);
    }
}

int _anjay_server_reschedule_update_job(anjay_t *anjay,
                                        anjay_active_server_info_t *server) {
    _anjay_sched_del(anjay->sched, &server->sched_update_handle);
    if (schedule_next_update(anjay, &server->sched_update_handle, server)) {
        anjay_log(ERROR, "could not schedule next Update for server %u",
                  server->ssid);
        return -1;
    }
    return 0;
}

static int reschedule_update_for_server(anjay_t *anjay,
                                        anjay_active_server_info_t *server,
                                        reconnect_required_t refresh) {
    _anjay_sched_del(anjay->sched, &server->sched_update_handle);
    if (schedule_update(anjay, &server->sched_update_handle, server,
                        AVS_TIME_DURATION_ZERO, refresh)) {
        anjay_log(ERROR, "could not schedule send_update_sched_job");
        return -1;
    }
    return 0;
}

static int
reschedule_update_for_all_servers(anjay_t *anjay,
                                  reconnect_required_t refresh) {
    int result = 0;

    AVS_LIST(anjay_active_server_info_t) it;
    AVS_LIST_FOREACH(it, anjay->servers.active) {
        int partial = reschedule_update_for_server(anjay, it, refresh);
        if (!result) {
            result = partial;
        }
    }

    return result;
}

int anjay_schedule_registration_update(anjay_t *anjay,
                                       anjay_ssid_t ssid) {
    if (anjay_is_offline(anjay)) {
        anjay_log(ERROR,
                  "cannot schedule registration update while being offline");
        return -1;
    }
    int result = 0;

    if (ssid == ANJAY_SSID_ANY) {
        result = reschedule_update_for_all_servers(anjay, DONT_RECONNECT);
    } else {
        anjay_active_server_info_t *server =
                _anjay_servers_find_active(&anjay->servers, ssid);
        if (!server) {
            anjay_log(ERROR, "no active server with SSID = %u", ssid);
            result = -1;
        } else {
            result = reschedule_update_for_server(anjay, server,
                                                  DONT_RECONNECT);
        }
    }

    return result;
}

int anjay_schedule_reconnect(anjay_t *anjay) {
    int result = reschedule_update_for_all_servers(anjay, DO_RECONNECT);
    if (result) {
        return result;
    }
    anjay->offline = false;
    return 0;
}

int _anjay_schedule_server_reconnect(anjay_t *anjay,
                                     anjay_active_server_info_t *server) {
    return reschedule_update_for_server(anjay, server, DO_RECONNECT);
}

int _anjay_server_register(anjay_t *anjay,
                           anjay_active_server_info_t *server) {
    if (_anjay_server_setup_registration_connection(server)) {
        return -1;
    }
    anjay_connection_ref_t connection = {
        .server = server,
        .conn_type = server->registration_info.conn_type
    };
    if (_anjay_bind_server_stream(anjay, connection)) {
        return -1;
    }

    int result = _anjay_register(anjay);
    avs_stream_reset(anjay->comm_stream);

    if (!result) {
        _anjay_sched_del(anjay->sched, &server->sched_update_handle);
        if (schedule_next_update(anjay, &server->sched_update_handle, server)) {
            anjay_log(WARNING, "could not schedule Update for server %u",
                      server->ssid);
        }

        _anjay_observe_sched_flush_current_connection(anjay);
        _anjay_bootstrap_notify_regular_connection_available(anjay);
    }
    _anjay_release_server_stream(anjay);
    return result;
}

int _anjay_server_deregister(anjay_t *anjay,
                             anjay_active_server_info_t *server) {
    anjay_connection_ref_t connection = {
        .server = server,
        .conn_type = server->registration_info.conn_type
    };
    if (connection.conn_type >= ANJAY_CONNECTION_WILDCARD
            || _anjay_bind_server_stream(anjay, connection)) {
        anjay_log(ERROR, "could not get stream for server %u, skipping",
                  server->ssid);
        return 0;
    }

    int result = _anjay_deregister(anjay);
    if (result) {
        anjay_log(ERROR, "could not send De-Register request: %d", result);
    }

    avs_stream_reset(anjay->comm_stream);
    _anjay_release_server_stream_without_scheduling_queue(anjay);
    return result;
}
