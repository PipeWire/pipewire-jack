/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <regex.h>
#include <math.h>

#include <jack/jack.h>
#include <jack/session.h>
#include <jack/thread.h>
#include <jack/midiport.h>
#include <jack/uuid.h>

#include <spa/support/cpu.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>
#include <spa/debug/pod.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <pipewire/data-loop.h>

#include "extensions/client-node.h"

#define JACK_DEFAULT_VIDEO_TYPE	"32 bit float RGBA video"

#define JACK_CLIENT_NAME_SIZE		64
#define JACK_PORT_NAME_SIZE		256
#define JACK_PORT_MAX			4096
#define JACK_PORT_TYPE_SIZE             32
#define CONNECTION_NUM_FOR_PORT		1024

#define MAX_BUFFER_FRAMES		8192

#define MAX_ALIGN			16
#define MAX_OBJECTS			8192
#define MAX_PORTS			1024
#define MAX_BUFFERS			2
#define MAX_BUFFER_DATAS		4u
#define MAX_BUFFER_MEMS			4
#define MAX_MIX				4096
#define MAX_IO				32

#define DEFAULT_SAMPLE_RATE	48000
#define DEFAULT_BUFFER_FRAMES	1024
#define DEFAULT_LATENCY		SPA_STRINGIFY(DEFAULT_BUFFER_FRAMES/DEFAULT_SAMPLE_RATE)

#define REAL_JACK_PORT_NAME_SIZE (JACK_CLIENT_NAME_SIZE + JACK_PORT_NAME_SIZE)

#define NAME	"jack-client"

struct client;
struct port;

struct globals {
	jack_thread_creator_t creator;
};

static struct globals globals;

#define OBJECT_CHUNK	8

typedef void (*mix2_func) (float *dst, float *src1, float *src2, int n_samples);

static mix2_func mix2;

struct object {
	struct spa_list link;

	struct client *client;

	uint32_t type;
	uint32_t id;

	union {
		struct {
			char name[JACK_CLIENT_NAME_SIZE+1];
			int32_t priority;
		} node;
		struct {
			uint32_t src;
			uint32_t dst;
		} port_link;
		struct {
			unsigned long flags;
			char name[REAL_JACK_PORT_NAME_SIZE+1];
			char alias1[REAL_JACK_PORT_NAME_SIZE+1];
			char alias2[REAL_JACK_PORT_NAME_SIZE+1];
			uint32_t type_id;
			uint32_t node_id;
			uint32_t port_id;
			uint32_t monitor_requests;
			jack_latency_range_t capture_latency;
			jack_latency_range_t playback_latency;
			int32_t priority;
		} port;
	};
};

struct midi_buffer {
#define MIDI_BUFFER_MAGIC 0x900df00d
	uint32_t magic;
	int32_t buffer_size;
	uint32_t nframes;
	int32_t write_pos;
	uint32_t event_count;
	uint32_t lost_events;
};

#define MIDI_INLINE_MAX	4

struct midi_event {
	uint16_t time;
        uint16_t size;
        union {
		uint32_t byte_offset;
		uint8_t inline_data[MIDI_INLINE_MAX];
	};
};

struct buffer {
	struct spa_list link;
#define BUFFER_FLAG_OUT		(1<<0)
#define BUFFER_FLAG_MAPPED	(1<<1)
	uint32_t flags;
	uint32_t id;

	struct spa_data datas[MAX_BUFFER_DATAS];
	uint32_t n_datas;

	struct pw_memmap *mem[MAX_BUFFER_DATAS+1];
	uint32_t n_mem;
};

struct link {
	uint32_t node_id;
	struct pw_memmap *mem;
	struct pw_node_activation *activation;
	int signalfd;
};

struct mix {
	struct spa_list link;
	struct spa_list port_link;
	uint32_t id;
	struct port *port;

	struct spa_io_buffers *io;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
	struct spa_list queue;
};

struct port {
	bool valid;
	struct spa_list link;

	struct client *client;

	enum spa_direction direction;
	uint32_t id;
	struct object *object;

	struct spa_io_buffers io;
	struct spa_list mix;

	bool have_format;
	uint32_t rate;

	bool zeroed;
	float *emptyptr;
	float empty[MAX_BUFFER_FRAMES + MAX_ALIGN];
};

struct context {
	struct pw_main_loop *main;
	struct pw_thread_loop *loop;
	struct pw_core *core;

	struct pw_map globals;
	struct spa_list free_objects;
	struct spa_list ports;
	struct spa_list nodes;
	struct spa_list links;
};

#define GET_DIRECTION(f)	((f) & JackPortIsInput ? SPA_DIRECTION_INPUT : SPA_DIRECTION_OUTPUT)

#define GET_IN_PORT(c,p)	(&c->port_pool[SPA_DIRECTION_INPUT][p])
#define GET_OUT_PORT(c,p)	(&c->port_pool[SPA_DIRECTION_OUTPUT][p])
#define GET_PORT(c,d,p)		(d == SPA_DIRECTION_INPUT ? GET_IN_PORT(c,p) : GET_OUT_PORT(c,p))

struct client {
	char name[JACK_CLIENT_NAME_SIZE+1];

	struct context context;

	struct pw_data_loop *loop;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;
	struct spa_hook core_listener;
	int last_sync;
	bool error;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct pw_client_node_proxy *node_proxy;
	struct spa_hook node_listener;
        struct spa_hook proxy_listener;

	uint32_t node_id;
	struct spa_source *socket_source;

	JackThreadCallback thread_callback;
	void *thread_arg;
	JackThreadInitCallback thread_init_callback;
	void *thread_init_arg;
	JackShutdownCallback shutdown_callback;
	void *shutdown_arg;
	JackInfoShutdownCallback info_shutdown_callback;
	void *info_shutdown_arg;
	JackProcessCallback process_callback;
	void *process_arg;
	JackFreewheelCallback freewheel_callback;
	void *freewheel_arg;
	JackBufferSizeCallback bufsize_callback;
	void *bufsize_arg;
	JackSampleRateCallback srate_callback;
	void *srate_arg;
	JackClientRegistrationCallback registration_callback;
	void *registration_arg;
	JackPortRegistrationCallback portregistration_callback;
	void *portregistration_arg;
	JackPortConnectCallback connect_callback;
	void *connect_arg;
	JackPortRenameCallback rename_callback;
	void *rename_arg;
	JackGraphOrderCallback graph_callback;
	void *graph_arg;
	JackXRunCallback xrun_callback;
	void *xrun_arg;
	JackLatencyCallback latency_callback;
	void *latency_arg;
	JackSyncCallback sync_callback;
	void *sync_arg;
	JackTimebaseCallback timebase_callback;
	void *timebase_arg;

	struct spa_io_position *position;
	uint32_t sample_rate;
	uint32_t buffer_frames;

	struct mix mix_pool[MAX_MIX];
	struct spa_list free_mix;

	struct port port_pool[2][MAX_PORTS];
	struct spa_list ports[2];
	struct spa_list free_ports[2];

	struct pw_array links;
	uint32_t driver_id;
	struct pw_node_activation *driver_activation;

	struct pw_memmap *mem;
	struct pw_node_activation *activation;
	uint32_t xrun_count;

	unsigned int started:1;
	unsigned int active:1;
	unsigned int destroyed:1;
	unsigned int first:1;
	unsigned int thread_entered:1;

	jack_position_t jack_position;
	jack_transport_state_t jack_state;
};

static void init_port_pool(struct client *c, enum spa_direction direction)
{
	int i;

	spa_list_init(&c->ports[direction]);
	spa_list_init(&c->free_ports[direction]);
	for (i = 0; i < MAX_PORTS; i++) {
		c->port_pool[direction][i].direction = direction;
		c->port_pool[direction][i].id = i;
		c->port_pool[direction][i].emptyptr =
			SPA_PTR_ALIGN(c->port_pool[direction][i].empty, MAX_ALIGN, float);
		spa_list_append(&c->free_ports[direction], &c->port_pool[direction][i].link);
	}
}

static struct object * alloc_object(struct client *c)
{
	struct object *o;
	int i;

	if (spa_list_is_empty(&c->context.free_objects)) {
		o = calloc(OBJECT_CHUNK, sizeof(struct object));
		if (o == NULL)
			return NULL;
		for (i = 0; i < OBJECT_CHUNK; i++)
			spa_list_append(&c->context.free_objects, &o[i].link);
	}

        o = spa_list_first(&c->context.free_objects, struct object, link);
        spa_list_remove(&o->link);
	o->client = c;

	return o;
}

static void free_object(struct client *c, struct object *o)
{
        spa_list_remove(&o->link);
	spa_list_append(&c->context.free_objects, &o->link);
}

static struct mix *find_mix(struct client *c, struct port *port, uint32_t mix_id)
{
	struct mix *mix;

	spa_list_for_each(mix, &port->mix, port_link) {
		if (mix->id == mix_id)
			return mix;
	}
	return NULL;
}

static struct mix *ensure_mix(struct client *c, struct port *port, uint32_t mix_id)
{
	struct mix *mix;

	if ((mix = find_mix(c, port, mix_id)) != NULL)
		return mix;

	if (spa_list_is_empty(&c->free_mix))
		return NULL;

	mix = spa_list_first(&c->free_mix, struct mix, link);
	spa_list_remove(&mix->link);

	spa_list_append(&port->mix, &mix->port_link);

	mix->id = mix_id;
	mix->port = port;
	mix->io = NULL;
	mix->n_buffers = 0;

	return mix;
}

static void free_mix(struct client *c, struct mix *mix)
{
	spa_list_remove(&mix->link);
	spa_list_remove(&mix->port_link);
	spa_list_append(&c->free_mix, &mix->link);
}

static struct port * alloc_port(struct client *c, enum spa_direction direction)
{
	struct port *p;
	struct object *o;

	if (spa_list_is_empty(&c->free_ports[direction]))
		return NULL;

	p = spa_list_first(&c->free_ports[direction], struct port, link);
	spa_list_remove(&p->link);

	o = alloc_object(c);
	o->type = PW_TYPE_INTERFACE_Port;
	o->id = SPA_ID_INVALID;
	o->port.node_id = c->node_id;
	o->port.port_id = p->id;
	spa_list_append(&c->context.ports, &o->link);

	p->valid = true;
	p->zeroed = false;
	p->client = c;
	p->object = o;
	spa_list_init(&p->mix);

	spa_list_append(&c->ports[direction], &p->link);

	return p;
}

static void free_port(struct client *c, struct port *p)
{
	struct mix *m, *t;

	if (!p->valid)
		return;

	spa_list_for_each_safe(m, t, &p->mix, port_link)
		free_mix(c, m);

	spa_list_remove(&p->link);
	p->valid = false;
	free_object(c, p->object);
	spa_list_append(&c->free_ports[p->direction], &p->link);
}

static struct object *find_port(struct client *c, const char *name)
{
	struct object *o;

	spa_list_for_each(o, &c->context.ports, link) {
		if (!strcmp(o->port.name, name))
			return o;
	}
	return NULL;
}

static struct object *find_link(struct client *c, uint32_t src, uint32_t dst)
{
	struct object *l;

	spa_list_for_each(l, &c->context.links, link) {
		if (l->port_link.src == src &&
		    l->port_link.dst == dst) {
			return l;
		}
	}
	return NULL;
}

static struct buffer *dequeue_buffer(struct mix *mix)
{
        struct buffer *b;

        if (spa_list_is_empty(&mix->queue))
                return NULL;

        b = spa_list_first(&mix->queue, struct buffer, link);
        spa_list_remove(&b->link);
	SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);

        return b;
}

#if defined (__SSE__)
#include <xmmintrin.h>
static void mix2_sse(float *dst, float *src1, float *src2, int n_samples)
{
	int n, unrolled;
	__m128 in[2];

	if (SPA_IS_ALIGNED(src1, 16) &&
	    SPA_IS_ALIGNED(src2, 16) &&
	    SPA_IS_ALIGNED(dst, 16))
		unrolled = n_samples / 4;
	else
		unrolled = 0;

	for (n = 0; unrolled--; n += 4) {
		in[0] = _mm_load_ps(&src1[n]),
		in[1] = _mm_load_ps(&src2[n]),
		in[0] = _mm_add_ps(in[0], in[1]);
		_mm_store_ps(&dst[n], in[0]);
	}
	for (; n < n_samples; n++) {
		in[0] = _mm_load_ss(&src1[n]),
		in[1] = _mm_load_ss(&src2[n]),
		in[0] = _mm_add_ss(in[0], in[1]);
		_mm_store_ss(&dst[n], in[0]);
	}
}
#endif

static void mix2_c(float *dst, float *src1, float *src2, int n_samples)
{
	int i;
	for (i = 0; i < n_samples; i++)
		dst[i] = src1[i] + src2[i];
}

SPA_EXPORT
void jack_get_version(int *major_ptr, int *minor_ptr, int *micro_ptr, int *proto_ptr)
{
	*major_ptr = 0;
	*minor_ptr = 0;
	*micro_ptr = 0;
	*proto_ptr = 0;
}

SPA_EXPORT
const char *
jack_get_version_string(void)
{
	return "0.0.0.0";
}

static void on_state_changed(void *data, enum pw_remote_state old,
                             enum pw_remote_state state, const char *error)
{
	struct client *client = data;

	pw_log_debug(NAME" %p: state %s", client, pw_remote_state_as_string(state));
	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		client->error = true;
		/* fallthrough*/
        case PW_REMOTE_STATE_UNCONNECTED:
		/* don't call shutdown when we do client_close, only
		 * on unexpected errors */
		if (client->shutdown_callback && !client->destroyed)
			client->shutdown_callback(client->shutdown_arg);
		/* fallthrough*/
        case PW_REMOTE_STATE_CONNECTED:
		pw_thread_loop_signal(client->context.loop, false);
                break;
        default:
                break;
        }
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
};

static void on_sync_reply(void *data, uint32_t id, int seq)
{
	struct client *client = data;
	if (id != 0)
		return;
	client->last_sync = seq;
	pw_thread_loop_signal(client->context.loop, false);
}

static const struct pw_core_proxy_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_sync_reply,
};

static int do_sync(struct client *client)
{
	int seq;

	seq = pw_proxy_sync((struct pw_proxy*)client->core_proxy, client->last_sync);

	while (true) {
	        pw_thread_loop_wait(client->context.loop);

		if (client->error)
			return -1;

		if (client->last_sync == seq)
			break;
	}
	return 0;
}

static void on_node_proxy_destroy(void *data)
{
	struct client *client = data;

	client->node_proxy = NULL;
	spa_hook_remove(&client->proxy_listener);

}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = on_node_proxy_destroy,
};

static struct link *find_activation(struct pw_array *links, uint32_t node_id)
{
	struct link *l;

	pw_array_for_each(l, links) {
		if (l->node_id == node_id)
			return l;
	}
	return NULL;
}

