/* PipeWire ROC module integration tests.
 * SPDX-License-Identifier: MIT
 *
 * test_roc_sink
 *   - libpipewire-module-roc-sink is loaded pointing at loopback ports.
 *   - A PipeWire output stream pushes per-channel sine tones into the sink.
 *   - An in-process roc_receiver (separate roc_context) bound to those
 *     ports verifies the recovered samples against strict spectral and
 *     phase-continuity thresholds.
 *
 * test_roc_source
 *   - libpipewire-module-roc-source is loaded listening on loopback ports.
 *   - An in-process roc_sender (separate roc_context) writes per-channel
 *     sine tones to those ports.
 *   - A PipeWire input stream captures the source output and verifies it
 *     against the same strict thresholds.
 *
 * Test signal
 *   FL = 1000 Hz, FR = 1500 Hz, both bin-aligned to a 4410-sample analysis
 *   window at 44.1 kHz (k=100 / k=150). The bin alignment is what makes the
 *   strict spectral leakage thresholds achievable: a clean signal yields
 *   exactly zero leakage at every other integer bin in the same window.
 *
 * Pass criteria (per analysis window, after warmup)
 *   - On-bin amplitude within ±0.5 dB of nominal (-12 dBFS).
 *   - Cross-channel leakage at the other channel's nominal bin <= -60 dB.
 *   - Off-bin leakage at f_nominal ±50 Hz <= -50 dB (catches drift / SRC).
 *   - Goertzel phase relative to the locked first-window phase <= 2°
 *     (a single dropped/repeated sample is rejected: it shifts FL by
 *     2π*100/4410 ≈ 8.16° and FR by ≈ 12.24°).
 *
 * Resampler is intentionally disabled end-to-end: the same RATE is set on
 * the PipeWire stream format and on the ROC frame_encoding on both sides,
 * with ROC_CLOCK_SOURCE_INTERNAL. No SRC engages; any spectral or phase
 * deviation is therefore a real bug in the wire path.
 *
 * L16 note: ROC's AVP L16 stereo packet encoding is signed 16-bit
 * (range -32768..32767). The float→int16→float round trip introduces
 * ~1/32768 ≈ 3e-5 per-sample quantisation noise (~ -90 dBFS), well below
 * every threshold above; no per-sample tolerance is needed.
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

/* Analysis window: 100 ms at 44.1 kHz; bin width = RATE/WIN = 10 Hz. */
#define WIN               4410

/* Per-channel sine frequencies, both bin-aligned to WIN.
 * 1000 Hz → k=100, 1500 Hz → k=150. */
#define FREQ_FL           1000.0
#define FREQ_FR           1500.0

/* Nominal amplitude: -12 dBFS. Headroom for any module gain staging and
 * comfortably inside L16 range. */
#define AMP               0.25

/* Strict spectral thresholds (relative to nominal AMP). */
#define ON_BIN_TOL_DB     0.5
#define CROSS_CHAN_DB    (-60.0)
#define OFF_BIN_DB       (-50.0)

/* Off-bin probe offset around each channel's own frequency. */
#define OFF_BIN_HZ        50.0

/* Phase-continuity tolerance: 2°. A single dropped or repeated sample
 * shifts FL phase by 2π·100/4410 ≈ 8.16° / FR by ≈ 12.24°, so 2° is well
 * below the smallest single-sample defect we want to catch. */
#define PHASE_TOL_RAD     (2.0 * M_PI / 180.0)

/* Ports for the sink test (module-roc-sink sends TO these) */
#define SINK_SRC_PORT     10151   /* RTP audio */
#define SINK_CTRL_PORT    10153   /* RTCP control */

/* Ports for the source test (module-roc-source listens ON these) */
#define SRC_SRC_PORT      10161   /* RTP audio */
#define SRC_CTRL_PORT     10163   /* RTCP control */

/* Receiver target latency for both the test-side roc_receiver and the
 * source module.  Keep generous so the buffer fills reliably in CI. */
#define LATENCY_MS        500

/* Warmup: skip first LATENCY_SKIP_SEC after analyzer start before scoring.
 * Covers LATENCY_MS plus session-manager link-up wiggle. */
#define LATENCY_SKIP_SEC  1.5

/* How long to drive the PipeWire main loop per test. */
#define RUN_SEC           6.0

