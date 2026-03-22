/* PipeWire ROC module integration tests.
 * SPDX-License-Identifier: MIT
 *
 * test_roc_sink
 *   A roc_receiver is bound to loopback ports in a separate roc_context.
 *   libpipewire-module-roc-sink is loaded pointing at those ports.
 *   A PipeWire output stream feeds constant TEST_SAMPLE (0.5f) frames into
 *   the sink.  The test verifies that the roc_receiver receives the same
 *   samples after the initial latency fill (no FEC, no resampling on the
 *   sender side, L16 wire encoding → quantisation tolerance SAMPLE_TOL).
 *
 * test_roc_source
 *   libpipewire-module-roc-source is loaded and bound to loopback ports.
 *   A roc_sender in a separate roc_context sends constant TEST_SAMPLE frames
 *   to those ports.  A PipeWire input stream captures the source output and
 *   verifies the same samples arrive after the initial latency fill.
 *
 * Requirements
 *   - Running PipeWire daemon reachable on the default socket.
 *   - Running session manager (wireplumber) for automatic stream linking.
 *   - libpipewire-module-roc-sink / roc-source findable in PIPEWIRE_MODULE_DIR.
 *   If the daemon is unreachable the tests exit with EXIT_SKIP (77).
 */

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <roc/context.h>
#include <roc/endpoint.h>
#include <roc/frame.h>
#include <roc/log.h>
#include <roc/receiver.h>
#include <roc/sender.h>

#include <spa/param/audio/format-utils.h>
#include <spa/utils/defs.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>

/* -------------------------------------------------------------------------
 * Test parameters
 * ---------------------------------------------------------------------- */

#define RATE              44100
#define CHANNELS          2
#define TEST_SAMPLE       0.5f
/* AVP L16 round-trip: 0.5 * 32768 = 16384 → 16384/32768 = 0.5 exactly,
 * but allow a small tolerance for implementation variations. */
#define SAMPLE_TOL        0.002f

/* Ports for the sink test (module-roc-sink sends TO these) */
#define SINK_SRC_PORT     10151   /* RTP audio */
#define SINK_CTRL_PORT    10153   /* RTCP control */

/* Ports for the source test (module-roc-source listens ON these) */
#define SRC_SRC_PORT      10161   /* RTP audio */
#define SRC_CTRL_PORT     10163   /* RTCP control */

/* Receiver target latency for both the test-side roc_receiver and the
 * source module.  Keep generous so the buffer fills reliably in CI. */
#define LATENCY_MS        500

/* How long to drive the PipeWire main loop per test. */
#define RUN_SEC           6.0

/* Samples per channel per roc_receiver_read / sender write. */
#define FRAMES_PER_CALL   1024

/* Minimum number of received frames where ALL samples ≈ TEST_SAMPLE
 * needed to declare a test passing. */
#define MIN_GOOD_FRAMES   5

/* Exit code recognised by meson as "test skipped". */
#define EXIT_SKIP         77

/* -------------------------------------------------------------------------
 * Utilities
 * ---------------------------------------------------------------------- */

static double monotonic_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int make_endpoint(roc_endpoint **out, roc_protocol proto,
		const char *ip, int port)
{
	roc_endpoint *ep;
	if (roc_endpoint_allocate(&ep) != 0)
		return -1;
	if (roc_endpoint_set_protocol(ep, proto) != 0 ||
	    roc_endpoint_set_host(ep, ip) != 0 ||
	    roc_endpoint_set_port(ep, port) != 0) {
		roc_endpoint_deallocate(ep);
		return -1;
	}
	*out = ep;
	return 0;
}

static int recv_bind(roc_receiver *recv, roc_interface iface,
		roc_protocol proto, const char *ip, int port)
{
	roc_endpoint *ep;
	int r;
	if (make_endpoint(&ep, proto, ip, port) != 0)
		return -1;
	r = roc_receiver_bind(recv, ROC_SLOT_DEFAULT, iface, ep);
	roc_endpoint_deallocate(ep);
	return r;
}

static int sender_connect(roc_sender *sndr, roc_interface iface,
		roc_protocol proto, const char *ip, int port)
{
	roc_endpoint *ep;
	int r;
	if (make_endpoint(&ep, proto, ip, port) != 0)
		return -1;
	r = roc_sender_connect(sndr, ROC_SLOT_DEFAULT, iface, ep);
	roc_endpoint_deallocate(ep);
	return r;
}

