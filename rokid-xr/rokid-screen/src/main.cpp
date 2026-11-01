// rokid-screen - head-locked desktop screen for the Rokid Max glasses.
//
// Captures a Wayland output via wlr-screencopy (Hyprland, Sway, ...) and
// copies it every frame into both eyes of an OpenXR swapchain served by
// the bundled Monado runtime. No shaders, no mirror window: the desktop
// IS the swapchain image (transfer-only upload, letterboxed, black bars
// where aspect ratios differ).
//
// The image is anchored to the LOCAL reference space, so the driver's
// double-tap recenter gesture moves the screen with the world.
//
// Env:
//   ROKID_SCREEN_TEST=1   force animated test pattern (no Wayland needed)
//   ROKID_SCREEN_FPS=n    capture rate (default 30)
//   ROKID_SCREEN_SCALE=f  screen fill factor (default 1.0 = fill FOV)
//   ROKID_LOG=DEBUG       more logging

#define XR_USE_GRAPHICS_API_VULKAN 1
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include "portal.hpp"
#include "pwcap.hpp"
#include <spa/param/video/raw-utils.h>
#include <openxr/openxr_platform.h>
#include <wayland-client.h>
#include "screencopy-client-protocol.h"

#include "shaders/vert_spv.h"
#include "shaders/frag_spv.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <signal.h>
#include <string>
#include <list>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef _MSC_VER
#define strdup _strdup
#endif

#define P(...) fprintf(stderr, "[rokid-screen] " __VA_ARGS__)
static bool g_trace = false;
#define T(...)                                                                                                    \
	do {                                                                                                          \
		if (g_trace)                                                                                              \
			P(__VA_ARGS__);                                                                                       \
	} while (0)
#define FAIL(...)                                                                                                 \
	do {                                                                                                          \
		fprintf(stderr, "[rokid-screen] FATAL: " __VA_ARGS__);                                                    \
		fprintf(stderr, "\n");                                                                                    \
		exit(1);                                                                                                  \
	} while (0)

