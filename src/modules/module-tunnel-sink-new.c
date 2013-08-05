/***
    This file is part of PulseAudio.

    Copyright 2013 Alexander Couzens

    PulseAudio is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    PulseAudio is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with PulseAudio; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
    USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/context.h>
#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>
#include <pulse/stream.h>
#include <pulse/mainloop.h>
#include <pulse/subscribe.h>
#include <pulse/introspect.h>

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/i18n.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/poll.h>

#include "module-tunnel-sink-new-symdef.h"

PA_MODULE_AUTHOR("Alexander Couzens");
PA_MODULE_DESCRIPTION("Create a network sink which connects via a stream to a remote PulseAudio server");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "server=<address> "
        "sink=<name of the remote sink> "
        "sink_name=<name for the local sink> "
        "sink_properties=<properties for the local sink> "
        "format=<sample format> "
        "channels=<number of channels> "
        "rate=<sample rate> "
        "channel_map=<channel map>"
        );

/* libpulse callbacks */
static void stream_state_callback(pa_stream *stream, void *userdata);
static void context_state_callback(pa_context *c, void *userdata);
static void context_subscribe_callback(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata);
static void context_sink_input_info_callback(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata);
/* used for calls we ignore the response */
static void context_ignore_success_callback(pa_context *c, int success, void *userdata);

static void sink_update_requested_latency_cb(pa_sink *s);
static void sink_write_volume_callback(pa_sink *sink);

struct userdata {
    pa_module *module;
    pa_sink *sink;
    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_mainloop *thread_mainloop;

    pa_memchunk memchunk;

    /* libpulse context */
    pa_context *context;
    pa_stream *stream;

    /* volume is applied on the remote server - so we have a hw mixer */
    pa_cvolume volume;

    bool connected;

    const char *remote_server;
    const char *remote_sink_name;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    "server",
    "sink",
    "format",
    "channels",
    "rate",
    "channel_map",
    "cookie", /* unimplemented */
    "reconnect", /* reconnect if server comes back again - unimplemented*/
    NULL,
};

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    pa_proplist *proplist;
    pa_mainloop_api *rt_mainloop;

    pa_assert(u);

    pa_log_debug("Tunnelsink-new: Thread starting up");

    rt_mainloop = pa_mainloop_get_api(u->thread_mainloop);
    pa_log("rt_mainloop_api : %p", rt_mainloop );

    pa_thread_mq_install(&u->thread_mq);

    /* TODO: think about volume stuff remote<--stream--source */
    proplist = pa_proplist_new();
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, _("PulseAudio module-tunnel-sink-new"));
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_ID, "moule-tunnel-sink-new");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_ICON_NAME, "audio-card");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_VERSION, PACKAGE_VERSION);

    /* init libpulse */
    if (!(u->context = pa_context_new_with_proplist(pa_mainloop_get_api(u->thread_mainloop),
                                              "module-tunnel-sink-new",
                                              proplist))) {
        pa_log("Failed to create libpulse context");
        goto fail;
    }

    pa_context_set_subscribe_callback(u->context, context_subscribe_callback, u);
    pa_context_set_state_callback(u->context, context_state_callback, u);
    if (pa_context_connect(u->context,
                          u->remote_server,
                          PA_CONTEXT_NOFAIL | PA_CONTEXT_NOAUTOSPAWN,
                          NULL) < 0) {
        pa_log("Failed to connect libpulse context");
        goto fail;
    }

    pa_proplist_free(proplist);

    for(;;)
    {
        int ret;
        const void *p;

        size_t writeable = 0;

        if(pa_mainloop_iterate(u->thread_mainloop, 1, &ret) < 0) {
            if(ret == 0)
                goto finish;
            else
                goto fail;

        }

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
            pa_sink_process_rewind(u->sink, 0);

        if (u->connected &&
                PA_STREAM_IS_GOOD(pa_stream_get_state(u->stream)) &&
                PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            /* TODO: use IS_RUNNING + cork stream */

            if (pa_stream_is_corked(u->stream)) {
                pa_stream_cork(u->stream, 0, NULL, NULL);
            } else {
                writeable = pa_stream_writable_size(u->stream);
                if (writeable > 0) {
                    if (u->memchunk.length <= 0)
                        pa_sink_render(u->sink, writeable, &u->memchunk);

                    pa_assert(u->memchunk.length > 0);

                    /* we have new data to write */
                    p = (const uint8_t *) pa_memblock_acquire(u->memchunk.memblock);
                    ret = pa_stream_write(u->stream,
                                        ((uint8_t*) p + u->memchunk.index),         /**< The data to write */
                                        u->memchunk.length,            /**< The length of the data to write in bytes */
                                        NULL,     /**< A cleanup routine for the data or NULL to request an internal copy */
                                        0,          /**< Offset for seeking, must be 0 for upload streams */
                                        PA_SEEK_RELATIVE      /**< Seek mode, must be PA_SEEK_RELATIVE for upload streams */
                                        );
                    pa_memblock_release(u->memchunk.memblock);
                    pa_memblock_unref(u->memchunk.memblock);
                    pa_memchunk_reset(&u->memchunk);

                    if (ret != 0) {
                        /* TODO: we should consider a state change or is that already done ? */
                        pa_log_warn("Could not write data into the stream ... ret = %i", ret);
                    }
                }
            }
        }
    }
fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN
     *
     * Note: is this a race condition? When a PA_MESSAGE_SHUTDOWN already within the queue?
     */
    pa_asyncmsgq_flush(u->thread_mq.inq, false);
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->module->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_asyncmsgq_flush(u->thread_mq.inq, false);

    if (u->stream)
        pa_stream_disconnect(u->stream);

    if (u->context)
        pa_context_disconnect(u->context);

    if(u->thread_mainloop)
        pa_mainloop_free(u->thread_mainloop);

    pa_log_debug("Thread shutting down");
}

static void context_sink_input_info_callback(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    if(!i)
        return;

    if(eol < 0) {
        return;
    }

    if((pa_context_get_server_protocol_version(c) < 20) || (i->has_volume)) {
        u->volume = i->volume;
        pa_sink_update_volume_and_mute(u->sink);
    }
}

static void stream_state_callback(pa_stream *stream, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);
    pa_assert(stream == u->stream);

    switch(pa_stream_get_state(stream)) {
        case PA_STREAM_FAILED:
            pa_log_debug("Context failed.");
            pa_stream_unref(stream);
            u->stream = NULL;

            /* TODO: think about killing the context or should we just try again a creationg of a stream ? */
            if(u->context)
                pa_context_disconnect(u->context);
            break;
        case PA_STREAM_TERMINATED:
            pa_log_debug("Context terminated.");
            pa_stream_unref(stream);
            u->stream = NULL;

            if(u->context)
                pa_context_disconnect(u->context);
            break;
        default:
            break;
    }
}