/* Probe: connect to the PipeWire daemon and verify it responds to a sync
 * round-trip.  A 2-second timer guards against hanging indefinitely. */

struct probe_data {
	struct pw_main_loop *loop;
	int result;   /* 0 = daemon alive, -1 = error / timeout */
};

static void probe_done_cb(void *data, uint32_t id, int seq)
{
	struct probe_data *d = data;
	d->result = 0;
	pw_main_loop_quit(d->loop);
}

static void probe_error_cb(void *data, uint32_t id, int seq,
		int res, const char *message)
{
	struct probe_data *d = data;
	(void)res; (void)message;
	d->result = -1;
	pw_main_loop_quit(d->loop);
}

static void probe_timeout_cb(void *data, uint64_t expirations)
{
	struct probe_data *d = data;
	(void)expirations;
	d->result = -1;
	pw_main_loop_quit(d->loop);
}

static const struct pw_core_events probe_core_events = {
	PW_VERSION_CORE_EVENTS,
	.done  = probe_done_cb,
	.error = probe_error_cb,
};

static int probe_pw_daemon(void)
{
	struct pw_main_loop *loop;
	struct pw_context   *ctx;
	struct pw_core      *core;
	struct spa_hook      core_listener;
	struct spa_source   *timer;
	struct timespec      timeout = { .tv_sec = 2, .tv_nsec = 0 };
	struct probe_data    pd = { .result = -1 };

	loop = pw_main_loop_new(NULL);
	if (!loop)
		return -1;
	pd.loop = loop;

	ctx = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
	if (!ctx) {
		pw_main_loop_destroy(loop);
		return -1;
	}

	core = pw_context_connect(ctx, NULL, 0);
	if (!core) {
		pw_context_destroy(ctx);
		pw_main_loop_destroy(loop);
		return -1;
	}

	pw_core_add_listener(core, &core_listener, &probe_core_events, &pd);
	pw_core_sync(core, PW_ID_CORE, 0);

	timer = pw_loop_add_timer(pw_main_loop_get_loop(loop),
			probe_timeout_cb, &pd);
	pw_loop_update_timer(pw_main_loop_get_loop(loop), timer,
			&timeout, NULL, false);

	pw_main_loop_run(loop);

	spa_hook_remove(&core_listener);
	pw_core_disconnect(core);
	pw_context_destroy(ctx);
	pw_main_loop_destroy(loop);
	return pd.result;
}

/* =========================================================================
 * test_roc_sink
 *
 * Data path:
 *   [PW output stream 0.5f] → PW graph → [module-roc-sink roc_sender]
 *       → UDP loopback → [test-side roc_receiver thread]
 *                               ↓
 *                        verify ~0.5f samples
 * ======================================================================= */

struct sink_ctx {
	/* PipeWire */
	struct pw_main_loop   *loop;
	struct pw_context     *pw_ctx;
	struct pw_impl_module *module;
	struct pw_stream      *stream;
	uint32_t               stride;   /* bytes per interleaved sample pair */
	/* ROC (separate context, as required) */
	roc_context  *roc_ctx;
	roc_receiver *receiver;
	/* thread */
	pthread_t    recv_thread;
	volatile int stop;
	/* results */
	atomic_int frames_read;
	atomic_int frames_passed;
	atomic_int recv_error;
};

/* PW stream process callback: fill the output buffer with TEST_SAMPLE. */
static void sink_stream_process(void *userdata)
{
	struct sink_ctx *ctx = userdata;
	struct pw_buffer *b;
	struct spa_buffer *sb;
	float *dst;
	uint32_t n, i;

	b = pw_stream_dequeue_buffer(ctx->stream);
	if (!b)
		return;

	sb  = b->buffer;
	dst = sb->datas[0].data;
	if (!dst)
		goto done;

	n = sb->datas[0].maxsize / ctx->stride;
	if (b->requested)
		n = SPA_MIN(n, (uint32_t)b->requested);

	for (i = 0; i < n * CHANNELS; i++)
		dst[i] = TEST_SAMPLE;

	sb->datas[0].chunk->offset = 0;
	sb->datas[0].chunk->stride = (int32_t)ctx->stride;
	sb->datas[0].chunk->size   = n * ctx->stride;
done:
	pw_stream_queue_buffer(ctx->stream, b);
}

static const struct pw_stream_events sink_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = sink_stream_process,
};