static int
do_remove_sources(struct spa_loop *loop,
                  bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct client *c = user_data;

	if (c->socket_source) {
		pw_loop_destroy_source(c->loop->loop, c->socket_source);
		c->socket_source = NULL;
	}
	return 0;
}

static void unhandle_socket(struct client *c)
{
        pw_loop_invoke(c->loop->loop,
                       do_remove_sources, 1, NULL, 0, true, c);
}

static void reuse_buffer(struct client *c, struct mix *mix, uint32_t id)
{
	struct buffer *b;

	b = &mix->buffers[id];

	if (SPA_FLAG_IS_SET(b->flags, BUFFER_FLAG_OUT)) {
		pw_log_trace(NAME" %p: port %p: recycle buffer %d", c, mix->port, id);
		spa_list_append(&mix->queue, &b->link);
		SPA_FLAG_CLEAR(b->flags, BUFFER_FLAG_OUT);
	}
}


static void convert_from_midi(void *midi, void *buffer, size_t size)
{
	struct spa_pod_builder b = { 0, };
	uint32_t i, count;
	struct spa_pod_frame f;

	count = jack_midi_get_event_count(midi);

	spa_pod_builder_init(&b, buffer, size);
	spa_pod_builder_push_sequence(&b, &f, 0);

	for (i = 0; i < count; i++) {
		jack_midi_event_t ev;
		jack_midi_event_get(&ev, midi, i);
		spa_pod_builder_control(&b, ev.time, SPA_CONTROL_Midi);
		spa_pod_builder_bytes(&b, ev.buffer, ev.size);
	}
        spa_pod_builder_pop(&b, &f);
}

static void convert_to_midi(struct spa_pod_sequence **seq, uint32_t n_seq, void *midi)
{
	struct spa_pod_control *c[n_seq];
	uint32_t i;

	for (i = 0; i < n_seq; i++) {
		c[i] = spa_pod_control_first(&seq[i]->body);
	}

	while (true) {
		struct spa_pod_control *next = NULL;
		uint32_t next_index = 0;

		for (i = 0; i < n_seq; i++) {
			if (!spa_pod_control_is_inside(&seq[i]->body,
						SPA_POD_BODY_SIZE(seq[i]), c[i]))
				continue;

			if (next == NULL || c[i]->offset < next->offset) {
				next = c[i];
				next_index = i;
			}
		}
		if (next == NULL)
			break;

		switch(next->type) {
		case SPA_CONTROL_Midi:
			jack_midi_event_write(midi,
					next->offset,
					SPA_POD_BODY(&next->value),
					SPA_POD_BODY_SIZE(&next->value));
			break;
		}
		c[next_index] = spa_pod_control_next(c[next_index]);
	}
}


static void *get_buffer_output(struct client *c, struct port *p, uint32_t frames, uint32_t stride)
{
	struct mix *mix;
	void *ptr = NULL;

	p->io.status = -EPIPE;
	p->io.buffer_id = SPA_ID_INVALID;

	if ((mix = find_mix(c, p, -1)) != NULL) {
		struct buffer *b;

		if (mix->n_buffers == 0)
			goto done;

		pw_log_trace(NAME" %p: port %p %d get buffer %d n_buffers:%d",
				c, p, p->id, frames, mix->n_buffers);

		if ((b = dequeue_buffer(mix)) == NULL) {
			pw_log_warn("port %p: out of buffers", p);
			goto done;
		}
		reuse_buffer(c, mix, b->id);
		ptr = b->datas[0].data;

		b->datas[0].chunk->offset = 0;
		b->datas[0].chunk->size = frames * sizeof(float);
		b->datas[0].chunk->stride = stride;

		p->io.status = SPA_STATUS_HAVE_DATA;
		p->io.buffer_id = b->id;
	}
done:
	spa_list_for_each(mix, &p->mix, port_link) {
		struct spa_io_buffers *mio = mix->io;
		if (mio == NULL)
			continue;
		pw_log_trace(NAME" %p: port %p tee %d.%d get buffer %d io:%p",
				c, p, p->id, mix->id, frames, mio);
		*mio = p->io;
	}
	return ptr;
}

static void process_tee(struct client *c)
{
	struct port *p;

	spa_list_for_each(p, &c->ports[SPA_DIRECTION_OUTPUT], link) {
		if (p->object->port.type_id != 1)
			continue;
		void *ptr = get_buffer_output(c, p, MAX_BUFFER_FRAMES, 1);
		if (ptr != NULL)
			convert_from_midi(p->emptyptr, ptr, MAX_BUFFER_FRAMES * sizeof(float));
	}
}

static inline void debug_position(struct client *c, jack_position_t *p)
{
	pw_log_trace("usecs:       %lu", p->usecs);
	pw_log_trace("frame_rate:  %u", p->frame_rate);
	pw_log_trace("frame:       %u", p->frame);
	pw_log_trace("valid:       %08x", p->valid);

	if (p->valid & JackPositionBBT) {
		pw_log_trace("BBT");
		pw_log_trace(" bar:              %u", p->bar);
		pw_log_trace(" beat:             %u", p->beat);
		pw_log_trace(" tick:             %u", p->tick);
		pw_log_trace(" bar_start_tick:   %f", p->bar_start_tick);
		pw_log_trace(" beats_per_bar:    %f", p->beats_per_bar);
		pw_log_trace(" beat_type:        %f", p->beat_type);
		pw_log_trace(" ticks_per_beat:   %f", p->ticks_per_beat);
		pw_log_trace(" beats_per_minute: %f", p->beats_per_minute);
	}
	if (p->valid & JackPositionTimecode) {
		pw_log_trace("Timecode:");
		pw_log_trace(" frame_time:       %f", p->frame_time);
		pw_log_trace(" next_time:        %f", p->next_time);
	}
	if (p->valid & JackBBTFrameOffset) {
		pw_log_trace("BBTFrameOffset:");
		pw_log_trace(" bbt_offset:       %u", p->bbt_offset);
	}
	if (p->valid & JackAudioVideoRatio) {
		pw_log_trace("AudioVideoRatio:");
		pw_log_trace(" audio_frames_per_video_frame: %f", p->audio_frames_per_video_frame);
	}
	if (p->valid & JackVideoFrameOffset) {
		pw_log_trace("JackVideoFrameOffset:");
		pw_log_trace(" video_offset:     %u", p->video_offset);
	}
}

static inline void jack_to_position(jack_position_t *s, struct pw_node_activation *a)
{
	struct spa_io_segment *d = &a->segment;

	if (s->valid & JackPositionBBT) {
		d->bar.flags = SPA_IO_SEGMENT_BAR_FLAG_VALID;
		if (s->valid & JackBBTFrameOffset)
			d->bar.offset = s->bbt_offset;
		else
			d->bar.offset = 0;
		d->bar.signature_num = s->beats_per_bar;
		d->bar.signature_denom = s->beat_type;
		d->bar.bpm = s->beats_per_minute;
		d->bar.beat = (s->bar - 1) * s->beats_per_bar + (s->beat - 1) +
			(s->tick / s->ticks_per_beat);
	}
}

static inline jack_transport_state_t position_to_jack(struct pw_node_activation *a, jack_position_t *d)
{
	struct spa_io_position *s = &a->position;
	jack_transport_state_t state;
	struct spa_io_segment *seg = &s->segments[0];
	uint64_t running;

	switch (s->state) {
	default:
	case SPA_IO_POSITION_STATE_STOPPED:
		state = JackTransportStopped;
		break;
	case SPA_IO_POSITION_STATE_STARTING:
		state = JackTransportStarting;
		break;
	case SPA_IO_POSITION_STATE_RUNNING:
		if (seg->flags & SPA_IO_SEGMENT_FLAG_LOOPING)
			state = JackTransportLooping;
		else
			state = JackTransportRolling;
		break;
	}
	if (d == NULL)
		return state;


	d->unique_1++;
	d->usecs = s->clock.nsec / SPA_NSEC_PER_USEC;
	d->frame_rate = s->clock.rate.denom;

	running = s->clock.position - s->offset;

	if (running >= seg->start &&
	    (seg->duration == 0 || running < seg->start + seg->duration))
		d->frame = (running - seg->start) * seg->rate + seg->position;
	else
		d->frame = seg->position;

	d->valid = 0;
	if (a->segment_owner[0] && SPA_FLAG_IS_SET(seg->bar.flags, SPA_IO_SEGMENT_BAR_FLAG_VALID)) {
		double abs_beat;
		long beats;

		d->valid |= JackPositionBBT;

		d->bbt_offset = seg->bar.offset;
		if (seg->bar.offset)
			d->valid |= JackBBTFrameOffset;

		d->beats_per_bar = seg->bar.signature_num;
		d->beat_type = seg->bar.signature_denom;
		d->ticks_per_beat = 1920.0f;
		d->beats_per_minute = seg->bar.bpm;

		abs_beat = seg->bar.beat;

		d->bar = abs_beat / d->beats_per_bar;
		beats = d->bar * d->beats_per_bar;
		d->bar_start_tick = beats * d->ticks_per_beat;
		d->beat = abs_beat - beats;
		beats += d->beat;
		d->tick = (abs_beat - beats) * d->ticks_per_beat;
		d->bar++;
		d->beat++;
	}
	d->unique_2 = d->unique_1;
	return state;
}

static inline uint32_t cycle_run(struct client *c)
{
	uint64_t cmd, nsec;
	int fd = c->socket_source->fd;
	uint32_t buffer_frames, sample_rate;
	struct spa_io_position *pos = c->position;
	struct pw_node_activation *activation = c->activation;
	struct pw_node_activation *driver = c->driver_activation;

	/* this is blocking if nothing ready */
	if (read(fd, &cmd, sizeof(cmd)) != sizeof(cmd)) {
		pw_log_warn(NAME" %p: read failed %m", c);
		if (errno == EWOULDBLOCK)
			return 0;
	}
	if (cmd > 1)
		pw_log_warn(NAME" %p: missed %"PRIu64" wakeups", c, cmd - 1);

	if (pos == NULL) {
		pw_log_error(NAME" %p: missing position", c);
		return 0;
	}

	nsec = pos->clock.nsec;
	activation->status = PW_NODE_ACTIVATION_AWAKE;
	activation->awake_time = nsec;
	if (c->first) {
		if (c->thread_init_callback)
			c->thread_init_callback(c->thread_init_arg);
		c->first = false;
	}

	buffer_frames = pos->clock.duration;
	if (buffer_frames != c->buffer_frames) {
		pw_log_info(NAME" %p: bufferframes %d", c, buffer_frames);
		c->buffer_frames = buffer_frames;
		if (c->bufsize_callback)
			c->bufsize_callback(c->buffer_frames, c->bufsize_arg);
	}

	sample_rate = pos->clock.rate.denom;
	if (sample_rate != c->sample_rate) {
		pw_log_info(NAME" %p: sample_rate %d", c, sample_rate);
		c->sample_rate = sample_rate;
		if (c->srate_callback)
			c->srate_callback(c->sample_rate, c->srate_arg);
	}

	c->jack_state = position_to_jack(driver, &c->jack_position);

	if (driver) {
		if (activation->pending_sync) {
			if (c->sync_callback == NULL ||
			    c->sync_callback(c->jack_state, &c->jack_position, c->sync_arg))
				activation->pending_sync = false;
		}
		if (c->xrun_count != driver->xrun_count &&
		    c->xrun_count != 0 && c->xrun_callback)
			c->xrun_callback(c->xrun_arg);
		c->xrun_count = driver->xrun_count;
	}
	pw_log_trace(NAME" %p: wait %"PRIu64" frames:%d rate:%d pos:%d delay:%"PRIi64" corr:%f", c,
			activation->awake_time, c->buffer_frames, c->sample_rate,
			c->jack_position.frame, pos->clock.delay, pos->clock.rate_diff);

	return buffer_frames;
}

static inline uint32_t cycle_wait(struct client *c)
{
	int res;

	res = pw_data_loop_wait(c->loop, -1);
	if (res <= 0) {
		pw_log_warn(NAME" %p: wait error %m", c);
		return 0;
	}
	return cycle_run(c);
}

static inline void signal_sync(struct client *c)
{
	struct timespec ts;
	uint64_t cmd, nsec;
	struct link *l;
	struct pw_node_activation *activation = c->activation;

	process_tee(c);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	nsec = SPA_TIMESPEC_TO_NSEC(&ts);
	activation->status = PW_NODE_ACTIVATION_FINISHED;
	activation->finish_time = nsec;

	cmd = 1;
	pw_array_for_each(l, &c->links) {
		struct pw_node_activation_state *state;

		if (l->activation == NULL)
			continue;

		state = &l->activation->state[0];

		pw_log_trace(NAME" %p: link %p %p %d/%d", c, l, state,
				state->pending, state->required);

		if (pw_node_activation_state_dec(state, 1)) {
			l->activation->status = PW_NODE_ACTIVATION_TRIGGERED;
			l->activation->signal_time = nsec;

			pw_log_trace(NAME" %p: signal %p %p", c, l, state);

			if (write(l->signalfd, &cmd, sizeof(cmd)) != sizeof(cmd))
				pw_log_warn(NAME" %p: write failed %m", c);
		}
	}
}

static inline void cycle_signal(struct client *c, int status)
{
	struct pw_node_activation *driver = c->driver_activation;
	struct pw_node_activation *activation = c->activation;

	if (status == 0) {
		if (c->timebase_callback && driver && driver->segment_owner[0] == c->node_id) {
			if (activation->pending_new_pos ||
			    c->jack_state == JackTransportRolling ||
			    c->jack_state == JackTransportLooping) {
				c->timebase_callback(c->jack_state,
						     c->buffer_frames,
						     &c->jack_position,
						     activation->pending_new_pos,
						     c->timebase_arg);

				activation->pending_new_pos = false;

				debug_position(c, &c->jack_position);
				jack_to_position(&c->jack_position, activation);
			}
		}
	}
	signal_sync(c);
}

static void
on_rtsocket_condition(void *data, int fd, uint32_t mask)
{
	struct client *c = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_warn(NAME" %p: got error", c);
		unhandle_socket(c);
		return;
	}
	if (c->thread_callback) {
		if (!c->thread_entered) {
			c->thread_entered = true;
			c->thread_callback(c->thread_arg);
		}
		return;
	} else if (mask & SPA_IO_IN) {
		uint32_t buffer_frames;
		int status;

		buffer_frames = cycle_run(c);

		status = c->process_callback ? c->process_callback(buffer_frames, c->process_arg) : 0;

		cycle_signal(c, status);
	}
}

static void clear_link(struct client *c, struct link *link)
{
	link->node_id = SPA_ID_INVALID;
	link->activation = NULL;
	pw_memmap_free(link->mem);
	close(link->signalfd);
}