static void context_subscribe_callback(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(userdata);

    switch (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
    case PA_SUBSCRIPTION_EVENT_SINK_INPUT: {
        if(u->stream) {
            if((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_CHANGE) {
                if(pa_stream_get_index(u->stream) == idx) {
                    pa_context_get_sink_input_info(u->context, idx, context_sink_input_info_callback, u);
                }
            }
        }
        break;
    }
    default:
        /* ignoring event */
        break;
    }
}

/* active ignoring */
static void context_ignore_success_callback(pa_context *c, int success, void *userdata) {
}

static void context_state_callback(pa_context *c, void *userdata) {
    struct userdata *u = userdata;
    int c_errno;

    pa_assert(u);
    pa_assert(u->context == c);

    switch(pa_context_get_state(c)) {
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            pa_log_debug("Connection unconnected");
            break;
        case PA_CONTEXT_READY: {
            pa_proplist *proplist;
            pa_buffer_attr bufferattr;

            pa_log_debug("Connection successful. Creating stream.");
            pa_assert(!u->stream);

            proplist = pa_proplist_new();
            pa_assert(proplist);


            u->stream = pa_stream_new_with_proplist(u->context,
                                                    "module-tunnel-sink-new",
                                                    &u->sink->sample_spec,
                                                    &u->sink->channel_map,
                                                    proplist);

            pa_proplist_free(proplist);


            memset(&bufferattr, 0, sizeof(pa_buffer_attr));

            bufferattr.maxlength = (uint32_t) - 1;
            bufferattr.minreq = (uint32_t) - 1;
            bufferattr.prebuf = (uint32_t) - 1;
            bufferattr.tlength = (uint32_t) - 1;

            pa_context_subscribe(u->context, PA_SUBSCRIPTION_MASK_SINK_INPUT, NULL, NULL);

            pa_stream_set_state_callback(u->stream, stream_state_callback, userdata);
            pa_stream_connect_playback(u->stream,
                                       u->remote_sink_name,
                                       &bufferattr,
                                       PA_STREAM_START_CORKED | PA_STREAM_AUTO_TIMING_UPDATE,
                                       NULL,
                                       NULL);
            u->connected = true;
            break;
        }
        case PA_CONTEXT_FAILED:
            c_errno = pa_context_errno(u->context);
            pa_log_debug("Context failed.");
            u->connected = false;
            pa_context_unref(u->context);
            u->context = NULL;

            pa_module_unload_request(u->module, false);
            break;

        case PA_CONTEXT_TERMINATED:
            c_errno = pa_context_errno(u->context);
            pa_log_debug("Context terminated.");
            u->connected = false;
            pa_context_unref(u->context);
            u->context = NULL;

            pa_module_unload_request(u->module, false);
            break;
        default:
            break;
    }
}

static void sink_get_volume_callback(pa_sink *s) {
    struct userdata *u = s->userdata;

    pa_assert(u);

    if(!pa_cvolume_equal(&u->volume, &s->real_volume)) {
        s->real_volume = u->volume;
        pa_cvolume_set(&s->soft_volume, s->sample_spec.channels, PA_VOLUME_NORM);
    }
}

static void sink_set_volume_callback(pa_sink *s) {
    struct userdata *u = s->userdata;

    if(!u->stream)
        return;

    u->volume = s->real_volume;

    pa_context_set_sink_input_volume(u->context, pa_stream_get_index(u->stream), &u->volume, context_ignore_success_callback, NULL);
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;
    size_t nbytes;
    pa_usec_t block_usec;
    pa_buffer_attr bufferattr;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    block_usec = pa_sink_get_requested_latency_within_thread(s);

    bufferattr.maxlength = (uint32_t) - 1;
    bufferattr.minreq = (uint32_t) - 1;
    bufferattr.prebuf = (uint32_t) - 1;
    bufferattr.tlength = (uint32_t) - 1;

    if (block_usec == (pa_usec_t) -1)
        block_usec = s->thread_info.max_latency;

    nbytes = pa_usec_to_bytes(block_usec, &s->sample_spec);
    pa_sink_set_max_rewind_within_thread(s, nbytes);
    pa_sink_set_max_request_within_thread(s, nbytes);

    if (block_usec != (pa_usec_t) -1) {
        bufferattr.tlength = nbytes;
    }

    if(PA_STREAM_IS_GOOD(pa_stream_get_state(u->stream))) {
        pa_stream_set_buffer_attr(u->stream, &bufferattr, NULL, NULL);
    }
}

static void sink_write_volume_callback(pa_sink *s) {
    struct userdata *u = s->userdata;
    pa_cvolume hw_vol = s->thread_info.current_hw_volume;

    pa_assert(u);
}

static int sink_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY: {
            int negative;
            pa_usec_t remote_latency;

            if (!PA_SINK_IS_LINKED(u->sink->thread_info.state)) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            if(!u->stream) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            if(!PA_STREAM_IS_GOOD(pa_stream_get_state(u->stream))) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            if(pa_stream_get_latency(u->stream, &remote_latency, &negative) < 0) {
                *((pa_usec_t*) data) = 0;
                return 0;
            }

            *((pa_usec_t*) data) =
                /* Add the latency from libpulse */
                remote_latency;
                /* do we have to add more latency here ? */
            return 0;
        }
    }
    return pa_sink_process_msg(o, code, data, offset, chunk);
}

