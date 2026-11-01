// ScreenCast portal handshake: returns the PipeWire node id of the
// picked monitor. Standalone so the portal dance can be tested in
// isolation (`--portal-test`).
//
// Portal flow (org.freedesktop.portal.ScreenCast):
//   CreateSession   -> session path
//   SelectSources   (monitors; picker dialog shows on host)
//   Start           -> response contains ("streams", [(node_id, maps)])
#include "portal.hpp"

#include <dbus/dbus.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace portal {

static const char *PORTAL_DEST = "org.freedesktop.portal.Desktop";
static const char *PORTAL_PATH = "/org/freedesktop/portal/desktop";
static const char *SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast";

// Each portal call gets a handle_token; the Request object the portal
// replies on is /org/freedesktop/portal/desktop/request/<sender>/<token>
// where <sender> is our unique bus name (':' stripped, '.'->'_').
static unsigned g_ctr = 0;
static void make_token(char *token, size_t n) {
	snprintf(token, n, "rokid%u_%u", (unsigned)time(nullptr) % 100000, ++g_ctr);
}

static const char *sig_for(int type) {
	switch (type) {
	case DBUS_TYPE_STRING: return "s";
	case DBUS_TYPE_OBJECT_PATH: return "o";
	case DBUS_TYPE_UINT32: return "u";
	case DBUS_TYPE_INT32: return "i";
	case DBUS_TYPE_BOOLEAN: return "b";
	default: return "v";
	}
}

// Build one dict entry: {key: <type, value>} for a{sv}
static DBusMessage *dict_entry(DBusMessageIter *dict, const char *key, int type, const void *value) {
	DBusMessageIter entry, var;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig_for(type), &var);
	dbus_message_iter_append_basic(&var, type, value);
	dbus_message_iter_close_container(&entry, &var);
	dbus_message_iter_close_container(dict, &entry);
	return nullptr;
}

static DBusMessage *call(DBusConnection *c, const char *method, const char *session,
			 const char *token) {
	// Signatures (org.freedesktop.portal.ScreenCast):
	//   CreateSession(a{sv} options)
	//   SelectSources(o session_handle, a{sv} options)
	//   Start(o session_handle, s parent_window, a{sv} options)
	bool has_session = session && strcmp(method, "CreateSession") != 0;
	bool has_parent  = strcmp(method, "Start") == 0;
	DBusMessage *msg = dbus_message_new_method_call(PORTAL_DEST, PORTAL_PATH, SCREENCAST_IFACE, method);
	DBusMessageIter it, dict;
	dbus_message_iter_init_append(msg, &it);
	if (has_session) {
		const char *sh = session; // dbus wants const char**, not ptr-to-array
		dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &sh);
	}
	if (has_parent) {
		const char *pw = "";
		dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &pw);
	}
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
	if (strcmp(method, "CreateSession") == 0) {
		// session_handle_token is required by some xdg-desktop-portal
		// versions (assert session->token != NULL) - always send one
		const char *sht = token;
		dict_entry(&dict, "session_handle_token", DBUS_TYPE_STRING, &sht);
	}
	dict_entry(&dict, "handle_token", DBUS_TYPE_STRING, &token);
	if (strcmp(method, "SelectSources") == 0) {
		// 1 = MONITOR; cursor 2 = embedded (Hyprland-compat)
		uint32_t types = 1, cursor = 2;
		dict_entry(&dict, "types", DBUS_TYPE_UINT32, &types);
		dict_entry(&dict, "cursor_mode", DBUS_TYPE_UINT32, &cursor);
	}
	dbus_message_iter_close_container(&it, &dict);
	DBusError err;
	dbus_error_init(&err);
	DBusMessage *r = dbus_connection_send_with_reply_and_block(c, msg, 30000, &err);
	dbus_message_unref(msg);
	if (!r) {
		fprintf(stderr, "[portal] %s failed: %s (%s)\n", method, err.message, err.name);
		dbus_error_free(&err);
		return nullptr;
	}
	return r;
}