/* ROC receiver thread: read frames and check sample values. */
static void *recv_thread_fn(void *arg)
{
	struct sink_ctx *ctx = arg;
	const size_t buf_bytes = FRAMES_PER_CALL * CHANNELS * sizeof(float);
	float *buf = malloc(buf_bytes);
	roc_frame frame;
	uint32_t i;
	int good;

	if (!buf) {
		atomic_store(&ctx->recv_error, 1);
		return NULL;
	}

	memset(&frame, 0, sizeof(frame));
	frame.samples      = buf;
	frame.samples_size = buf_bytes;

	while (!ctx->stop) {
		if (roc_receiver_read(ctx->receiver, &frame) != 0) {
			atomic_store(&ctx->recv_error, 1);
			break;
		}
		atomic_fetch_add(&ctx->frames_read, 1);

		/* All samples should be TEST_SAMPLE once the latency is filled. */
		good = 1;
		for (i = 0; i < FRAMES_PER_CALL * CHANNELS; i++) {
			if (fabsf(buf[i] - TEST_SAMPLE) > SAMPLE_TOL) {
				good = 0;
				break;
			}
		}
		if (good)
			atomic_fetch_add(&ctx->frames_passed, 1);
	}

	free(buf);
	return NULL;
}

static int test_roc_sink(void)
{
	struct sink_ctx ctx;
	struct spa_audio_info_raw ainfo;
	const struct spa_pod *params[1];
	uint8_t pod_buf[1024];
	struct spa_pod_builder b;
	roc_context_config  rcc;
	roc_receiver_config rc;
	double t_start;
	int ret = 0;

	memset(&ctx, 0, sizeof(ctx));
	ctx.stride = CHANNELS * sizeof(float);

	/* ---- ROC receiver in a separate context ---- */
	memset(&rcc, 0, sizeof(rcc));
	if (roc_context_open(&rcc, &ctx.roc_ctx) != 0) {
		fprintf(stderr, "roc_sink: roc_context_open failed\n");
		return 1;
	}

	memset(&rc, 0, sizeof(rc));
	rc.frame_encoding.rate     = RATE;
	rc.frame_encoding.format   = ROC_FORMAT_PCM_FLOAT32;
	rc.frame_encoding.channels = ROC_CHANNEL_LAYOUT_STEREO;
	/* Internal clock: roc_receiver_read blocks until a full frame is ready. */
	rc.clock_source  = ROC_CLOCK_SOURCE_INTERNAL;
	rc.target_latency = (unsigned long long)LATENCY_MS * 1000000ULL;

	if (roc_receiver_open(ctx.roc_ctx, &rc, &ctx.receiver) != 0) {
		fprintf(stderr, "roc_sink: roc_receiver_open failed\n");
		roc_context_close(ctx.roc_ctx);
		return 1;
	}

	if (recv_bind(ctx.receiver, ROC_INTERFACE_AUDIO_SOURCE,
			ROC_PROTO_RTP, "127.0.0.1", SINK_SRC_PORT) != 0 ||
	    recv_bind(ctx.receiver, ROC_INTERFACE_AUDIO_CONTROL,
			ROC_PROTO_RTCP, "127.0.0.1", SINK_CTRL_PORT) != 0) {
		fprintf(stderr, "roc_sink: receiver bind failed\n");
		roc_receiver_close(ctx.receiver);
		roc_context_close(ctx.roc_ctx);
		return 1;
	}

	/* ---- PipeWire ---- */
	ctx.loop   = pw_main_loop_new(NULL);
	ctx.pw_ctx = pw_context_new(pw_main_loop_get_loop(ctx.loop), NULL, 0);

	/* The module internally calls pw_context_connect to reach the daemon. */
	ctx.module = pw_context_load_module(ctx.pw_ctx,
		"libpipewire-module-roc-sink",
		"remote.ip=127.0.0.1 "
		"remote.source.port=" SPA_STRINGIFY(SINK_SRC_PORT) " "
		"remote.repair.port=10152 "
		"remote.control.port=" SPA_STRINGIFY(SINK_CTRL_PORT) " "
		"fec.code=disable "
		"audio.position=[ FL FR ] "
		"sink.props = { node.name=test-roc-sink }",
		NULL);
	if (!ctx.module) {
		fprintf(stderr, "roc_sink: module load failed\n");
		ret = 1;
		goto out_pw;
	}

	/*
	 * Let the module's node propagate to the daemon and become visible to
	 * WirePlumber before we register the output stream.  WirePlumber
	 * evaluates PW_KEY_TARGET_OBJECT only when a stream first appears; if
	 * the sink node does not exist yet it may never retry, causing 0 frames
	 * to pass.  500 ms is more than enough for the round-trip.
	 */
	{
		double t_settle = monotonic_sec() + 0.5;
		while (monotonic_sec() < t_settle)
			pw_loop_iterate(pw_main_loop_get_loop(ctx.loop), 10);
	}

	/*
	 * PW output stream: produces TEST_SAMPLE audio.
	 * PW_KEY_TARGET_OBJECT directs the session manager to link it to the
	 * test-roc-sink node created by the module above.
	 */
	ctx.stream = pw_stream_new_simple(
		pw_main_loop_get_loop(ctx.loop),
		"test-roc-sink-source",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE,     "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_TARGET_OBJECT,  "test-roc-sink",
			NULL),
		&sink_stream_events, &ctx);
	if (!ctx.stream) {
		fprintf(stderr, "roc_sink: stream create failed\n");
		ret = 1;
		goto out_pw;
	}

	memset(&ainfo, 0, sizeof(ainfo));
	ainfo.format   = SPA_AUDIO_FORMAT_F32;
	ainfo.rate     = RATE;
	ainfo.channels = CHANNELS;

	spa_pod_builder_init(&b, pod_buf, sizeof(pod_buf));
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &ainfo);

	pw_stream_connect(ctx.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
		PW_STREAM_FLAG_RT_PROCESS,
		params, 1);

	if (pthread_create(&ctx.recv_thread, NULL, recv_thread_fn, &ctx) != 0) {
		fprintf(stderr, "roc_sink: pthread_create failed\n");
		ret = 1;
		goto out_pw;
	}

	t_start = monotonic_sec();
	while (monotonic_sec() - t_start < RUN_SEC)
		pw_loop_iterate(pw_main_loop_get_loop(ctx.loop), 100);

	ctx.stop = 1;
	pthread_join(ctx.recv_thread, NULL);

	fprintf(stderr, "roc_sink: frames_read=%d frames_passed=%d recv_error=%d\n",
		atomic_load(&ctx.frames_read),
		atomic_load(&ctx.frames_passed),
		atomic_load(&ctx.recv_error));

	if (atomic_load(&ctx.recv_error)) {
		fprintf(stderr, "roc_sink: FAIL – receiver error\n");
		ret = 1;
	} else if (atomic_load(&ctx.frames_passed) < MIN_GOOD_FRAMES) {
		fprintf(stderr, "roc_sink: FAIL – %d good frames, need %d\n",
			atomic_load(&ctx.frames_passed), MIN_GOOD_FRAMES);
		ret = 1;
	} else {
		fprintf(stderr, "roc_sink: PASS\n");
	}

