// piewin: ultra-simple XCB + Cairo fullscreen chooser
// Build: meson setup build && meson compile -C build
// Usage: printf "One\nTwo\nThree\n" | ./build/piewin
//
// Notes:
// - Compile fixes: enable POSIX prototypes for getline/strndup.
// - Optional debug logging is enabled when the DEBUG env var is set.
#define _POSIX_C_SOURCE 200809L

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xcb_keysyms.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// XK_Escape without pulling in Xlib headers
#define XK_Escape 0xff1b

// --- Debug helper -------------------------------------------------------
static int dbg_enabled(void)
{
	static int init = 0, on = 0;
	if (!init) {
		const char *d = getenv("DEBUG");
		on = (d && *d && strcmp(d, "0") != 0) ? 1 : 0;
		init = 1;
	}
	return on;
}
#define DBG(...) \
	do { \
		if (dbg_enabled()) fprintf(stderr, __VA_ARGS__); \
	} while (0)

typedef struct
{
	char *text;
} Entry;

typedef struct
{
	xcb_connection_t *conn;
	xcb_screen_t *screen;
	xcb_window_t win;
	xcb_atom_t WM_PROTOCOLS;
	xcb_atom_t WM_DELETE_WINDOW;
	xcb_atom_t NET_WM_STATE;
	xcb_atom_t NET_WM_STATE_FULLSCREEN;
	xcb_atom_t NET_WM_NAME;
	xcb_atom_t WM_NAME_ATOM;
	xcb_atom_t UTF8_STRING;
	xcb_key_symbols_t *keysyms;
	int width, height;
	cairo_surface_t *csurf;   // XCB surface (window)
	cairo_t *cr;               // XCB surface context
	cairo_surface_t *bufsurf;  // offscreen back buffer
	cairo_t *bufcr;            // offscreen context
} App;

static xcb_visualtype_t *find_visualtype(const xcb_screen_t *screen, xcb_visualid_t vid)
{
	xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen);
	for (; di.rem; xcb_depth_next(&di)) {
		xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
		for (; vi.rem; xcb_visualtype_next(&vi)) {
			if (vi.data->visual_id == vid) return vi.data;
		}
	}
	return NULL;
}

static xcb_atom_t intern_atom(xcb_connection_t *c, const char *name, int only_if_exists)
{
	xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, only_if_exists, strlen(name), name);
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, ck, NULL);
	if (!r) return XCB_NONE;
	xcb_atom_t a = r->atom;
	free(r);
	return a;
}

static void hsv_to_rgb(double h, double s, double v, double *r, double *g, double *b)
{
	if (s <= 0.0) {
		*r = *g = *b = v;
		return;
	}
	h = fmod(h, 1.0);
	if (h < 0) h += 1.0;
	double i = floor(h * 6.0);
	double f = h * 6.0 - i;
	double p = v * (1.0 - s);
	double q = v * (1.0 - s * f);
	double t = v * (1.0 - s * (1.0 - f));
	switch ((int)i % 6) {
		case 0:
			*r = v;
			*g = t;
			*b = p;
			break;
		case 1:
			*r = q;
			*g = v;
			*b = p;
			break;
		case 2:
			*r = p;
			*g = v;
			*b = t;
			break;
		case 3:
			*r = p;
			*g = q;
			*b = v;
			break;
		case 4:
			*r = t;
			*g = p;
			*b = v;
			break;
		case 5:
			*r = v;
			*g = p;
			*b = q;
			break;
	}
}