#define OXR(x)                                                                                                    \
	do {                                                                                                          \
		XrResult _r = (x);                                                                                        \
		if (XR_FAILED(_r)) {                                                                                      \
			FAIL("OpenXR call failed (code %d): %s", (int)_r, #x);                                                \
		}                                                                                                         \
	} while (0)

// enable2 functions are not declared by default; resolve via proc addr.
static PFN_xrCreateVulkanInstanceKHR pCreateVulkanInstanceKHR;
static PFN_xrCreateVulkanDeviceKHR pCreateVulkanDeviceKHR;
static PFN_xrGetVulkanGraphicsDevice2KHR pGetVulkanGraphicsDevice2KHR;
static PFN_xrGetVulkanGraphicsRequirements2KHR pGetVulkanGraphicsRequirements2KHR;

static void
resolve_enable2(XrInstance instance)
{
	struct Entry {
		const char *name;
		PFN_xrVoidFunction *pfn;
	} table[] = {
	    {"xrCreateVulkanInstanceKHR", reinterpret_cast<PFN_xrVoidFunction *>(&pCreateVulkanInstanceKHR)},
	    {"xrCreateVulkanDeviceKHR", reinterpret_cast<PFN_xrVoidFunction *>(&pCreateVulkanDeviceKHR)},
	    {"xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction *>(&pGetVulkanGraphicsDevice2KHR)},
	    {"xrGetVulkanGraphicsRequirements2KHR", reinterpret_cast<PFN_xrVoidFunction *>(&pGetVulkanGraphicsRequirements2KHR)},
	};
	for (auto &e : table) {
		if (XR_FAILED(xrGetInstanceProcAddr(instance, e.name, e.pfn)) || !*e.pfn) {
			FAIL("runtime does not provide %s", e.name);
		}
	}
}

#include <chrono>
static double
now_s()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

std::atomic<bool> running{true}; // extern-shared with pwcap.cpp
static void
on_sigint(int)
{
	running = false;
}

static bool
env_flag(const char *name, bool def)
{
	const char *v = getenv(name);
	return v ? (v[0] == '0' ? false : true) : def;
}

static int
env_int(const char *name, int def)
{
	const char *v = getenv(name);
	return v ? atoi(v) : def;
}

static float
env_float(const char *name, float def)
{
	const char *v = getenv(name);
	return v ? (float)atof(v) : def;
}

// ---------------------------------------------------------------------------
// Shared latest-frame slot between the capture thread and the render loop.
// ---------------------------------------------------------------------------
struct FrameSlot {
	std::mutex mtx;
	std::condition_variable cv;
	std::vector<uint8_t> pixels; // BGRA/XRGB, packed rows (stride compacted)
	uint32_t width = 0, height = 0;
	uint64_t seq = 0;
};

// ---------------------------------------------------------------------------
// Wayland wlr-screencopy capture thread (falls back to a test pattern).
// ---------------------------------------------------------------------------
struct Capture {
	FrameSlot slot;
	std::thread thread;
	int fps = 30;

	void
	start()
	{
		thread = std::thread([this] { run(); });
	}

	void
	stop()
	{
		running = false;
		{
			std::lock_guard<std::mutex> lk(slot.mtx);
			slot.cv.notify_all();
		}
		if (thread.joinable())
			thread.join();
	}

  private:
void
	publish(const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride, bool r_first = false)
	{
		std::lock_guard<std::mutex> lk(slot.mtx);
		size_t n = (size_t)w * h * 4;
		slot.width = w;
		slot.height = h;
		slot.pixels.resize(n);
		// compact stride; *BGR formats store R first but our texture is
		// B8G8R8A8 (B first) - swap R/B while copying.
		if (stride == w * 4 && !r_first) {
			memcpy(slot.pixels.data(), src, n);
		} else {
			for (uint32_t y = 0; y < h; y++) {
				const uint8_t *sr = src + (size_t)y * stride;
				uint8_t *dst = slot.pixels.data() + (size_t)y * w * 4;
				if (!r_first) {
					memcpy(dst, sr, (size_t)w * 4);
				} else {
					for (uint32_t x = 0; x < w; x++) {
						dst[x * 4 + 0] = sr[x * 4 + 2];
						dst[x * 4 + 1] = sr[x * 4 + 1];
						dst[x * 4 + 2] = sr[x * 4 + 0];
						dst[x * 4 + 3] = 255;
					}
				}
			}
		}
		slot.seq++;
		slot.cv.notify_all();
	}

	// periodic content sanity: is the capture actually black?
	uint64_t dbg_frames = 0;

	void
	run()
	{
		if (env_flag("ROKID_SCREEN_TEST", false)) {
			run_test_pattern();
			return;
		}
		const char *src = getenv("ROKID_SCREEN_SOURCE"); // wayland|portal
		bool want_wayland = src ? strcmp(src, "wayland") == 0 : true;
		if (want_wayland) {
			if (try_wayland()) {
				return;
			}
			P("Wayland capture unavailable\n");
			if (src) { // explicitly requested -> no fallback
				run_test_pattern();
				return;
			}
		}
		// fallback: xdg-desktop-portal ScreenCast -> PipeWire (GNOME et al)
		long node = -1;
		if (const char *n = getenv("ROKID_SCREEN_NODE"))
			node = atol(n); // reuse a previous node id, skips the consent dialog
		if (node < 0) {
			node = portal::screencast_node(g_trace);
		}
		if (node >= 0) {
			pwcap::start_stream((uint32_t)node, g_trace, on_pw_frame, this);
			pwcap::stop_stream(); // returns when `running` goes false
			return;
		}
		P("portal capture unavailable, using test pattern\n");
		run_test_pattern();
	}

	// ---- pipewire frame -> publish ----------------------------------------
	static void
	on_pw_frame(void *ud, const uint8_t *data, uint32_t w, uint32_t h, uint32_t stride, uint32_t fmt)
	{
		auto *c = (Capture *)ud;
		switch (fmt) {
		case SPA_VIDEO_FORMAT_BGRx:
		case SPA_VIDEO_FORMAT_BGRA:
			c->publish(data, w, h, stride, false); // B,G,R,x - memcpy path
			break;
		case SPA_VIDEO_FORMAT_RGBx:
		case SPA_VIDEO_FORMAT_RGBA:
			c->publish(data, w, h, stride, true);
			break;
		case SPA_VIDEO_FORMAT_RGB16: {
			c->pw_scratch.resize((size_t)w * h * 4);
			for (uint32_t y = 0; y < h; y++) {
				const uint8_t *sr = data + (size_t)y * stride;
				uint8_t *dst = c->pw_scratch.data() + (size_t)y * w * 4;
				for (uint32_t x = 0; x < w; x++) {
					uint16_t v = (uint16_t)(sr[x * 2] | (sr[x * 2 + 1] << 8));
					dst[x * 4 + 0] = (uint8_t)(((v & 0x001f) << 3) | ((v >> 2) & 7));
					dst[x * 4 + 1] = (uint8_t)(((v & 0x07e0) >> 3) | ((v >> 9) & 3));
					dst[x * 4 + 2] = (uint8_t)(((v & 0xf800) >> 8) | ((v >> 13) & 7));
					dst[x * 4 + 3] = 255;
				}
			}
			c->publish(c->pw_scratch.data(), w, h, w * 4, false);
			break;
		}
		case SPA_VIDEO_FORMAT_YUY2: {
			// BT.601 YUV 4:2:2 packed -> BGRA
			c->pw_scratch.resize((size_t)w * h * 4);
			for (uint32_t y = 0; y < h; y++) {
				const uint8_t *sr = data + (size_t)y * stride;
				uint8_t *dst = c->pw_scratch.data() + (size_t)y * w * 4;
				for (uint32_t x = 0; x < w; x += 2) {
					int y0 = sr[x * 2] - 16, y1 = sr[x * 2 + 2] - 16;
					int u = sr[x * 2 + 1] - 128, v = sr[x * 2 + 3] - 128;
					int r = (298 * y0 + 409 * v + 128) >> 8;
					int g0 = (298 * y0 - 100 * u - 208 * v + 128) >> 8;
					int g1 = (298 * y1 - 100 * u - 208 * v + 128) >> 8;
					int b = (298 * y0 + 516 * u + 128) >> 8;
					int r1 = (298 * y1 + 409 * v + 128) >> 8;
					int b1 = (298 * y1 + 516 * u + 128) >> 8;
					dst[x * 4 + 0] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
					dst[x * 4 + 1] = (uint8_t)(g0 < 0 ? 0 : g0 > 255 ? 255 : g0);
					dst[x * 4 + 2] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
					dst[x * 4 + 3] = 255;
					dst[x * 4 + 4] = (uint8_t)(b1 < 0 ? 0 : b1 > 255 ? 255 : b1);
					dst[x * 4 + 5] = (uint8_t)(g1 < 0 ? 0 : g1 > 255 ? 255 : g1);
					dst[x * 4 + 6] = (uint8_t)(r1 < 0 ? 0 : r1 > 255 ? 255 : r1);
					dst[x * 4 + 7] = 255;
				}
			}
			c->publish(c->pw_scratch.data(), w, h, w * 4, false);
			break;
		}
		default:
			P("pw frame: unsupported format %u\n", fmt);
			break;
		}
	}

	std::vector<uint8_t> pw_scratch;

	// ---- real capture ----------------------------------------------------
	bool
	try_wayland()
	{
		wl_display *disp = wl_display_connect(nullptr);
		if (!disp) {
			P("wl_display_connect failed (no WAYLAND_DISPLAY?)\n");
			return false;
		}

		struct OutputInfo {
			wl_output *obj = nullptr;
			uint32_t name_id = 0;
			char name[64] = "?";
			char desc[128] = "";
			uint32_t w = 0, h = 0;
			bool glasses_like = false;
			bool done = false;
		};
		struct Globals {
			wl_shm *shm = nullptr;
			zwlr_screencopy_manager_v1 *mgr = nullptr;
			std::list<OutputInfo> outputs; // stable addresses: Wayland listeners hold pointers
			uint32_t mgr_version = 0;
		} g;

		wl_registry *reg = wl_display_get_registry(disp);
		static wl_output_listener out_listener;
		out_listener.geometry = +[](void *data, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t,
		                            const char *, const char *, int32_t) {
			(void)data;
		};
		out_listener.mode = +[](void *data, wl_output *, uint32_t flags, int32_t w, int32_t h, int32_t) {
			auto o = (OutputInfo *)data;
			if (flags & WL_OUTPUT_MODE_CURRENT) {
				o->w = (uint32_t)w;
				o->h = (uint32_t)h;
			}
		};
		out_listener.done = +[](void *data, wl_output *) { ((OutputInfo *)data)->done = true; };
		out_listener.scale = +[](void *, wl_output *, int32_t) {};
		out_listener.name = +[](void *data, wl_output *, const char *name) {
			snprintf(((OutputInfo *)data)->name, sizeof(((OutputInfo *)data)->name), "%s", name);
		};
		out_listener.description = +[](void *data, wl_output *, const char *desc) {
			snprintf(((OutputInfo *)data)->desc, sizeof(((OutputInfo *)data)->desc), "%s", desc);
		};

		wl_registry_listener rl{};
		rl.global = +[](void *data, wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
			auto g = (Globals *)data;
			if (strcmp(iface, wl_shm_interface.name) == 0) {
				g->shm = (wl_shm *)wl_registry_bind(r, name, &wl_shm_interface, ver < 2 ? ver : 2);
			} else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) {
				g->mgr_version = ver;
				g->mgr = (zwlr_screencopy_manager_v1 *)wl_registry_bind(
				    r, name, &zwlr_screencopy_manager_v1_interface, ver < 3 ? ver : 3);
			} else if (strcmp(iface, wl_output_interface.name) == 0) {
				OutputInfo o;
				o.name_id = name;
				o.obj = (wl_output *)wl_registry_bind(r, name, &wl_output_interface, ver < 4 ? ver : 4);
				if (o.obj) {
					wl_output_add_listener(o.obj, &out_listener, &g->outputs.emplace_back(o));
				}
			}
		};
		rl.global_remove = +[](void *, wl_registry *, uint32_t) {};
		wl_registry_add_listener(reg, &rl, &g);
		wl_display_roundtrip(disp);

		if (!g.mgr || !g.shm || g.outputs.empty()) {
			P("missing globals: %s%s%s\n", g.mgr ? "" : "screencopy-manager ", g.shm ? "" : "shm ",
			  g.outputs.empty() ? "output" : "");
			return false;
		}

		// one more roundtrip so wl_output v4 name/mode events arrive
		wl_display_roundtrip(disp);

		// Rokid Max native portrait-ish panel is 2688x1512 - treat outputs
		// whose only mode that matches as "the glasses" and skip them
		// (capturing the glasses output yields black).
		for (auto &o : g.outputs) {
			o.glasses_like = (o.w == 2688 && o.h == 1512);
		}
		P("outputs:");
		for (auto &o : g.outputs)
			P(" \"%s\" %ux%u%s", o.name, o.w, o.h, o.glasses_like ? " (glasses?)" : "");
		P("\n");

		OutputInfo *chosen = nullptr;
		if (const char *want = getenv("ROKID_SCREEN_OUTPUT")) {
			for (auto &o : g.outputs) {
				if (strcmp(o.name, want) == 0) {
					chosen = &o;
					break;
				}
			}
			if (!chosen)
				P("ROKID_SCREEN_OUTPUT=\"%s\" not found, falling back\n", want);
		}
		if (!chosen) {
			for (auto &o : g.outputs) {
				if (!o.glasses_like && o.w > 0) {
					chosen = &o;
					break;
				}
			}
		}
		if (!chosen && !g.outputs.empty())
			chosen = &g.outputs.front();
		if (!chosen)
			return false;
		P("capturing output \"%s\" (%ux%u)\n", chosen->name, chosen->w, chosen->h);

		// persistent shm: two buffers in one pool, recreated on size change
		struct Shm {
			int fd = -1;
			uint8_t *map = nullptr;
			size_t pool_size = 0;
			wl_shm_pool *pool = nullptr;
			wl_buffer *buf[2] = {nullptr, nullptr};
			uint32_t w = 0, h = 0, stride = 0, format = 0;
		} s;

		auto destroy_shm = [&]() {
			for (auto &b : s.buf) {
				if (b) {
					wl_buffer_destroy(b);
					b = nullptr;
				}
			}
			if (s.pool) {
				wl_shm_pool_destroy(s.pool);
				s.pool = nullptr;
			}
			if (s.map) {
				munmap(s.map, s.pool_size);
				s.map = nullptr;
			}
			if (s.fd >= 0) {
				close(s.fd);
				s.fd = -1;
			}
			s.pool_size = 0;
		};

		auto create_shm = [&](uint32_t w, uint32_t h, uint32_t stride, uint32_t format) -> bool {
			destroy_shm();
			size_t one = (size_t)stride * h;
			s.pool_size = one; // grim-parity: exact-size pool, single buffer at 0
			s.fd = memfd_create("rokid-screen-shm", 0);
			if (s.fd < 0 || ftruncate(s.fd, (off_t)s.pool_size) != 0) {
				P("memfd/ftruncate failed\n");
				return false;
			}
			s.map = (uint8_t *)mmap(nullptr, s.pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, s.fd, 0);
			if (s.map == MAP_FAILED) {
				s.map = nullptr;
				return false;
			}
			s.pool = wl_shm_create_pool(g.shm, s.fd, (int32_t)s.pool_size);
			s.buf[0] = wl_shm_pool_create_buffer(s.pool, 0, w, h, stride, (wl_shm_format)format);
			s.buf[1] = nullptr; // single buffer (grim-parity); [1] kept null
			s.w = w;
			s.h = h;
			s.stride = stride;
			s.format = format;
			return true;
		};

		// frame listener state
		struct FrameState {
			uint32_t w = 0, h = 0, stride = 0, format = 0;
			bool have_buffer = false, ready = false, failed = false;
		} fs;

		wl_buffer *cur_buf = nullptr;
		int cur_idx = 0;

		zwlr_screencopy_frame_v1_listener fl{};
		// NOTE arg order: (format, width, height, stride) - format first!
		fl.buffer = +[](void *data, zwlr_screencopy_frame_v1 *, uint32_t format, uint32_t w, uint32_t h,
		                uint32_t stride) {
			auto fs = (FrameState *)data;
			fs->format = format;
			fs->w = w;
			fs->h = h;
			fs->stride = stride;
			fs->have_buffer = true;
		};
		fl.flags = +[](void *, zwlr_screencopy_frame_v1 *, uint32_t) {};
		fl.ready = +[](void *data, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t) {
			((FrameState *)data)->ready = true;
		};
		fl.failed = +[](void *data, zwlr_screencopy_frame_v1 *) { ((FrameState *)data)->failed = true; };
		fl.damage = +[](void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t, uint32_t) {};
		fl.linux_dmabuf = +[](void *, zwlr_screencopy_frame_v1 *, uint32_t, uint32_t, uint32_t) {};
		fl.buffer_done = +[](void *, zwlr_screencopy_frame_v1 *) {};

		const auto frame_period = std::chrono::microseconds(1000000 / fps);
		auto next = std::chrono::steady_clock::now();
		int consecutive_failures = 0;

		while (running) {
			fs = FrameState{};
			auto *frame = zwlr_screencopy_manager_v1_capture_output(g.mgr, 0, chosen->obj);
			zwlr_screencopy_frame_v1_add_listener(frame, &fl, &fs);

			// wait for the buffer event (gives w/h/stride/format)
			int guard = 0;
			while (running && !fs.have_buffer && !fs.failed && guard++ < 100) {
				if (wl_display_dispatch(disp) < 0) {
					goto out;
				}
			}
			if (!fs.have_buffer) {
				zwlr_screencopy_frame_v1_destroy(frame);
				if (++consecutive_failures > 5) {
					P("capture never produced buffer params, giving up\n");
					goto out;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}

			P("frame params: format=0x%08x %ux%u stride=%u (frame id=%u shm id=%u mgr id=%u)\n",
			  fs.format, fs.w, fs.h, fs.stride, wl_proxy_get_id((wl_proxy *)frame),
			  g.shm ? wl_proxy_get_id((wl_proxy *)g.shm) : 0, g.mgr ? wl_proxy_get_id((wl_proxy *)g.mgr) : 0);
			// niri/smithay sends DRM fourccs (e.g. XR24=0x34325258) without
			// the >=0x80000000 convention - map the common ones to wl_shm enums
			uint32_t wl_fmt = fs.format;
			if (wl_fmt == 0x34325258)
				wl_fmt = WL_SHM_FORMAT_XRGB8888; // 1
			else if (wl_fmt == 0x34324142)
				wl_fmt = WL_SHM_FORMAT_ARGB8888; // 0
			if (fs.stride * fs.h > 0 &&
			    (fs.w != s.w || fs.h != s.h || fs.stride != s.stride || wl_fmt != s.format)) {
				if (!create_shm(fs.w, fs.h, fs.stride, wl_fmt)) {
					P("shm setup failed\n");
					zwlr_screencopy_frame_v1_destroy(frame);
					goto out;
				}
				// let the server see the pool + buffers before we copy into them
				wl_display_roundtrip(disp);
			} else {
				wl_display_flush(disp);
			}

			cur_buf = s.buf[0];
			zwlr_screencopy_frame_v1_copy(frame, cur_buf);

			// wait for ready
			guard = 0;
			while (running && !fs.ready && !fs.failed && guard++ < 200) {
				if (wl_display_dispatch(disp) < 0) {
					goto out;
				}
			}
			zwlr_screencopy_frame_v1_destroy(frame);

			if (fs.ready) {
				const uint8_t *src = s.map + (size_t)cur_idx * (size_t)s.stride * s.h;
				consecutive_failures = 0;
				bool r_first = (s.format == WL_SHM_FORMAT_ABGR8888 || s.format == WL_SHM_FORMAT_XBGR8888);
				if (env_flag("ROKID_SCREEN_SWAP_RB", true))
					r_first = !r_first; // default swap: correct with the .bgr shader; =0 restores
				publish(src, s.w, s.h, s.stride, r_first);
				if (++dbg_frames % 60 == 0) {
					uint32_t mn = 255, mx = 0;
					uint64_t sum = 0, n = 0;
					for (uint32_t y = 0; y < s.h; y += 37) {
						for (uint32_t x = 0; x < s.w; x += 41) {
							const uint8_t *p = src + (size_t)y * s.stride + (size_t)x * 4;
							uint32_t v = (p[0] + p[1] + p[2]) / 3;
							mn = v < mn ? v : mn;
							mx = v > mx ? v : mx;
							sum += v;
							n++;
						}
					}
					P("capture: %ux%u stride=%u min=%u max=%u avg=%u\n", s.w, s.h, s.stride, mn, mx,
					  n ? (uint32_t)(sum / n) : 0);
				}
			} else {
				if (++consecutive_failures > 5) {
					P("capture failed 5 times in a row, giving up\n");
					goto out;
				}
			}

			next += frame_period;
			std::this_thread::sleep_until(next);
		}
	out:
		destroy_shm();
		wl_display_disconnect(disp);
		P("capture thread done\n");
		return true;
	}

	// ---- test pattern ------------------------------------------------------
	void
	run_test_pattern()
	{
		const uint32_t w = 1280, h = 720;
		std::vector<uint8_t> img((size_t)w * h * 4);
		uint64_t frame = 0;
		const auto frame_period = std::chrono::microseconds(1000000 / fps);
		auto next = std::chrono::steady_clock::now();
		P("test pattern %ux%u @%d fps\n", w, h, fps);
		while (running) {
			uint8_t *p = img.data();
			for (uint32_t y = 0; y < h; y++) {
				for (uint32_t x = 0; x < w; x++) {
					uint32_t i = ((size_t)y * w + x) * 4;
					// moving checker + gradient
					uint32_t c = ((x / 80 + y / 80 + frame / 30) & 1) ? 235 : 20;
					p[i + 0] = (uint8_t)(c * (0.3f + 0.7f * x / w));      // B
					p[i + 1] = (uint8_t)(c * (0.3f + 0.7f * y / h));      // G
					p[i + 2] = (uint8_t)c;                                // R
					p[i + 3] = 255;
				}
			}
			publish(img.data(), w, h, w * 4);
			frame++;
			next += frame_period;
			std::this_thread::sleep_until(next);
		}
	}
};

// ---------------------------------------------------------------------------
// Vulkan helpers (upload path only)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Renderer: hello_xr-style render path.
//   capture -> staging buffer -> private texture (transfer)
//   -> fullscreen-triangle pipeline sampling that texture, drawn into the
//   acquired swapchain image via a render pass (COLOR_ATTACHMENT_OPTIMAL in
//   and out, matching Monado's client acquire/release barriers).
// ---------------------------------------------------------------------------
struct Renderer {
	VkDevice device = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	bool in_flight = false;

	// staging
	VkBuffer stage = VK_NULL_HANDLE;
	VkDeviceMemory stage_mem = VK_NULL_HANDLE;
	uint8_t *map = nullptr;
	VkDeviceSize cap = 0;

	// texture
	VkImage tex = VK_NULL_HANDLE;
	VkDeviceMemory tex_mem = VK_NULL_HANDLE;
	VkImageView tex_view = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;
	VkDescriptorPool desc_pool = VK_NULL_HANDLE;
	VkDescriptorSet desc_set = VK_NULL_HANDLE;
	uint32_t tex_w = 0, tex_h = 0;

	// gfx
	VkRenderPass render_pass = VK_NULL_HANDLE;
	VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;

	uint64_t stat_ok = 0, stat_skip_fence = 0, stat_no_content = 0;

	bool
	mem_alloc(VkMemoryRequirements req, VkMemoryPropertyFlags props, VkDeviceMemory *out)
	{
		VkPhysicalDeviceMemoryProperties mp{};
		vkGetPhysicalDeviceMemoryProperties(physical, &mp);
		for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
			if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) {
				VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
				ai.allocationSize = req.size;
				ai.memoryTypeIndex = i;
				return vkAllocateMemory(device, &ai, nullptr, out) == VK_SUCCESS;
			}
		}
		return false;
	}

	void
	init(VkDevice dev, VkPhysicalDevice phys, VkQueue q, uint32_t qf, VkFormat fmt)
	{
		device = dev;
		physical = phys;
		queue = q;
		queue_family = qf;
		format = fmt;

		VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = qf;
		if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS)
			FAIL("vkCreateCommandPool");
		VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS)
			FAIL("vkAllocateCommandBuffers");
		VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (vkCreateFence(dev, &fi, nullptr, &fence) != VK_SUCCESS)
			FAIL("vkCreateFence");

		// render pass: color attachment arrives in COLOR_ATTACHMENT_OPTIMAL
		// (Monado's acquire barrier) and must be left in the same layout
		// (its release barrier expects it).
		VkAttachmentDescription att{};
		att.format = fmt;
		att.samples = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		VkAttachmentReference aref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub{};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments = &aref;
		VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		rpi.attachmentCount = 1;
		rpi.pAttachments = &att;
		rpi.subpassCount = 1;
		rpi.pSubpasses = &sub;
		if (vkCreateRenderPass(dev, &rpi, nullptr, &render_pass) != VK_SUCCESS)
			FAIL("vkCreateRenderPass");

		// shaders
		VkShaderModule vs = load_shader("vert.spv");
		VkShaderModule fs = load_shader("frag.spv");
		VkPipelineShaderStageCreateInfo stages[2] = {};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vs;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fs;
		stages[1].pName = "main";

		VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
		VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pli.pushConstantRangeCount = 1;
		pli.pPushConstantRanges = &pcr;
		VkDescriptorSetLayout dsl = make_desc_layout();
		pli.setLayoutCount = 1;
		pli.pSetLayouts = &dsl;
		if (vkCreatePipelineLayout(dev, &pli, nullptr, &pipe_layout) != VK_SUCCESS)
			FAIL("vkCreatePipelineLayout");

		VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkViewport vp{};
		VkRect2D sc{};
		VkPipelineViewportStateCreateInfo vsi{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vsi.viewportCount = 1;
		vsi.pViewports = &vp; // dynamic
		vsi.scissorCount = 1;
		vsi.pScissors = &sc;
		VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dsi.dynamicStateCount = 2;
		dsi.pDynamicStates = dyns;
		VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode = VK_CULL_MODE_NONE;
		rs.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState cba{};
		cba.colorWriteMask = 0xf;
		VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		cb.attachmentCount = 1;
		cb.pAttachments = &cba;

		VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gpi.stageCount = 2;
		gpi.pStages = stages;
		gpi.pVertexInputState = &vi;
		gpi.pInputAssemblyState = &ia;
		gpi.pViewportState = &vsi;
		gpi.pRasterizationState = &rs;
		gpi.pMultisampleState = &ms;
		gpi.pColorBlendState = &cb;
		gpi.pDynamicState = &dsi;
		gpi.layout = pipe_layout;
		gpi.renderPass = render_pass;
		gpi.subpass = 0;
		if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline) != VK_SUCCESS)
			FAIL("vkCreateGraphicsPipelines");
		vkDestroyShaderModule(dev, vs, nullptr);
		vkDestroyShaderModule(dev, fs, nullptr);

		// sampler
		VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		si.magFilter = VK_FILTER_LINEAR;
		si.minFilter = VK_FILTER_LINEAR;
		si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		if (vkCreateSampler(dev, &si, nullptr, &sampler) != VK_SUCCESS)
			FAIL("vkCreateSampler");

		// descriptor pool + set (filled in ensure_texture)
		VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
		VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		dpi.maxSets = 1;
		dpi.poolSizeCount = 1;
		dpi.pPoolSizes = &ps;
		if (vkCreateDescriptorPool(dev, &dpi, nullptr, &desc_pool) != VK_SUCCESS)
			FAIL("vkCreateDescriptorPool");
		VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
		dsai.descriptorPool = desc_pool;
		dsai.descriptorSetCount = 1;
		dsai.pSetLayouts = &dsl;
		if (vkAllocateDescriptorSets(dev, &dsai, &desc_set) != VK_SUCCESS)
			FAIL("vkAllocateDescriptorSets");
		vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
	}

	VkDescriptorSetLayout
	make_desc_layout()
	{
		VkDescriptorSetLayoutBinding b{};
		b.binding = 0;
		b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		li.bindingCount = 1;
		li.pBindings = &b;
		VkDescriptorSetLayout l;
		if (vkCreateDescriptorSetLayout(device, &li, nullptr, &l) != VK_SUCCESS)
			FAIL("vkCreateDescriptorSetLayout");
		return l;
	}

	VkShaderModule
	load_shader(const char *name)
	{
		const uint32_t *code;
		size_t words;
		if (strcmp(name, "vert.spv") == 0) {
			code = vert_spv;
			words = sizeof(vert_spv) / 4;
		} else {
			code = frag_spv;
			words = sizeof(frag_spv) / 4;
		}
		VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		mi.codeSize = words * 4;
		mi.pCode = code;
		VkShaderModule m;
		if (vkCreateShaderModule(device, &mi, nullptr, &m) != VK_SUCCESS)
			FAIL("vkCreateShaderModule %s", name);
		return m;
	}

	void
	destroy()
	{
		if (device == VK_NULL_HANDLE)
			return;
		free_staging();
		free_texture();
		if (pipeline)
			vkDestroyPipeline(device, pipeline, nullptr);
		if (pipe_layout)
			vkDestroyPipelineLayout(device, pipe_layout, nullptr);
		if (render_pass)
			vkDestroyRenderPass(device, render_pass, nullptr);
		if (sampler)
			vkDestroySampler(device, sampler, nullptr);
		if (desc_pool)
			vkDestroyDescriptorPool(device, desc_pool, nullptr);
		vkDestroyFence(device, fence, nullptr);
		vkDestroyCommandPool(device, pool, nullptr);
	}

  private:
	void
	free_staging()
	{
		if (map) {
			vkUnmapMemory(device, stage_mem);
			map = nullptr;
		}
		if (stage) {
			vkDestroyBuffer(device, stage, nullptr);
			stage = VK_NULL_HANDLE;
		}
		if (stage_mem) {
			vkFreeMemory(device, stage_mem, nullptr);
			stage_mem = VK_NULL_HANDLE;
		}
		cap = 0;
	}

	void
	free_texture()
	{
		if (tex_view) {
			vkDestroyImageView(device, tex_view, nullptr);
			tex_view = VK_NULL_HANDLE;
		}
		if (tex) {
			vkDestroyImage(device, tex, nullptr);
			tex = VK_NULL_HANDLE;
		}
		if (tex_mem) {
			vkFreeMemory(device, tex_mem, nullptr);
			tex_mem = VK_NULL_HANDLE;
		}
		tex_w = tex_h = 0;
	}

	bool
	ensure_staging(VkDeviceSize size)
	{
		if (cap >= size)
			return true;
		free_staging();
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = size;
		bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		if (vkCreateBuffer(device, &bi, nullptr, &stage) != VK_SUCCESS)
			return false;
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device, stage, &req);
		if (!mem_alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		               &stage_mem))
			return false;
		if (vkBindBufferMemory(device, stage, stage_mem, 0) != VK_SUCCESS)
			return false;
		if (vkMapMemory(device, stage_mem, 0, req.size, 0, (void **)&map) != VK_SUCCESS)
			return false;
		cap = size;
		return true;
	}

	bool
	ensure_texture(uint32_t w, uint32_t h)
	{
		if (tex_w == w && tex_h == h)
			return true;
		free_texture();
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = format; // same as swapchain (SRGB sampling = identity)
		ii.extent = {w, h, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(device, &ii, nullptr, &tex) != VK_SUCCESS)
			return false;
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(device, tex, &req);
		if (!mem_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tex_mem))
			return false;
		if (vkBindImageMemory(device, tex, tex_mem, 0) != VK_SUCCESS)
			return false;
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = tex;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if (vkCreateImageView(device, &vi, nullptr, &tex_view) != VK_SUCCESS)
			return false;
		VkDescriptorImageInfo dii{sampler, tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		wr.dstSet = desc_set;
		wr.descriptorCount = 1;
		wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr.pImageInfo = &dii;
		vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
		tex_w = w;
		tex_h = h;
		return true;
	}

  public:
	// Copies the latest captured frame into the texture and draws it into
	// both eye images. Returns false if the previous frame's submit is
	// still in flight (skip, don't stall).
	bool
	render(FrameSlot &slot, VkFramebuffer fb_l, VkFramebuffer fb_r, uint32_t iw, uint32_t ih, float scale)
	{
		if (in_flight) {
			if (vkWaitForFences(device, 1, &fence, VK_TRUE, 8'000'000) != VK_SUCCESS) {
				stat_skip_fence++;
				return false;
			}
			in_flight = false;
		}
		vkResetFences(device, 1, &fence);

		{
			std::lock_guard<std::mutex> lk(slot.mtx);
			if (slot.seq == 0 || slot.pixels.empty()) {
				stat_no_content++;
				return false;
			}
			VkDeviceSize bytes = (VkDeviceSize)slot.pixels.size();
			if (!ensure_staging(bytes) || !ensure_texture(slot.width, slot.height))
				return false;
			memcpy(map, slot.pixels.data(), (size_t)bytes);

			// letterbox: map the swapchain viewport to a sub-uv-window of
			// the source so the aspect ratio is preserved (black bars via
			// the fragment shader's uv bounds check).
			float sx = (float)iw / slot.width;
			float sy = (float)ih / slot.height;
			float s = sx < sy ? sx : sy;
			(void)scale;
			// shrink around the center; scale<1 letterboxes further
			float uw = s / sx * scale;
			float uh = s / sy * scale;
			float u0 = (1.0f - uw) * 0.5f;
			float v0 = (1.0f - uh) * 0.5f;

			VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			vkBeginCommandBuffer(cmd, &bi);

			// texture upload
			VkImageMemoryBarrier tb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			tb.image = tex;
			tb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			tb.srcQueueFamilyIndex = tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			tb.srcAccessMask = 0;
			tb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			tb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			tb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
			                     nullptr, 0, nullptr, 1, &tb);
			VkBufferImageCopy c{};
			c.bufferRowLength = slot.width;
			c.bufferImageHeight = slot.height;
			c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			c.imageExtent = {slot.width, slot.height, 1};
			vkCmdCopyBufferToImage(cmd, stage, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
			tb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			tb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			tb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			tb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
			                     nullptr, 0, nullptr, 1, &tb);

			float uv[4] = {u0, v0, uw, uh};
			vkCmdPushConstants(cmd, pipe_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, uv);

			VkViewport vp{0.0f, 0.0f, (float)iw, (float)ih, 0.0f, 1.0f};
			VkRect2D sc{{0, 0}, {iw, ih}};
			vkCmdSetViewport(cmd, 0, 1, &vp);
			vkCmdSetScissor(cmd, 0, 1, &sc);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_layout, 0, 1, &desc_set, 0, nullptr);

			VkFramebuffer fbs[2] = {fb_l, fb_r};
			for (int i = 0; i < 2; i++) {
				VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
				rbi.renderPass = render_pass;
				rbi.framebuffer = fbs[i];
				rbi.renderArea.extent = {iw, ih};
				vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
				vkCmdDraw(cmd, 3, 1, 0, 0);
				vkCmdEndRenderPass(cmd);
			}
			vkEndCommandBuffer(cmd);
		}

		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
			FAIL("vkQueueSubmit");
		}
		in_flight = true;
		stat_ok++;
		return true;
	}
};