out_pw:
	if (ctx.stream)
		pw_stream_destroy(ctx.stream);
	if (ctx.module)
		pw_impl_module_destroy(ctx.module);
	/* Drain: flush the node-removed / disconnect messages to the daemon so
	 * the test-roc-sink node is gone before the next test run.  Without
	 * this the daemon keeps the stale node and WirePlumber may link the
	 * next run's output stream to the dead node, causing frames_passed=0. */
	{
		double t_drain = monotonic_sec() + 0.3;
		while (monotonic_sec() < t_drain)
			pw_loop_iterate(pw_main_loop_get_loop(ctx.loop), 10);
	}
	pw_context_destroy(ctx.pw_ctx);
	pw_main_loop_destroy(ctx.loop);

	roc_receiver_close(ctx.receiver);
	roc_context_close(ctx.roc_ctx);
	return ret;
}

/* =========================================================================
 * test_roc_source
 *
 * Data path:
 *   [test-side roc_sender 0.5f] → UDP loopback
 *       → [module-roc-source roc_receiver] → PW graph
 *           → [PW input stream capture]
 *                   ↓
 *            verify ~0.5f samples
 * ======================================================================= */

struct source_ctx {
	/* PipeWire */
	struct pw_main_loop   *loop;
	struct pw_context     *pw_ctx;
	struct pw_impl_module *module;
	struct pw_stream      *stream;
	uint32_t               stride;
	/* ROC (separate context) */
	roc_context *roc_ctx;
	roc_sender  *sender;
	/* thread */
	pthread_t    send_thread;
	volatile int stop;
	/* results */
	atomic_int frames_captured;
	atomic_int frames_passed;
	atomic_int send_error;
};