static double distance_to_rect_edge(int W, int H, double cx, double cy, double ang)
{
	double dx = cos(ang), dy = sin(ang);
	double tx = INFINITY, ty = INFINITY;
	if (fabs(dx) > 1e-9) {
		double t1 = (0.0 - cx) / dx;
		double y1 = cy + t1 * dy;
		if (t1 > 0 && y1 >= 0 && y1 <= H) tx = t1;
		double t2 = ((double)W - cx) / dx;
		double y2 = cy + t2 * dy;
		if (t2 > 0 && y2 >= 0 && y2 <= H) tx = fmin(tx, t2);
	}
	if (fabs(dy) > 1e-9) {
		double t3 = (0.0 - cy) / dy;
		double x3 = cx + t3 * dx;
		if (t3 > 0 && x3 >= 0 && x3 <= W) ty = t3;
		double t4 = ((double)H - cy) / dy;
		double x4 = cx + t4 * dx;
		if (t4 > 0 && x4 >= 0 && x4 <= W) ty = fmin(ty, t4);
	}
	double t = fmin(tx, ty);
	if (!isfinite(t) || t <= 0) t = 0.0;
	return t;
}

static int sector_index_from_point(int n, int W, int H, int x, int y)
{
	if (n <= 0) return -1;
	double cx = W * 0.5, cy = H * 0.5;
	double ang = atan2((double)y - cy, (double)x - cx);
	if (ang < 0) ang += 2.0 * M_PI;
	double step = (2.0 * M_PI) / (double)n;
	int idx = (int)floor(ang / step);
	if (idx < 0) idx = 0;
	if (idx >= n) idx = n - 1;
	return idx;
}