static void clean_transport(struct client *c)
{
	struct link *l;

	if (c->node_id == SPA_ID_INVALID)
		return;

	pw_data_loop_stop(c->loop);

	unhandle_socket(c);

	pw_array_for_each(l, &c->links)
		if (l->node_id != SPA_ID_INVALID)
			clear_link(c, l);
	pw_array_clear(&c->links);

	c->node_id = SPA_ID_INVALID;
}

static int client_node_transport(void *object,
                           uint32_t node_id,
                           int readfd, int writefd,
			   uint32_t mem_id, uint32_t offset, uint32_t size)
{
	struct client *c = (struct client *) object;

	clean_transport(c);

	c->node_id = node_id;

	c->mem = pw_mempool_map_id(c->remote->pool, mem_id,
				PW_MEMMAP_FLAG_READWRITE, offset, size, NULL);
	if (c->mem == NULL) {
		pw_log_debug(NAME" %p: can't map activation: %m", c);
		return -errno;
	}
	c->activation = c->mem->ptr;

	pw_log_debug(NAME" %p: create client transport with fds %d %d for node %u",
			c, readfd, writefd, node_id);

	close(writefd);
	c->socket_source = pw_loop_add_io(c->loop->loop,
					  readfd,
					  SPA_IO_ERR | SPA_IO_HUP,
					  true, on_rtsocket_condition, c);
	return 0;
}

static int client_node_set_param(void *object,
			uint32_t id, uint32_t flags,
			const struct spa_pod *param)
{
	struct client *c = (struct client *) object;
	pw_proxy_error((struct pw_proxy*)c->node_proxy, -ENOTSUP, "not supported");
	return -ENOTSUP;
}

static int update_driver_activation(struct client *c)
{
	struct link *link;
	pw_log_debug(NAME" %p: driver %d", c, c->driver_id);

	link = find_activation(&c->links, c->driver_id);
	c->driver_activation = link ? link->activation : NULL;
	return 0;
}

static int client_node_set_io(void *object,
			uint32_t id,
			uint32_t mem_id,
			uint32_t offset,
			uint32_t size)
{
	struct client *c = (struct client *) object;
        struct pw_memmap *mm;
        void *ptr;
	uint32_t tag[5] = { c->node_id, id, };

	if ((mm = pw_mempool_find_tag(c->remote->pool, tag, sizeof(tag))) != NULL)
		pw_memmap_free(mm);

        if (mem_id == SPA_ID_INVALID) {
		mm = ptr = NULL;
		size = 0;
        }
        else {
		mm = pw_mempool_map_id(c->remote->pool, mem_id,
				PW_MEMMAP_FLAG_READWRITE, offset, size, tag);
                if (mm == NULL) {
                        pw_log_warn(NAME" %p: can't map memory id %u", c, mem_id);
			return -errno;
                }
		ptr = mm->ptr;
        }
	pw_log_debug(NAME" %p: set io %s %p", c,
			spa_debug_type_find_name(spa_type_io, id), ptr);

	switch (id) {
	case SPA_IO_Position:
		c->position = ptr;
		c->driver_id = ptr ? c->position->clock.id : SPA_ID_INVALID;
		update_driver_activation(c);
		break;
	default:
		break;
	}

	return 0;
}

static int client_node_event(void *object, const struct spa_event *event)
{
	return -ENOTSUP;
}

static int client_node_command(void *object, const struct spa_command *command)
{
	struct client *c = (struct client *) object;

	pw_log_debug(NAME" %p: got command %d", c, SPA_COMMAND_TYPE(command));

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Suspend:
	case SPA_NODE_COMMAND_Pause:
		if (c->started) {
			pw_loop_update_io(c->loop->loop,
					  c->socket_source, SPA_IO_ERR | SPA_IO_HUP);

			c->started = false;
		}
		break;

	case SPA_NODE_COMMAND_Start:
		if (!c->started) {
			pw_loop_update_io(c->loop->loop,
					  c->socket_source,
					  SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
			c->started = true;
			c->first = true;
			c->thread_entered = false;
		}
		break;
	default:
		pw_log_warn(NAME" %p: unhandled node command %d", c, SPA_COMMAND_TYPE(command));
		pw_proxy_error((struct pw_proxy*)c->node_proxy, -ENOTSUP,
				"unhandled command %d", SPA_COMMAND_TYPE(command));
	}
	return 0;
}

static int client_node_add_port(void *object,
                          enum spa_direction direction,
                          uint32_t port_id, const struct spa_dict *props)
{
	struct client *c = (struct client *) object;
	pw_proxy_error((struct pw_proxy*)c->node_proxy, -ENOTSUP, "add port not supported");
	return -ENOTSUP;
}

static int client_node_remove_port(void *object,
                             enum spa_direction direction,
                             uint32_t port_id)
{
	struct client *c = (struct client *) object;
	pw_proxy_error((struct pw_proxy*)c->node_proxy, -ENOTSUP, "remove port not supported");
	return -ENOTSUP;
}

static int clear_buffers(struct client *c, struct mix *mix)
{
	struct port *port = mix->port;
        struct buffer *b;
	uint32_t i, j;

        pw_log_debug(NAME" %p: port %p clear buffers", c, port);

	for (i = 0; i < mix->n_buffers; i++) {
		b = &mix->buffers[i];

		for (j = 0; j < b->n_mem; j++)
			pw_memmap_free(b->mem[j]);

		b->n_mem = 0;
        }
	mix->n_buffers = 0;
	spa_list_init(&mix->queue);
	return 0;
}

static int param_enum_format(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (p->object->port.type_id) {
	case 0:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
	                SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_F32P),
	                SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(DEFAULT_SAMPLE_RATE, 1, INT32_MAX),
	                SPA_FORMAT_AUDIO_channels, SPA_POD_Int(1));
		break;
	case 1:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		break;
	case 2:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
	                SPA_FORMAT_VIDEO_format,   SPA_POD_Id(SPA_VIDEO_FORMAT_RGBA_F32),
			SPA_FORMAT_VIDEO_size,     SPA_POD_CHOICE_RANGE_Rectangle(
                                                        &SPA_RECTANGLE(320, 240),
                                                        &SPA_RECTANGLE(1,1),
                                                        &SPA_RECTANGLE(INT32_MAX, INT32_MAX)),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                                                        &SPA_FRACTION(25,1),
                                                        &SPA_FRACTION(0,1),
                                                        &SPA_FRACTION(INT32_MAX,1)));
		break;
	default:
		return -EINVAL;
	}
	return 1;
}

static int param_format(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	uint32_t channels[] = { SPA_AUDIO_CHANNEL_MONO };
	struct spa_pod_frame f;
	switch (p->object->port.type_id) {
	case 0:
		spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
		spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
	                SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_F32P), NULL);
		if (p->have_format) {
			spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_rate,     SPA_POD_Int(p->rate), NULL);
		} else {
			spa_pod_builder_add(b,
				SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(DEFAULT_SAMPLE_RATE,
								1, INT32_MAX), NULL);
		}
		spa_pod_builder_add(b,
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(1),
	                SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id, 1, channels), NULL);
		*param = spa_pod_builder_pop(b, &f);
		break;
	case 1:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_control));
		break;
	case 2:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
	                SPA_FORMAT_VIDEO_format,   SPA_POD_Id(SPA_VIDEO_FORMAT_RGBA_F32),
			SPA_FORMAT_VIDEO_size,     SPA_POD_CHOICE_RANGE_Rectangle(
                                                        &SPA_RECTANGLE(320, 240),
                                                        &SPA_RECTANGLE(1,1),
                                                        &SPA_RECTANGLE(INT32_MAX, INT32_MAX)),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                                                        &SPA_FRACTION(25,1),
                                                        &SPA_FRACTION(0,1),
                                                        &SPA_FRACTION(INT32_MAX,1)));
		break;
	default:
		return -EINVAL;
	}
	return 1;
}

static int param_buffers(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	switch (p->object->port.type_id) {
	case 0:
	case 1:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_STEP_Int(
								MAX_BUFFER_FRAMES * sizeof(float),
								sizeof(float),
								MAX_BUFFER_FRAMES * sizeof(float),
								sizeof(float)),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(4),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
		break;
	case 2:
		*param = spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(
								320 * 240 * 4 * 4,
								0,
								INT32_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_CHOICE_RANGE_Int(4, 4, INT32_MAX),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
		break;
	default:
		return -EINVAL;
	}
	return 1;
}

static int param_io(struct client *c, struct port *p,
		struct spa_pod **param, struct spa_pod_builder *b)
{
	*param = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
		SPA_PARAM_IO_id,	SPA_POD_Id(SPA_IO_Buffers),
		SPA_PARAM_IO_size,	SPA_POD_Int(sizeof(struct spa_io_buffers)));
	return 1;
}