/* Samples per channel per roc_receiver_read / sender write. */
#define FRAMES_PER_CALL   1024

/* Minimum number of post-warmup analysis windows required for the test
 * to be considered statistically valid. RUN_SEC=6.0 - LATENCY_SKIP_SEC=1.5
 * = 4.5 s of scoring time → ~45 windows nominal. */
#define MIN_WINDOWS_AFTER_WARMUP 30

/* Required fraction of post-warmup windows passing all spectral checks. */
#define MIN_PASS_RATIO    0.99

/* Exit code recognised by meson as "test skipped". */
#define EXIT_SKIP         77

/* 
 * Utilities
 **/

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

/*
 * Test signal generator + spectral / phase-continuity analyzer
 **/

/* Fill `frames` interleaved stereo F32 samples starting at producer-relative
 * sample index `start_idx`. The signal is bin-aligned to WIN so successive
 * non-overlapping WIN-sized windows have identical Goertzel phase. */
static void gen_sine_frames(float *dst, uint32_t frames, uint64_t start_idx)
{
	const double wfl = 2.0 * M_PI * FREQ_FL / (double)RATE;
	const double wfr = 2.0 * M_PI * FREQ_FR / (double)RATE;
	uint32_t i;
	for (i = 0; i < frames; i++) {
		double n = (double)(start_idx + i);
		dst[2 * i + 0] = (float)(AMP * sin(wfl * n));
		dst[2 * i + 1] = (float)(AMP * sin(wfr * n));
	}
}

/* Single-frequency Goertzel over a real-valued buffer.
 * amp_out: linear amplitude estimate (unit sine on-bin returns ~1.0).
 * phase_out: radians, atan2(im, re); pass NULL to skip. */