static double fit_font_size(cairo_t *cr, const char *text, double maxw, double maxh)
{
	// Binary search for largest font size that fits into (maxw x maxh)
	double lo = 1.0, hi = fmax(1.0, fmin(maxw, maxh));
	cairo_text_extents_t ext;
	for (int it = 0; it < 20; ++it) {
		double mid = (lo + hi) * 0.5;
		cairo_set_font_size(cr, mid);
		cairo_text_extents(cr, text, &ext);
		double w = ext.width;
		double h = ext.height;
		if (w <= maxw && h <= maxh)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

static void draw(App *app, Entry *entries, int n, int hover_idx)
{
	int W = app->width, H = app->height;
	double cx = W * 0.5, cy = H * 0.5;

	cairo_t *cr = app->bufcr;          // draw into back buffer
	cairo_save(cr);

	// Clear back buffer
	cairo_set_source_rgb(cr, 0.08, 0.08, 0.10);
	cairo_rectangle(cr, 0, 0, W, H);
	cairo_fill(cr);

	if (n <= 0) {
		cairo_set_source_rgb(cr, 0.9, 0.2, 0.2);
		cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		double s = fmin(W, H) * 0.08;
		cairo_set_font_size(cr, s);
		const char *msg = "No entries.";
		cairo_text_extents_t ext;
		cairo_text_extents(cr, msg, &ext);
		cairo_move_to(cr, cx - (ext.width * 0.5 + ext.x_bearing), cy - (ext.height * 0.5 + ext.y_bearing));
		cairo_show_text(cr, msg);
		cairo_restore(cr);

		// Blit back buffer to the window in one go
		cairo_set_source_surface(app->cr, app->bufsurf, 0, 0);
		cairo_set_operator(app->cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(app->cr);
		cairo_surface_flush(app->csurf);
		xcb_flush(app->conn);
		return;
	}

	double step = (2.0 * M_PI) / (double)n;
	// Big radius so the arc is outside the window; ensures wedge fills to edges after clipping.
	double R = hypot((double)W, (double)H);  // safely beyond all corners

	for (int i = 0; i < n; ++i) {
		double a0 = step * i;
		double a1 = step * (i + 1);

		// Fill wedge
		double r, g, b;
		double base_v = (i == hover_idx) ? 0.85 : 0.62;
		hsv_to_rgb((double)i / (double)n, 0.55, base_v, &r, &g, &b);
		cairo_set_source_rgb(cr, r, g, b);

		cairo_new_path(cr);
		cairo_move_to(cr, cx, cy);
		cairo_line_to(cr, cx + R * cos(a0), cy + R * sin(a0));
		cairo_arc(cr, cx, cy, R, a0, a1);
		cairo_close_path(cr);
		cairo_fill(cr);

		// Text: place at mid-angle, mid-radius
		const char *txt = entries[i].text ? entries[i].text : "";
		double amid = (a0 + a1) * 0.5;
		double t_edge = distance_to_rect_edge(W, H, cx, cy, amid);
		double rmid = fmax(10.0, t_edge * 0.5);
		double px = cx + rmid * cos(amid);
		double py = cy + rmid * sin(amid);

		// Available width approximated by distance between sector boundaries at rmid
		double avail_w = fmax(20.0, 0.9 * 2.0 * rmid * sin(step * 0.5));
		// And limited by distance to nearest window edge to avoid clipping
		double dist_x = fmin(px, (double)W - px);
		double dist_y = fmin(py, (double)H - py);
		avail_w = fmin(avail_w, 1.8 * fmin(dist_x, dist_y));
		double avail_h = fmin(avail_w, 0.6 * rmid);

		cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		double fs = fit_font_size(cr, txt, avail_w, avail_h);
		cairo_set_font_size(cr, fs);
		cairo_set_source_rgb(cr, 0.05, 0.05, 0.07);  // drop shadow
		cairo_text_extents_t ext;
		cairo_text_extents(cr, txt, &ext);
		double tx = px - (ext.width * 0.5 + ext.x_bearing);
		double ty = py - (ext.height * 0.5 + ext.y_bearing);
		cairo_move_to(cr, tx + 1.5, ty + 1.5);
		cairo_show_text(cr, txt);
		cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
		cairo_move_to(cr, tx, ty);
		cairo_show_text(cr, txt);
	}

	cairo_restore(cr);

	// Blit back buffer to the window in one go (reduces artifacts)
	cairo_set_source_surface(app->cr, app->bufsurf, 0, 0);
	cairo_set_operator(app->cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint(app->cr);

	cairo_surface_flush(app->csurf);
	xcb_flush(app->conn);
}

static void recreate_cairo(App *app)
{
	if (app->cr) {
		cairo_destroy(app->cr);
		app->cr = NULL;
	}
	if (app->csurf) {
		cairo_surface_destroy(app->csurf);
		app->csurf = NULL;
	}
	if (app->bufcr) {
		cairo_destroy(app->bufcr);
		app->bufcr = NULL;
	}
	if (app->bufsurf) {
		cairo_surface_destroy(app->bufsurf);
		app->bufsurf = NULL;
	}
	xcb_visualtype_t *vt = find_visualtype(app->screen, app->screen->root_visual);
	app->csurf = cairo_xcb_surface_create(app->conn, app->win, vt, app->width, app->height);
	app->cr = cairo_create(app->csurf);
	cairo_set_antialias(app->cr, CAIRO_ANTIALIAS_FAST);

	app->bufsurf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, app->width, app->height);
	app->bufcr = cairo_create(app->bufsurf);
	cairo_set_antialias(app->bufcr, CAIRO_ANTIALIAS_FAST);

	DBG("[piewin] Recreated Cairo surfaces %dx%d (double-buffer)\n", app->width, app->height);
}

static void set_fullscreen_hint(App *app)
{
	// Set initial _NET_WM_STATE to FULLSCREEN before mapping (best-effort)
	if (app->NET_WM_STATE != XCB_NONE && app->NET_WM_STATE_FULLSCREEN != XCB_NONE) {
		xcb_change_property(app->conn, XCB_PROP_MODE_REPLACE, app->win, app->NET_WM_STATE, XCB_ATOM_ATOM, 32, 1, &app->NET_WM_STATE_FULLSCREEN);
	}
}

static void set_wm_delete_protocol(App *app)
{
	if (app->WM_PROTOCOLS != XCB_NONE && app->WM_DELETE_WINDOW != XCB_NONE) {
		xcb_change_property(app->conn, XCB_PROP_MODE_REPLACE, app->win, app->WM_PROTOCOLS, XCB_ATOM_ATOM, 32, 1, &app->WM_DELETE_WINDOW);
	}
}

static void set_window_title(App *app, const char *title)
{
	// Prefer _NET_WM_NAME with UTF8_STRING, also set WM_NAME for compatibility.
	if (app->NET_WM_NAME != XCB_NONE) {
		xcb_atom_t type = (app->UTF8_STRING != XCB_NONE) ? app->UTF8_STRING : XCB_ATOM_STRING;
		xcb_change_property(app->conn, XCB_PROP_MODE_REPLACE, app->win, app->NET_WM_NAME, type, 8, strlen(title), title);
	}
	if (app->WM_NAME_ATOM != XCB_NONE) {
		xcb_change_property(app->conn, XCB_PROP_MODE_REPLACE, app->win, app->WM_NAME_ATOM, XCB_ATOM_STRING, 8, strlen(title), title);
	}
}

static void usage(const char *argv0)
{
	fprintf(stderr, "piewin – XCB+Cairo fullscreen chooser\n");
	fprintf(stderr, "Usage: echo -e \"A\\nB\\nC\" | %s\n", argv0);
}

static void grab_input(App *app)
{
	// Grab pointer to avoid click-through to underlying apps.
	xcb_grab_pointer_cookie_t pc =
	    xcb_grab_pointer(app->conn,
	                     0,                 // owner_events
	                     app->win,          // grab on our window
	                     XCB_EVENT_MASK_BUTTON_PRESS |
	                         XCB_EVENT_MASK_BUTTON_RELEASE |
	                         XCB_EVENT_MASK_POINTER_MOTION,
	                     XCB_GRAB_MODE_ASYNC,
	                     XCB_GRAB_MODE_ASYNC,
	                     app->win,          // confine_to
	                     XCB_NONE,          // cursor
	                     XCB_CURRENT_TIME);
	xcb_grab_pointer_reply_t *pr = xcb_grab_pointer_reply(app->conn, pc, NULL);
	if (pr) {
		DBG("[piewin] Grab pointer status=%u\n", pr->status);
		free(pr);
	} else {
		DBG("[piewin] Grab pointer: no reply\n");
	}

	// Also grab keyboard so Esc doesn't leak.
	xcb_grab_keyboard_cookie_t kc =
	    xcb_grab_keyboard(app->conn,
	                      0,
	                      app->win,
	                      XCB_CURRENT_TIME,
	                      XCB_GRAB_MODE_ASYNC,
	                      XCB_GRAB_MODE_ASYNC);
	xcb_grab_keyboard_reply_t *kr = xcb_grab_keyboard_reply(app->conn, kc, NULL);
	if (kr) {
		DBG("[piewin] Grab keyboard status=%u\n", kr->status);
		free(kr);
	} else {
		DBG("[piewin] Grab keyboard: no reply\n");
	}
	xcb_flush(app->conn);
}

static void ungrab_input(App *app)
{
	xcb_ungrab_pointer(app->conn, XCB_CURRENT_TIME);
	xcb_ungrab_keyboard(app->conn, XCB_CURRENT_TIME);
	xcb_flush(app->conn);
	DBG("[piewin] Ungrab input\n");
}

int main(int argc, char **argv)
{
	if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(argv[0]);
		return 0;
	}

	DBG("[piewin] Debug logging enabled\n");
	if (dbg_enabled()) {
		fprintf(stderr, "[piewin] Cairo: %s\n", cairo_version_string());
	}

	// Read entries from stdin (fast, simple)
	Entry *entries = NULL;
	size_t cap = 0, count = 0;
	char *line = NULL;
	size_t len = 0;
	ssize_t r;
	while ((r = getline(&line, &len, stdin)) != -1) {
		// Trim newline(s)
		while (r > 0 && (line[r - 1] == '\n' || line[r - 1] == '\r')) {
			line[--r] = '\0';
		}
		if (r == 0) continue;  // skip empty lines
		if (count == cap) {
			cap = cap ? cap * 2 : 8;
			entries = (Entry *)realloc(entries, cap * sizeof(Entry));
			if (!entries) {
				perror("realloc");
				return 1;
			}
		}
		entries[count].text = strndup(line, r);
		if (!entries[count].text) {
			perror("strndup");
			return 1;
		}
		DBG("[piewin] Read entry[%zu]: \"%s\"\n", count, entries[count].text);
		count++;
	}
	free(line);

	if (count == 0) {
		DBG("[piewin] No entries on stdin; exiting 1\n");
		return 1;
	}

	// XCB setup
	int scrno = 0;
	xcb_connection_t *conn = xcb_connect(NULL, &scrno);
	if (xcb_connection_has_error(conn)) {
		fprintf(stderr, "Failed to connect to X server.\n");
		return 1;
	}
	const xcb_setup_t *setup = xcb_get_setup(conn);
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
	for (int s = 0; s < scrno; ++s)
		xcb_screen_next(&it);
	xcb_screen_t *screen = it.data;
	DBG("[piewin] Connected to X server: screen=%d size=%dx%d root=0x%08x\n",
	    scrno,
	    screen->width_in_pixels,
	    screen->height_in_pixels,
	    (unsigned)screen->root);

	App app = (App){0};
	app.conn = conn;
	app.screen = screen;
	app.width = screen->width_in_pixels;
	app.height = screen->height_in_pixels;
	app.WM_PROTOCOLS = intern_atom(conn, "WM_PROTOCOLS", 0);
	app.WM_DELETE_WINDOW = intern_atom(conn, "WM_DELETE_WINDOW", 0);
	app.NET_WM_STATE = intern_atom(conn, "_NET_WM_STATE", 0);
	app.NET_WM_STATE_FULLSCREEN = intern_atom(conn, "_NET_WM_STATE_FULLSCREEN", 0);
	app.NET_WM_NAME = intern_atom(conn, "_NET_WM_NAME", 0);
	app.WM_NAME_ATOM = intern_atom(conn, "WM_NAME", 0);
	app.UTF8_STRING = intern_atom(conn, "UTF8_STRING", 1);
	app.keysyms = xcb_key_symbols_alloc(conn);

	uint32_t event_mask =
		XCB_EVENT_MASK_EXPOSURE |
		XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		XCB_EVENT_MASK_BUTTON_PRESS |
		XCB_EVENT_MASK_BUTTON_RELEASE |
		XCB_EVENT_MASK_POINTER_MOTION |
		XCB_EVENT_MASK_KEY_PRESS;

	uint32_t vals[2];
	vals[0] = screen->black_pixel;
	vals[1] = event_mask;

	app.win = xcb_generate_id(conn);
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, app.win, screen->root, 0, 0, app.width, app.height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);

	set_wm_delete_protocol(&app);
	set_fullscreen_hint(&app);
	set_window_title(&app, "piewin");

	// Map and raise
	xcb_map_window(conn, app.win);
	xcb_flush(conn);

	// Create cairo surfaces (includes back buffer)
	recreate_cairo(&app);

	// Grab input so clicks/keys don't leak to other apps
	grab_input(&app);

	int hover_idx = -1;
	int pressed_idx = -1;
	DBG("[piewin] Initial draw %dx%d, entries=%zu\n", app.width, app.height, count);
	draw(&app, entries, (int)count, hover_idx);

	int exit_code = 1;  // default to "cancel"
	int running = 1;

	while (running) {
		xcb_generic_event_t *ev = xcb_wait_for_event(conn);
		if (!ev) break;
		uint8_t rt = ev->response_type & ~0x80;
		switch (rt) {
			case XCB_EXPOSE:
				{
					DBG("[piewin] EXPOSE\n");
					draw(&app, entries, (int)count, hover_idx);
				}
				break;

			case XCB_MOTION_NOTIFY:
				{
					xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t *)ev;
					int idx = sector_index_from_point((int)count, app.width, app.height, e->event_x, e->event_y);
					if (idx != hover_idx) {
						DBG("[piewin] HOVER %d -> %d (x=%d y=%d)\n", hover_idx, idx, e->event_x, e->event_y);
						hover_idx = idx;
						draw(&app, entries, (int)count, hover_idx);
					}
				}
				break;

			case XCB_BUTTON_PRESS:
				{
					xcb_button_press_event_t *e = (xcb_button_press_event_t *)ev;
					DBG("[piewin] BUTTON_PRESS detail=%u at %d,%d\n", e->detail, e->event_x, e->event_y);
					if (e->detail == 1) {  // left button
						pressed_idx = sector_index_from_point((int)count, app.width, app.height, e->event_x, e->event_y);
						DBG("[piewin] PRESS on idx=%d\n", pressed_idx);
					}
				}
				break;

			case XCB_BUTTON_RELEASE:
				{
					xcb_button_release_event_t *e = (xcb_button_release_event_t *)ev;
					DBG("[piewin] BUTTON_RELEASE detail=%u at %d,%d\n", e->detail, e->event_x, e->event_y);
					if (e->detail == 1) {  // left button release confirms selection
						int idx = sector_index_from_point((int)count, app.width, app.height, e->event_x, e->event_y);
						DBG("[piewin] RELEASE on idx=%d (pressed=%d)\n", idx, pressed_idx);
						// Select based on release location (common UX)
						if (idx >= 0 && idx < (int)count) {
							fprintf(stdout, "%s\n", entries[idx].text);
							fflush(stdout);
							DBG("[piewin] SELECT idx=%d \"%s\"\n", idx, entries[idx].text);
							exit_code = 0;
							running = 0;
						}
					}
					pressed_idx = -1;
				}
				break;

			case XCB_KEY_PRESS:
				{
					xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;
					xcb_keysym_t sym = xcb_key_symbols_get_keysym(app.keysyms, e->detail, 0);
					DBG("[piewin] KEY_PRESS detail=%u sym=0x%08x\n", e->detail, (unsigned)sym);
					if (sym == XK_Escape || sym == 'q' || sym == 'Q') {
						DBG("[piewin] Quit key pressed (sym=0x%08x)\n", (unsigned)sym);
						exit_code = 1;
						running = 0;
					}
				}
				break;

			case XCB_CONFIGURE_NOTIFY:
				{
					xcb_configure_notify_event_t *e = (xcb_configure_notify_event_t *)ev;
					DBG("[piewin] CONFIGURE_NOTIFY w=%u h=%u (cur=%d,%d)\n",
					    e->width, e->height, app.width, app.height);
					if (e->width != app.width || e->height != app.height) {
						app.width = e->width;
						app.height = e->height;
						DBG("[piewin] RESIZE -> %dx%d (recreate surfaces)\n", app.width, app.height);
						recreate_cairo(&app);
						draw(&app, entries, (int)count, hover_idx);
					}
				}
				break;

			case XCB_CLIENT_MESSAGE:
				{
					xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
					DBG("[piewin] CLIENT_MESSAGE type=%u data0=%u\n", cm->type, (unsigned)cm->data.data32[0]);
					if (cm->type == app.WM_PROTOCOLS && (xcb_atom_t)cm->data.data32[0] == app.WM_DELETE_WINDOW) {
						exit_code = 1;
						running = 0;
					}
				}
				break;

			default: break;
		}
		free(ev);
	}

	// Cleanup
	ungrab_input(&app);
	if (app.cr) cairo_destroy(app.cr);
	if (app.csurf) cairo_surface_destroy(app.csurf);
	if (app.bufcr) cairo_destroy(app.bufcr);
	if (app.bufsurf) cairo_surface_destroy(app.bufsurf);
	if (app.keysyms) xcb_key_symbols_free(app.keysyms);
	if (app.win) xcb_destroy_window(conn, app.win);
	xcb_flush(conn);
	xcb_disconnect(conn);

	for (size_t i = 0; i < count; ++i)
		free(entries[i].text);
	free(entries);
	DBG("[piewin] Exit code %d\n", exit_code);
	return exit_code;
}