static int port_set_format(struct client *c, struct port *p,
		uint32_t flags, const struct spa_pod *param)
{
	if (param == NULL) {
		struct mix *mix;

		pw_log_debug(NAME" %p: port %p clear format", c, p);

		spa_list_for_each(mix, &p->mix, port_link)
			clear_buffers(c, mix);
		p->have_format = false;
	}
	else {
		struct spa_audio_info info = { 0 };
		spa_format_parse(param, &info.media_type, &info.media_subtype);

		switch (info.media_type) {
		case SPA_MEDIA_TYPE_audio:
		{
			if (info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
				return -EINVAL;

			if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
				return -EINVAL;

			p->rate = info.info.raw.rate;
			break;
		}
		case SPA_MEDIA_TYPE_application:
			if (info.media_subtype != SPA_MEDIA_SUBTYPE_control)
				return -EINVAL;
			break;
		case SPA_MEDIA_TYPE_video:
		{
			struct spa_video_info vinfo = { 0 };

			if (info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
				return -EINVAL;
			if (spa_format_video_raw_parse(param, &vinfo.info.raw) < 0)
				return -EINVAL;
			break;
		}
		default:
			return -EINVAL;
		}
		p->have_format = true;
	}
	return 0;
}

static int client_node_port_set_param(void *object,
                                enum spa_direction direction,
                                uint32_t port_id,
                                uint32_t id, uint32_t flags,
                                const struct spa_pod *param)
{
	struct client *c = (struct client *) object;
	struct port *p = GET_PORT(c, direction, port_id);
	struct spa_pod *params[4];
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	pw_log_debug("port %p: %d.%d id:%d %p", p, direction, port_id, id, param);

        if (id == SPA_PARAM_Format) {
		port_set_format(c, p, flags, param);
	}

	param_enum_format(c, p, &params[0], &b);
	param_format(c, p, &params[1], &b);
	param_buffers(c, p, &params[2], &b);
	param_io(c, p, &params[3], &b);

	return pw_client_node_proxy_port_update(c->node_proxy,
					 direction,
					 port_id,
					 PW_CLIENT_NODE_PORT_UPDATE_PARAMS,
					 4,
					 (const struct spa_pod **) params,
					 NULL);
}

static void init_buffer(struct port *p, void *data, size_t maxframes)
{
	if (p->object->port.type_id == 1) {
		struct midi_buffer *mb = data;
		mb->magic = MIDI_BUFFER_MAGIC;
		mb->buffer_size = MAX_BUFFER_FRAMES * sizeof(float);
		mb->nframes = maxframes;
		mb->write_pos = 0;
		mb->event_count = 0;
		mb->lost_events = 0;
		pw_log_debug("port %p: init midi buffer %p size:%d", p, data, mb->buffer_size);
	}
	else
		memset(data, 0, maxframes * sizeof(float));
}

static int client_node_port_use_buffers(void *object,
                                  enum spa_direction direction,
                                  uint32_t port_id,
                                  uint32_t mix_id,
                                  uint32_t flags,
                                  uint32_t n_buffers,
                                  struct pw_client_node_buffer *buffers)
{
	struct client *c = (struct client *) object;
	struct port *p = GET_PORT(c, direction, port_id);
	struct buffer *b;
	uint32_t i, j, fl, res;
	struct mix *mix;

	if (!p->valid) {
		res = -EINVAL;
		goto done;
	}

	if ((mix = ensure_mix(c, p, mix_id)) == NULL) {
		res = -ENOMEM;
		goto done;
	}

	pw_log_debug(NAME" %p: port %p %d %d.%d use_buffers %d", c, p, direction,
			port_id, mix_id, n_buffers);

	if (p->object->port.type_id == 2 && direction == SPA_DIRECTION_INPUT) {
		fl = PW_MEMMAP_FLAG_READ;
	} else {
		/* some apps write to the input buffer so we want everything readwrite */
		fl = PW_MEMMAP_FLAG_READWRITE;
	}

	/* clear previous buffers */
	clear_buffers(c, mix);

	for (i = 0; i < n_buffers; i++) {
		off_t offset;
		struct spa_buffer *buf;
		struct pw_memmap *mm;

		mm = pw_mempool_map_id(c->remote->pool, buffers[i].mem_id,
				fl, buffers[i].offset, buffers[i].size, NULL);
		if (mm == NULL) {
			pw_log_warn(NAME" %p: can't map memory id %u: %m", c, buffers[i].mem_id);
			continue;
		}

		buf = buffers[i].buffer;

		b = &mix->buffers[i];
		b->id = i;
		b->flags = 0;
		b->n_mem = 0;
		b->mem[b->n_mem++] = mm;

		pw_log_debug(NAME" %p: add buffer id:%u offset:%u size:%u map:%p ptr:%p",
				c, buffers[i].mem_id, buffers[i].offset,
				buffers[i].size, mm, mm->ptr);

		offset = 0;
		for (j = 0; j < buf->n_metas; j++) {
			struct spa_meta *m = &buf->metas[j];
			offset += SPA_ROUND_UP_N(m->size, 8);
		}

		b->n_datas = SPA_MIN(buf->n_datas, MAX_BUFFER_DATAS);

		for (j = 0; j < b->n_datas; j++) {
			struct spa_data *d = &b->datas[j];

			memcpy(d, &buf->datas[j], sizeof(struct spa_data));
			d->chunk =
			    SPA_MEMBER(mm->ptr, offset + sizeof(struct spa_chunk) * j,
				       struct spa_chunk);

			if (d->type == SPA_DATA_MemId) {
				uint32_t mem_id = SPA_PTR_TO_UINT32(d->data);
				struct pw_memblock *bm;
				struct pw_memmap *bmm;

				bm = pw_mempool_find_id(c->remote->pool, mem_id);
				if (bm == NULL) {
					pw_log_error(NAME" %p: unknown buffer mem %u", c, mem_id);
					res = -ENODEV;
					goto done;

				}

				d->fd = bm->fd;
				d->type = bm->type;
				d->data = NULL;

				bmm = pw_memblock_map(bm, fl, d->mapoffset, d->maxsize, NULL);
				if (bmm == NULL) {
					res = -errno;
					pw_log_error(NAME" %p: failed to map buffer mem %m", c);
					d->data = NULL;
					goto done;
				}
				b->mem[b->n_mem++] = bmm;
				d->data = bmm->ptr;

				pw_log_debug(NAME" %p: data %d %u -> fd %d %d",
						c, j, bm->id, bm->fd, d->maxsize);
			} else if (d->type == SPA_DATA_MemPtr) {
				int offs = SPA_PTR_TO_INT(d->data);
				d->data = SPA_MEMBER(mm->ptr, offs, void);
				d->fd = -1;
				pw_log_debug(NAME" %p: data %d %u -> mem %p %d",
						c, j, b->id, d->data, d->maxsize);
			} else {
				pw_log_warn("unknown buffer data type %d", d->type);
			}
			if (mlock(d->data, d->maxsize) < 0)
				pw_log_warn(NAME" %p: Failed to mlock memory %p %u: %m", c,
						d->data, d->maxsize);
		}

		init_buffer(p, p->emptyptr, MAX_BUFFER_FRAMES);
		p->zeroed = true;

		SPA_FLAG_SET(b->flags, BUFFER_FLAG_OUT);
		if (direction == SPA_DIRECTION_OUTPUT)
			reuse_buffer(c, mix, b->id);

	}
	pw_log_debug(NAME" %p: have %d buffers", c, n_buffers);
	mix->n_buffers = n_buffers;
	res = 0;

      done:
	if (res < 0)
		pw_proxy_error((struct pw_proxy*)c->node_proxy, res, spa_strerror(res));
	return res;
}

static int client_node_port_set_io(void *object,
                             enum spa_direction direction,
                             uint32_t port_id,
                             uint32_t mix_id,
                             uint32_t id,
                             uint32_t mem_id,
                             uint32_t offset,
                             uint32_t size)
{
	struct client *c = (struct client *) object;
	struct port *p = GET_PORT(c, direction, port_id);
        struct pw_memmap *mm;
        struct mix *mix;
	uint32_t tag[5] = { c->node_id, direction, port_id, mix_id, id };
        void *ptr;
	int res = 0;

	if ((mix = ensure_mix(c, p, mix_id)) == NULL) {
		res = -ENOMEM;
		goto exit;
	}

	if ((mm = pw_mempool_find_tag(c->remote->pool, tag, sizeof(tag))) != NULL)
		pw_memmap_free(mm);

        if (mem_id == SPA_ID_INVALID) {
                mm = ptr = NULL;
                size = 0;
        }
        else {
		mm = pw_mempool_map_id(c->remote->pool, mem_id,
				PW_MEMMAP_FLAG_READWRITE, offset, size, tag);
                if (mm == NULL) {
                        pw_log_warn(NAME" %p: can't map memory id %u", c, mem_id);
			res = -EINVAL;
                        goto exit;
                }
		ptr = mm->ptr;
        }

	pw_log_debug(NAME" %p: port %p mix:%d set io:%s id:%u ptr:%p", c, p, mix_id,
			spa_debug_type_find_name(spa_type_io, id), id, ptr);

	switch (id) {
	case SPA_IO_Buffers:
                mix->io = ptr;
		break;
	default:
		break;
	}

      exit:
	if (res < 0)
		pw_proxy_error((struct pw_proxy*)c->node_proxy, res, spa_strerror(res));
	return res;
}

static int client_node_set_activation(void *object,
                             uint32_t node_id,
                             int signalfd,
                             uint32_t mem_id,
                             uint32_t offset,
                             uint32_t size)
{
	struct client *c = (struct client *) object;
	struct pw_memmap *mm;
	struct link *link;
	void *ptr;
	int res = 0;

	if (c->node_id == node_id) {
		pw_log_debug(NAME" %p: our activation %u: %u %u %u", c, node_id,
				mem_id, offset, size);
		close(signalfd);
		return 0;
	}

	if (mem_id == SPA_ID_INVALID) {
		mm = ptr = NULL;
		size = 0;
	}
	else {
		mm = pw_mempool_map_id(c->remote->pool, mem_id,
				PW_MEMMAP_FLAG_READWRITE, offset, size, NULL);
		if (mm == NULL) {
			pw_log_warn(NAME" %p: can't map memory id %u", c, mem_id);
			res = -EINVAL;
			goto exit;
		}
		ptr = mm->ptr;
	}

	pw_log_debug(NAME" %p: set activation %u: %u %u %u %p", c, node_id,
			mem_id, offset, size, ptr);

	if (ptr) {
		link = pw_array_add(&c->links, sizeof(struct link));
		if (link == NULL) {
			res = -errno;
			goto exit;
		}
		link->node_id = node_id;
		link->mem = mm;
		link->activation = ptr;
		link->signalfd = signalfd;
	}
	else {
		link = find_activation(&c->links, node_id);
		if (link == NULL) {
			res = -EINVAL;
			goto exit;
		}
		clear_link(c, link);
	}

	if (c->driver_id == node_id)
		update_driver_activation(c);

      exit:
	if (res < 0)
		pw_proxy_error((struct pw_proxy*)c->node_proxy, res, spa_strerror(res));
	return res;
}

static const struct pw_client_node_proxy_events client_node_events = {
	PW_VERSION_CLIENT_NODE_PROXY_EVENTS,
	.transport = client_node_transport,
	.set_param = client_node_set_param,
	.set_io = client_node_set_io,
	.event = client_node_event,
	.command = client_node_command,
	.add_port = client_node_add_port,
	.remove_port = client_node_remove_port,
	.port_set_param = client_node_port_set_param,
	.port_use_buffers = client_node_port_use_buffers,
	.port_set_io = client_node_port_set_io,
	.set_activation = client_node_set_activation,
};

static jack_port_type_id_t string_to_type(const char *port_type)
{
	if (!strcmp(JACK_DEFAULT_AUDIO_TYPE, port_type))
		return 0;
	else if (!strcmp(JACK_DEFAULT_MIDI_TYPE, port_type))
		return 1;
	else if (!strcmp(JACK_DEFAULT_VIDEO_TYPE, port_type))
		return 2;
	else if (!strcmp("other", port_type))
		return 3;
	else
		return SPA_ID_INVALID;
}

static const char* type_to_string(jack_port_type_id_t type_id)
{
	switch(type_id) {
	case 0:
		return JACK_DEFAULT_AUDIO_TYPE;
	case 1:
		return JACK_DEFAULT_MIDI_TYPE;
	case 2:
		return JACK_DEFAULT_VIDEO_TYPE;
	case 3:
		return "other";
	default:
		return NULL;
	}
}

static void registry_event_global(void *data, uint32_t id,
                                  uint32_t permissions, uint32_t type, uint32_t version,
                                  const struct spa_dict *props)
{
	struct client *c = (struct client *) data;
	struct object *o, *ot;
	const char *str;
	size_t size;

	if (props == NULL)
		return;

	switch (type) {
	case PW_TYPE_INTERFACE_Node:
		o = alloc_object(c);

		if ((str = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION)) == NULL &&
		    (str = spa_dict_lookup(props, PW_KEY_NODE_NICK)) == NULL &&
		    (str = spa_dict_lookup(props, PW_KEY_NODE_NAME)) == NULL) {
			str = "node";
		}
		snprintf(o->node.name, sizeof(o->node.name), "%s/%d", str, id);

		if ((str = spa_dict_lookup(props, PW_KEY_PRIORITY_MASTER)) != NULL)
			o->node.priority = pw_properties_parse_int(str);

		pw_log_debug(NAME" %p: add node %d", c, id);
		spa_list_append(&c->context.nodes, &o->link);
		break;

	case PW_TYPE_INTERFACE_Port:
	{
		const struct spa_dict_item *item;
		unsigned long flags = 0;
		jack_port_type_id_t type_id;
		uint32_t node_id;
		char full_name[1024];

		if ((str = spa_dict_lookup(props, PW_KEY_FORMAT_DSP)) == NULL)
			str = "other";
		if ((type_id = string_to_type(str)) == SPA_ID_INVALID)
			goto exit;

		if ((str = spa_dict_lookup(props, PW_KEY_NODE_ID)) == NULL)
			goto exit;

		node_id = atoi(str);

		if ((str = spa_dict_lookup(props, PW_KEY_PORT_NAME)) == NULL)
			goto exit;

		spa_dict_for_each(item, props) {
	                if (!strcmp(item->key, PW_KEY_PORT_DIRECTION)) {
				if (strcmp(item->value, "in") == 0)
					flags |= JackPortIsInput;
				else if (strcmp(item->value, "out") == 0)
					flags |= JackPortIsOutput;
			}
			else if (!strcmp(item->key, PW_KEY_PORT_PHYSICAL)) {
				if (pw_properties_parse_bool(item->value))
					flags |= JackPortIsPhysical;
			}
			else if (!strcmp(item->key, PW_KEY_PORT_TERMINAL)) {
				if (pw_properties_parse_bool(item->value))
					flags |= JackPortIsTerminal;
			}
			else if (!strcmp(item->key, PW_KEY_PORT_CONTROL)) {
				if (pw_properties_parse_bool(item->value))
					type_id = 1;
			}
		}

		o = NULL;
		if (node_id == c->node_id) {
			snprintf(full_name, sizeof(full_name), "%s:%s", c->name, str);
			o = find_port(c, full_name);
			if (o != NULL)
				pw_log_debug(NAME" %p: %s found our port %p", c, full_name, o);
		}
		if (o == NULL) {
			o = alloc_object(c);
			if (o == NULL)
				goto exit;

			spa_list_append(&c->context.ports, &o->link);
			ot = pw_map_lookup(&c->context.globals, node_id);
			if (ot == NULL || ot->type != PW_TYPE_INTERFACE_Node)
				goto exit_free;

			snprintf(o->port.name, sizeof(o->port.name), "%s:%s", ot->node.name, str);
			o->port.port_id = SPA_ID_INVALID;
			o->port.priority = ot->node.priority;
		}

		if ((str = spa_dict_lookup(props, PW_KEY_OBJECT_PATH)) != NULL)
			snprintf(o->port.alias1, sizeof(o->port.alias1), "%s", str);
		else
			o->port.alias1[0] = '\0';

		if ((str = spa_dict_lookup(props, PW_KEY_PORT_ALIAS)) != NULL)
			snprintf(o->port.alias2, sizeof(o->port.alias2), "%s", str);
		else
			o->port.alias2[0] = '\0';

		o->port.flags = flags;
		o->port.type_id = type_id;
		o->port.node_id = node_id;

		if (o->port.flags & JackPortIsOutput) {
			o->port.capture_latency.min = 1024;
			o->port.capture_latency.max = 1024;
		} else {
			o->port.playback_latency.min = 1024;
			o->port.playback_latency.max = 1024;
		}

		pw_log_debug(NAME" %p: add port %d %s %d", c, id, o->port.name, type_id);
		break;
	}
	case PW_TYPE_INTERFACE_Link:
		o = alloc_object(c);
		spa_list_append(&c->context.links, &o->link);

		if ((str = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT)) == NULL)
			goto exit_free;
		o->port_link.src = pw_properties_parse_int(str);

		if ((str = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT)) == NULL)
			goto exit_free;
		o->port_link.dst = pw_properties_parse_int(str);

		pw_log_debug(NAME" %p: add link %d %d->%d", c, id,
				o->port_link.src, o->port_link.dst);
		break;

	default:
		goto exit;
	}

	o->type = type;
	o->id = id;

        size = pw_map_get_size(&c->context.globals);
        while (id > size)
		pw_map_insert_at(&c->context.globals, size++, NULL);
	pw_map_insert_at(&c->context.globals, id, o);

	pw_thread_loop_unlock(c->context.loop);

	switch (type) {
	case PW_TYPE_INTERFACE_Node:
		if (c->registration_callback)
			c->registration_callback(o->node.name, 1, c->registration_arg);
		break;

	case PW_TYPE_INTERFACE_Port:
		if (c->portregistration_callback)
			c->portregistration_callback(o->id, 1, c->portregistration_arg);
		break;

	case PW_TYPE_INTERFACE_Link:
		if (c->connect_callback)
			c->connect_callback(o->port_link.src, o->port_link.dst, 1, c->connect_arg);
		break;
	}
	pw_thread_loop_lock(c->context.loop);

      exit:
	return;
      exit_free:
	free_object(c, o);
	return;
}

static void registry_event_global_remove(void *object, uint32_t id)
{
	struct client *c = (struct client *) object;
	struct object *o;

	pw_log_debug(NAME" %p: removed: %u", c, id);

	o = pw_map_lookup(&c->context.globals, id);
	if (o == NULL)
		return;

	pw_thread_loop_unlock(c->context.loop);

	switch (o->type) {
	case PW_TYPE_INTERFACE_Node:
		if (c->registration_callback)
			c->registration_callback(o->node.name, 0, c->registration_arg);
		break;
	case PW_TYPE_INTERFACE_Port:
		if (c->portregistration_callback)
			c->portregistration_callback(o->id, 0, c->portregistration_arg);
		break;
	case PW_TYPE_INTERFACE_Link:
		if (c->connect_callback)
			c->connect_callback(o->port_link.src, o->port_link.dst, 0, c->connect_arg);
		break;
	}
	pw_thread_loop_lock(c->context.loop);

	/* JACK clients expect the objects to hang around after
	 * they are unregistered. We keep them in the map but reuse the
	 * object when we can
	 * pw_map_insert_at(&c->context.globals, id, NULL);
	 **/
	free_object(c, o);
	return;
}

static const struct pw_registry_proxy_events registry_events = {
        PW_VERSION_REGISTRY_PROXY_EVENTS,
        .global = registry_event_global,
        .global_remove = registry_event_global_remove,
};