/* PW stream process callback: inspect the captured samples. */
static void source_stream_process(void *userdata)
{
	struct source_ctx *ctx = userdata;
	struct pw_buffer  *b;
	struct spa_buffer *sb;
	const float *src;
	uint32_t n, i;
	int good;

	b = pw_stream_dequeue_buffer(ctx->stream);
	if (!b)
		return;

	sb  = b->buffer;
	src = sb->datas[0].data;
	if (!src || sb->datas[0].chunk->size == 0)
		goto done;

	n = sb->datas[0].chunk->size / ctx->stride;
	if (n == 0)
		goto done;

	atomic_fetch_add(&ctx->frames_captured, 1);

	good = 1;
	for (i = 0; i < n * CHANNELS; i++) {
		if (fabsf(src[i] - TEST_SAMPLE) > SAMPLE_TOL) {
			good = 0;
			break;
		}
	}
	if (good)
		atomic_fetch_add(&ctx->frames_passed, 1);

done:
	pw_stream_queue_buffer(ctx->stream, b);
}

static const struct pw_stream_events source_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = source_stream_process,
};

/* ROC sender thread: write TEST_SAMPLE frames continuously. */
static void *send_thread_fn(void *arg)
{
	struct source_ctx *ctx = arg;
	const size_t buf_bytes = FRAMES_PER_CALL * CHANNELS * sizeof(float);
	float *buf = malloc(buf_bytes);
	roc_frame frame;
	uint32_t i;

	if (!buf) {
		atomic_store(&ctx->send_error, 1);
		return NULL;
	}

	for (i = 0; i < FRAMES_PER_CALL * CHANNELS; i++)
		buf[i] = TEST_SAMPLE;

	memset(&frame, 0, sizeof(frame));
	frame.samples      = buf;
	frame.samples_size = buf_bytes;

	while (!ctx->stop) {
		if (roc_sender_write(ctx->sender, &frame) != 0) {
			atomic_store(&ctx->send_error, 1);
			break;
		}
	}

	free(buf);
	return NULL;
}

