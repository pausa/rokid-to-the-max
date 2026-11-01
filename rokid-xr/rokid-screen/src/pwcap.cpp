#include "pwcap.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/video/raw-utils.h>
#include <spa/utils/result.h>
#include <atomic>
#include <cstdio>
#include <thread>

extern std::atomic<bool> running; // defined in main.cpp

namespace pwcap {

struct State {
	pw_thread_loop *loop = nullptr;
	pw_stream *stream = nullptr;
	pw_stream_events events{};
	long frames = 0;
	bool verbose = false;
	spa_video_info_raw neg_format{};
	uint32_t node = 0;
	FrameCb cb = nullptr;
	void *ud = nullptr;
};

static uint32_t
bpp_for(uint32_t fmt)
{
	switch (fmt) {
	case SPA_VIDEO_FORMAT_BGRx:
	case SPA_VIDEO_FORMAT_RGBx:
	case SPA_VIDEO_FORMAT_BGRA:
	case SPA_VIDEO_FORMAT_RGBA:
		return 4;
	case SPA_VIDEO_FORMAT_RGB16:
		return 2;
	default:
		return 4;
	}
}

static void on_state_changed(void *d, pw_stream_state old, pw_stream_state state, const char *error) {
	fprintf(stderr, "[pwcap] state %s -> %s%s%s\n",
		pw_stream_state_as_string(old), pw_stream_state_as_string(state),
		error ? ": " : "", error ? error : "");
}

static void on_param_changed(void *d, uint32_t id, const struct spa_pod *param) {
	auto *s = (State *)d;
	if (!param || id != SPA_PARAM_Format)
		return;
	spa_format_video_raw_parse(param, &s->neg_format);
	const char *fn = s->neg_format.format == SPA_VIDEO_FORMAT_BGRx ? "BGRx"
		       : s->neg_format.format == SPA_VIDEO_FORMAT_RGBx ? "RGBx"
		       : s->neg_format.format == SPA_VIDEO_FORMAT_BGRA ? "BGRA"
		       : s->neg_format.format == SPA_VIDEO_FORMAT_RGBA ? "RGBA" : "?";
	fprintf(stderr, "[pwcap] negotiated: fmt=%s %ux%u @%u/%u fps\n", fn,
		s->neg_format.size.width, s->neg_format.size.height,
		s->neg_format.framerate.num, s->neg_format.framerate.denom);
}

static void on_process(void *d) {
	auto *s = (State *)d;
	pw_buffer *b = pw_stream_dequeue_buffer(s->stream);
	if (!b)
		return;
	spa_buffer *buf = b->buffer;
	s->frames++;
	if (s->cb && buf->n_datas > 0 && buf->datas[0].data) {
		spa_data *d = &buf->datas[0];
		uint32_t bpp = bpp_for(s->neg_format.format);
		uint32_t stride = d->chunk && d->chunk->stride ? (uint32_t)d->chunk->stride
							       : s->neg_format.size.width * bpp;
		s->cb(s->ud, (const uint8_t *)d->data + (d->chunk ? d->chunk->offset : 0),
		      s->neg_format.size.width, s->neg_format.size.height, stride,
		      s->neg_format.format);
	}
	if (s->verbose && (s->frames % 60) == 0 && buf->n_datas > 0) {
		spa_data *data = &buf->datas[0];
		fprintf(stderr, "[pwcap] frame %ld: %ux%u chunk=%u bytes fd=%d\n", s->frames,
			s->neg_format.size.width, s->neg_format.size.height,
			data->chunk ? data->chunk->size : 0, data->type == SPA_DATA_DmaBuf ? data->fd : -1);
	}
	pw_stream_queue_buffer(s->stream, b);
}

static const pw_stream_events make_events() {
	pw_stream_events ev{};
	ev.version = PW_VERSION_STREAM_EVENTS;
	ev.param_changed = on_param_changed;
	ev.process = on_process;
	ev.state_changed = on_state_changed;
	return ev;
}

static std::thread g_thr;

long
run_stream(State *sp)
{
	State &s = *sp;
	s.events = make_events();

	pw_init(nullptr, nullptr);
	s.loop = pw_thread_loop_new("pwcap", nullptr);

	char target[32];
	snprintf(target, sizeof(target), "%u", s.node);
	pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE, "Video",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_TARGET_OBJECT, target,
		nullptr);

	s.stream = pw_stream_new_simple(pw_thread_loop_get_loop(s.loop), "rokid-screen", props, &s.events, &s);

	uint8_t podbuf[1024];
	spa_pod_builder b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
	const spa_pod *params[2];
	// offer BGRx first (matches our Vulkan swapchain 44), then RGBx fallback
	params[0] = (const spa_pod *)spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		// prefer RGB; YUY2 as last resort
		SPA_FORMAT_VIDEO_format,
		SPA_POD_CHOICE_ENUM_Id(7, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
			SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA,
			SPA_VIDEO_FORMAT_RGB16, SPA_VIDEO_FORMAT_YUY2),
		0);

	// screencast nodes provide dmabuf/memfd buffers; declare what we accept
	params[1] = (const spa_pod *)spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
			(1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_DmaBuf)),
		0);

	int rc = pw_stream_connect(s.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
				   (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
				   params, 2);
	if (rc < 0) {
		fprintf(stderr, "[pwcap] connect failed: %s\n", spa_strerror(rc));
		return -1;
	}

	pw_thread_loop_start(s.loop);
	if (s.cb)
		fprintf(stderr, "[pwcap] streaming from node %u\n", s.node);
	else
		fprintf(stderr, "[pwcap] streaming from node %u - Ctrl-C to stop\n", s.node);
	while (running.load()) {
		struct timespec ts{0, 100 * 1000 * 1000};
		nanosleep(&ts, nullptr);
	}
	long n = s.frames;
	if (s.verbose)
		fprintf(stderr, "[pwcap] done, %ld frames\n", n);

	pw_thread_loop_lock(s.loop);
	pw_stream_destroy(s.stream);
	pw_thread_loop_unlock(s.loop);
	pw_thread_loop_destroy(s.loop);
	pw_deinit();
	return n;
}

long start_stream(uint32_t node, bool verbose, FrameCb cb, void *ud) {
	static State s; // one stream at a time
	s = State{};
	s.verbose = verbose;
	s.node = node;
	s.cb = cb;
	s.ud = ud;
	g_thr = std::thread([sp = &s] { run_stream(sp); });
	return 0;
}

void stop_stream() {
	if (g_thr.joinable())
		g_thr.join();
}

long capture_stats(uint32_t node, bool verbose) {
	State s{};
	s.verbose = verbose;
	s.node = node;
	return run_stream(&s);
}

} // namespace pwcap