SPA_EXPORT
jack_client_t * jack_client_open (const char *client_name,
                                  jack_options_t options,
                                  jack_status_t *status, ...)
{
	struct client *client;
	bool busy = true;
	struct spa_dict props;
	struct spa_dict_item items[6];
	const struct spa_support *support;
	uint32_t n_support;
	const char *str;
	struct spa_cpu *cpu_iface;
	struct spa_node_info ni;
	int i;

        if (getenv("PIPEWIRE_NOJACK") != NULL)
		goto disabled;

	client = calloc(1, sizeof(struct client));
	if (client == NULL)
		goto init_failed;

	pw_log_debug(NAME" %p: open '%s' options:%d", client, client_name, options);

	client->node_id = SPA_ID_INVALID;
	strncpy(client->name, client_name, JACK_CLIENT_NAME_SIZE);
	client->context.main = pw_main_loop_new(NULL);
	client->context.loop = pw_thread_loop_new(pw_main_loop_get_loop(client->context.main), client_name);
        client->context.core = pw_core_new(pw_thread_loop_get_loop(client->context.loop), NULL, 0);
	spa_list_init(&client->context.free_objects);
	spa_list_init(&client->context.nodes);
	spa_list_init(&client->context.ports);
	spa_list_init(&client->context.links);

	support = pw_core_get_support(client->context.core, &n_support);

	mix2 = mix2_c;
	cpu_iface = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_CPU);
	if (cpu_iface) {
#if defined (__SSE__)
		uint32_t flags = spa_cpu_get_flags(cpu_iface);
		if (flags & SPA_CPU_FLAG_SSE)
			mix2 = mix2_sse;
#endif
	}

	client->loop = pw_data_loop_new(NULL);
	if (client->loop == NULL)
		goto init_failed;

	pw_array_init(&client->links, 64);

	client->buffer_frames = (uint32_t)-1;
	client->sample_rate = (uint32_t)-1;

        spa_list_init(&client->free_mix);
	for (i = 0; i < MAX_MIX; i++)
		spa_list_append(&client->free_mix, &client->mix_pool[i].link);

	init_port_pool(client, SPA_DIRECTION_INPUT);
	init_port_pool(client, SPA_DIRECTION_OUTPUT);

	pw_map_init(&client->context.globals, 64, 64);

	pw_thread_loop_start(client->context.loop);

	pw_thread_loop_lock(client->context.loop);
        client->remote = pw_remote_new(client->context.core,
				pw_properties_new(
					PW_KEY_CLIENT_NAME, client_name,
					PW_KEY_CLIENT_API, "jack",
					NULL),
				0);

        pw_remote_add_listener(client->remote, &client->remote_listener, &remote_events, client);

        if (pw_remote_connect(client->remote) < 0)
		goto server_failed;

	while (busy) {
		const char *error = NULL;

		switch (pw_remote_get_state(client->remote, &error)) {
		case PW_REMOTE_STATE_ERROR:
			goto server_failed;

		case PW_REMOTE_STATE_CONNECTED:
			busy = false;
			break;

		default:
			break;
		}
		if (busy)
		        pw_thread_loop_wait(client->context.loop);

	}
	client->core_proxy = pw_remote_get_core_proxy(client->remote);
	pw_core_proxy_add_listener(client->core_proxy,
                                               &client->core_listener,
                                               &core_events, client);
	client->registry_proxy = pw_core_proxy_get_registry(client->core_proxy,
						PW_VERSION_REGISTRY_PROXY, 0);
	pw_registry_proxy_add_listener(client->registry_proxy,
                                               &client->registry_listener,
                                               &registry_events, client);


	props = SPA_DICT_INIT(items, 0);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_NAME, client_name);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_TYPE, "Audio");
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_CATEGORY, "Duplex");
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_MEDIA_ROLE, "DSP");
	if ((str = getenv("PIPEWIRE_LATENCY")) == NULL)
		str = DEFAULT_LATENCY;
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, str);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_ALWAYS_PROCESS, "1");

	client->node_proxy = pw_core_proxy_create_object(client->core_proxy,
				"client-node",
				PW_TYPE_INTERFACE_ClientNode,
				PW_VERSION_CLIENT_NODE,
				&props,
				0);
	if (client->node_proxy == NULL)
		goto init_failed;

	pw_client_node_proxy_add_listener(client->node_proxy,
			&client->node_listener, &client_node_events, client);
        pw_proxy_add_listener((struct pw_proxy*)client->node_proxy,
			&client->proxy_listener, &proxy_events, client);

	ni = SPA_NODE_INFO_INIT();
	ni.max_input_ports = MAX_PORTS;
	ni.max_output_ports = MAX_PORTS;
	ni.change_mask = SPA_NODE_CHANGE_MASK_FLAGS;
	ni.flags = SPA_NODE_FLAG_RT;

	pw_client_node_proxy_update(client->node_proxy,
                                    PW_CLIENT_NODE_UPDATE_INFO,
				    0, NULL, &ni);

	if (do_sync(client) < 0)
		goto init_failed;

	pw_thread_loop_unlock(client->context.loop);

	if (status)
		*status = 0;

	pw_log_trace(NAME" %p: new", client);
	return (jack_client_t *)client;

      init_failed:
	if (status)
		*status = JackFailure | JackInitFailure;
	goto exit;
      server_failed:
	if (status)
		*status = JackFailure | JackServerFailed;
	goto exit;
     exit:
	pw_thread_loop_unlock(client->context.loop);
	return NULL;
     disabled:
	if (status)
		*status = JackFailure | JackServerFailed;
	return NULL;
}

SPA_EXPORT
jack_client_t * jack_client_new (const char *client_name)
{
	jack_options_t options = JackUseExactName;
	jack_status_t status;

        if (getenv("JACK_START_SERVER") == NULL)
		options |= JackNoStartServer;

	return jack_client_open(client_name, options, &status, NULL);
}

SPA_EXPORT
int jack_client_close (jack_client_t *client)
{
	struct client *c = (struct client *) client;

	pw_log_debug(NAME" %p: close", client);

	pw_thread_loop_stop(c->context.loop);

	c->destroyed = true;
	pw_core_destroy(c->context.core);
	pw_thread_loop_destroy(c->context.loop);
	pw_main_loop_destroy(c->context.main);

	pw_log_debug(NAME" %p: free", client);
	free(c);

	return 0;
}

SPA_EXPORT
int jack_client_name_size (void)
{
	pw_log_trace("%d", JACK_CLIENT_NAME_SIZE);
	return JACK_CLIENT_NAME_SIZE;
}

SPA_EXPORT
char * jack_get_client_name (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	pw_log_trace(NAME" %p: %s", c, c->name);
	return c->name;
}

static jack_uuid_t cuuid = 0x2;

SPA_EXPORT
char *jack_get_uuid_for_client_name (jack_client_t *client,
                                     const char    *client_name)
{
	struct client *c = (struct client *) client;
	struct object *o;

	spa_list_for_each(o, &c->context.nodes, link) {
		if (strcmp(o->node.name, client_name) == 0) {
			char *uuid;
			asprintf(&uuid, "%" PRIu64, (cuuid << 32) | o->id);
			pw_log_debug(NAME" %p: name %s -> %s",
					client, client_name, uuid);
			return uuid;
		}
	}
	return NULL;
}

SPA_EXPORT
char *jack_get_client_name_by_uuid (jack_client_t *client,
                                    const char    *client_uuid )
{
	struct client *c = (struct client *) client;
	struct object *o;
	jack_uuid_t uuid;
	jack_uuid_t cuuid = 0x2;

	if (jack_uuid_parse(client_uuid, &uuid) < 0)
		return NULL;

	spa_list_for_each(o, &c->context.nodes, link) {
		if ((cuuid << 32 | o->id) == uuid) {
			pw_log_debug(NAME" %p: uuid %s (%"PRIu64")-> %s",
					client, client_uuid, uuid, o->node.name);
			return strdup(o->node.name);
		}
	}
	return NULL;
}

SPA_EXPORT
int jack_internal_client_new (const char *client_name,
                              const char *load_name,
                              const char *load_init)
{
	pw_log_warn("not implemented %s %s %s", client_name, load_name, load_init);
	return -ENOTSUP;
}

SPA_EXPORT
void jack_internal_client_close (const char *client_name)
{
	pw_log_warn("not implemented %s", client_name);
}

static int do_activate(struct client *c)
{
	int res;

	pw_data_loop_start(c->loop);

	pw_thread_loop_lock(c->context.loop);
	pw_log_debug(NAME" %p: activate", c);
	pw_client_node_proxy_set_active(c->node_proxy, true);

	res = do_sync(c);

	pw_thread_loop_unlock(c->context.loop);
	return res;
}

SPA_EXPORT
int jack_activate (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	int res;

	if (c->active)
		return 0;

	if ((res = do_activate(c)) < 0)
		return res;

	c->activation->pending_new_pos = true;
	c->activation->pending_sync = true;
	c->active = true;

	return 0;
}

SPA_EXPORT
int jack_deactivate (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	int res;

	if (!c->active)
		return 0;

	pw_thread_loop_lock(c->context.loop);
	pw_log_debug(NAME" %p: deactivate", c);
	pw_client_node_proxy_set_active(c->node_proxy, false);

	c->activation->pending_new_pos = false;
	c->activation->pending_sync = false;

	res = do_sync(c);

	pw_thread_loop_unlock(c->context.loop);

	pw_data_loop_stop(c->loop);

	if (res < 0)
		return res;

	c->active = false;
	return 0;
}

SPA_EXPORT
int jack_get_client_pid (const char *name)
{
	pw_log_error("not implemented on library side");
	return 0;
}

SPA_EXPORT
jack_native_thread_t jack_client_thread_id (jack_client_t *client)
{
	return pthread_self();
}

SPA_EXPORT
int jack_is_realtime (jack_client_t *client)
{
	return 1;
}

SPA_EXPORT
jack_nframes_t jack_thread_wait (jack_client_t *client, int status)
{
	pw_log_error(NAME" %p: jack_thread_wait: deprecated, use jack_cycle_wait/jack_cycle_signal", client);
	return 0;
}

SPA_EXPORT
jack_nframes_t jack_cycle_wait (jack_client_t* client)
{
	struct client *c = (struct client *) client;
	jack_nframes_t res;
	res = cycle_wait(c);
	pw_log_trace(NAME" %p: result:%d", c, res);
	return res;
}

SPA_EXPORT
void jack_cycle_signal (jack_client_t* client, int status)
{
	struct client *c = (struct client *) client;
	pw_log_trace(NAME" %p: status:%d", c, status);
	cycle_signal(c, status);
}

SPA_EXPORT
int jack_set_process_thread(jack_client_t* client, JackThreadCallback thread_callback, void *arg)
{
	struct client *c = (struct client *) client;

	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	} else if (c->process_callback) {
		pw_log_error(NAME" %p: process callback was already set", c);
		return -EIO;
	}
	pw_log_debug(NAME" %p: %p %p", c, thread_callback, arg);
	c->thread_callback = thread_callback;
	c->thread_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_thread_init_callback (jack_client_t *client,
                                   JackThreadInitCallback thread_init_callback,
                                   void *arg)
{
	struct client *c = (struct client *) client;
	pw_log_debug(NAME" %p: %p %p", c, thread_init_callback, arg);
	c->thread_init_callback = thread_init_callback;
	c->thread_init_arg = arg;
	return 0;
}

SPA_EXPORT
void jack_on_shutdown (jack_client_t *client,
                       JackShutdownCallback shutdown_callback, void *arg)
{
	struct client *c = (struct client *) client;

	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
	} else {
		pw_log_debug(NAME" %p: %p %p", c, shutdown_callback, arg);
		c->shutdown_callback = shutdown_callback;
		c->shutdown_arg = arg;
	}
}

SPA_EXPORT
void jack_on_info_shutdown (jack_client_t *client,
                            JackInfoShutdownCallback shutdown_callback, void *arg)
{
	struct client *c = (struct client *) client;

	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
	} else {
		pw_log_debug(NAME" %p: %p %p", c, shutdown_callback, arg);
		c->info_shutdown_callback = shutdown_callback;
		c->info_shutdown_arg = arg;
	}
}

SPA_EXPORT
int jack_set_process_callback (jack_client_t *client,
                               JackProcessCallback process_callback,
                               void *arg)
{
	struct client *c = (struct client *) client;

	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	} else if (c->thread_callback) {
		pw_log_error(NAME" %p: thread callback was already set", c);
		return -EIO;
	}

	pw_log_debug(NAME" %p: %p %p", c, process_callback, arg);
	c->process_callback = process_callback;
	c->process_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_freewheel_callback (jack_client_t *client,
                                 JackFreewheelCallback freewheel_callback,
                                 void *arg)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug(NAME" %p: %p %p", c, freewheel_callback, arg);
	c->freewheel_callback = freewheel_callback;
	c->freewheel_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_buffer_size_callback (jack_client_t *client,
                                   JackBufferSizeCallback bufsize_callback,
                                   void *arg)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug(NAME" %p: %p %p", c, bufsize_callback, arg);
	c->bufsize_callback = bufsize_callback;
	c->bufsize_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_sample_rate_callback (jack_client_t *client,
                                   JackSampleRateCallback srate_callback,
                                   void *arg)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug(NAME" %p: %p %p", c, srate_callback, arg);
	c->srate_callback = srate_callback;
	c->srate_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_client_registration_callback (jack_client_t *client,
                                            JackClientRegistrationCallback
                                            registration_callback, void *arg)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug(NAME" %p: %p %p", c, registration_callback, arg);
	c->registration_callback = registration_callback;
	c->registration_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_port_registration_callback (jack_client_t *client,
                                          JackPortRegistrationCallback
                                          registration_callback, void *arg)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug(NAME" %p: %p %p", c, registration_callback, arg);
	c->portregistration_callback = registration_callback;
	c->portregistration_arg = arg;
	return 0;
}


SPA_EXPORT
int jack_set_port_connect_callback (jack_client_t *client,
                                    JackPortConnectCallback
                                    connect_callback, void *arg)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug(NAME" %p: %p %p", c, connect_callback, arg);
	c->connect_callback = connect_callback;
	c->connect_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_port_rename_callback (jack_client_t *client,
                                   JackPortRenameCallback rename_callback,
				   void *arg)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug(NAME" %p: %p %p", c, rename_callback, arg);
	c->rename_callback = rename_callback;
	c->rename_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_graph_order_callback (jack_client_t *client,
                                   JackGraphOrderCallback graph_callback,
                                   void *data)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -1;
	}
	pw_log_trace(NAME" %p: %p %p", c, graph_callback, data);
	c->graph_callback = graph_callback;
	c->graph_arg = data;
	return 0;
}

SPA_EXPORT
int jack_set_xrun_callback (jack_client_t *client,
                            JackXRunCallback xrun_callback, void *arg)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -1;
	}
	pw_log_debug(NAME" %p: %p %p", c, xrun_callback, arg);
	c->xrun_callback = xrun_callback;
	c->xrun_arg = arg;
	return 0;
}

SPA_EXPORT
int jack_set_latency_callback (jack_client_t *client,
			       JackLatencyCallback latency_callback,
			       void *data)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_debug(NAME" %p: %p %p", c, latency_callback, data);
	c->latency_callback = latency_callback;
	c->latency_arg = data;
	return 0;
}

SPA_EXPORT
int jack_set_freewheel(jack_client_t* client, int onoff)
{
	pw_log_warn(NAME" %p: not implemented %d", client, onoff);
	return -ENOTSUP;
}

SPA_EXPORT
int jack_set_buffer_size (jack_client_t *client, jack_nframes_t nframes)
{
	struct client *c = (struct client *) client;
	struct spa_node_info ni;
	struct spa_dict_item items[1];
	char latency[128];

	snprintf(latency, sizeof(latency), "%d/%d", nframes, jack_get_sample_rate(client));

	ni = SPA_NODE_INFO_INIT();
	ni.max_input_ports = MAX_PORTS;
	ni.max_output_ports = MAX_PORTS;
	ni.change_mask = SPA_NODE_CHANGE_MASK_PROPS;
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency);
	ni.props = &SPA_DICT_INIT_ARRAY(items);

	pw_client_node_proxy_update(c->node_proxy,
                                    PW_CLIENT_NODE_UPDATE_INFO,
				    0, NULL, &ni);

	return 0;
}

