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
#include <pulsecore/source.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/poll.h>

#include "module-tunnel-source-new-symdef.h"

PA_MODULE_AUTHOR("Alexander Couzens");
PA_MODULE_DESCRIPTION(_("Create a network source which connects via a stream to a remote pulseserver"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(_("source_name=<name of source>"));

#define DEFAULT_SINK_NAME "remote_source"

#define MEMBLOCKQ_MAXLENGTH (16*1024*1024)

/* libpulse callbacks */
static void stream_state_callback(pa_stream *stream, void *userdata);
static void context_state_callback(pa_context *c, void *userdata);
static void context_subscribe_callback(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata);
//static void context_source_input_info_callback(pa_context *c, const pa_source_input_info *i, int eol, void *userdata);
/* used for calls we ignore the response */
static void context_ignore_success_callback(pa_context *c, int success, void *userdata);

static void source_update_requested_latency_cb(pa_source *s);
static void source_write_volume_callback(pa_source *source);

struct userdata {
    pa_module *module;
    pa_source *source;
    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_mainloop *rt_mainloop;

    pa_memchunk memchunk;

    /* libpulse context */
    pa_context *context;
    pa_stream *stream;

    /* volume is applied on the remote server - so we have a hw mixer */
    pa_cvolume volume;

    bool connected;

    const char *remote_server;
    const char *remote_source_name;
};

static const char* const valid_modargs[] = {
    "source_name",
    "source_properties",
    "server",
    "source",
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

    pa_log_debug("Tunnelsource-new: Thread starting up");

    rt_mainloop = pa_mainloop_get_api(u->rt_mainloop);
    pa_log("rt_mainloop_api : %p", rt_mainloop );

    pa_thread_mq_install(&u->thread_mq);

    /* TODO: think about volume stuff remote<--stream--source */
    proplist = pa_proplist_new();
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, _("PulseAudio module-tunnel-source-new"));
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_ID, "moule-tunnel-source-new");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_ICON_NAME, "audio-card");
    pa_proplist_sets(proplist, PA_PROP_APPLICATION_VERSION, PACKAGE_VERSION);

    /* init libpulse */
    if (!(u->context = pa_context_new_with_proplist(pa_mainloop_get_api(u->rt_mainloop),
                                              "module-tunnel-source-new",
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
        void **p;

        size_t readable = 0;

        if(pa_mainloop_iterate(u->rt_mainloop, 1, &ret) < 0) {
            if(ret == 0)
                goto finish;
            else
                goto fail;

        }

//        if (PA_UNLIKELY(u->source->thread_info.rewind_requested))
//            pa_source_process_rewind(u->source, 0);

        if (u->connected &&
                PA_STREAM_IS_GOOD(pa_stream_get_state(u->stream)) &&
                PA_SOURCE_IS_OPENED(u->source->thread_info.state)) {
            /* TODO: use IS_RUNNING + cork stream */

            if (pa_stream_is_corked(u->stream)) {
                pa_stream_cork(u->stream, 0, NULL, NULL);
            } else {
                readable = pa_stream_readable_size(u->stream);
                if (readable > 0) {
                    if (u->memchunk.length <= 0) {
                        //                        pa_source_render(u->source, writeable, &u->memchunk);
                    }

                    pa_assert(u->memchunk.length > 0);

                    /* we have new data to write */
                    memchunk.memblock = pa_memblock_new(u->core->mempool, len);
                    pa_assert(memchunk.memblock);

                    p = pa_memblock_acquire(memchunk.memblock);
                    r = pa_stream_peek(u->stream, p, len);
//                    pa_memblock_release(memchunk.memblock);

                    if(r == 0)

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

    if(u->rt_mainloop)
        pa_mainloop_free(u->rt_mainloop);

    pa_log_debug("Thread shutting down");
}

/*static void context_source_input_info_callback(pa_context *c, const pa_source_input_info *i, int eol, void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);

    if(!i)
        return;

    if(eol < 0) {
        return;
    }

    if((pa_context_get_server_protocol_version(c) < 20) || (i->has_volume)) {
        u->volume = i->volume;
        pa_source_update_volume_and_mute(u->source);
    }
} */

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
    case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT: {
        if(u->stream) {
            if((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_CHANGE) {
                if(pa_stream_get_index(u->stream) == idx) {
//                    pa_context_get_source_input_info(u->context, idx, context_source_input_info_callback, u);
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
                                                    "module-tunnel-source-new",
                                                    &u->source->sample_spec,
                                                    &u->source->channel_map,
                                                    proplist);

            pa_proplist_free(proplist);


            memset(&bufferattr, 0, sizeof(pa_buffer_attr));

            bufferattr.maxlength = (uint32_t) - 1;
            bufferattr.minreq = (uint32_t) - 1;
            bufferattr.prebuf = (uint32_t) - 1;
            bufferattr.tlength = (uint32_t) - 1;

            pa_context_subscribe(u->context, PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT, NULL, NULL);

            pa_stream_set_state_callback(u->stream, stream_state_callback, userdata);
            pa_stream_connect_record(u->stream, u->remote_source_name, &bufferattr, PA_STREAM_AUTO_TIMING_UPDATE);
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

static void source_get_volume_callback(pa_source *s) {
    struct userdata *u = s->userdata;

    pa_assert(u);

    if(!pa_cvolume_equal(&u->volume, &s->real_volume)) {
        s->real_volume = u->volume;
        pa_cvolume_set(&s->soft_volume, s->sample_spec.channels, PA_VOLUME_NORM);
    }
}

static void source_set_volume_callback(pa_source *s) {
    struct userdata *u = s->userdata;

    if(!u->stream)
        return;

    u->volume = s->real_volume;

//    pa_context_set_source_input_volume(u->context, pa_stream_get_index(u->stream), &u->volume, context_ignore_success_callback, NULL);
}

static void source_update_requested_latency_cb(pa_source *s) {
    struct userdata *u;
    size_t nbytes;
    pa_usec_t block_usec;
    pa_buffer_attr bufferattr;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    block_usec = pa_source_get_requested_latency_within_thread(s);

    bufferattr.maxlength = (uint32_t) - 1;
    bufferattr.minreq = (uint32_t) - 1;
    bufferattr.prebuf = (uint32_t) - 1;
    bufferattr.tlength = (uint32_t) - 1;

    if (block_usec == (pa_usec_t) -1)
        block_usec = s->thread_info.max_latency;

    nbytes = pa_usec_to_bytes(block_usec, &s->sample_spec);

    if (block_usec != (pa_usec_t) -1) {
        bufferattr.tlength = nbytes;
    }

    if(PA_STREAM_IS_GOOD(pa_stream_get_state(u->stream))) {
        pa_stream_set_buffer_attr(u->stream, &bufferattr, NULL, NULL);
    }
}

static void source_write_volume_callback(pa_source *s) {
    struct userdata *u = s->userdata;
    pa_cvolume hw_vol = s->thread_info.current_hw_volume;

    pa_assert(u);
}

static int source_process_msg_cb(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY: {
            int negative;
            pa_usec_t remote_latency;

            if (!PA_SINK_IS_LINKED(u->source->thread_info.state)) {
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
    return pa_source_process_msg(o, code, data, offset, chunk);
}

int pa__init(pa_module*m) {
    struct userdata *u = NULL;
    pa_modargs *ma = NULL;
    pa_source_new_data source_data;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_proplist *proplist = NULL;
    const char *remote_server = NULL;

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
    u->rt_mainloop = pa_mainloop_new();
    if(u->rt_mainloop == NULL) {
        pa_log("Failed to create mainloop");
        goto fail;
    }

    u->remote_source_name = pa_modargs_get_value(ma, "source", NULL);

    pa_cvolume_init(&u->volume);
    pa_cvolume_reset(&u->volume, ss.channels);

    pa_thread_mq_init_rtmainloop(&u->thread_mq, m->core->mainloop, pa_mainloop_get_api(u->rt_mainloop));

    /* Create source */
    pa_source_new_data_init(&source_data);
    source_data.driver = __FILE__;
    source_data.module = m;

    pa_source_new_data_set_name(&source_data, pa_modargs_get_value(ma, "source_name", DEFAULT_SINK_NAME));
    pa_source_new_data_set_sample_spec(&source_data, &ss);
    pa_source_new_data_set_channel_map(&source_data, &map);

    /* TODO: set DEVICE CLASS */
    pa_proplist_sets(source_data.proplist, PA_PROP_DEVICE_CLASS, "abstract");

    pa_proplist_setf(source_data.proplist, PA_PROP_DEVICE_DESCRIPTION, "tunnel to remote pulseaudio %s", remote_server);

    if (pa_modargs_get_proplist(ma, "source_properties", source_data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_source_new_data_done(&source_data);
        goto fail;
    }
    /* TODO: check PA_SINK_LATENCY + PA_SINK_DYNAMIC_LATENCY */
    if (!(u->source = pa_source_new(m->core, &source_data, (PA_SINK_LATENCY|PA_SINK_DYNAMIC_LATENCY|PA_SINK_NETWORK)))) {
        pa_log("Failed to create source.");
        pa_source_new_data_done(&source_data);
        goto fail;
    }

    pa_source_new_data_done(&source_data);
    u->source->userdata = u;

    /* source callbacks */
    u->source->parent.process_msg = source_process_msg_cb;
    u->source->update_requested_latency = source_update_requested_latency_cb;

    /* set thread queue */
    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);

    pa_source_set_get_volume_callback(u->source, source_get_volume_callback);
    pa_source_set_set_volume_callback(u->source, source_set_volume_callback);
    pa_source_set_write_volume_callback(u->source, source_write_volume_callback);
    /* TODO: latency / rewind
    u->source->update_requested_latency = source_update_requested_latency_cb;
    u->block_usec = BLOCK_USEC;
    nbytes = pa_usec_to_bytes(u->block_usec, &u->source->sample_spec);
    pa_source_set_max_rewind(u->source, nbytes);
    pa_source_set_max_request(u->source, nbytes);
    pa_source_set_latency_range(u->source, 0, BLOCK_USEC); */

    if (!(u->thread = pa_thread_new("module-tunnel-source-new", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_source_put(u->source);
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

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->source)
        pa_source_unlink(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if(u->remote_source_name)
        free((void *) u->remote_source_name);

    if(u->remote_server)
        free((void *) u->remote_server);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->source)
        pa_source_unref(u->source);

    pa_xfree(u);
}