static int test_roc_source(void)
{
	struct source_ctx ctx;
	struct spa_audio_info_raw ainfo;
	const struct spa_pod *params[1];
	uint8_t pod_buf[1024];
	struct spa_pod_builder b;
	roc_context_config rcc;
	roc_sender_config  sc;
	double t_start;
	int ret = 0;

	memset(&ctx, 0, sizeof(ctx));
	ctx.stride = CHANNELS * sizeof(float);

	/* ---- ROC sender in a separate context ---- */
	memset(&rcc, 0, sizeof(rcc));
	if (roc_context_open(&rcc, &ctx.roc_ctx) != 0) {
		fprintf(stderr, "roc_source: roc_context_open failed\n");
		return 1;
	}

	memset(&sc, 0, sizeof(sc));
	sc.frame_encoding.rate     = RATE;
	sc.frame_encoding.format   = ROC_FORMAT_PCM_FLOAT32;
	sc.frame_encoding.channels = ROC_CHANNEL_LAYOUT_STEREO;
	sc.fec_encoding            = ROC_FEC_ENCODING_DISABLE;
	/* Internal clock: roc_sender_write paces itself at the nominal rate. */
	sc.clock_source    = ROC_CLOCK_SOURCE_INTERNAL;
	/* Standard AVP L16 stereo (PT=10), decodable by any RTP receiver. */
	sc.packet_encoding = ROC_PACKET_ENCODING_AVP_L16_STEREO;

	if (roc_sender_open(ctx.roc_ctx, &sc, &ctx.sender) != 0) {
		fprintf(stderr, "roc_source: roc_sender_open failed\n");
		roc_context_close(ctx.roc_ctx);
		return 1;
	}

	if (sender_connect(ctx.sender, ROC_INTERFACE_AUDIO_SOURCE,
			ROC_PROTO_RTP, "127.0.0.1", SRC_SRC_PORT) != 0 ||
	    sender_connect(ctx.sender, ROC_INTERFACE_AUDIO_CONTROL,
			ROC_PROTO_RTCP, "127.0.0.1", SRC_CTRL_PORT) != 0) {
		fprintf(stderr, "roc_source: sender connect failed\n");
		roc_sender_close(ctx.sender);
		roc_context_close(ctx.roc_ctx);
		return 1;
	}

	/* ---- PipeWire ---- */
	ctx.loop   = pw_main_loop_new(NULL);
	ctx.pw_ctx = pw_context_new(pw_main_loop_get_loop(ctx.loop), NULL, 0);

	ctx.module = pw_context_load_module(ctx.pw_ctx,
		"libpipewire-module-roc-source",
		"local.ip=127.0.0.1 "
		"local.source.port=" SPA_STRINGIFY(SRC_SRC_PORT) " "
		"local.repair.port=10162 "
		"local.control.port=" SPA_STRINGIFY(SRC_CTRL_PORT) " "
		"fec.code=disable "
		"sess.latency.msec=" SPA_STRINGIFY(LATENCY_MS) " "
		"audio.position=[ FL FR ] "
		"source.props = { node.name=test-roc-source }",
		NULL);
	if (!ctx.module) {
		fprintf(stderr, "roc_source: module load failed\n");
		ret = 1;
		goto out_pw;
	}

	/* Same settling wait as in test_roc_sink: let the module's source node
	 * appear in the daemon before we create the capturing input stream. */
	{
		double t_settle = monotonic_sec() + 0.5;
		while (monotonic_sec() < t_settle)
			pw_loop_iterate(pw_main_loop_get_loop(ctx.loop), 10);
	}

	/*
	 * PW input stream: captures audio produced by the source module.
	 * PW_KEY_TARGET_OBJECT links it to test-roc-source via the session manager.
	 */
	ctx.stream = pw_stream_new_simple(
		pw_main_loop_get_loop(ctx.loop),
		"test-roc-source-sink",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE,     "Audio",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_TARGET_OBJECT,  "test-roc-source",
			NULL),
		&source_stream_events, &ctx);
	if (!ctx.stream) {
		fprintf(stderr, "roc_source: stream create failed\n");
		ret = 1;
		goto out_pw;
	}

	memset(&ainfo, 0, sizeof(ainfo));
	ainfo.format   = SPA_AUDIO_FORMAT_F32;
	ainfo.rate     = RATE;
	ainfo.channels = CHANNELS;

	spa_pod_builder_init(&b, pod_buf, sizeof(pod_buf));
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &ainfo);

	pw_stream_connect(ctx.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
		PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
		PW_STREAM_FLAG_RT_PROCESS,
		params, 1);

	if (pthread_create(&ctx.send_thread, NULL, send_thread_fn, &ctx) != 0) {
		fprintf(stderr, "roc_source: pthread_create failed\n");
		ret = 1;
		goto out_pw;
	}

	t_start = monotonic_sec();
	while (monotonic_sec() - t_start < RUN_SEC)
		pw_loop_iterate(pw_main_loop_get_loop(ctx.loop), 100);

	ctx.stop = 1;
	pthread_join(ctx.send_thread, NULL);

	fprintf(stderr, "roc_source: frames_captured=%d frames_passed=%d send_error=%d\n",
		atomic_load(&ctx.frames_captured),
		atomic_load(&ctx.frames_passed),
		atomic_load(&ctx.send_error));

	if (atomic_load(&ctx.send_error)) {
		fprintf(stderr, "roc_source: FAIL – sender error\n");
		ret = 1;
	} else if (atomic_load(&ctx.frames_passed) < MIN_GOOD_FRAMES) {
		fprintf(stderr, "roc_source: FAIL – %d good frames, need %d\n",
			atomic_load(&ctx.frames_passed), MIN_GOOD_FRAMES);
		ret = 1;
	} else {
		fprintf(stderr, "roc_source: PASS\n");
	}

out_pw:
	if (ctx.stream)
		pw_stream_destroy(ctx.stream);
	if (ctx.module)
		pw_impl_module_destroy(ctx.module);
	/* Drain: flush node-removed / disconnect messages to the daemon. */
	{
		double t_drain = monotonic_sec() + 0.3;
		while (monotonic_sec() < t_drain)
			pw_loop_iterate(pw_main_loop_get_loop(ctx.loop), 10);
	}
	pw_context_destroy(ctx.pw_ctx);
	pw_main_loop_destroy(ctx.loop);

	roc_sender_close(ctx.sender);
	roc_context_close(ctx.roc_ctx);
	return ret;
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
	int r;

	roc_log_set_level(ROC_LOG_NONE);
	pw_init(NULL, NULL);

	if (probe_pw_daemon() != 0) {
		fprintf(stderr, "PipeWire daemon not available – skipping tests\n");
		pw_deinit();
		return EXIT_SKIP;
	}

	r = test_roc_sink();
	if (r != 0 && r != EXIT_SKIP) {
		pw_deinit();
		return r;
	}

	r = test_roc_source();
	pw_deinit();
	return r;
}