SPA_EXPORT
jack_nframes_t jack_get_sample_rate (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	if (c->sample_rate == (uint32_t)-1)
		return DEFAULT_SAMPLE_RATE;
	return c->sample_rate;
}

SPA_EXPORT
jack_nframes_t jack_get_buffer_size (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	if (c->buffer_frames == (uint32_t)-1)
		return DEFAULT_BUFFER_FRAMES;
	return c->buffer_frames;
}

SPA_EXPORT
int jack_engine_takeover_timebase (jack_client_t *client)
{
	pw_log_error(NAME" %p: deprecated", client);
	return 0;
}

SPA_EXPORT
float jack_cpu_load (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	float res = 0.0f;

	if (c->driver_activation)
		res = c->driver_activation->cpu_load[0] * 100.0f;

	pw_log_trace(NAME" %p: cpu load %f", client, res);
	return res;
}

#include "statistics.c"

SPA_EXPORT
jack_port_t * jack_port_register (jack_client_t *client,
                                  const char *port_name,
                                  const char *port_type,
                                  unsigned long flags,
                                  unsigned long buffer_frames)
{
	struct client *c = (struct client *) client;
	enum spa_direction direction;
	struct spa_port_info port_info;
	struct spa_param_info port_params[5];
	struct spa_dict dict;
	struct spa_dict_item items[10];
	struct object *o;
	jack_port_type_id_t type_id;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *params[4];
	uint32_t n_params = 0;
	struct port *p;
	int res;

	pw_log_debug(NAME" %p: port register \"%s\" \"%s\" %08lx %ld",
			c, port_name, port_type, flags, buffer_frames);

	if (flags & JackPortIsInput)
		direction = PW_DIRECTION_INPUT;
	else if (flags & JackPortIsOutput)
		direction = PW_DIRECTION_OUTPUT;
	else
		return NULL;

	if ((type_id = string_to_type(port_type)) == SPA_ID_INVALID)
		return NULL;

	if ((p = alloc_port(c, direction)) == NULL)
		return NULL;

	o = p->object;
	o->port.flags = flags;
	snprintf(o->port.name, sizeof(o->port.name), "%s:%s", c->name, port_name);
	o->port.type_id = type_id;

	pw_log_debug(NAME" %p: port %p", c, p);

	spa_list_init(&p->mix);

	port_info = SPA_PORT_INFO_INIT();
	port_info.change_mask |= SPA_PORT_CHANGE_MASK_FLAGS;
	port_info.flags = SPA_PORT_FLAG_NO_REF;
	port_info.change_mask |= SPA_PORT_CHANGE_MASK_PROPS;
	dict = SPA_DICT_INIT(items, 0);
	items[dict.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_FORMAT_DSP, port_type);
	items[dict.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_PORT_NAME, port_name);
	port_info.props = &dict;
	port_info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	port_params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port_params[1] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	port_params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port_params[3] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port_info.params = port_params;
	port_info.n_params = 4;

	param_enum_format(c, p, &params[n_params++], &b);
	param_buffers(c, p, &params[n_params++], &b);
	param_io(c, p, &params[n_params++], &b);

	pw_thread_loop_lock(c->context.loop);

	pw_client_node_proxy_port_update(c->node_proxy,
					 direction,
					 p->id,
					 PW_CLIENT_NODE_PORT_UPDATE_PARAMS |
					 PW_CLIENT_NODE_PORT_UPDATE_INFO,
					 n_params,
					 (const struct spa_pod **) params,
					 &port_info);

	res = do_sync(c);

	pw_thread_loop_unlock(c->context.loop);

	if (res < 0)
		return NULL;

	return (jack_port_t *) o;
}

SPA_EXPORT
int jack_port_unregister (jack_client_t *client, jack_port_t *port)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	struct port *p;
	int res;

	if (o->type != PW_TYPE_INTERFACE_Port || o->port.port_id == SPA_ID_INVALID) {
		pw_log_error(NAME" %p: invalid port %p", client, port);
		return -EINVAL;
	}
	pw_log_debug(NAME" %p: port unregister %p", client, port);

	pw_thread_loop_lock(c->context.loop);

	p = GET_PORT(c, GET_DIRECTION(o->port.flags), o->port.port_id);

	free_port(c, p);

	pw_client_node_proxy_port_update(c->node_proxy,
					 p->direction,
					 p->id,
					 0, 0, NULL, NULL);

	res = do_sync(c);

	pw_thread_loop_unlock(c->context.loop);

	return res;
}

static inline void *get_buffer_input_float(struct client *c, struct port *p, jack_nframes_t frames)
{
	struct mix *mix;
	struct buffer *b;
	struct spa_io_buffers *io;
	int layer = 0;
	void *ptr = NULL;

	spa_list_for_each(mix, &p->mix, port_link) {
		pw_log_trace(NAME" %p: port %p mix %d.%d get buffer %d",
				c, p, p->id, mix->id, frames);
		io = mix->io;
		if (io == NULL || io->buffer_id >= mix->n_buffers)
			continue;

		io->status = SPA_STATUS_NEED_DATA;
		b = &mix->buffers[io->buffer_id];
		if (layer++ == 0)
			ptr = b->datas[0].data;
		else  {
			mix2(p->emptyptr, ptr, b->datas[0].data, frames);
			ptr = p->emptyptr;
			p->zeroed = false;
		}
	}
	return ptr;
}

static inline void *get_buffer_input_midi(struct client *c, struct port *p, jack_nframes_t frames)
{
	struct mix *mix;
	struct spa_io_buffers *io;
	void *ptr = p->emptyptr;
	struct spa_pod_sequence *seq[CONNECTION_NUM_FOR_PORT];
	uint32_t n_seq = 0;

	jack_midi_clear_buffer(ptr);

	spa_list_for_each(mix, &p->mix, port_link) {
		struct spa_data *d;
		void *pod;

		pw_log_trace(NAME" %p: port %p mix %d.%d get buffer %d",
				c, p, p->id, mix->id, frames);

		io = mix->io;
		if (io == NULL || io->buffer_id >= mix->n_buffers)
			continue;

		io->status = SPA_STATUS_NEED_DATA;
		d = &mix->buffers[io->buffer_id].datas[0];

		if ((pod = spa_pod_from_data(d->data, d->maxsize, d->chunk->offset, d->chunk->size)) == NULL)
			continue;
		if (!spa_pod_is_sequence(pod))
			continue;

		seq[n_seq++] = pod;
	}
	convert_to_midi(seq, n_seq, ptr);

	return ptr;
}

static inline void *get_buffer_output_float(struct client *c, struct port *p, jack_nframes_t frames)
{
	void *ptr;

	ptr = get_buffer_output(c, p, frames, sizeof(float));
	if (ptr == NULL)
		ptr = p->emptyptr;

	return ptr;
}

static inline void *get_buffer_output_midi(struct client *c, struct port *p, jack_nframes_t frames)
{
	return p->emptyptr;
}

SPA_EXPORT
void * jack_port_get_buffer (jack_port_t *port, jack_nframes_t frames)
{
	struct object *o = (struct object *) port;
	struct client *c;
	struct port *p;
	void *ptr = NULL;

	if (o == NULL)
		return NULL;

	c = o->client;

	if (o->type != PW_TYPE_INTERFACE_Port || o->port.port_id == SPA_ID_INVALID) {
		pw_log_error(NAME" %p: invalid port %p", c, port);
		return NULL;
	}
	p = GET_PORT(c, GET_DIRECTION(o->port.flags), o->port.port_id);

	if (p->direction == SPA_DIRECTION_INPUT) {
		switch (p->object->port.type_id) {
		case 0:
			ptr = get_buffer_input_float(c, p, frames);
			break;
		case 1:
			ptr = get_buffer_input_midi(c, p, frames);
			break;
		case 2:
			ptr = get_buffer_input_float(c, p, frames);
			break;
		}
		if (ptr == NULL) {
			ptr = p->emptyptr;
			if (!p->zeroed) {
				init_buffer(p, ptr, MAX_BUFFER_FRAMES);
				p->zeroed = true;
			}
		}
	} else {
		switch (p->object->port.type_id) {
		case 0:
			ptr = get_buffer_output_float(c, p, frames);
			break;
		case 1:
			ptr = get_buffer_output_midi(c, p, frames);
			break;
		case 2:
			ptr = get_buffer_output_float(c, p, frames);
			break;
		}
	}

	pw_log_trace(NAME" %p: port %p buffer %p", c, p, ptr);
	return ptr;
}

SPA_EXPORT
jack_uuid_t jack_port_uuid (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return jack_port_uuid_generate(o->id);
}

SPA_EXPORT
const char * jack_port_name (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return o->port.name;
}

SPA_EXPORT
const char * jack_port_short_name (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return strchr(o->port.name, ':') + 1;
}

SPA_EXPORT
int jack_port_flags (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return o->port.flags;
}

SPA_EXPORT
const char * jack_port_type (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return type_to_string(o->port.type_id);
}

SPA_EXPORT
jack_port_type_id_t jack_port_type_id (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return o->port.type_id;
}

SPA_EXPORT
int jack_port_is_mine (const jack_client_t *client, const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return o->type == PW_TYPE_INTERFACE_Port && o->port.port_id != SPA_ID_INVALID;
}

SPA_EXPORT
int jack_port_connected (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	struct object *l;
	int res = 0;

	pw_thread_loop_lock(c->context.loop);
	spa_list_for_each(l, &c->context.links, link) {
		if (l->port_link.src == o->id ||
		    l->port_link.dst == o->id)
			res++;
	}
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_port_connected_to (const jack_port_t *port,
                            const char *port_name)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	struct object *p, *l;
	int res = 0;

	pw_thread_loop_lock(c->context.loop);

	p = find_port(c, port_name);
	if (p == NULL)
		goto exit;

	if (GET_DIRECTION(p->port.flags) == GET_DIRECTION(o->port.flags))
		goto exit;

	if (p->port.flags & JackPortIsOutput) {
		l = p;
		p = o;
		o = l;
	}
	if (find_link(c, o->id, p->id))
		res = 1;

     exit:
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
const char ** jack_port_get_connections (const jack_port_t *port)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;

	return jack_port_get_all_connections((jack_client_t *)c, port);
}

SPA_EXPORT
const char ** jack_port_get_all_connections (const jack_client_t *client,
                                             const jack_port_t *port)
{
	struct client *c = (struct client *) client;
	struct object *o = (struct object *) port;
	struct object *p, *l;
	const char **res = malloc(sizeof(char*) * (CONNECTION_NUM_FOR_PORT + 1));
	int count = 0;

	pw_thread_loop_lock(c->context.loop);

	spa_list_for_each(l, &c->context.links, link) {
		if (l->port_link.src == o->id)
			p = pw_map_lookup(&c->context.globals, l->port_link.dst);
		else if (l->port_link.dst == o->id)
			p = pw_map_lookup(&c->context.globals, l->port_link.src);
		else
			continue;

		if (p == NULL)
			continue;

		res[count++] = p->port.name;
		if (count == CONNECTION_NUM_FOR_PORT)
			break;
	}
	pw_thread_loop_unlock(c->context.loop);

	if (count == 0) {
		free(res);
		res = NULL;
	} else
		res[count] = NULL;

	return res;
}

SPA_EXPORT
int jack_port_tie (jack_port_t *src, jack_port_t *dst)
{
	pw_log_warn("not implemented %p %p", src, dst);
	return -ENOTSUP;
}

SPA_EXPORT
int jack_port_untie (jack_port_t *port)
{
	pw_log_warn("not implemented %p", port);
	return -ENOTSUP;
}

SPA_EXPORT
int jack_port_set_name (jack_port_t *port, const char *port_name)
{
	pw_log_warn("deprecated");
	return 0;
}

SPA_EXPORT
int jack_port_rename (jack_client_t* client, jack_port_t *port, const char *port_name)
{
	struct client *c = (struct client *) client;
	struct object *o = (struct object *) port;
	struct port *p;
	struct spa_port_info port_info;
	struct spa_dict dict;
	struct spa_dict_item items[1];

	pw_thread_loop_lock(c->context.loop);

	p = GET_PORT(c, GET_DIRECTION(o->port.flags), o->port.port_id);

	port_info = SPA_PORT_INFO_INIT();
	port_info.change_mask |= SPA_PORT_CHANGE_MASK_PROPS;
	dict = SPA_DICT_INIT(items, 0);
	items[dict.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_PORT_NAME, port_name);
	port_info.props = &dict;

	pw_client_node_proxy_port_update(c->node_proxy,
					 p->direction,
					 p->id,
					 PW_CLIENT_NODE_PORT_UPDATE_INFO,
					 0, NULL,
					 &port_info);
	pw_thread_loop_unlock(c->context.loop);

	return 0;
}

SPA_EXPORT
int jack_port_set_alias (jack_port_t *port, const char *alias)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	struct port *p;
	struct spa_port_info port_info;
	struct spa_dict dict;
	struct spa_dict_item items[1];
	const char *key;

	if (c == NULL)
		return -1;

	pw_thread_loop_lock(c->context.loop);

	if (o->port.alias1[0] == '\0') {
		key = PW_KEY_OBJECT_PATH;
		snprintf(o->port.alias1, sizeof(o->port.alias1), "%s", alias);
	}
	else if (o->port.alias2[0] == '\0') {
		key = PW_KEY_PORT_ALIAS;
		snprintf(o->port.alias2, sizeof(o->port.alias2), "%s", alias);
	}
	else
		goto error;

	p = GET_PORT(c, GET_DIRECTION(o->port.flags), o->port.port_id);

	port_info = SPA_PORT_INFO_INIT();
	port_info.change_mask |= SPA_PORT_CHANGE_MASK_PROPS;
	dict = SPA_DICT_INIT(items, 0);
	items[dict.n_items++] = SPA_DICT_ITEM_INIT(key, alias);
	port_info.props = &dict;

	pw_client_node_proxy_port_update(c->node_proxy,
					 p->direction,
					 p->id,
					 PW_CLIENT_NODE_PORT_UPDATE_INFO,
					 0, NULL,
					 &port_info);
	pw_thread_loop_unlock(c->context.loop);

	return 0;

error:
	pw_thread_loop_unlock(c->context.loop);
	return -1;
}