// Subscribe to /org/freedesktop/portal/desktop/request/<sender>/<token>
// Response signals. Returns the path we must match on.
static void request_path_for(DBusConnection *c, const char *token, char *out, size_t n) {
	const char *name = dbus_bus_get_unique_name(c); // ":1.23"
	char sender[128];
	snprintf(sender, sizeof(sender), "%s", name + 1); // strip ':'
	for (char *p = sender; *p; p++)
		if (*p == '.')
			*p = '_';
	snprintf(out, n, "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
}

static bool add_response_match(DBusConnection *c, const char *path) {
	char rule[256];
	snprintf(rule, sizeof(rule), "type='signal',interface='org.freedesktop.portal.Request',path='%s'", path);
	DBusError err;
	dbus_error_init(&err);
	dbus_bus_add_match(c, rule, &err);
	bool ok = !dbus_error_is_set(&err);
	dbus_error_free(&err);
	return ok;
}

static bool wait_response(DBusConnection *c, const char *path, DBusMessage **resp_out) {
	// pump until we see the Response signal on this path (or 60 s)
	for (int i = 0; i < 6000; i++) {
		DBusMessage *m = nullptr;
		// non-blocking pop, then short sleep-ish via dispatch
		while ((m = dbus_connection_pop_message(c))) {
			if (dbus_message_is_signal(m, "org.freedesktop.portal.Request", "Response") &&
			    strcmp(dbus_message_get_path(m), path) == 0) {
				*resp_out = m;
				return true;
			}
			dbus_message_unref(m);
		}
		dbus_connection_read_write_dispatch(c, 10);
		if (i % 1000 == 999)
			fprintf(stderr, "[portal] waiting for user to pick a monitor... (%ds)\n", (i + 1) / 100 + 1);
	}
	return false;
}

static bool extract_streams(DBusMessage *resp, uint32_t *node_out) {
	// body: u response, a{sv} results; results["streams"] = v<a(ua{sv})>
	DBusMessageIter it;
	if (!dbus_message_iter_init(resp, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
		fprintf(stderr, "[portal] Response: bad signature\n");
		return false;
	}
	uint32_t code = 0;
	dbus_message_iter_get_basic(&it, &code);
	if (code != 0) {
		fprintf(stderr, "[portal] response code %u (1=cancelled)\n", code);
		return false;
	}
	dbus_message_iter_next(&it); // a{sv}
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
		return false;
	DBusMessageIter dict;
	dbus_message_iter_recurse(&it, &dict);
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry;
		dbus_message_iter_recurse(&dict, &entry);
		const char *key = nullptr;
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		if (strcmp(key, "streams") == 0) {
			DBusMessageIter var, arr;
			dbus_message_iter_recurse(&entry, &var);   // variant
			dbus_message_iter_recurse(&var, &arr);     // a(ua{sv})
			if (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_STRUCT) {
				fprintf(stderr, "[portal] no stream in response\n");
				return false;
			}
			// log every stream incl. its maps (source_type tells us what it is);
			// return the last monitor stream, else the last stream
			int idx = 0;
			bool found = false;
			uint32_t fallback = 0;
			while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
				DBusMessageIter strct;
				dbus_message_iter_recurse(&arr, &strct);
				uint32_t nid = 0;
				dbus_message_iter_get_basic(&strct, &nid);
				dbus_message_iter_next(&strct); // a{sv} maps
				uint32_t stype = 0;
				if (dbus_message_iter_get_arg_type(&strct) == DBUS_TYPE_ARRAY) {
					DBusMessageIter md, me;
					dbus_message_iter_recurse(&strct, &md);
					fprintf(stderr, "[portal] stream[%d]: node %u maps:", idx++, nid);
					while (dbus_message_iter_get_arg_type(&md) == DBUS_TYPE_DICT_ENTRY) {
						dbus_message_iter_recurse(&md, &me);
						const char *k = nullptr;
						dbus_message_iter_get_basic(&me, &k);
						dbus_message_iter_next(&me); // variant
						if (strcmp(k, "source_type") == 0 || strcmp(k, "position") == 0 ||
						    strcmp(k, "size") == 0) {
							DBusMessageIter v;
							dbus_message_iter_recurse(&me, &v);
							if (strcmp(k, "source_type") == 0) {
								dbus_message_iter_get_basic(&v, &stype);
								fprintf(stderr, " %s=%u", k, stype);
							} else {
								// position/size are struct(s) - just report presence
								fprintf(stderr, " %s=(...)", k);
							}
						} else {
							fprintf(stderr, " %s", k);
						}
						dbus_message_iter_next(&md);
					}
					fprintf(stderr, " (source_type=%u: 1=monitor 2=window 4=cursor)\n", stype);
				} else {
					fprintf(stderr, "[portal] stream[%d]: node %u (no maps)\n", idx++, nid);
				}
				if (stype == 1) { // monitor wins
					*node_out = nid;
					found = true;
				} else if (!found) {
					fallback = nid;
				}
				dbus_message_iter_next(&arr);
			}
			if (!found && fallback) {
				fprintf(stderr, "[portal] no monitor stream, using node %u\n", fallback);
				*node_out = fallback;
				found = true;
			}
			return found;
		}
		dbus_message_iter_next(&dict);
	}
	fprintf(stderr, "[portal] Response lacks 'streams'\n");
	return false;
}