static void goertzel(const float *x, int n, double freq, double rate,
		double *amp_out, double *phase_out)
{
	const double w     = 2.0 * M_PI * freq / rate;
	const double cw    = cos(w);
	const double sw    = sin(w);
	const double coeff = 2.0 * cw;
	double s0, s1 = 0.0, s2 = 0.0;
	int i;
	for (i = 0; i < n; i++) {
		s0 = (double)x[i] + coeff * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	double re = s1 - s2 * cw;
	double im = s2 * sw;
	*amp_out = 2.0 * sqrt(re * re + im * im) / (double)n;
	if (phase_out)
		*phase_out = atan2(im, re);
}

static double wrap_pi(double a)
{
	while (a >   M_PI) a -= 2.0 * M_PI;
	while (a <= -M_PI) a += 2.0 * M_PI;
	return a;
}

struct analyzer {
	/* deinterleaved per-channel window buffers */
	float fl[WIN];
	float fr[WIN];
	int   fill;             /* 0..WIN */

	/* warmup gate */
	double t_start;         /* monotonic_sec at construction */

	/* phase lock (set on the first post-warmup window) */
	int    locked;
	double phi0_fl, phi0_fr;

	/* counters */
	int windows_total;
	int windows_after_warmup;
	int windows_passed;
	int spectral_failures;
	int continuity_failures;

	/* worst-case metrics for diagnostics */
	double worst_amp_dev_db_fl;
	double worst_amp_dev_db_fr;
	double worst_cross_db_fl;   /* leak at FR's bin within FL channel */
	double worst_cross_db_fr;   /* leak at FL's bin within FR channel */
	double worst_offbin_db;
	double worst_phase_err_deg;
};

static void analyzer_init(struct analyzer *a)
{
	memset(a, 0, sizeof(*a));
	a->t_start = monotonic_sec();
	a->worst_cross_db_fl = -INFINITY;
	a->worst_cross_db_fr = -INFINITY;
	a->worst_offbin_db   = -INFINITY;
}

static int analyzer_check_window(struct analyzer *a, int score)
{
	const double nominal_db = 20.0 * log10(AMP);
	double amp_fl, ph_fl, amp_fr, ph_fr;
	double cross_fl, cross_fr;
	double off_fl_lo, off_fl_hi, off_fr_lo, off_fr_hi;

	/* On-bin amplitudes + phases */
	goertzel(a->fl, WIN, FREQ_FL, RATE, &amp_fl, &ph_fl);
	goertzel(a->fr, WIN, FREQ_FR, RATE, &amp_fr, &ph_fr);

	/* Cross-channel leakage: FL probed at FR's bin and vice versa. */
	goertzel(a->fl, WIN, FREQ_FR, RATE, &cross_fl, NULL);
	goertzel(a->fr, WIN, FREQ_FL, RATE, &cross_fr, NULL);

	/* Off-bin leakage at ±OFF_BIN_HZ around each channel's own bin. */
	goertzel(a->fl, WIN, FREQ_FL - OFF_BIN_HZ, RATE, &off_fl_lo, NULL);
	goertzel(a->fl, WIN, FREQ_FL + OFF_BIN_HZ, RATE, &off_fl_hi, NULL);
	goertzel(a->fr, WIN, FREQ_FR - OFF_BIN_HZ, RATE, &off_fr_lo, NULL);
	goertzel(a->fr, WIN, FREQ_FR + OFF_BIN_HZ, RATE, &off_fr_hi, NULL);

#define DBREL(x) (20.0 * log10((x) + 1e-12) - nominal_db)
	double amp_fl_db   = DBREL(amp_fl);
	double amp_fr_db   = DBREL(amp_fr);
	double cross_fl_db = DBREL(cross_fl);
	double cross_fr_db = DBREL(cross_fr);
	double off_max     = fmax(fmax(off_fl_lo, off_fl_hi),
				   fmax(off_fr_lo, off_fr_hi));
	double off_max_db  = DBREL(off_max);
#undef DBREL

	int spectral_ok =
		fabs(amp_fl_db) <= ON_BIN_TOL_DB &&
		fabs(amp_fr_db) <= ON_BIN_TOL_DB &&
		cross_fl_db    <= CROSS_CHAN_DB &&
		cross_fr_db    <= CROSS_CHAN_DB &&
		off_max_db     <= OFF_BIN_DB;

	double phase_err_deg = 0.0;
	int continuity_ok = 1;
	if (!a->locked) {
		a->phi0_fl = ph_fl;
		a->phi0_fr = ph_fr;
		a->locked  = 1;
	} else {
		double e_fl = fabs(wrap_pi(ph_fl - a->phi0_fl));
		double e_fr = fabs(wrap_pi(ph_fr - a->phi0_fr));
		double e    = fmax(e_fl, e_fr);
		phase_err_deg = e * 180.0 / M_PI;
		continuity_ok = (e <= PHASE_TOL_RAD);
	}

	if (!score)
		return spectral_ok && continuity_ok;

	if (fabs(amp_fl_db) > a->worst_amp_dev_db_fl) a->worst_amp_dev_db_fl = fabs(amp_fl_db);
	if (fabs(amp_fr_db) > a->worst_amp_dev_db_fr) a->worst_amp_dev_db_fr = fabs(amp_fr_db);
	if (cross_fl_db    > a->worst_cross_db_fl)    a->worst_cross_db_fl    = cross_fl_db;
	if (cross_fr_db    > a->worst_cross_db_fr)    a->worst_cross_db_fr    = cross_fr_db;
	if (off_max_db     > a->worst_offbin_db)      a->worst_offbin_db      = off_max_db;
	if (phase_err_deg  > a->worst_phase_err_deg)  a->worst_phase_err_deg  = phase_err_deg;

	if (!spectral_ok)   a->spectral_failures++;
	if (!continuity_ok) a->continuity_failures++;
	return spectral_ok && continuity_ok;
}

/* Feed `frames` interleaved stereo float samples into the analyzer.
 * Runs Goertzel + continuity checks on every full WIN-sized window. */
static void analyzer_feed(struct analyzer *a, const float *src, uint32_t frames)
{
	uint32_t i = 0;
	while (i < frames) {
		uint32_t take = (uint32_t)(WIN - a->fill);
		uint32_t avail = frames - i;
		uint32_t j;
		if (take > avail)
			take = avail;
		for (j = 0; j < take; j++) {
			a->fl[a->fill + j] = src[2 * (i + j) + 0];
			a->fr[a->fill + j] = src[2 * (i + j) + 1];
		}
		a->fill += (int)take;
		i       += take;
		if (a->fill < WIN)
			break;

		int post_warmup =
			(monotonic_sec() - a->t_start) >= LATENCY_SKIP_SEC;
		int passed = analyzer_check_window(a, post_warmup);
		a->windows_total++;
		if (post_warmup) {
			a->windows_after_warmup++;
			if (passed)
				a->windows_passed++;
		} else {
			/* Pre-warmup: do not score and do not lock phase yet;
			 * the locked reference must come from the first
			 * post-warmup window. */
			a->locked = 0;
		}
		a->fill = 0;
	}
}

static int analyzer_verdict(struct analyzer *a, const char *tag)
{
	int ok = 1;
	double pass_ratio = (a->windows_after_warmup > 0)
		? (double)a->windows_passed / a->windows_after_warmup
		: 0.0;

	if (a->windows_after_warmup < MIN_WINDOWS_AFTER_WARMUP) {
		fprintf(stderr, "%s: FAIL – only %d post-warmup windows (need %d)\n",
			tag, a->windows_after_warmup, MIN_WINDOWS_AFTER_WARMUP);
		ok = 0;
	}
	if (pass_ratio < MIN_PASS_RATIO) {
		fprintf(stderr, "%s: FAIL – pass_ratio=%.4f < %.4f\n",
			tag, pass_ratio, MIN_PASS_RATIO);
		ok = 0;
	}
	if (a->continuity_failures > 0) {
		fprintf(stderr, "%s: FAIL – continuity_failures=%d (must be 0)\n",
			tag, a->continuity_failures);
		ok = 0;
	}

	fprintf(stderr,
		"%s: windows_total=%d post_warmup=%d passed=%d "
		"spectral_fail=%d continuity_fail=%d "
		"worst_amp_dev_db=[FL=%.3f FR=%.3f] "
		"worst_cross_db=[FL@FR=%.2f FR@FL=%.2f] "
		"worst_offbin_db=%.2f worst_phase_err_deg=%.3f "
		"pass_ratio=%.4f\n",
		tag, a->windows_total, a->windows_after_warmup, a->windows_passed,
		a->spectral_failures, a->continuity_failures,
		a->worst_amp_dev_db_fl, a->worst_amp_dev_db_fr,
		a->worst_cross_db_fl, a->worst_cross_db_fr,
		a->worst_offbin_db, a->worst_phase_err_deg,
		pass_ratio);
	fprintf(stderr, "%s: %s\n", tag, ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}

/*
 * test_roc_sink
 *
 * Data path:
 *   [PW output stream sines] → PW graph → [module-roc-sink roc_sender]
 *       → UDP loopback → [test-side roc_receiver thread]
 *                               ↓
 *                  Goertzel + phase-continuity analyzer
 **/

struct sink_ctx {
	/* PipeWire */
	struct pw_main_loop   *loop;
	struct pw_context     *pw_ctx;
	struct pw_impl_module *module;
	struct pw_stream      *stream;
	uint32_t               stride;   /* bytes per interleaved sample pair */
	/* generator state (PW thread only) */
	uint64_t               next_sample_idx;
	/* ROC (separate context, as required) */
	roc_context  *roc_ctx;
	roc_receiver *receiver;
	/* receiver thread + analyzer */
	pthread_t    recv_thread;
	volatile int stop;
	struct analyzer ana;          /* owned by recv_thread only */
	atomic_int recv_error;
};

/* PW stream process callback: produce per-channel sine tones. */
static void sink_stream_process(void *userdata)
{
	struct sink_ctx *ctx = userdata;
	struct pw_buffer *b;
	struct spa_buffer *sb;
	float *dst;
	uint32_t n;

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

	gen_sine_frames(dst, n, ctx->next_sample_idx);
	ctx->next_sample_idx += n;

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

/* ROC receiver thread: read frames and feed the analyzer. */
static void *recv_thread_fn(void *arg)
{
	struct sink_ctx *ctx = arg;
	const size_t buf_bytes = FRAMES_PER_CALL * CHANNELS * sizeof(float);
	float *buf = malloc(buf_bytes);
	roc_frame frame;

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
		analyzer_feed(&ctx->ana, buf, FRAMES_PER_CALL);
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
	analyzer_init(&ctx.ana);

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
	/* Disable ROC's adaptive latency tuner / clock-recovery resampler.
	 * Sender and receiver run at the same nominal rate with INTERNAL
	 * clocks, so any drift is below the per-window phase tolerance and
	 * we want strictly bit-stable behavior in the test. */
	rc.latency_tuner_profile = ROC_LATENCY_TUNER_PROFILE_INTACT;
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
	 * PW output stream: produces the per-channel sine signal.
	 * PW_KEY_TARGET_OBJECT directs the session manager to link it to the
	 * test-roc-sink node created by the module above.
	 * PW_KEY_NODE_RATE pins the graph rate: without it PipeWire defaults
	 * to 48 kHz and inserts a resampler between this 44.1 kHz output
	 * stream and the module's 44.1 kHz capture, defeating the
	 * "resampler off" invariant.
	 */
	ctx.stream = pw_stream_new_simple(
		pw_main_loop_get_loop(ctx.loop),
		"test-roc-sink-source",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE,     "Audio",
			PW_KEY_MEDIA_CATEGORY, "Playback",
			PW_KEY_TARGET_OBJECT,  "test-roc-sink",
			PW_KEY_NODE_RATE,      "1/" SPA_STRINGIFY(RATE),
			"node.lock-quantum",   "true",
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

	if (atomic_load(&ctx.recv_error)) {
		fprintf(stderr, "roc_sink: FAIL – receiver error\n");
		ret = 1;
	} else {
		ret = analyzer_verdict(&ctx.ana, "roc_sink");
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

/*
 * test_roc_source
 *
 * Data path:
 *   [test-side roc_sender sines] → UDP loopback
 *       → [module-roc-source roc_receiver] → PW graph
 *           → [PW input stream capture]
 *                   ↓
 *          Goertzel + phase-continuity analyzer
 **/

struct source_ctx {
	/* PipeWire */
	struct pw_main_loop   *loop;
	struct pw_context     *pw_ctx;
	struct pw_impl_module *module;
	struct pw_stream      *stream;
	uint32_t               stride;
	/* analyzer (PW main-loop thread only) */
	struct analyzer ana;
	/* ROC (separate context) */
	roc_context *roc_ctx;
	roc_sender  *sender;
	/* sender thread */
	pthread_t    send_thread;
	volatile int stop;
	atomic_int   send_error;
};

/* PW stream process callback: feed captured samples into the analyzer. */
static void source_stream_process(void *userdata)
{
	struct source_ctx *ctx = userdata;
	struct pw_buffer  *b;
	struct spa_buffer *sb;
	const float *src;
	uint32_t n;

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

	analyzer_feed(&ctx->ana, src, n);

done:
	pw_stream_queue_buffer(ctx->stream, b);
}

static const struct pw_stream_events source_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = source_stream_process,
};

/* ROC sender thread: write per-channel sine frames continuously. */
static void *send_thread_fn(void *arg)
{
	struct source_ctx *ctx = arg;
	const size_t buf_bytes = FRAMES_PER_CALL * CHANNELS * sizeof(float);
	float *buf = malloc(buf_bytes);
	roc_frame frame;
	uint64_t next_idx = 0;

	if (!buf) {
		atomic_store(&ctx->send_error, 1);
		return NULL;
	}

	memset(&frame, 0, sizeof(frame));
	frame.samples      = buf;
	frame.samples_size = buf_bytes;

	while (!ctx->stop) {
		gen_sine_frames(buf, FRAMES_PER_CALL, next_idx);
		next_idx += FRAMES_PER_CALL;
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
	analyzer_init(&ctx.ana);

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
		/* Disable ROC's adaptive latency tuner inside the module's
		 * receiver: matched rates, INTERNAL clocks, no SRC desired. */
		"roc.latency-tuner.profile=intact "
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
	 * PW_KEY_TARGET_OBJECT links it to test-roc-source via the session
	 * manager. PW_KEY_NODE_RATE pins the graph rate (see test_roc_sink).
	 */
	ctx.stream = pw_stream_new_simple(
		pw_main_loop_get_loop(ctx.loop),
		"test-roc-source-sink",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE,     "Audio",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_TARGET_OBJECT,  "test-roc-source",
			PW_KEY_NODE_RATE,      "1/" SPA_STRINGIFY(RATE),
			"node.lock-quantum",   "true",
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

	if (atomic_load(&ctx.send_error)) {
		fprintf(stderr, "roc_source: FAIL – sender error\n");
		ret = 1;
	} else {
		ret = analyzer_verdict(&ctx.ana, "roc_source");
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

/*
 * main
 **/

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