SPA_EXPORT
int jack_port_unset_alias (jack_port_t *port, const char *alias)
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	struct port *p;
	struct spa_port_info port_info;
	struct spa_dict dict;
	struct spa_dict_item items[1];
	const char *key;

	if (c == NULL)
		return -1;

	pw_thread_loop_lock(c->context.loop);

	if (strcmp(o->port.alias1, alias) == 0)
		key = PW_KEY_OBJECT_PATH;
	else if (strcmp(o->port.alias2, alias) == 0)
		key = PW_KEY_PORT_ALIAS;
	else
		goto error;

	p = GET_PORT(c, GET_DIRECTION(o->port.flags), o->port.port_id);

	port_info = SPA_PORT_INFO_INIT();
	port_info.change_mask |= SPA_PORT_CHANGE_MASK_PROPS;
	dict = SPA_DICT_INIT(items, 0);
	items[dict.n_items++] = SPA_DICT_ITEM_INIT(key, NULL);
	port_info.props = &dict;

	pw_client_node_proxy_port_update(c->node_proxy,
					 p->direction,
					 p->id,
					 PW_CLIENT_NODE_PORT_UPDATE_INFO,
					 0, NULL,
					 &port_info);
	pw_thread_loop_unlock(c->context.loop);

	return 0;

error:
	pw_thread_loop_unlock(c->context.loop);
	return -1;
}

SPA_EXPORT
int jack_port_get_aliases (const jack_port_t *port, char* const aliases[2])
{
	struct object *o = (struct object *) port;
	struct client *c = o->client;
	int res = 0;

	pw_thread_loop_lock(c->context.loop);

	if (o->port.alias1[0] != '\0') {
		snprintf(aliases[0], REAL_JACK_PORT_NAME_SIZE+1, "%s", o->port.alias1);
		res++;
	}
	if (o->port.alias2[0] != '\0') {
		snprintf(aliases[1], REAL_JACK_PORT_NAME_SIZE+1, "%s", o->port.alias2);
		res++;
	}
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_port_request_monitor (jack_port_t *port, int onoff)
{
	struct object *o = (struct object *) port;
	if (onoff)
		o->port.monitor_requests++;
	else if (o->port.monitor_requests > 0)
		o->port.monitor_requests--;
	return 0;
}

SPA_EXPORT
int jack_port_request_monitor_by_name (jack_client_t *client,
                                       const char *port_name, int onoff)
{
	struct client *c = (struct client *) client;
	struct object *p;

	pw_thread_loop_lock(c->context.loop);

	p = find_port(c, port_name);

	pw_thread_loop_unlock(c->context.loop);

	if (p == NULL) {
		pw_log_error(NAME" %p: jack_port_request_monitor_by_name called"
				" with an incorrect port %s", client, port_name);
		return -1;
	}

	return jack_port_request_monitor((jack_port_t*)p, onoff);
}

SPA_EXPORT
int jack_port_ensure_monitor (jack_port_t *port, int onoff)
{
	struct object *o = (struct object *) port;
	if (onoff) {
		if (o->port.monitor_requests == 0)
			o->port.monitor_requests++;
	} else {
		if (o->port.monitor_requests > 0)
			o->port.monitor_requests = 0;
	}
	return 0;
}

SPA_EXPORT
int jack_port_monitoring_input (jack_port_t *port)
{
	struct object *o = (struct object *) port;
	return o->port.monitor_requests > 0;
}

SPA_EXPORT
int jack_connect (jack_client_t *client,
                  const char *source_port,
                  const char *destination_port)
{
	struct client *c = (struct client *) client;
	struct object *src, *dst;
	struct spa_dict props;
	struct spa_dict_item items[5];
	char val[4][16];
	int res;

	pw_log_debug(NAME" %p: connect %s %s", client, source_port, destination_port);

	pw_thread_loop_lock(c->context.loop);

	src = find_port(c, source_port);
	dst = find_port(c, destination_port);

	if (src == NULL || dst == NULL ||
	    !(src->port.flags & JackPortIsOutput) ||
	    !(dst->port.flags & JackPortIsInput) ||
	    src->port.type_id != dst->port.type_id) {
		res = -EINVAL;
		goto exit;
	}

	snprintf(val[0], sizeof(val[0]), "%d", src->port.node_id);
	snprintf(val[1], sizeof(val[1]), "%d", src->id);
	snprintf(val[2], sizeof(val[2]), "%d", dst->port.node_id);
	snprintf(val[3], sizeof(val[3]), "%d", dst->id);

	props = SPA_DICT_INIT(items, 0);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_NODE, val[0]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_LINK_OUTPUT_PORT, val[1]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_NODE, val[2]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_LINK_INPUT_PORT, val[3]);
	items[props.n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_OBJECT_LINGER, "1");

	pw_core_proxy_create_object(c->core_proxy,
				    "link-factory",
				    PW_TYPE_INTERFACE_Link,
				    PW_VERSION_LINK_PROXY,
				    &props,
				    0);
	res = do_sync(c);

      exit:
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_disconnect (jack_client_t *client,
                     const char *source_port,
                     const char *destination_port)
{
	struct client *c = (struct client *) client;
	struct object *src, *dst, *l;
	int res;

	pw_log_debug(NAME" %p: disconnect %s %s", client, source_port, destination_port);

	pw_thread_loop_lock(c->context.loop);

	src = find_port(c, source_port);
	dst = find_port(c, destination_port);

	pw_log_debug(NAME" %p: %d %d", client, src->id, dst->id);

	if (src == NULL || dst == NULL ||
	    !(src->port.flags & JackPortIsOutput) ||
	    !(dst->port.flags & JackPortIsInput)) {
		res = -EINVAL;
		goto exit;
	}

	if ((l = find_link(c, src->id, dst->id)) == NULL) {
		res = -ENOENT;
		goto exit;
	}

	pw_registry_proxy_destroy(c->registry_proxy, l->id);

	res = do_sync(c);

      exit:
	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_port_disconnect (jack_client_t *client, jack_port_t *port)
{
	struct client *c = (struct client *) client;
	struct object *o = (struct object *) port;
	struct object *l;
	int res;

	pw_log_debug(NAME" %p: disconnect %p", client, port);

	pw_thread_loop_lock(c->context.loop);

	spa_list_for_each(l, &c->context.links, link) {
		if (l->port_link.src == o->id ||
		    l->port_link.dst == o->id) {
			pw_registry_proxy_destroy(c->registry_proxy, l->id);
		}
	}
	res = do_sync(c);

	pw_thread_loop_unlock(c->context.loop);

	return res;
}

SPA_EXPORT
int jack_port_name_size(void)
{
	return REAL_JACK_PORT_NAME_SIZE+1;
}

SPA_EXPORT
int jack_port_type_size(void)
{
	return JACK_PORT_TYPE_SIZE+1;
}

SPA_EXPORT
size_t jack_port_type_get_buffer_size (jack_client_t *client, const char *port_type)
{
	if (!strcmp(JACK_DEFAULT_AUDIO_TYPE, port_type))
		return jack_get_buffer_size(client) * sizeof(float);
	else if (!strcmp(JACK_DEFAULT_MIDI_TYPE, port_type))
		return MAX_BUFFER_FRAMES * sizeof(float);
	else if (!strcmp(JACK_DEFAULT_VIDEO_TYPE, port_type))
		return 320 * 240 * 4 * sizeof(float);
	else
		return 0;
}

SPA_EXPORT
void jack_port_set_latency (jack_port_t *port, jack_nframes_t frames)
{
	struct object *o = (struct object *) port;
	jack_latency_range_t range = { frames, frames };
	if (o->port.flags & JackPortIsOutput) {
		jack_port_set_latency_range(port, JackCaptureLatency, &range);
        }
        if (o->port.flags & JackPortIsInput) {
		jack_port_set_latency_range(port, JackPlaybackLatency, &range);
        }
}

SPA_EXPORT
void jack_port_get_latency_range (jack_port_t *port, jack_latency_callback_mode_t mode, jack_latency_range_t *range)
{
	struct object *o = (struct object *) port;
	if (mode == JackCaptureLatency) {
		*range = o->port.capture_latency;
	} else {
		*range = o->port.playback_latency;
	}
}

SPA_EXPORT
void jack_port_set_latency_range (jack_port_t *port, jack_latency_callback_mode_t mode, jack_latency_range_t *range)
{
	struct object *o = (struct object *) port;
	if (mode == JackCaptureLatency) {
		o->port.capture_latency = *range;
	} else {
		o->port.playback_latency = *range;
	}
}

SPA_EXPORT
int jack_recompute_total_latencies (jack_client_t *client)
{
	pw_log_warn(NAME" %p: not implemented", client);
	return 0;
}

SPA_EXPORT
jack_nframes_t jack_port_get_latency (jack_port_t *port)
{
	struct object *o = (struct object *) port;
	jack_latency_range_t range;
	if (o->port.flags & JackPortIsOutput) {
		jack_port_get_latency_range(port, JackCaptureLatency, &range);
        }
        if (o->port.flags & JackPortIsInput) {
		jack_port_get_latency_range(port, JackPlaybackLatency, &range);
        }
	return (range.min + range.max) / 2;
}

SPA_EXPORT
jack_nframes_t jack_port_get_total_latency (jack_client_t *client,
					    jack_port_t *port)
{
	pw_log_warn(NAME" %p: not implemented %p", client, port);
	return 0;
}

SPA_EXPORT
int jack_recompute_total_latency (jack_client_t *client, jack_port_t* port)
{
	pw_log_warn(NAME" %p: not implemented %p", client, port);
	return 0;
}

static int port_compare_func(const void *v1, const void *v2)
{
	const struct object *const*o1 = v1, *const*o2 = v2;

	if ((*o1)->port.type_id != (*o2)->port.type_id)
		return (*o1)->port.type_id - (*o2)->port.type_id;

	if ((*o1)->port.priority != (*o2)->port.priority)
		return (*o2)->port.priority - (*o1)->port.priority;

	return (*o1)->id - (*o2)->id;
}

SPA_EXPORT
const char ** jack_get_ports (jack_client_t *client,
                              const char *port_name_pattern,
                              const char *type_name_pattern,
                              unsigned long flags)
{
	struct client *c = (struct client *) client;
	const char **res;
	struct object *o;
	struct object *tmp[JACK_PORT_MAX];
	const char *str;
	uint32_t i, count, id;
	regex_t port_regex, type_regex;

	if ((str = getenv("PIPEWIRE_NODE")) != NULL)
		id = pw_properties_parse_int(str);
	else
		id = SPA_ID_INVALID;

	if (port_name_pattern && port_name_pattern[0])
		regcomp(&port_regex, port_name_pattern, REG_EXTENDED | REG_NOSUB);
	if (type_name_pattern && type_name_pattern[0])
		regcomp(&type_regex, type_name_pattern, REG_EXTENDED | REG_NOSUB);

	pw_thread_loop_lock(c->context.loop);

	pw_log_debug(NAME" %p: ports id:%d name:%s type:%s flags:%08lx", c, id,
			port_name_pattern, type_name_pattern, flags);

	count = 0;
	spa_list_for_each(o, &c->context.ports, link) {
		pw_log_debug(NAME" %p: check port type:%d flags:%08lx name:%s", c,
				o->port.type_id, o->port.flags, o->port.name);
		if (count == JACK_PORT_MAX)
			break;
		if (o->port.type_id > 2)
			continue;
		if (!SPA_FLAG_IS_SET(o->port.flags, flags))
			continue;
		if (id != SPA_ID_INVALID && o->port.node_id != id)
			continue;

		if (port_name_pattern && port_name_pattern[0]) {
			if (regexec(&port_regex, o->port.name, 0, NULL, 0) == REG_NOMATCH)
				continue;
		}
		if (type_name_pattern && type_name_pattern[0]) {
			if (regexec(&type_regex, type_to_string(o->port.type_id),
						0, NULL, 0) == REG_NOMATCH)
				continue;
		}

		pw_log_debug(NAME" %p: port %s prio:%d matches (%d)",
				c, o->port.name, o->port.priority, count);
		tmp[count++] = o;
	}
	if (count > 0) {
		qsort(tmp, count, sizeof(struct object *), port_compare_func);

		res = malloc(sizeof(char*) * (count + 1));
		for (i = 0; i < count; i++)
			res[i] = tmp[i]->port.name;
		res[count] = NULL;
	} else {
		res = NULL;
	}

	pw_thread_loop_unlock(c->context.loop);

	if (port_name_pattern && port_name_pattern[0])
		regfree(&port_regex);
	if (type_name_pattern && type_name_pattern[0])
		regfree(&type_regex);

	return res;
}

SPA_EXPORT
jack_port_t * jack_port_by_name (jack_client_t *client, const char *port_name)
{
	struct client *c = (struct client *) client;
	struct object *res;

	pw_thread_loop_lock(c->context.loop);

	res = find_port(c, port_name);

	pw_thread_loop_unlock(c->context.loop);

	return (jack_port_t *)res;
}

SPA_EXPORT
jack_port_t * jack_port_by_id (jack_client_t *client,
                               jack_port_id_t port_id)
{
	struct client *c = (struct client *) client;
	struct object *res = NULL, *o;

	pw_thread_loop_lock(c->context.loop);

	o = pw_map_lookup(&c->context.globals, port_id);
	pw_log_debug(NAME" %p: port %d -> %p", c, port_id, o);

	if (o == NULL || o->type != PW_TYPE_INTERFACE_Port)
		goto exit;

	res = o;

      exit:
	pw_thread_loop_unlock(c->context.loop);

	return (jack_port_t *)res;
}

SPA_EXPORT
jack_nframes_t jack_frames_since_cycle_start (const jack_client_t *client)
{
	struct client *c = (struct client *) client;
	struct spa_io_position *pos = c->position;
	struct timespec ts;
	uint64_t diff;

	if (pos == NULL)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	diff = SPA_TIMESPEC_TO_NSEC(&ts) - pos->clock.nsec;
	return (jack_nframes_t) floor(((float)c->sample_rate * diff) / SPA_NSEC_PER_SEC);
}

SPA_EXPORT
jack_nframes_t jack_frame_time (const jack_client_t *client)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return jack_time_to_frames(client, SPA_TIMESPEC_TO_USEC(&ts));
}

SPA_EXPORT
jack_nframes_t jack_last_frame_time (const jack_client_t *client)
{
	struct client *c = (struct client *) client;
	struct spa_io_position *pos = c->position;

	if (pos == NULL)
		return 0;

	return pos->clock.position;
}

SPA_EXPORT
int jack_get_cycle_times(const jack_client_t *client,
                        jack_nframes_t *current_frames,
                        jack_time_t    *current_usecs,
                        jack_time_t    *next_usecs,
                        float          *period_usecs)
{
	struct client *c = (struct client *) client;
	struct spa_io_position *pos = c->position;

	if (pos == NULL)
		return -1;

	*current_frames = pos->clock.position;
	*current_usecs = pos->clock.nsec / SPA_NSEC_PER_USEC;
	*period_usecs = pos->clock.duration * (float)SPA_USEC_PER_SEC / (c->sample_rate * pos->clock.rate_diff);
	*next_usecs = pos->clock.next_nsec / SPA_NSEC_PER_USEC;

	pw_log_trace(NAME" %p: %d %"PRIu64" %"PRIu64" %f", c, *current_frames,
			*current_usecs, *next_usecs, *period_usecs);
	return 0;
}

SPA_EXPORT
jack_time_t jack_frames_to_time(const jack_client_t *client, jack_nframes_t frames)
{
	struct client *c = (struct client *) client;
	struct spa_io_position *pos = c->position;
	double df;

	if (pos == NULL)
		return 0;

	df = (frames - pos->clock.position) * (double)SPA_NSEC_PER_SEC / c->sample_rate;
	return (pos->clock.nsec + (int64_t)rint(df)) / SPA_NSEC_PER_USEC;
}

SPA_EXPORT
jack_nframes_t jack_time_to_frames(const jack_client_t *client, jack_time_t usecs)
{
	struct client *c = (struct client *) client;
	struct spa_io_position *pos = c->position;
	double du;

	if (pos == NULL)
		return 0;

	du = (usecs - pos->clock.nsec/SPA_NSEC_PER_USEC) * (double)c->sample_rate / SPA_USEC_PER_SEC;
	return pos->clock.position + (int32_t)rint(du);
}

SPA_EXPORT
jack_time_t jack_get_time()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_USEC(&ts);
}