long screencast_node(bool verbose) {
	DBusError err;
	dbus_error_init(&err);
	DBusConnection *c = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (!c) {
		fprintf(stderr, "[portal] no session bus: %s\n", err.message);
		return -1;
	}
	long result = -1;

	char token[64], rpath[256];

	// 1. CreateSession — subscribe BEFORE calling (response may beat the reply)
	DBusMessage *resp = nullptr;
	char session[256] = "";
	make_token(token, sizeof(token));
	request_path_for(c, token, rpath, sizeof(rpath));
	if (verbose)
		fprintf(stderr, "[portal] session request path: %s\n", rpath);
	if (!add_response_match(c, rpath))
		goto out;
	if (!call(c, "CreateSession", nullptr, token))
		goto out;
	{
		if (!wait_response(c, rpath, &resp))
			goto out;
		// grab session_handle from results
		DBusMessageIter it, dict;
		dbus_message_iter_init(resp, &it);
		dbus_message_iter_next(&it);
		if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
			dbus_message_unref(resp);
			goto out;
		}
		dbus_message_iter_recurse(&it, &dict);
		while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
			DBusMessageIter e;
			dbus_message_iter_recurse(&dict, &e);
			const char *k = nullptr;
			dbus_message_iter_get_basic(&e, &k);
			dbus_message_iter_next(&e);
			if (strcmp(k, "session_handle") == 0) {
				DBusMessageIter v;
				dbus_message_iter_recurse(&e, &v);
				const char *s = nullptr;
				dbus_message_iter_get_basic(&v, &s);
				snprintf(session, sizeof(session), "%s", s);
			}
			dbus_message_iter_next(&dict);
		}
		dbus_message_unref(resp);
		if (!session[0]) {
			fprintf(stderr, "[portal] no session_handle\n");
			goto out;
		}
		if (verbose)
			fprintf(stderr, "[portal] session: %s\n", session);
	}

	// 2. SelectSources (monitors only)
	make_token(token, sizeof(token));
	request_path_for(c, token, rpath, sizeof(rpath));
	add_response_match(c, rpath);
	if (!call(c, "SelectSources", session, token))
		goto out;
	if (!wait_response(c, rpath, &resp))
		goto out;
	dbus_message_unref(resp);

	// 3. Start
	make_token(token, sizeof(token));
	request_path_for(c, token, rpath, sizeof(rpath));
	add_response_match(c, rpath);
	if (!call(c, "Start", session, token))
		goto out;
	if (!wait_response(c, rpath, &resp))
		goto out;
	{
		uint32_t node = 0;
		bool ok = extract_streams(resp, &node);
		dbus_message_unref(resp);
		if (ok) {
			fprintf(stderr, "[portal] node id: %u\n", node);
			result = (long)node;
		}
	}
out:
	dbus_connection_unref(c);
	return result;
}

} // namespace portal