int pa__init(pa_module *m) {
    struct userdata *u = NULL;
    pa_modargs *ma = NULL;
    pa_sink_new_data sink_data;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_proplist *proplist = NULL;
    const char *remote_server = NULL;
    const char *sink_name = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    remote_server = pa_modargs_get_value(ma, "server", NULL);
    if (!remote_server) {
        pa_log("No server given!");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->module = m;
    m->userdata = u;
    u->remote_server = strdup(remote_server);
    pa_memchunk_reset(&u->memchunk);
    u->thread_mainloop = pa_mainloop_new();
    if(u->thread_mainloop == NULL) {
        pa_log("Failed to create mainloop");
        goto fail;
    }

    u->remote_sink_name = pa_modargs_get_value(ma, "sink", NULL);

    pa_cvolume_init(&u->volume);
    pa_cvolume_reset(&u->volume, ss.channels);

    pa_thread_mq_init_thread_mainloop(&u->thread_mq, m->core->mainloop, pa_mainloop_get_api(u->thread_mainloop));

    /* Create sink */
    pa_sink_new_data_init(&sink_data);
    sink_data.driver = __FILE__;
    sink_data.module = m;

    sink_name = pa_modargs_get_value(ma, "sink_name", NULL);
    if(!sink_name)
        sink_name = pa_sprintf_malloc("tunnel.%s", remote_server);

    pa_sink_new_data_set_name(&sink_data, sink_name);
    pa_sink_new_data_set_sample_spec(&sink_data, &ss);
    pa_sink_new_data_set_channel_map(&sink_data, &map);

    /* TODO: set DEVICE CLASS */
    pa_proplist_sets(sink_data.proplist, PA_PROP_DEVICE_CLASS, "abstract");

    pa_proplist_setf(sink_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "tunnel to remote pulseaudio %s", remote_server);

    if (pa_modargs_get_proplist(ma, "sink_properties", sink_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&sink_data);
        goto fail;
    }
    /* TODO: check PA_SINK_LATENCY + PA_SINK_DYNAMIC_LATENCY */
    if (!(u->sink = pa_sink_new(m->core, &sink_data, (PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY|PA_SINK_NETWORK)))) {
        pa_log("Failed to create sink.");
        pa_sink_new_data_done(&sink_data);
        goto fail;
    }

    pa_sink_new_data_done(&sink_data);
    u->sink->userdata = u;

    /* sink callbacks */
    u->sink->parent.process_msg = sink_process_msg_cb;
    u->sink->update_requested_latency = sink_update_requested_latency_cb;

    /* set thread queue */
    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);

    pa_sink_set_get_volume_callback(u->sink, sink_get_volume_callback);
    pa_sink_set_set_volume_callback(u->sink, sink_set_volume_callback);
    pa_sink_set_write_volume_callback(u->sink, sink_write_volume_callback);
    /* TODO: latency / rewind
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->block_usec = BLOCK_USEC;
    nbytes = pa_usec_to_bytes(u->block_usec, &u->sink->sample_spec);
    pa_sink_set_max_rewind(u->sink, nbytes);
    pa_sink_set_max_request(u->sink, nbytes);
    pa_sink_set_latency_range(u->sink, 0, BLOCK_USEC); */

    if (!(u->thread = pa_thread_new("module-tunnel-sink-new", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_put(u->sink);
    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    if (proplist)
        pa_proplist_free(proplist);

    pa__done(m);

    return -1;
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if(u->remote_sink_name)
        free((void *) u->remote_sink_name);

    if(u->remote_server)
        free((void *) u->remote_server);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->sink)
        pa_sink_unref(u->sink);

    pa_xfree(u);
}