SPA_EXPORT
void jack_set_error_function (void (*func)(const char *))
{
	pw_log_warn("not implemented");
}

SPA_EXPORT
void jack_set_info_function (void (*func)(const char *))
{
	pw_log_warn("not implemented");
}

SPA_EXPORT
void jack_free(void* ptr)
{
	free(ptr);
}

SPA_EXPORT
int jack_release_timebase (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	struct pw_node_activation *a = c->driver_activation;

	if (a == NULL)
		return -EIO;

	if (!ATOMIC_CAS(a->segment_owner[0], c->node_id, 0))
		return -EINVAL;

	c->timebase_callback = NULL;
	c->timebase_arg = NULL;
	c->activation->pending_new_pos = false;

	return 0;
}

SPA_EXPORT
int jack_set_sync_callback (jack_client_t *client,
			    JackSyncCallback sync_callback,
			    void *arg)
{
	int res;
	struct client *c = (struct client *) client;

	c->sync_callback = sync_callback;
	c->sync_arg = arg;

	if ((res = do_activate(c)) < 0)
		return res;

	c->activation->pending_sync = true;
	return 0;
}

SPA_EXPORT
int jack_set_sync_timeout (jack_client_t *client,
			   jack_time_t timeout)
{
	struct client *c = (struct client *) client;
	struct pw_node_activation *a = c->driver_activation;

	if (a == NULL)
		return -EIO;

	ATOMIC_STORE(a->sync_timeout, timeout);

	return 0;
}

SPA_EXPORT
int  jack_set_timebase_callback (jack_client_t *client,
				 int conditional,
				 JackTimebaseCallback timebase_callback,
				 void *arg)
{
	int res;
	struct client *c = (struct client *) client;
	struct pw_node_activation *a = c->driver_activation;
	uint32_t owner;

	pw_log_debug(NAME" %p: activation %p", c, a);

	if (a == NULL)
		return -EIO;

	/* was ok */
	owner = ATOMIC_LOAD(a->segment_owner[0]);
	if (owner == c->node_id)
		return 0;

	/* try to become master */
	if (conditional) {
		if (!ATOMIC_CAS(a->segment_owner[0], 0, c->node_id)) {
			pw_log_debug(NAME" %p: owner:%u id:%u", c, owner, c->node_id);
			return -EBUSY;
		}
	} else {
		ATOMIC_STORE(a->segment_owner[0], c->node_id);
	}

	c->timebase_callback = timebase_callback;
	c->timebase_arg = arg;

	pw_log_debug(NAME" %p: timebase set id:%u", c, c->node_id);

	if ((res = do_activate(c)) < 0)
		return res;

	c->activation->pending_new_pos = true;

	return 0;
}

SPA_EXPORT
int  jack_transport_locate (jack_client_t *client,
			    jack_nframes_t frame)
{
	jack_position_t pos;
	pos.frame = frame;
	pos.valid = (jack_position_bits_t)0;
	return jack_transport_reposition(client, &pos);
}

SPA_EXPORT
jack_transport_state_t jack_transport_query (const jack_client_t *client,
					     jack_position_t *pos)
{
	struct client *c = (struct client *) client;
	struct pw_node_activation *a = c->driver_activation;
	jack_transport_state_t jack_state = JackTransportStopped;

	if (a != NULL)
		jack_state = position_to_jack(a, pos);
	else if (pos != NULL)
		memset(pos, 0, sizeof(jack_position_t));

	return jack_state;
}

SPA_EXPORT
jack_nframes_t jack_get_current_transport_frame (const jack_client_t *client)
{
	struct client *c = (struct client *) client;
	struct pw_node_activation *a = c->driver_activation;
	struct spa_io_position *pos;
	struct spa_io_segment *seg;
	uint64_t running;
	if (!a)
		return -1;

	pos = &a->position;
	running = pos->clock.position - pos->offset;

	if (pos->state == SPA_IO_POSITION_STATE_RUNNING) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint64_t nsecs = SPA_TIMESPEC_TO_NSEC(&ts) - pos->clock.nsec;
		running += (uint64_t)floor((((float) c->sample_rate) / SPA_NSEC_PER_SEC) * nsecs);
	}
	seg = &pos->segments[0];

	return (running - seg->start) * seg->rate + seg->position;
}

SPA_EXPORT
int  jack_transport_reposition (jack_client_t *client,
				const jack_position_t *pos)
{
	struct client *c = (struct client *) client;
	struct pw_node_activation *a = c->driver_activation;
	struct pw_node_activation *na = c->activation;
	if (!a || !na)
		return -EIO;

	if (pos->valid & ~(JackPositionBBT|JackPositionTimecode))
		return -EINVAL;

	pw_log_debug("frame:%u", pos->frame);
	na->reposition.flags = 0;
	na->reposition.start = 0;
	na->reposition.duration = 0;
	na->reposition.position = pos->frame;
	na->reposition.rate = 1.0;
	ATOMIC_STORE(a->reposition_owner, c->node_id);

	return 0;
}

static void update_command(struct client *c, uint32_t command)
{
	struct pw_node_activation *a = c->driver_activation;
	if (!a)
		return;
	ATOMIC_STORE(a->command, command);
}

SPA_EXPORT
void jack_transport_start (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	update_command(c, PW_NODE_ACTIVATION_COMMAND_START);
}

SPA_EXPORT
void jack_transport_stop (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	update_command(c, PW_NODE_ACTIVATION_COMMAND_STOP);
}

SPA_EXPORT
void jack_get_transport_info (jack_client_t *client,
			      jack_transport_info_t *tinfo)
{
	pw_log_error(NAME" %p: deprecated", client);
	if (tinfo)
		memset(tinfo, 0, sizeof(jack_transport_info_t));
}

SPA_EXPORT
void jack_set_transport_info (jack_client_t *client,
			      jack_transport_info_t *tinfo)
{
	pw_log_error(NAME" %p: deprecated", client);
	if (tinfo)
		memset(tinfo, 0, sizeof(jack_transport_info_t));
}

SPA_EXPORT
int jack_set_session_callback (jack_client_t       *client,
                               JackSessionCallback  session_callback,
                               void                *arg)
{
	struct client *c = (struct client *) client;
	if (c->active) {
		pw_log_error(NAME" %p: can't set callback on active client", c);
		return -EIO;
	}
	pw_log_warn(NAME" %p: not implemented", client);
	return -ENOTSUP;
}

SPA_EXPORT
int jack_session_reply (jack_client_t        *client,
                        jack_session_event_t *event)
{
	pw_log_warn(NAME" %p: not implemented", client);
	return -ENOTSUP;
}


SPA_EXPORT
void jack_session_event_free (jack_session_event_t *event)
{
	pw_log_warn("not implemented");
}

SPA_EXPORT
char *jack_client_get_uuid (jack_client_t *client)
{
	struct client *c = (struct client *) client;
	char *uuid = NULL;
	asprintf(&uuid, "%d", c->node_id);
	return uuid;
}

SPA_EXPORT
int jack_client_real_time_priority (jack_client_t * client)
{
	return 20;
}

SPA_EXPORT
int jack_client_max_real_time_priority (jack_client_t *client)
{
	return 20;
}

SPA_EXPORT
int jack_acquire_real_time_scheduling (jack_native_thread_t thread, int priority)
{
	pw_log_warn("not implemented %lu %d", thread, priority);
	return -ENOTSUP;
}

/**
 * Create a thread for JACK or one of its clients.  The thread is
 * created executing @a start_routine with @a arg as its sole
 * argument.
 *
 * @param client the JACK client for whom the thread is being created. May be
 * NULL if the client is being created within the JACK server.
 * @param thread place to return POSIX thread ID.
 * @param priority thread priority, if realtime.
 * @param realtime true for the thread to use realtime scheduling.  On
 * some systems that may require special privileges.
 * @param start_routine function the thread calls when it starts.
 * @param arg parameter passed to the @a start_routine.
 *
 * @returns 0, if successful; otherwise some error number.
 */
SPA_EXPORT
int jack_client_create_thread (jack_client_t* client,
                               jack_native_thread_t *thread,
                               int priority,
                               int realtime,	/* boolean */
                               void *(*start_routine)(void*),
                               void *arg)
{
	if (globals.creator == NULL)
		globals.creator = pthread_create;

	pw_log_info("client %p: create thread", client);
	return globals.creator(thread, NULL, start_routine, arg);
}

SPA_EXPORT
int jack_drop_real_time_scheduling (jack_native_thread_t thread)
{
	pw_log_warn("not implemented %lu", thread);
	return -ENOTSUP;
}

SPA_EXPORT
int jack_client_stop_thread(jack_client_t* client, jack_native_thread_t thread)
{
	void* status;

	if (thread == (jack_native_thread_t)NULL)
		return -1;

	pw_log_debug("join thread %lu", thread);
        pthread_join(thread, &status);
	pw_log_debug("stopped thread %lu", thread);
	return 0;
}

SPA_EXPORT
int jack_client_kill_thread(jack_client_t* client, jack_native_thread_t thread)
{
	void* status;

	if (thread == (jack_native_thread_t)NULL)
		return -1;

	pw_log_debug("cancel thread %lu", thread);
        pthread_cancel(thread);
	pw_log_debug("join thread %lu", thread);
        pthread_join(thread, &status);
	pw_log_debug("stopped thread %lu", thread);
	return 0;
}

SPA_EXPORT
void jack_set_thread_creator (jack_thread_creator_t creator)
{
	if (creator == NULL)
		globals.creator = pthread_create;
	else
		globals.creator = creator;
}

static inline uint8_t * midi_event_data (void* port_buffer,
                      const struct midi_event* event)
{
        if (event->size <= MIDI_INLINE_MAX)
                return (uint8_t *)event->inline_data;
        else
                return SPA_MEMBER(port_buffer, event->byte_offset, uint8_t);
}

SPA_EXPORT
uint32_t jack_midi_get_event_count(void* port_buffer)
{
	struct midi_buffer *mb = port_buffer;
	return mb->event_count;
}

SPA_EXPORT
int jack_midi_event_get(jack_midi_event_t *event,
			void        *port_buffer,
			uint32_t    event_index)
{
	struct midi_buffer *mb = port_buffer;
	struct midi_event *ev = SPA_MEMBER(mb, sizeof(*mb), struct midi_event);
	ev += event_index;
	event->time = ev->time;
	event->size = ev->size;
	event->buffer = midi_event_data (port_buffer, ev);
	return 0;
}

SPA_EXPORT
void jack_midi_clear_buffer(void *port_buffer)
{
	struct midi_buffer *mb = port_buffer;
	mb->event_count = 0;
	mb->write_pos = 0;
	mb->lost_events = 0;
}

SPA_EXPORT
void jack_midi_reset_buffer(void *port_buffer)
{
	jack_midi_clear_buffer(port_buffer);
}

SPA_EXPORT
size_t jack_midi_max_event_size(void* port_buffer)
{
	struct midi_buffer *mb = port_buffer;
	size_t buffer_size = mb->buffer_size;

        /* (event_count + 1) below accounts for jack_midi_port_internal_event_t
         * which would be needed to store the next event */
        size_t used_size = sizeof(struct midi_buffer)
                           + mb->write_pos
                           + ((mb->event_count + 1)
                              * sizeof(struct midi_event));

        if (used_size > buffer_size) {
                return 0;
        } else if ((buffer_size - used_size) < MIDI_INLINE_MAX) {
                return MIDI_INLINE_MAX;
        } else {
                return buffer_size - used_size;
        }
}

SPA_EXPORT
jack_midi_data_t* jack_midi_event_reserve(void *port_buffer,
                        jack_nframes_t  time,
                        size_t data_size)
{
	struct midi_buffer *mb = port_buffer;
	struct midi_event *events = SPA_MEMBER(mb, sizeof(*mb), struct midi_event);
	size_t buffer_size = mb->buffer_size;

	if (time < 0 || time >= mb->nframes) {
		pw_log_warn("midi %p: time:%d frames:%d", port_buffer, time, mb->nframes);
		goto failed;
	}

	if (mb->event_count > 0 && time < events[mb->event_count - 1].time) {
		pw_log_warn("midi %p: time:%d ev:%d", port_buffer, time, mb->event_count);
		goto failed;
	}

	/* Check if data_size is >0 and there is enough space in the buffer for the event. */
	if (data_size <= 0) {
		pw_log_warn("midi %p: data_size:%zd", port_buffer, data_size);
		goto failed; // return NULL?
	} else if (jack_midi_max_event_size (port_buffer) < data_size) {
		pw_log_warn("midi %p: event too large: data_size:%zd", port_buffer, data_size);
		goto failed;
	} else {
		struct midi_event *ev = &events[mb->event_count];
		uint8_t *res;

		ev->time = time;
		ev->size = data_size;
		if (data_size <= MIDI_INLINE_MAX) {
			res = ev->inline_data;
		} else {
			mb->write_pos += data_size;
			ev->byte_offset = buffer_size - 1 - mb->write_pos;
			res = SPA_MEMBER(mb, ev->byte_offset, uint8_t);
		}
		mb->event_count += 1;
		return res;
	}
failed:
	mb->lost_events++;
	return NULL;
}

SPA_EXPORT
int jack_midi_event_write(void *port_buffer,
                      jack_nframes_t time,
                      const jack_midi_data_t *data,
                      size_t data_size)
{
	jack_midi_data_t *retbuf = jack_midi_event_reserve (port_buffer, time, data_size);
        if (retbuf) {
                memcpy (retbuf, data, data_size);
                return 0;
        } else {
                return ENOBUFS;
        }
}

SPA_EXPORT
uint32_t jack_midi_get_lost_event_count(void *port_buffer)
{
	struct midi_buffer *mb = port_buffer;
	return mb->lost_events;
}

static void reg(void) __attribute__ ((constructor));
static void reg(void)
{
	pw_init(NULL, NULL);
}