// ---------------------------------------------------------------------------
// OpenXR + main loop
// ---------------------------------------------------------------------------
struct Eye {
	XrSwapchain sc = XR_NULL_HANDLE;
	std::vector<VkImage> images;      // indexed by acquired swapchain index
	std::vector<VkImageView> views;   // swapchain image views
	std::vector<VkFramebuffer> fbs;   // one per image, for the render pass
	uint32_t width = 0, height = 0;
};

int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--portal-test") == 0) {
		signal(SIGINT, on_sigint);
		long node = portal::screencast_node(true);
		if (node >= 0)
			pwcap::capture_stats((uint32_t)node, true);
		return 0;
	}
	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	g_trace = env_flag("ROKID_SCREEN_TRACE", false);
	P("rokid-screen starting (pid %d)\n", getpid());

	// ---- OpenXR instance ---------------------------------------------------
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	ici.enabledExtensionCount = 1;
	const char *xrExts[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
	ici.enabledExtensionNames = xrExts;
	snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "rokid-screen");
	ici.applicationInfo.applicationVersion = 1;
	ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	XrInstance xr = XR_NULL_HANDLE;
	OXR(xrCreateInstance(&ici, &xr));
	resolve_enable2(xr);

	XrInstanceProperties iprops{XR_TYPE_INSTANCE_PROPERTIES};
	OXR(xrGetInstanceProperties(xr, &iprops));
	P("runtime: %s\n", iprops.runtimeName);

	// ---- system ------------------------------------------------------------
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys = 0;
	OXR(xrGetSystem(xr, &sgi, &sys));

	XrSystemProperties sysp{XR_TYPE_SYSTEM_PROPERTIES};
	OXR(xrGetSystemProperties(xr, sys, &sysp));
	P("system: \"%s\" maxLayers=%u\n", sysp.systemName, sysp.graphicsProperties.maxLayerCount);

	// ---- vulkan via runtime (enable2) --------------------------------------
	XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	OXR(pGetVulkanGraphicsRequirements2KHR(xr, sys, &req));

	VkInstance vk_inst = VK_NULL_HANDLE;
	{
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "rokid-screen";
		app.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo vi{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		vi.pApplicationInfo = &app;
		VkResult vr;
		XrVulkanInstanceCreateInfoKHR ci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
		ci.systemId = sys;
		ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
		ci.vulkanCreateInfo = &vi;
		OXR(pCreateVulkanInstanceKHR(xr, &ci, &vk_inst, &vr));
		if (vr != VK_SUCCESS)
			FAIL("vkCreateInstance (runtime) failed: %d", vr);
	}

	VkPhysicalDevice phys = VK_NULL_HANDLE;
	{
		XrVulkanGraphicsDeviceGetInfoKHR di{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
		di.systemId = sys;
		di.vulkanInstance = vk_inst;
		OXR(pGetVulkanGraphicsDevice2KHR(xr, &di, &phys));
	}

	VkDevice vk_dev = VK_NULL_HANDLE;
	uint32_t qf = 0;
	{
		VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		float prio = 1.0f;
		qi.queueCount = 1;
		qi.pQueuePriorities = &prio;

		uint32_t n = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, nullptr);
		std::vector<VkQueueFamilyProperties> qfam(n);
		vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, qfam.data());
		for (uint32_t i = 0; i < n; i++) {
			if (qfam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				qf = i;
				break;
			}
		}
		qi.queueFamilyIndex = qf;

		VkPhysicalDeviceFeatures feats{};
		VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		di.queueCreateInfoCount = 1;
		di.pQueueCreateInfos = &qi;
		di.pEnabledFeatures = &feats;
		VkResult vr;
		XrVulkanDeviceCreateInfoKHR ci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
		ci.systemId = sys;
		ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
		ci.vulkanCreateInfo = &di;
		ci.vulkanPhysicalDevice = phys;
		OXR(pCreateVulkanDeviceKHR(xr, &ci, &vk_dev, &vr));
		if (vr != VK_SUCCESS)
			FAIL("vkCreateDevice (runtime) failed: %d", vr);
	}
	VkQueue vk_queue;
	vkGetDeviceQueue(vk_dev, qf, 0, &vk_queue);

	// ---- session -----------------------------------------------------------
	XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	binding.instance = vk_inst;
	binding.physicalDevice = phys;
	binding.device = vk_dev;
	binding.queueFamilyIndex = qf;
	binding.queueIndex = 0;

	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.next = &binding;
	sci.systemId = sys;
	XrSession session = XR_NULL_HANDLE;
	OXR(xrCreateSession(xr, &sci, &session));
	P("session created\n");

	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	bool session_running = false;

	// ---- spaces ------------------------------------------------------------
	XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsi.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
	XrSpace local_space;
	OXR(xrCreateReferenceSpace(session, &rsi, &local_space));

	// ---- views + swapchains -------------------------------------------------
	uint32_t view_count = 0;
	OXR(xrEnumerateViewConfigurationViews(xr, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr));
	if (view_count != 2)
		P("warning: view_count=%u (expected 2)\n", view_count);
	std::vector<XrViewConfigurationView> views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	OXR(xrEnumerateViewConfigurationViews(xr, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count,
	                                      views.data()));

	int64_t format = 0;
	{
		uint32_t nf = 0;
		OXR(xrEnumerateSwapchainFormats(session, 0, &nf, nullptr));
		std::vector<int64_t> fmts(nf);
		OXR(xrEnumerateSwapchainFormats(session, nf, &nf, fmts.data()));
		P("formats:");
		for (auto f : fmts)
			P(" %d", (int)f);
		P("\n");
		int64_t force = 0;
		if (const char *fe = getenv("ROKID_SCREEN_FORMAT"))
			force = atoll(fe);
		// SRGB default: the comp then samples sRGB-encoded content correctly
		// (UNORM content gets treated as linear -> washed-out/low contrast)
		const int64_t prefer[] = {force ? force : VK_FORMAT_B8G8R8A8_SRGB,
		                          force ? force : VK_FORMAT_B8G8R8A8_UNORM,
		                          VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};
		for (int64_t p : prefer) {
			for (int64_t f : fmts) {
				if (f == p) {
					format = p;
					break;
				}
			}
			if (format)
				break;
		}
		if (!format && nf)
			format = fmts[0];
		if (!format)
			FAIL("no swapchain formats");
	}
	P("swapchain format: %d\n", (int)format);

	std::vector<Eye> eyes(view_count);
	for (uint32_t i = 0; i < view_count; i++) {
		XrSwapchainCreateInfo sc_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		sc_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		sc_info.format = format;
		sc_info.sampleCount = views[i].recommendedSwapchainSampleCount;
		sc_info.width = views[i].recommendedImageRectWidth;
		sc_info.height = views[i].recommendedImageRectHeight;
		sc_info.faceCount = 1;
		sc_info.arraySize = 1;
		sc_info.mipCount = 1;
		eyes[i].width = sc_info.width;
		eyes[i].height = sc_info.height;
		OXR(xrCreateSwapchain(session, &sc_info, &eyes[i].sc));

		uint32_t count = 0;
		OXR(xrEnumerateSwapchainImages(eyes[i].sc, 0, &count, nullptr));
		std::vector<XrSwapchainImageVulkanKHR> imgs(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
		OXR(xrEnumerateSwapchainImages(eyes[i].sc, count, &count, (XrSwapchainImageBaseHeader *)imgs.data()));
		for (auto &im : imgs)
			eyes[i].images.push_back(im.image);
		P("eye %u: %ux%u, %u images\n", i, eyes[i].width, eyes[i].height, count);
	}

	// ---- renderer ------------------------------------------------------------
	Renderer renderer;
	renderer.init(vk_dev, phys, vk_queue, qf, (VkFormat)format);

	// swapchain image views + framebuffers (render pass targets)
	for (auto &e : eyes) {
		for (auto img : e.images) {
			VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vi.image = img;
			vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vi.format = (VkFormat)format;
			vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			VkImageView view;
			if (vkCreateImageView(vk_dev, &vi, nullptr, &view) != VK_SUCCESS)
				FAIL("vkCreateImageView");
			e.views.push_back(view);
			VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			fbi.renderPass = renderer.render_pass;
			fbi.attachmentCount = 1;
			VkImageView att = view;
			fbi.pAttachments = &att;
			fbi.width = e.width;
			fbi.height = e.height;
			fbi.layers = 1;
			VkFramebuffer fb;
			if (vkCreateFramebuffer(vk_dev, &fbi, nullptr, &fb) != VK_SUCCESS)
				FAIL("vkCreateFramebuffer");
			e.fbs.push_back(fb);
		}
	}

	// ---- capture -------------------------------------------------------------
	Capture capture;
	capture.fps = env_int("ROKID_SCREEN_FPS", 30);
	capture.start();

	// ---- upload --------------------------------------------------------------
	float scale = env_float("ROKID_SCREEN_SCALE", 1.0f);

	// ---- loop -----------------------------------------------------------------
	XrFrameState frame_state{XR_TYPE_FRAME_STATE};
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	uint64_t frames = 0, rendered = 0, ended_no_layer = 0;
	double last_report = 0.0;
	auto report = [&](const char *when) {
		double t = now_s();
		if (t - last_report < 1.0)
			return;
		last_report = t;
		P("stats[%s] state=%d render=%d frames=%" PRIu64 " rendered=%" PRIu64 " nolayer=%" PRIu64
		  " upload(ok=%" PRIu64 " fence=%" PRIu64 " empty=%" PRIu64 ") seq=%" PRIu64 " %ux%u\n",
		  when, (int)state, (int)frame_state.shouldRender, frames, rendered, ended_no_layer, renderer.stat_ok,
		  renderer.stat_skip_fence, renderer.stat_no_content, capture.slot.seq, capture.slot.width, capture.slot.height);
	};

	P("running - Ctrl-C to exit, double-tap the glasses to recenter\n");
	while (running) {
		// pump events
		for (;;) {
			XrEventDataBaseHeader *e = (XrEventDataBaseHeader *)&event;
			XrResult r = xrPollEvent(xr, &event);
			if (r == XR_EVENT_UNAVAILABLE) {
				break;
			}
			OXR(r);
			switch (e->type) {
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				auto *ev = (XrEventDataSessionStateChanged *)&event;
				state = ev->state;
				P("session state: %d\n", (int)state);
				if (state == XR_SESSION_STATE_READY && !session_running) {
					XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					OXR(xrBeginSession(session, &bi));
					session_running = true;
				}
				if (state == XR_SESSION_STATE_STOPPING) {
					OXR(xrEndSession(session));
					session_running = false;
				}
				if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
					running = false;
				}
				break;
			}
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				running = false;
				break;
			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
				P("reference space change pending (driver recenter)\n");
				break;
			default:
				break;
			}
			event = {XR_TYPE_EVENT_DATA_BUFFER};
		}

		XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
		OXR(xrWaitFrame(session, &wi, &frame_state));
		frames++;
		// ALWAYS begin/end the frame: Monado advances the session state
		// machine (READY->SYNCHRONIZED->VISIBLE) inside xrEndFrame, so
		// skipping it until VISIBLE deadlocks at the first frame.
		if (!session_running) {
			report("wait");
			continue;
		}
		XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
		OXR(xrBeginFrame(session, &bi));

		std::vector<XrCompositionLayerProjectionView> proj_views;
		XrCompositionLayerProjection proj_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		// XrFrameEndInfo.layers is an array of POINTERS; a stale-but-typical
		// bug is passing &proj_layer and making the runtime read the struct's
		// type field (0x23) as layers[0] -> SIGSEGV in oxr_session_frame_end.
		const XrCompositionLayerBaseHeader *layer_ptrs[1] = {
		    (const XrCompositionLayerBaseHeader *)&proj_layer};

		if (frame_state.shouldRender) {
			// locate views
			std::vector<XrView> xr_views(view_count, {XR_TYPE_VIEW});
			XrViewState view_state{XR_TYPE_VIEW_STATE};
			XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
			vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			vli.displayTime = frame_state.predictedDisplayTime;
			vli.space = local_space;
			uint32_t located = 0;
			T("stage: locating views\n");
			OXR(xrLocateViews(session, &vli, &view_state, view_count, &located, xr_views.data()));
			T("stage: located %u\n", located);

			// acquire both
			uint32_t idx[2] = {0, 0};
			bool acquired[2] = {false, false};
			bool ok = located == view_count;
			for (uint32_t i = 0; ok && i < 2; i++) {
				T("stage: acquire eye %u\n", i);
				XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
				if (XR_FAILED(xrAcquireSwapchainImage(eyes[i].sc, &ai, &idx[i]))) {
					ok = false;
					break;
				}
				acquired[i] = true;
				XrSwapchainImageWaitInfo winfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				winfo.timeout = XR_INFINITE_DURATION;
				if (XR_FAILED(xrWaitSwapchainImage(eyes[i].sc, &winfo))) {
					ok = false;
					break;
				}
			}

				rendered++;
			T("stage: acquire done ok=%d\n", (int)ok);
			if (ok) {
				T("stage: upload\n");
				renderer.render(capture.slot, eyes[0].fbs[idx[0]], eyes[1].fbs[idx[1]], eyes[0].width,
				                eyes[0].height, scale);
				T("stage: upload done (ok=%d)\n", (int)renderer.stat_ok);

				// projection layer: both views sample the images we just filled
				proj_views.resize(view_count, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
				for (uint32_t i = 0; i < view_count; i++) {
					auto &pv = proj_views[i];
					pv.pose = xr_views[i].pose;
					pv.fov = xr_views[i].fov;
					pv.subImage.swapchain = eyes[i].sc;
					pv.subImage.imageRect.offset = {0, 0};
					pv.subImage.imageRect.extent = {(int32_t)eyes[i].width, (int32_t)eyes[i].height};
					pv.subImage.imageArrayIndex = 0;
				}
				proj_layer.layerFlags = 0;
				proj_layer.space = local_space;
				proj_layer.viewCount = (uint32_t)proj_views.size();
				proj_layer.views = proj_views.data();
			}

			for (uint32_t i = 0; i < 2; i++) {
				if (acquired[i]) {
					XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
					OXR(xrReleaseSwapchainImage(eyes[i].sc, &ri));
				}
			}
		}

		XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
		ei.displayTime = frame_state.predictedDisplayTime;
		ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		if (frame_state.shouldRender && proj_layer.views != nullptr) {
			ei.layerCount = 1;
			ei.layers = layer_ptrs;
		}
		if (ei.layerCount == 0)
			ended_no_layer++;
		T("stage: end frame (layers=%u)\n", ei.layerCount);
		OXR(xrEndFrame(session, &ei));
		T("stage: end frame done\n");
		report("render");
	}

	P("shutting down\n");
	capture.stop();
	renderer.destroy();
	for (auto &e : eyes) {
		if (e.sc)
			xrDestroySwapchain(e.sc);
	}
	xrDestroySpace(local_space);
	xrDestroySession(session);
	xrDestroyInstance(xr);
	P("bye\n");
	return 0;
}
