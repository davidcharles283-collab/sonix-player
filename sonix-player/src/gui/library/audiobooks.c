#include "audiobooks.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/library/audiobookextras.h"
#include "src/gui/library/search.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/audio/audio.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/library/audiobookdb.h"
#include "src/system/playback/audiobook.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/sleeptimer.h"

lv_obj_t *audiobooks_screen;
lv_obj_t *audiobooklibrary_screen;
lv_obj_t *book_detail_screen;
lv_obj_t *audiobooksettings_screen;
lv_obj_t *audiobookscan_screen;
lv_obj_t *audiobookcontrols_screen;

static gui_config_t *saved_cfg;
static void start_scan(void *user);

// ---------------------------------------------------------------------------
// Layout & Coverloader Constants
//
// All coverloader slots stay strictly inside the 12 slots reserved for
// audiobooks (COVERLOADER_AUDIOBOOKS_BASE .. +11).
// ---------------------------------------------------------------------------

#define BOTTOM_BAR_H      66
#define CARD_RADIUS       18
#define HERO_COVER_SIZE   72
#define GRID_GAP          14
#define GRID_COVER_DECODE 180
#define BM_COVER_SIZE     44
#define THUMB_POLL_MS     120

#define SLOT_HERO         (COVERLOADER_AUDIOBOOKS_BASE + 0)
#define SLOT_POOL_BASE    (COVERLOADER_AUDIOBOOKS_BASE + 1)
#define SLOT_POOL_COUNT   10

// ---------------------------------------------------------------------------
// Adaptive Theme & Dynamic Color Helpers
//
// Every surface, text, badge, and highlight derives from theme(), so Dark /
// Light mode, Accent colour, and Dynamic Tint all propagate automatically.
// ---------------------------------------------------------------------------

static lv_color_t color_ink(lv_color_t bg) {
	int lum = ((int)bg.red * 299 + (int)bg.green * 587 + (int)bg.blue * 114) / 1000;
	return lum >= 140 ? lv_color_make(24, 24, 24) : lv_color_white();
}

static lv_color_t accent_ink(void) {
	return color_ink(theme()->accent);
}

// Primary headings and titles subtly borrow the accent hue when Dynamic Tint
// is enabled (matching the warm tinted typography in the mockups), while
// staying crisp neutral white/charcoal when Dynamic Tint is off.
static lv_color_t heading_color(void) {
	if (!theme_dynamic_tint()) {
		return theme()->text_primary;
	}
	if (theme_is_dark()) {
		return lv_color_mix(theme()->accent, lv_color_white(), 42);
	}
	return lv_color_mix(theme()->accent, theme()->text_primary, 48);
}

// Highlight fill for the currently playing chapter card: luminous soft-accent
// in dark mode and vibrant accent in light mode.
static lv_color_t active_chapter_bg(void) {
	if (theme_is_dark()) {
		return lv_color_mix(lv_color_white(), theme()->accent, 68);
	}
	return theme()->accent;
}

// Soft accent fill for circular bookmark play buttons.
static lv_color_t soft_accent_bg(void) {
	if (theme_is_dark()) {
		return lv_color_mix(lv_color_white(), theme()->accent, 115);
	}
	return lv_color_mix(theme()->surface, theme()->accent, 35);
}

// Muted accent badge for bookmark group counts ("Dune [2]").
static lv_color_t badge_accent_bg(void) {
	return lv_color_mix(theme()->accent, theme()->surface, 170);
}

// Track behind book progress bars.
static lv_color_t progress_track_color(void) {
	if (theme_is_dark()) {
		return lv_color_mix(theme()->text_secondary, theme()->surface, 70);
	}
	return lv_color_mix(theme()->text_secondary, theme()->surface, 55);
}

// ---------------------------------------------------------------------------
// Formatting & Metadata Helpers
// ---------------------------------------------------------------------------

static void format_clock(double seconds, char *out, size_t size) {
	if (seconds < 0) {
		seconds = 0;
	}
	long total = (long)seconds;
	long hours = total / 3600;
	long minutes = (total % 3600) / 60;
	long secs = total % 60;
	if (hours > 0) {
		snprintf(out, size, "%ld:%02ld:%02ld", hours, minutes, secs);
	} else {
		snprintf(out, size, "%02ld:%02ld", minutes, secs);
	}
}

static void format_remaining(double seconds, char *out, size_t size) {
	if (seconds <= 0) {
		out[0] = '\0';
		return;
	}
	long total_min = (long)((seconds + 59.0) / 60.0);
	long hours = total_min / 60;
	long mins = total_min % 60;
	if (hours > 0) {
		snprintf(out, size, "%ldh %02ldm", hours, mins);
	} else {
		snprintf(out, size, "%ldm", mins > 0 ? mins : 1);
	}
}

typedef struct {
	char title[256];
	char author[256];
	bool finished;
	int current_chapter;
	int total_chapters;
	int percent;
	double total_secs;
	double remaining_secs;
	char resume_file[512];
	double resume_pos;
} book_progress_info_t;

typedef struct {
	const char *target_file;
	int count;
	int found_idx;
} folder_part_counter_t;

static bool count_parts_cb(const char *part_path, const char *title, void *user) {
	(void)title;
	folder_part_counter_t *ctx = user;
	ctx->count++;
	if (ctx->target_file && ctx->target_file[0] && strcmp(part_path, ctx->target_file) == 0) {
		ctx->found_idx = ctx->count;
	}
	return true;
}

static void get_book_progress_info(const char *path, book_progress_info_t *info) {
	memset(info, 0, sizeof(*info));
	if (!path || !path[0]) {
		return;
	}
	audiobookdb_book_info(path, info->title, sizeof(info->title), info->author, sizeof(info->author), NULL);
	if (!info->title[0]) {
		const char *slash = strrchr(path, '/');
		snprintf(info->title, sizeof(info->title), "%s", slash ? slash + 1 : path);
	}
	info->finished = audiobookdb_is_finished(path);
	if (info->finished) {
		info->percent = 100;
		info->current_chapter = 1;
		info->total_chapters = 1;
		return;
	}

	bool has_pos = audiobookdb_get_position(path, info->resume_file, sizeof(info->resume_file), &info->resume_pos);
	bool is_folder = audiobookdb_is_folder_book(path);

	// If this book is currently loaded in playback, prefer live progress/duration.
	char playing_book[512] = "";
	if (audiobook_current_book(playing_book, sizeof(playing_book), NULL, 0) && strcmp(playing_book, path) == 0) {
		device_state_t st;
		device_state_get(&st);
		if (st.progress_total_secs > 1.0 && !is_folder) {
			info->total_secs = st.progress_total_secs;
			info->resume_pos = st.progress_current_secs;
			info->remaining_secs = st.progress_total_secs - st.progress_current_secs;
			if (info->remaining_secs < 0) {
				info->remaining_secs = 0;
			}
			has_pos = true;
		}
	}

	if (is_folder) {
		folder_part_counter_t ctx = {info->resume_file, 0, 0};
		audiobookdb_parts_for_each(path, count_parts_cb, &ctx);
		info->total_chapters = ctx.count > 0 ? ctx.count : 1;
		if (has_pos && ctx.found_idx > 0) {
			info->current_chapter = ctx.found_idx;
			info->percent = (ctx.found_idx * 100) / info->total_chapters;
		}
	} else {
		audiobook_chapter_entry_t *entries = NULL;
		int count = audiobook_get_file_chapters(path, &entries);
		if (count > 0 && entries) {
			info->total_chapters = count;
			if (has_pos && info->resume_pos > 1.0) {
				int idx = 1;
				for (int i = 0; i < count; i++) {
					double next_start = (i + 1 < count) ? entries[i + 1].start : 1e9;
					if (info->resume_pos >= entries[i].start && info->resume_pos < next_start) {
						idx = i + 1;
						break;
					}
				}
				info->current_chapter = idx;
				if (info->total_secs > 1.0) {
					info->percent = (int)((info->resume_pos * 100.0) / info->total_secs);
				} else {
					info->percent = (idx * 100) / count;
				}
			}
			audiobook_free_file_chapters(entries);
		} else {
			info->total_chapters = 1;
			if (info->total_secs > 1.0 && info->resume_pos > 0.5) {
				info->current_chapter = 1;
				info->percent = (int)((info->resume_pos * 100.0) / info->total_secs);
			} else if (has_pos && info->resume_pos > 1.0) {
				info->current_chapter = 1;
				info->percent = 50;
			}
		}
	}

	if (info->percent > 100) info->percent = 100;
	if (info->percent < 0) info->percent = 0;
}

// ---------------------------------------------------------------------------
// Playback Dispatch
// ---------------------------------------------------------------------------

static bool collect_part(const char *path, const char *title, void *user) {
	(void)title;
	char ***list = user;
	size_t n = 0;
	while ((*list) && (*list)[n]) {
		n++;
	}
	char **grown = realloc(*list, (n + 2) * sizeof(char *));
	if (!grown) {
		return false;
	}
	*list = grown;
	grown[n] = strdup(path);
	grown[n + 1] = NULL;
	return grown[n] != NULL;
}

static void play_from(const char *book, const char *file, double seconds) {
	if (!audiobookdb_is_folder_book(book)) {
		if (seconds > 1.0) {
			device_state_play_file_at(file, seconds);
		} else {
			device_state_play_file(file);
		}
		return;
	}

	char **list = NULL;
	audiobookdb_parts_for_each(book, collect_part, &list);
	int count = 0;
	int start = -1;
	while (list && list[count]) {
		if (start < 0 && strcmp(list[count], file) == 0) {
			start = count;
		}
		count++;
	}
	if (count > 0) {
		double at = start >= 0 && seconds > 1.0 ? seconds : -1.0;
		device_state_play_list_ordered_at((const char *const *)list, count, start >= 0 ? start : 0, at);
	}
	for (int i = 0; i < count; i++) {
		free(list[i]);
	}
	free(list);
}

static void play_book(const char *book) {
	char file[512];
	double resume = 0;
	if (!audiobook_resume_point(book, file, sizeof(file), &resume)) {
		return;
	}
	play_from(book, file, resume);

	audiobookdb_touch(book);
	player_refresh_now_playing();
	player_sheet_open(true);
}

void audiobooks_play_at(const char *book, const char *file, double seconds) {
	if (!book || !book[0] || !file || !file[0]) {
		return;
	}
	play_from(book, file, seconds);
	audiobookdb_save_position(book, file, seconds);
	audiobookdb_touch(book);
	player_refresh_now_playing();
}

static bool first_part_cb(const char *path, const char *title, void *user) {
	(void)title;
	snprintf(user, 512, "%s", path);
	return false;
}

static const char *cover_source(const char *book, char *first, size_t size) {
	if (size < 512 || !audiobookdb_is_folder_book(book)) {
		return book;
	}
	first[0] = '\0';
	audiobookdb_parts_for_each(book, first_part_cb, first);
	return first[0] ? first : book;
}

static lv_obj_t *corner_button(lv_obj_t *screen, gui_config_t *cfg, int slot, const lv_image_dsc_t *glyph, lv_event_cb_t cb) {
	lv_obj_t *button = lv_btn_create(screen);
	lv_obj_set_size(button, 56, 56);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_pad_all(button, 0, 0);
	lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -cfg->padding - slot * (56 + 6), cfg->padding + cfg->top_bar_height);
	if (cb) {
		lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
	}

	lv_obj_t *icon = lv_image_create(button);
	lv_image_set_src(icon, glyph);
	lv_obj_add_style(icon, &theme_style_icon, 0);
	lv_obj_center(icon);
	return button;
}

static void settings_click_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) {
		return;
	}
	switch_screen(audiobooksettings_screen);
}

// ---------------------------------------------------------------------------
// Unified 3-Tab Audiobooks Hub (audiobooks_screen)
//
//   Tab 0: Library   — Continue Listening hero card + 2-column book cover grid
//   Tab 1: Chapters  — Book subtitle + rounded chapter cards with active highlight
//   Tab 2: Bookmarks — Full-width "+ Add Bookmark" CTA + book-grouped marks
//   Bottom Pill Bar  — [ Library ] [ Chapters ] [ Bookmarks ] [ 1.25x ]
// ---------------------------------------------------------------------------

typedef enum {
	AB_TAB_LIBRARY   = 0,
	AB_TAB_CHAPTERS  = 1,
	AB_TAB_BOOKMARKS = 2,
} ab_tab_t;

static ab_tab_t active_tab = AB_TAB_LIBRARY;
static char selected_book_path[512]; // book shown in Chapters tab

static lv_obj_t *header_title;
static lv_obj_t *section_empty;
static lv_obj_t *view_library;
static lv_obj_t *view_chapters;
static lv_obj_t *view_bookmarks;
static lv_obj_t *bottom_bar;
static lv_obj_t *nav_pills[3];
static lv_obj_t *nav_labels[3];
static lv_obj_t *speed_pill;
static lv_obj_t *speed_label;

static lv_timer_t *thumb_timer;

// Forward declarations for tab builders & refreshers.
static void select_tab(ab_tab_t tab, bool reload);
static void reload_library_tab(void);
static void reload_chapters_tab(void);
static void reload_bookmarks_tab(void);
static void library_thumbs_update(void);
static void bookmarks_thumbs_update(void);
static void library_drop_all_thumbs(void);
static void bookmarks_drop_all_thumbs(void);

// ---------------------------------------------------------------------------
// Tab 0: Library (Continue Listening + 2-column Cover Grid)
// ---------------------------------------------------------------------------

typedef struct {
	char path[512];
	char title[256];
	int percent;
	lv_obj_t *card;
	lv_obj_t *cover_box;
	lv_obj_t *img;
	lv_obj_t *bar;
	lv_obj_t *title_lbl;
	int slot; // 0..SLOT_POOL_COUNT-1 when assigned, else -1
	bool has_thumb;
	bool thumb_requested;
	bool thumb_settled;
	cover_image_t thumb;
} grid_book_t;

static lv_obj_t *lib_continue_heading;
static lv_obj_t *lib_hero_card;
static lv_obj_t *lib_hero_cover_box;
static lv_obj_t *lib_hero_img;
static lv_obj_t *lib_hero_title;
static lv_obj_t *lib_hero_sub;
static lv_obj_t *lib_hero_play_btn;
static lv_obj_t *lib_hero_play_icon;
static lv_obj_t *lib_grid_cont;

static char hero_book_path[512];
static cover_image_t hero_thumb;
static bool hero_has_thumb;
static bool hero_thumb_requested;
static bool hero_thumb_settled;

static grid_book_t *grid_books;
static int grid_book_count;
static int slot_owner[SLOT_POOL_COUNT]; // index in grid_books, or -1

static void hero_drop_thumb(void) {
	coverloader_release(SLOT_HERO);
	if (hero_has_thumb) {
		if (lib_hero_img) {
			lv_image_set_src(lib_hero_img, &icon_book_headphones_row);
			lv_image_set_inner_align(lib_hero_img, LV_IMAGE_ALIGN_CENTER);
			lv_obj_add_style(lib_hero_img, &theme_style_icon, 0);
			lv_obj_set_style_image_recolor_opa(lib_hero_img, LV_OPA_COVER, 0);
		}
		cover_free(&hero_thumb);
		hero_has_thumb = false;
	}
	hero_thumb_requested = false;
	hero_thumb_settled = false;
}

static void grid_book_drop_thumb(int idx) {
	if (!grid_books || idx < 0 || idx >= grid_book_count) {
		return;
	}
	grid_book_t *b = &grid_books[idx];
	if (b->slot >= 0 && b->slot < SLOT_POOL_COUNT) {
		coverloader_release(SLOT_POOL_BASE + b->slot);
		if (slot_owner[b->slot] == idx) {
			slot_owner[b->slot] = -1;
		}
		b->slot = -1;
	}
	if (b->has_thumb) {
		if (b->img) {
			lv_image_set_src(b->img, &icon_book_headphones_row);
			lv_image_set_inner_align(b->img, LV_IMAGE_ALIGN_CENTER);
			lv_obj_add_style(b->img, &theme_style_icon, 0);
			lv_obj_set_style_image_recolor_opa(b->img, LV_OPA_COVER, 0);
		}
		cover_free(&b->thumb);
		b->has_thumb = false;
	}
	b->thumb_requested = false;
	b->thumb_settled = false;
}

static void library_drop_all_thumbs(void) {
	hero_drop_thumb();
	for (int i = 0; i < grid_book_count; i++) {
		grid_book_drop_thumb(i);
	}
	for (int s = 0; s < SLOT_POOL_COUNT; s++) {
		slot_owner[s] = -1;
		coverloader_release(SLOT_POOL_BASE + s);
	}
}

static void library_free_grid(void) {
	library_drop_all_thumbs();
	if (lib_grid_cont) {
		lv_obj_clean(lib_grid_cont);
	}
	free(grid_books);
	grid_books = NULL;
	grid_book_count = 0;
}

static int acquire_grid_slot(int book_idx) {
	// Prefer a free slot.
	for (int s = 0; s < SLOT_POOL_COUNT; s++) {
		if (slot_owner[s] < 0) {
			slot_owner[s] = book_idx;
			return s;
		}
	}
	// Evict the slot furthest from `book_idx`.
	int best_s = 0;
	int best_dist = -1;
	for (int s = 0; s < SLOT_POOL_COUNT; s++) {
		int dist = abs(slot_owner[s] - book_idx);
		if (dist > best_dist) {
			best_dist = dist;
			best_s = s;
		}
	}
	grid_book_drop_thumb(slot_owner[best_s]);
	slot_owner[best_s] = book_idx;
	return best_s;
}

static void library_thumbs_update(void) {
	if (active_tab != AB_TAB_LIBRARY) {
		return;
	}
	bool pending = false;

	// 1. Continue Listening hero cover
	if (hero_book_path[0] && !hero_thumb_settled && lib_hero_img) {
		if (!hero_thumb_requested) {
			char first[512];
			coverloader_request(SLOT_HERO, cover_source(hero_book_path, first, sizeof(first)), HERO_COVER_SIZE);
			hero_thumb_requested = true;
		}
		bool finished = false;
		cover_image_t image;
		if (coverloader_take(SLOT_HERO, &image, &finished)) {
			hero_thumb = image;
			hero_has_thumb = true;
			hero_thumb_settled = true;
			lv_image_set_src(lib_hero_img, &hero_thumb.dsc);
			lv_image_set_inner_align(lib_hero_img, LV_IMAGE_ALIGN_COVER);
			lv_obj_set_style_image_recolor_opa(lib_hero_img, LV_OPA_TRANSP, 0);
		} else if (finished) {
			hero_thumb_settled = true;
		} else {
			pending = true;
		}
	}

	// 2. Visible 2-column grid covers (viewport-pooled to keep RAM low)
	if (grid_books && grid_book_count > 0 && view_library) {
		lv_area_t view_a;
		lv_obj_get_coords(view_library, &view_a);
		int margin = 260;

		for (int i = 0; i < grid_book_count; i++) {
			grid_book_t *b = &grid_books[i];
			if (!b->card || !b->img) {
				continue;
			}
			lv_area_t ca;
			lv_obj_get_coords(b->card, &ca);
			bool near = (ca.y2 >= view_a.y1 - margin && ca.y1 <= view_a.y2 + margin);

			if (!near) {
				if (b->slot >= 0 || b->has_thumb) {
					grid_book_drop_thumb(i);
				}
				continue;
			}

			if (b->thumb_settled) {
				continue;
			}
			if (b->slot < 0) {
				b->slot = acquire_grid_slot(i);
			}
			int cl_slot = SLOT_POOL_BASE + b->slot;
			if (!b->thumb_requested) {
				char first[512];
				coverloader_request(cl_slot, cover_source(b->path, first, sizeof(first)), GRID_COVER_DECODE);
				b->thumb_requested = true;
			}
			bool finished = false;
			cover_image_t image;
			if (coverloader_take(cl_slot, &image, &finished)) {
				b->thumb = image;
				b->has_thumb = true;
				b->thumb_settled = true;
				lv_image_set_src(b->img, &b->thumb.dsc);
				lv_image_set_inner_align(b->img, LV_IMAGE_ALIGN_COVER);
				lv_obj_set_style_image_recolor_opa(b->img, LV_OPA_TRANSP, 0);
			} else if (finished) {
				b->thumb_settled = true;
			} else {
				pending = true;
			}
		}
	}

	if (pending && thumb_timer) {
		lv_timer_resume(thumb_timer);
	}
}

static void library_scroll_cb(lv_event_t *e) {
	(void)e;
	library_thumbs_update();
}

static void hero_play_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	if (hero_book_path[0]) {
		play_book(hero_book_path);
	}
}

static void hero_card_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	lv_obj_t *t = lv_event_get_target(e);
	if (t == lib_hero_play_btn || (t && lv_obj_get_parent(t) == lib_hero_play_btn)) {
		return;
	}
	if (hero_book_path[0]) {
		snprintf(selected_book_path, sizeof(selected_book_path), "%s", hero_book_path);
		select_tab(AB_TAB_CHAPTERS, true);
	}
}

static void grid_book_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	if (!grid_books || idx < 0 || idx >= grid_book_count) {
		return;
	}
	snprintf(selected_book_path, sizeof(selected_book_path), "%s", grid_books[idx].path);
	select_tab(AB_TAB_CHAPTERS, true);
}

typedef struct {
	char first_path[512];
} first_book_ctx_t;

static bool pick_first_cb(const char *name, const char *path, void *user) {
	(void)name;
	first_book_ctx_t *ctx = user;
	if (!ctx->first_path[0] && path && path[0]) {
		snprintf(ctx->first_path, sizeof(ctx->first_path), "%s", path);
	}
	return false;
}

typedef struct {
	grid_book_t *items;
	int count;
	int cap;
} grid_collect_t;

static bool collect_all_books_cb(const char *name, const char *path, void *user) {
	grid_collect_t *c = user;
	if (!path || !path[0]) {
		return true;
	}
	if (c->count >= c->cap) {
		int grown = c->cap ? c->cap * 2 : 32;
		grid_book_t *bigger = realloc(c->items, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		c->items = bigger;
		c->cap = grown;
	}
	grid_book_t *b = &c->items[c->count];
	memset(b, 0, sizeof(*b));
	snprintf(b->path, sizeof(b->path), "%s", path);
	snprintf(b->title, sizeof(b->title), "%s", name ? name : "");
	b->slot = -1;
	c->count++;
	return true;
}

static void resolve_hero_book(char *out, size_t size) {
	out[0] = '\0';
	// 1. Currently playing audiobook takes priority.
	if (audiobook_current_book(out, size, NULL, 0) && out[0]) {
		return;
	}
	// 2. Most recently listened book in progress.
	audiobookdb_index_t *ix = audiobookdb_index_open(AUDIOBOOK_LIST_CONTINUE, NULL, AUDIOBOOK_ORDER_PLAYED, true);
	if (ix) {
		if (audiobookdb_index_count(ix) > 0) {
			first_book_ctx_t ctx = {{0}};
			audiobookdb_index_window(ix, 0, 1, pick_first_cb, &ctx);
			snprintf(out, size, "%s", ctx.first_path);
		}
		audiobookdb_index_close(ix);
	}
	if (out[0]) {
		return;
	}
	// 3. Fallback: first book in library.
	ix = audiobookdb_index_open(AUDIOBOOK_LIST_ALL, NULL, AUDIOBOOK_ORDER_NAME, false);
	if (ix) {
		if (audiobookdb_index_count(ix) > 0) {
			first_book_ctx_t ctx = {{0}};
			audiobookdb_index_window(ix, 0, 1, pick_first_cb, &ctx);
			snprintf(out, size, "%s", ctx.first_path);
		}
		audiobookdb_index_close(ix);
	}
}

static void paint_library_dynamic_colors(void) {
	lv_color_t head = heading_color();
	lv_color_t track = progress_track_color();
	lv_color_t accent = theme()->accent;
	lv_color_t ink = accent_ink();

	if (lib_continue_heading) {
		lv_obj_set_style_text_color(lib_continue_heading, head, 0);
	}
	if (lib_hero_cover_box) {
		lv_obj_set_style_bg_color(lib_hero_cover_box, theme()->cover_bg, 0);
	}
	if (lib_hero_title) {
		lv_obj_set_style_text_color(lib_hero_title, head, 0);
	}
	if (lib_hero_play_btn) {
		lv_obj_set_style_bg_color(lib_hero_play_btn, accent, 0);
	}
	if (lib_hero_play_icon) {
		lv_obj_set_style_image_recolor(lib_hero_play_icon, ink, 0);
	}

	for (int i = 0; i < grid_book_count; i++) {
		grid_book_t *b = &grid_books[i];
		if (b->cover_box) {
			lv_obj_set_style_bg_color(b->cover_box, theme()->cover_bg, 0);
		}
		if (b->bar) {
			lv_obj_set_style_bg_color(b->bar, track, LV_PART_MAIN);
			lv_obj_set_style_bg_color(b->bar, accent, LV_PART_INDICATOR);
		}
		if (b->title_lbl) {
			lv_obj_set_style_text_color(b->title_lbl, head, 0);
		}
	}
}

static void reload_library_tab(void) {
	library_free_grid();

	if (audiobookdb_count() == 0) {
		lv_obj_set_hidden(view_library, true);
		lv_obj_set_hidden(section_empty, false);
		return;
	}
	lv_obj_set_hidden(section_empty, true);
	if (active_tab == AB_TAB_LIBRARY) {
		lv_obj_set_hidden(view_library, false);
	}

	// ---- Continue Listening hero card ----
	resolve_hero_book(hero_book_path, sizeof(hero_book_path));
	if (hero_book_path[0]) {
		book_progress_info_t info;
		get_book_progress_info(hero_book_path, &info);
		lv_label_set_text(lib_hero_title, info.title);

		char sub[288];
		char rem[48];
		format_remaining(info.remaining_secs, rem, sizeof(rem));
		if (rem[0]) {
			snprintf(sub, sizeof(sub), "%d%% · %s", info.percent, rem);
		} else if (info.total_chapters > 1 && info.current_chapter > 0) {
			snprintf(sub, sizeof(sub), "%d%% · %s %d/%d", info.percent, tr("chapter"), info.current_chapter, info.total_chapters);
		} else if (info.author[0]) {
			snprintf(sub, sizeof(sub), "%d%% · %s", info.percent, info.author);
		} else {
			snprintf(sub, sizeof(sub), "%d%%", info.percent);
		}
		lv_label_set_text(lib_hero_sub, sub);
		lv_obj_set_hidden(lib_continue_heading, false);
		lv_obj_set_hidden(lib_hero_card, false);

		if (!selected_book_path[0]) {
			snprintf(selected_book_path, sizeof(selected_book_path), "%s", hero_book_path);
		}
	} else {
		lv_obj_set_hidden(lib_continue_heading, true);
		lv_obj_set_hidden(lib_hero_card, true);
	}

	// ---- 2-column Book Grid ----
	audiobookdb_index_t *ix = audiobookdb_index_open(AUDIOBOOK_LIST_ALL, NULL, AUDIOBOOK_ORDER_PLAYED, true);
	if (ix) {
		int total = audiobookdb_index_count(ix);
		if (total > 0) {
			grid_collect_t coll = {NULL, 0, 0};
			int cap = total > 120 ? 120 : total;
			audiobookdb_index_window(ix, 0, cap, collect_all_books_cb, &coll);
			grid_books = coll.items;
			grid_book_count = coll.count;
		}
		audiobookdb_index_close(ix);
	}

	int scr_w = saved_cfg ? (int)saved_cfg->screen_width : 480;
	int pad = saved_cfg ? (int)saved_cfg->padding : 15;
	int avail_w = scr_w - 2 * pad;
	int card_w = (avail_w - GRID_GAP) / 2;
	int cover_h = card_w - 8;

	lv_color_t head = heading_color();
	lv_color_t track = progress_track_color();
	lv_color_t accent = theme()->accent;

	for (int i = 0; i < grid_book_count; i++) {
		grid_book_t *b = &grid_books[i];
		book_progress_info_t info;
		get_book_progress_info(b->path, &info);
		b->percent = info.percent;
		if (info.title[0]) {
			snprintf(b->title, sizeof(b->title), "%s", info.title);
		}

		b->card = lv_btn_create(lib_grid_cont);
		lv_obj_set_size(b->card, card_w, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(b->card, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(b->card, 0, 0);
		lv_obj_set_style_shadow_width(b->card, 0, 0);
		lv_obj_set_style_pad_all(b->card, 0, 0);
		lv_obj_set_style_pad_row(b->card, 8, 0);
		lv_obj_set_style_opa(b->card, LV_OPA_70, LV_STATE_PRESSED);
		lv_obj_set_scrollable(b->card, false);
		lv_obj_set_event_bubble(b->card, true);
		lv_obj_set_flex_flow(b->card, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(b->card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_event_cb(b->card, grid_book_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		b->cover_box = lv_obj_create(b->card);
		lv_obj_set_size(b->cover_box, card_w, cover_h);
		lv_obj_set_style_bg_color(b->cover_box, theme()->cover_bg, 0);
		lv_obj_set_style_bg_opa(b->cover_box, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(b->cover_box, 18, 0);
		lv_obj_set_style_clip_corner(b->cover_box, true, 0);
		lv_obj_set_style_border_width(b->cover_box, 0, 0);
		lv_obj_set_style_pad_all(b->cover_box, 0, 0);
		lv_obj_set_scrollable(b->cover_box, false);
		lv_obj_set_clickable(b->cover_box, false);
		lv_obj_set_event_bubble(b->cover_box, true);

		b->img = lv_image_create(b->cover_box);
		lv_obj_set_size(b->img, card_w, cover_h);
		lv_image_set_src(b->img, &icon_book_headphones_row);
		lv_image_set_inner_align(b->img, LV_IMAGE_ALIGN_CENTER);
		lv_obj_add_style(b->img, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(b->img, LV_OPA_COVER, 0);
		lv_obj_set_clickable(b->img, false);
		lv_obj_set_event_bubble(b->img, true);

		b->bar = lv_bar_create(b->card);
		lv_obj_set_size(b->bar, card_w, 8);
		lv_bar_set_range(b->bar, 0, 100);
		lv_bar_set_value(b->bar, b->percent, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(b->bar, track, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(b->bar, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_bg_color(b->bar, accent, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(b->bar, LV_OPA_COVER, LV_PART_INDICATOR);
		lv_obj_set_style_radius(b->bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_radius(b->bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
		lv_obj_set_clickable(b->bar, false);
		lv_obj_set_event_bubble(b->bar, true);

		b->title_lbl = lv_label_create(b->card);
		lv_label_set_text(b->title_lbl, b->title);
		lv_label_set_long_mode(b->title_lbl, LV_LABEL_LONG_DOT);
		lv_obj_set_width(b->title_lbl, card_w);
		lv_obj_set_height(b->title_lbl, lv_font_get_line_height(&font_ui_20));
		lv_obj_set_style_text_align(b->title_lbl, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_font(b->title_lbl, &font_ui_20, 0);
		lv_obj_set_style_text_color(b->title_lbl, head, 0);
		lv_obj_set_clickable(b->title_lbl, false);
		lv_obj_set_event_bubble(b->title_lbl, true);
	}

	paint_library_dynamic_colors();
	lv_obj_update_layout(view_library);
	library_thumbs_update();
}

// ---------------------------------------------------------------------------
// Tab 1: Chapters
// ---------------------------------------------------------------------------

typedef struct {
	char title[192];
	char path[512];
	double start;
	double duration;
} ab_chap_t;

static ab_chap_t *chap_items;
static int chap_count;
static int chap_active_idx = -1;
static char chap_book_path[512];

static lv_obj_t *chap_subtitle;
static lv_obj_t *chap_list;

static bool collect_chap_parts_cb(const char *path, const char *title, void *user) {
	(void)user;
	int grown = chap_count + 1;
	ab_chap_t *bigger = realloc(chap_items, (size_t)grown * sizeof(*bigger));
	if (!bigger) {
		return false;
	}
	chap_items = bigger;
	ab_chap_t *c = &chap_items[chap_count];
	snprintf(c->path, sizeof(c->path), "%s", path ? path : "");
	if (title && title[0]) {
		snprintf(c->title, sizeof(c->title), "%s", title);
	} else {
		snprintf(c->title, sizeof(c->title), "%s %d", tr("chapter"), chap_count + 1);
	}
	c->start = 0;
	c->duration = 0;
	chap_count++;
	return true;
}

static void chap_row_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (!chap_items || i < 0 || i >= chap_count || !chap_book_path[0]) {
		return;
	}
	if (audiobookdb_is_folder_book(chap_book_path)) {
		audiobooks_play_at(chap_book_path, chap_items[i].path, 0.0);
	} else {
		audiobooks_play_at(chap_book_path, chap_book_path, chap_items[i].start);
	}
	player_sheet_open(true);
}

static bool title_starts_with_number(const char *s) {
	return s && isdigit((unsigned char)s[0]);
}

static void reload_chapters_tab(void) {
	free(chap_items);
	chap_items = NULL;
	chap_count = 0;
	chap_active_idx = -1;
	lv_obj_clean(chap_list);

	// Pick book: selected book -> currently playing -> hero/first book.
	if (selected_book_path[0]) {
		snprintf(chap_book_path, sizeof(chap_book_path), "%s", selected_book_path);
	} else {
		resolve_hero_book(chap_book_path, sizeof(chap_book_path));
		snprintf(selected_book_path, sizeof(selected_book_path), "%s", chap_book_path);
	}

	if (!chap_book_path[0]) {
		lv_label_set_text(chap_subtitle, "");
		lv_obj_t *empty = lv_label_create(chap_list);
		lv_label_set_text(empty, tr("chapters_empty"));
		lv_obj_add_style(empty, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(empty, &font_ui_22, 0);
		lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(empty, lv_pct(100));
		lv_obj_set_style_pad_top(empty, 60, 0);
		return;
	}

	char title[256] = "";
	audiobookdb_book_info(chap_book_path, title, sizeof(title), NULL, 0, NULL);
	if (!title[0]) {
		const char *slash = strrchr(chap_book_path, '/');
		snprintf(title, sizeof(title), "%.255s", slash ? slash + 1 : chap_book_path);
	}

	char resume_file[512] = "";
	double resume_pos = 0;
	bool has_pos = audiobookdb_get_position(chap_book_path, resume_file, sizeof(resume_file), &resume_pos);
	bool finished = audiobookdb_is_finished(chap_book_path);

	// Prefer live playback position if this book is currently playing.
	char cur_book[512] = "";
	char cur_file[512] = "";
	double live_total = 0;
	if (audiobook_current_book(cur_book, sizeof(cur_book), cur_file, sizeof(cur_file)) &&
	    strcmp(cur_book, chap_book_path) == 0) {
		device_state_t st;
		device_state_get(&st);
		snprintf(resume_file, sizeof(resume_file), "%s", cur_file);
		resume_pos = st.progress_current_secs;
		live_total = st.progress_total_secs;
		has_pos = true;
	}

	if (audiobookdb_is_folder_book(chap_book_path)) {
		audiobookdb_parts_for_each(chap_book_path, collect_chap_parts_cb, NULL);
		if (!finished && has_pos && resume_file[0]) {
			for (int i = 0; i < chap_count; i++) {
				if (strcmp(chap_items[i].path, resume_file) == 0) {
					chap_active_idx = i;
					break;
				}
			}
		}
	} else {
		audiobook_chapter_entry_t *entries = NULL;
		int count = audiobook_get_file_chapters(chap_book_path, &entries);
		if (count > 0 && entries) {
			chap_items = calloc((size_t)count, sizeof(*chap_items));
			if (chap_items) {
				chap_count = count;
				for (int i = 0; i < count; i++) {
					snprintf(chap_items[i].title, sizeof(chap_items[i].title), "%s", entries[i].title);
					snprintf(chap_items[i].path, sizeof(chap_items[i].path), "%s", chap_book_path);
					chap_items[i].start = entries[i].start;
					if (i + 1 < count) {
						chap_items[i].duration = entries[i + 1].start - entries[i].start;
					} else if (live_total > entries[i].start) {
						chap_items[i].duration = live_total - entries[i].start;
					}
				}
			}
			audiobook_free_file_chapters(entries);

			if (!finished && has_pos && resume_pos >= 0.0) {
				for (int i = 0; i < chap_count; i++) {
					double next_start = (i + 1 < chap_count) ? chap_items[i + 1].start : 1e9;
					if (resume_pos >= chap_items[i].start && resume_pos < next_start) {
						chap_active_idx = i;
						break;
					}
				}
			}
		} else {
			chap_items = calloc(1, sizeof(*chap_items));
			if (chap_items) {
				chap_count = 1;
				snprintf(chap_items[0].title, sizeof(chap_items[0].title), "%s 1", tr("chapter"));
				snprintf(chap_items[0].path, sizeof(chap_items[0].path), "%s", chap_book_path);
				chap_items[0].start = 0;
				chap_items[0].duration = live_total;
			}
			if (!finished && has_pos && resume_pos > 1.0) {
				chap_active_idx = 0;
			}
		}
	}

	char sub_buf[320];
	snprintf(sub_buf, sizeof(sub_buf), "%s · %d %s", title, chap_count, tr("audiobook_chapters_count"));
	lv_label_set_text(chap_subtitle, sub_buf);

	lv_color_t head = heading_color();
	lv_color_t act_bg = active_chapter_bg();
	lv_color_t act_ink = color_ink(act_bg);

	lv_obj_t *active_row = NULL;
	for (int i = 0; i < chap_count; i++) {
		bool is_active = (i == chap_active_idx);
		bool is_completed = finished || (chap_active_idx >= 0 && i < chap_active_idx);

		lv_obj_t *row = lv_btn_create(chap_list);
		lv_obj_set_size(row, lv_pct(100), 72);
		lv_obj_set_style_radius(row, CARD_RADIUS, 0);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_pad_hor(row, 20, 0);
		lv_obj_set_style_pad_ver(row, 0, 0);
		lv_obj_set_style_pad_column(row, 10, 0);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_event_bubble(row, true);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_event_cb(row, chap_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

		if (is_active) {
			lv_obj_set_style_bg_color(row, act_bg, 0);
			lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
			lv_obj_set_style_opa(row, LV_OPA_80, LV_STATE_PRESSED);
			active_row = row;
		} else {
			lv_obj_add_style(row, &theme_style_card, 0);
			lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		}

		// Left: "09. Arrakis"
		char label_buf[224];
		if (title_starts_with_number(chap_items[i].title)) {
			snprintf(label_buf, sizeof(label_buf), "%s", chap_items[i].title);
		} else {
			snprintf(label_buf, sizeof(label_buf), "%02d. %s", i + 1, chap_items[i].title);
		}

		lv_obj_t *lbl = lv_label_create(row);
		lv_label_set_text(lbl, label_buf);
		lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
		lv_obj_set_width(lbl, 0);
		lv_obj_set_flex_grow(lbl, 1);
		lv_obj_set_height(lbl, lv_font_get_line_height(&font_ui_24));
		lv_obj_set_style_text_font(lbl, &font_ui_24, 0);
		lv_obj_set_style_text_color(lbl, is_active ? act_ink : head, 0);
		lv_obj_set_event_bubble(lbl, true);

		// Right: duration (if known)
		if (chap_items[i].duration > 0.5) {
			char dur_buf[32];
			format_clock(chap_items[i].duration, dur_buf, sizeof(dur_buf));
			lv_obj_t *dur = lv_label_create(row);
			lv_label_set_text(dur, dur_buf);
			lv_obj_set_style_text_font(dur, &font_ui_18, 0);
			if (is_active) {
				lv_obj_set_style_text_color(dur, act_ink, 0);
			} else {
				lv_obj_add_style(dur, &theme_style_text_dim, 0);
			}
			lv_obj_set_event_bubble(dur, true);
		}

		// Trailing glyph: waveform + play for active, checkmark for completed.
		if (is_active) {
			lv_obj_t *wave = lv_image_create(row);
			lv_image_set_src(wave, &icon_audio_waveform);
			lv_image_set_scale(wave, 192);
			lv_obj_set_style_image_recolor(wave, act_ink, 0);
			lv_obj_set_style_image_recolor_opa(wave, LV_OPA_COVER, 0);
			lv_obj_set_event_bubble(wave, true);

			lv_obj_t *play = lv_image_create(row);
			lv_image_set_src(play, &icon_play_small);
			lv_obj_set_style_image_recolor(play, act_ink, 0);
			lv_obj_set_style_image_recolor_opa(play, LV_OPA_COVER, 0);
			lv_obj_set_event_bubble(play, true);
		} else if (is_completed) {
			lv_obj_t *chk = lv_image_create(row);
			lv_image_set_src(chk, &icon_check_small);
			lv_obj_set_style_image_recolor(chk, theme()->text_secondary, 0);
			lv_obj_set_style_image_recolor_opa(chk, LV_OPA_COVER, 0);
			lv_obj_set_event_bubble(chk, true);
		}
	}

	if (active_row) {
		lv_obj_update_layout(chap_list);
		lv_obj_scroll_to_view(active_row, LV_ANIM_OFF);
	}
}

// ---------------------------------------------------------------------------
// Tab 2: Bookmarks (CTA "+ Add Bookmark" + Grouped Bookmarks by Book)
// ---------------------------------------------------------------------------

typedef struct {
	long long id;
	char book[512];
	char file[512];
	char label[192];
	double seconds;
} bm_entry_t;

typedef struct {
	char book[512];
	char title[256];
	int count;
	lv_obj_t *img;
	bool has_thumb;
	bool thumb_requested;
	bool thumb_settled;
	cover_image_t thumb;
} bm_group_t;

static lv_obj_t *bm_add_btn;
static lv_obj_t *bm_add_label;
static lv_obj_t *bm_list;

static bm_group_t *bm_groups;
static int bm_group_count;
static bm_entry_t *bm_entries;
static int bm_entry_count;

static void bookmarks_drop_all_thumbs(void) {
	for (int i = 0; i < bm_group_count; i++) {
		if (i < SLOT_POOL_COUNT) {
			coverloader_release(SLOT_POOL_BASE + i);
		}
		if (bm_groups[i].has_thumb) {
			cover_free(&bm_groups[i].thumb);
			bm_groups[i].has_thumb = false;
		}
		bm_groups[i].thumb_requested = false;
		bm_groups[i].thumb_settled = false;
	}
}

static void bookmarks_free_data(void) {
	bookmarks_drop_all_thumbs();
	if (bm_list) {
		lv_obj_clean(bm_list);
	}
	free(bm_groups);
	bm_groups = NULL;
	bm_group_count = 0;
	free(bm_entries);
	bm_entries = NULL;
	bm_entry_count = 0;
}

static void bookmarks_thumbs_update(void) {
	if (active_tab != AB_TAB_BOOKMARKS || !bm_groups) {
		return;
	}
	bool pending = false;
	int limit = bm_group_count < SLOT_POOL_COUNT ? bm_group_count : SLOT_POOL_COUNT;

	for (int i = 0; i < limit; i++) {
		bm_group_t *g = &bm_groups[i];
		if (!g->img || g->thumb_settled) {
			continue;
		}
		int cl_slot = SLOT_POOL_BASE + i;
		if (!g->thumb_requested) {
			char first[512];
			coverloader_request(cl_slot, cover_source(g->book, first, sizeof(first)), BM_COVER_SIZE);
			g->thumb_requested = true;
		}
		bool finished = false;
		cover_image_t image;
		if (coverloader_take(cl_slot, &image, &finished)) {
			g->thumb = image;
			g->has_thumb = true;
			g->thumb_settled = true;
			lv_image_set_src(g->img, &g->thumb.dsc);
			lv_image_set_inner_align(g->img, LV_IMAGE_ALIGN_COVER);
			lv_obj_set_style_image_recolor_opa(g->img, LV_OPA_TRANSP, 0);
		} else if (finished) {
			g->thumb_settled = true;
		} else {
			pending = true;
		}
	}

	if (pending && thumb_timer) {
		lv_timer_resume(thumb_timer);
	}
}

static void bm_add_click_cb(lv_event_t *e) {
	(void)e;
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	char book[512] = "";
	char file[512] = "";
	double secs = 0;
	char label[192] = "";

	if (audiobook_current_book(book, sizeof(book), file, sizeof(file))) {
		device_state_t st;
		device_state_get(&st);
		secs = st.progress_current_secs;
		audiobook_place_label(secs, label, sizeof(label));
	} else if (selected_book_path[0] && audiobook_resume_point(selected_book_path, file, sizeof(file), &secs)) {
		snprintf(book, sizeof(book), "%s", selected_book_path);
		book_progress_info_t info;
		get_book_progress_info(book, &info);
		if (info.current_chapter > 0) {
			snprintf(label, sizeof(label), "%s %d", tr("chapter"), info.current_chapter);
		}
	} else {
		return;
	}

	switch (audiobookdb_bookmark_add(book, file, secs, label)) {
	case AUDIOBOOK_BOOKMARK_SAVED:
		toast_glyph(&icon_bookmark_check, "ebookreader_bookmark_saved");
		reload_bookmarks_tab();
		break;
	case AUDIOBOOK_BOOKMARK_FULL:
		toast_error("audiobook_bookmarks_full");
		break;
	default:
		toast_error("audiobook_bookmark_failed");
		break;
	}
}

static void bm_play_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	if (!bm_entries || idx < 0 || idx >= bm_entry_count) {
		return;
	}
	const bm_entry_t *m = &bm_entries[idx];
	char cur_book[512] = "";
	char cur_file[512] = "";
	if (audiobook_current_book(cur_book, sizeof(cur_book), cur_file, sizeof(cur_file)) &&
	    strcmp(cur_book, m->book) == 0 && strcmp(cur_file, m->file) == 0) {
		device_state_seek(m->seconds);
		audiobook_note_position(m->seconds, 0, true);
		player_refresh_now_playing();
	} else {
		audiobooks_play_at(m->book, m->file, m->seconds);
	}
	player_sheet_open(true);
}

static void bm_reload_async(void *unused) {
	(void)unused;
	reload_bookmarks_tab();
}

static void bm_delete_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	if (!bm_entries || idx < 0 || idx >= bm_entry_count) {
		return;
	}
	if (audiobookdb_bookmark_remove(bm_entries[idx].id)) {
		toast_plain("ebookmarks_bookmark_deleted");
		lv_async_call(bm_reload_async, NULL);
	}
}

static bool collect_bm_group_cb(const char *title, const char *book, int count, void *user) {
	(void)user;
	int grown = bm_group_count + 1;
	bm_group_t *bigger = realloc(bm_groups, (size_t)grown * sizeof(*bigger));
	if (!bigger) {
		return false;
	}
	bm_groups = bigger;
	bm_group_t *g = &bm_groups[bm_group_count];
	memset(g, 0, sizeof(*g));
	snprintf(g->book, sizeof(g->book), "%s", book ? book : "");
	snprintf(g->title, sizeof(g->title), "%s", title ? title : "");
	g->count = count;
	bm_group_count++;
	return true;
}

typedef struct {
	const char *book;
	lv_obj_t *parent;
} bm_render_ctx_t;

static bool collect_and_render_bm_cb(long long id, const char *file, double seconds, const char *label, void *user) {
	bm_render_ctx_t *ctx = user;
	int grown = bm_entry_count + 1;
	bm_entry_t *bigger = realloc(bm_entries, (size_t)grown * sizeof(*bigger));
	if (!bigger) {
		return false;
	}
	bm_entries = bigger;
	int idx = bm_entry_count++;
	bm_entry_t *m = &bm_entries[idx];
	m->id = id;
	snprintf(m->book, sizeof(m->book), "%s", ctx->book);
	snprintf(m->file, sizeof(m->file), "%s", file ? file : "");
	snprintf(m->label, sizeof(m->label), "%s", label ? label : "");
	m->seconds = seconds;

	lv_color_t accent = theme()->accent;
	lv_color_t a_ink = accent_ink();
	lv_color_t play_bg = soft_accent_bg();
	lv_color_t play_ink = color_ink(play_bg);

	lv_obj_t *card = lv_obj_create(ctx->parent);
	lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, CARD_RADIUS, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_hor(card, 14, 0);
	lv_obj_set_style_pad_ver(card, 12, 0);
	lv_obj_set_style_pad_column(card, 12, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// Bookmark ribbon icon
	lv_obj_t *rib = lv_image_create(card);
	lv_image_set_src(rib, &icon_bookmark);
	lv_image_set_scale(rib, 210);
	lv_obj_set_style_image_recolor(rib, soft_accent_bg(), 0);
	lv_obj_set_style_image_recolor_opa(rib, LV_OPA_COVER, 0);
	lv_obj_set_event_bubble(rib, true);

	// Middle column: chapter title + accent timestamp pill
	lv_obj_t *mid = lv_obj_create(card);
	lv_obj_set_size(mid, 0, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(mid, 1);
	lv_obj_set_style_bg_opa(mid, 0, 0);
	lv_obj_set_style_border_width(mid, 0, 0);
	lv_obj_set_style_pad_all(mid, 0, 0);
	lv_obj_set_style_pad_row(mid, 6, 0);
	lv_obj_set_scrollable(mid, false);
	lv_obj_set_event_bubble(mid, true);
	lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *lbl = lv_label_create(mid);
	lv_label_set_text(lbl, m->label[0] ? m->label : tr("chapter"));
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lbl, lv_pct(100));
	lv_obj_set_height(lbl, lv_font_get_line_height(&font_ui_20));
	lv_obj_add_style(lbl, &theme_style_text, 0);
	lv_obj_set_style_text_font(lbl, &font_ui_20, 0);
	lv_obj_set_event_bubble(lbl, true);

	char clk[24];
	format_clock(seconds, clk, sizeof(clk));
	lv_obj_t *pill = lv_obj_create(mid);
	lv_obj_set_size(pill, LV_SIZE_CONTENT, 28);
	lv_obj_set_style_bg_color(pill, accent, 0);
	lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(pill, 8, 0);
	lv_obj_set_style_border_width(pill, 0, 0);
	lv_obj_set_style_pad_hor(pill, 10, 0);
	lv_obj_set_style_pad_ver(pill, 2, 0);
	lv_obj_set_scrollable(pill, false);
	lv_obj_set_clickable(pill, false);
	lv_obj_set_event_bubble(pill, true);

	lv_obj_t *plbl = lv_label_create(pill);
	lv_label_set_text(plbl, clk);
	lv_obj_set_style_text_font(plbl, &font_ui_16, 0);
	lv_obj_set_style_text_color(plbl, a_ink, 0);
	lv_obj_center(plbl);

	// Right: circular Play button
	lv_obj_t *play_btn = lv_btn_create(card);
	lv_obj_set_size(play_btn, 42, 42);
	lv_obj_set_style_radius(play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(play_btn, play_bg, 0);
	lv_obj_set_style_bg_opa(play_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(play_btn, 0, 0);
	lv_obj_set_style_shadow_width(play_btn, 0, 0);
	lv_obj_set_style_pad_all(play_btn, 0, 0);
	lv_obj_set_style_opa(play_btn, LV_OPA_70, LV_STATE_PRESSED);
	lv_obj_add_event_cb(play_btn, bm_play_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

	lv_obj_t *play_ic = lv_image_create(play_btn);
	lv_image_set_src(play_ic, &icon_play_small);
	lv_obj_set_style_image_recolor(play_ic, play_ink, 0);
	lv_obj_set_style_image_recolor_opa(play_ic, LV_OPA_COVER, 0);
	lv_obj_center(play_ic);

	// Trash delete button
	lv_obj_t *del_btn = lv_btn_create(card);
	lv_obj_set_size(del_btn, 40, 40);
	lv_obj_set_style_bg_opa(del_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(del_btn, 0, 0);
	lv_obj_set_style_shadow_width(del_btn, 0, 0);
	lv_obj_set_style_pad_all(del_btn, 0, 0);
	lv_obj_set_style_opa(del_btn, LV_OPA_60, LV_STATE_PRESSED);
	lv_obj_add_event_cb(del_btn, bm_delete_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

	lv_obj_t *del_ic = lv_image_create(del_btn);
	lv_image_set_src(del_ic, &icon_trash);
	lv_image_set_scale(del_ic, 200);
	lv_obj_set_style_image_recolor(del_ic, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(del_ic, LV_OPA_COVER, 0);
	lv_obj_center(del_ic);

	return true;
}

static void update_bm_add_cta(void) {
	if (!bm_add_btn || !bm_add_label) {
		return;
	}
	lv_obj_set_style_bg_color(bm_add_btn, theme()->accent, 0);
	lv_obj_set_style_text_color(bm_add_label, accent_ink(), 0);

	char book[512] = "";
	char file[512] = "";
	double secs = 0;
	bool can_add = false;

	if (audiobook_current_book(book, sizeof(book), file, sizeof(file))) {
		device_state_t st;
		device_state_get(&st);
		secs = st.progress_current_secs;
		can_add = true;
	} else if (selected_book_path[0] && audiobook_resume_point(selected_book_path, file, sizeof(file), &secs)) {
		can_add = true;
	}

	if (can_add) {
		char clk[24];
		format_clock(secs, clk, sizeof(clk));
		lv_label_set_text_fmt(bm_add_label, "+ %s · %s", tr("audiobook_add_bookmark"), clk);
		lv_obj_set_hidden(bm_add_btn, false);
	} else {
		lv_obj_set_hidden(bm_add_btn, true);
	}
}

static void reload_bookmarks_tab(void) {
	bookmarks_free_data();
	update_bm_add_cta();

	audiobookdb_bookmarked_books_for_each(collect_bm_group_cb, NULL);

	if (bm_group_count == 0) {
		lv_obj_t *empty = lv_label_create(bm_list);
		lv_label_set_text(empty, tr("audiobook_bookmarks_empty"));
		lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(empty, lv_pct(100));
		lv_obj_add_style(empty, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(empty, &font_ui_20, 0);
		lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_top(empty, 48, 0);
		return;
	}

	lv_color_t head = heading_color();
	lv_color_t b_bg = badge_accent_bg();
	lv_color_t b_ink = color_ink(b_bg);

	for (int i = 0; i < bm_group_count; i++) {
		bm_group_t *g = &bm_groups[i];

		lv_obj_t *hdr = lv_obj_create(bm_list);
		lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(hdr, 0, 0);
		lv_obj_set_style_border_width(hdr, 0, 0);
		lv_obj_set_style_pad_all(hdr, 0, 0);
		lv_obj_set_style_pad_top(hdr, i == 0 ? 4 : 12, 0);
		lv_obj_set_style_pad_bottom(hdr, 2, 0);
		lv_obj_set_style_pad_column(hdr, 12, 0);
		lv_obj_set_scrollable(hdr, false);
		lv_obj_set_event_bubble(hdr, true);
		lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		lv_obj_t *cbox = lv_obj_create(hdr);
		lv_obj_set_size(cbox, BM_COVER_SIZE, BM_COVER_SIZE);
		lv_obj_set_style_bg_color(cbox, theme()->cover_bg, 0);
		lv_obj_set_style_bg_opa(cbox, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(cbox, 8, 0);
		lv_obj_set_style_clip_corner(cbox, true, 0);
		lv_obj_set_style_border_width(cbox, 0, 0);
		lv_obj_set_style_pad_all(cbox, 0, 0);
		lv_obj_set_scrollable(cbox, false);
		lv_obj_set_clickable(cbox, false);

		g->img = lv_image_create(cbox);
		lv_obj_set_size(g->img, BM_COVER_SIZE, BM_COVER_SIZE);
		lv_image_set_src(g->img, &icon_book_headphones_row);
		lv_image_set_scale(g->img, 160);
		lv_image_set_inner_align(g->img, LV_IMAGE_ALIGN_CENTER);
		lv_obj_add_style(g->img, &theme_style_icon, 0);
		lv_obj_set_style_image_recolor_opa(g->img, LV_OPA_COVER, 0);

		lv_obj_t *tlbl = lv_label_create(hdr);
		lv_label_set_text(tlbl, g->title[0] ? g->title : tr("audiobooks"));
		lv_label_set_long_mode(tlbl, LV_LABEL_LONG_DOT);
		lv_obj_set_style_max_width(tlbl, 320, 0);
		lv_obj_set_height(tlbl, lv_font_get_line_height(&font_ui_24));
		lv_obj_set_style_text_font(tlbl, &font_ui_24, 0);
		lv_obj_set_style_text_color(tlbl, head, 0);

		lv_obj_t *badge = lv_obj_create(hdr);
		lv_obj_set_size(badge, 30, 30);
		lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(badge, b_bg, 0);
		lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
		lv_obj_set_style_border_width(badge, 0, 0);
		lv_obj_set_style_pad_all(badge, 0, 0);
		lv_obj_set_scrollable(badge, false);

		lv_obj_t *blbl = lv_label_create(badge);
		lv_label_set_text_fmt(blbl, "%d", g->count);
		lv_obj_set_style_text_font(blbl, &font_ui_16, 0);
		lv_obj_set_style_text_color(blbl, b_ink, 0);
		lv_obj_center(blbl);

		bm_render_ctx_t rctx = {g->book, bm_list};
		audiobookdb_bookmarks_for_each(g->book, collect_and_render_bm_cb, &rctx);
	}

	bookmarks_thumbs_update();
}

// ---------------------------------------------------------------------------
// Bottom Pill Bar & Tab Switching
// ---------------------------------------------------------------------------

static void format_speed_text(int permille, char *out, size_t size) {
	int whole = permille / 1000;
	int frac = permille % 1000;
	if (frac == 0) {
		snprintf(out, size, "%d.0x", whole);
	} else if (frac % 100 == 0) {
		snprintf(out, size, "%d.%dx", whole, frac / 100);
	} else {
		snprintf(out, size, "%d.%02dx", whole, frac / 10);
	}
}

static void refresh_bottom_bar(void) {
	if (!bottom_bar) {
		return;
	}
	lv_color_t accent = theme()->accent;
	lv_color_t dim = theme()->text_secondary;
	lv_color_t pill_on = theme()->surface_pressed;
	lv_color_t pill_off = theme()->surface;

	for (int i = 0; i < 3; i++) {
		if (!nav_pills[i] || !nav_labels[i]) {
			continue;
		}
		bool on = ((int)active_tab == i);
		lv_obj_set_style_bg_color(nav_pills[i], on ? pill_on : pill_off, 0);
		lv_obj_set_style_text_color(nav_labels[i], on ? accent : dim, 0);
		lv_obj_set_style_border_width(nav_pills[i], on ? 1 : 0, 0);
		lv_obj_set_style_border_color(nav_pills[i], accent, 0);
		lv_obj_set_style_border_opa(nav_pills[i], LV_OPA_40, 0);
	}

	if (speed_pill && speed_label) {
		int p = audiobook_speed_permille();
		char buf[16];
		format_speed_text(p, buf, sizeof(buf));
		lv_label_set_text(speed_label, buf);
		bool non_normal = (p != AUDIOBOOK_SPEED_NORMAL);
		lv_obj_set_style_bg_color(speed_pill, non_normal ? pill_on : pill_off, 0);
		lv_obj_set_style_text_color(speed_label, non_normal ? accent : dim, 0);
	}
}

static void speed_picked_cb(void *user) {
	int permille = (int)(intptr_t)user;
	audiobook_set_speed_permille(permille);
	if (audiobook_is_playing()) {
		audio_set_speed((double)audiobook_speed_permille() / 1000.0);
	}
	refresh_bottom_bar();
}

static void speed_pill_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	static const int CHOICES[] = {500, 750, 1000, 1250, 1500, 1750, 2000};
	static const char *const LABELS[] = {"0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "1.75x", "2.0x"};
	enum { N = (int)(sizeof(CHOICES) / sizeof(CHOICES[0])) };

	popover_item_t items[N];
	memset(items, 0, sizeof(items));
	int cur = audiobook_speed_permille();
	for (int i = 0; i < N; i++) {
		items[i].label = LABELS[i];
		items[i].action = speed_picked_cb;
		items[i].user = (void *)(intptr_t)CHOICES[i];
		items[i].checked = (CHOICES[i] == cur);
	}
	popover_show(lv_event_get_current_target(e), items, N);
}

static void select_tab(ab_tab_t tab, bool reload) {
	if (active_tab != tab) {
		if (active_tab == AB_TAB_LIBRARY) {
			library_drop_all_thumbs();
		} else if (active_tab == AB_TAB_BOOKMARKS) {
			bookmarks_drop_all_thumbs();
		}
		active_tab = tab;
	}

	bool has_books = audiobookdb_count() > 0;
	lv_obj_set_hidden(section_empty, has_books || tab != AB_TAB_LIBRARY);
	lv_obj_set_hidden(view_library, !has_books || tab != AB_TAB_LIBRARY);
	lv_obj_set_hidden(view_chapters, tab != AB_TAB_CHAPTERS);
	lv_obj_set_hidden(view_bookmarks, tab != AB_TAB_BOOKMARKS);

	if (header_title) {
		const char *tag = tab == AB_TAB_CHAPTERS ? "chapters" :
		                  tab == AB_TAB_BOOKMARKS ? "bookmarks" : "audiobook_library";
		lv_label_set_text(header_title, tr(tag));
		lv_obj_set_style_text_color(header_title, heading_color(), 0);
	}

	refresh_bottom_bar();

	if (reload) {
		if (tab == AB_TAB_LIBRARY) {
			reload_library_tab();
		} else if (tab == AB_TAB_CHAPTERS) {
			reload_chapters_tab();
		} else {
			reload_bookmarks_tab();
		}
	}
}

static void nav_pill_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	ab_tab_t tab = (ab_tab_t)(intptr_t)lv_event_get_user_data(e);
	select_tab(tab, true);
}

static void thumb_timer_cb(lv_timer_t *timer) {
	lv_timer_pause(timer);
	if (lv_screen_active() != audiobooks_screen) {
		return;
	}
	if (active_tab == AB_TAB_LIBRARY) {
		library_thumbs_update();
	} else if (active_tab == AB_TAB_BOOKMARKS) {
		bookmarks_thumbs_update();
	}
}

static void section_loaded_cb(lv_event_t *e) {
	(void)e;
	select_tab(active_tab, true);
}

static void section_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (thumb_timer) {
		lv_timer_pause(thumb_timer);
	}
}

static void section_scan_cb(lv_event_t *e) {
	(void)e;
	start_scan(NULL);
}

// Forward declarations for theme refresh of settings/controls/scan pages.
static void refresh_settings_page(void);
static void refresh_control_buttons(void);
static lv_obj_t *scan_count_label;
static lv_obj_t *scan_ok_button;

static void audiobooks_refresh_theme(void) {
	if (header_title) {
		lv_obj_set_style_text_color(header_title, heading_color(), 0);
	}
	refresh_bottom_bar();
	paint_library_dynamic_colors();
	if (chap_list && lv_obj_get_child_count(chap_list) > 0) {
		reload_chapters_tab();
	}
	if (bm_add_btn) {
		update_bm_add_cta();
	}
	if (bm_list && active_tab == AB_TAB_BOOKMARKS) {
		reload_bookmarks_tab();
	}
	refresh_settings_page();
	refresh_control_buttons();
	if (scan_count_label) {
		lv_obj_set_style_text_color(scan_count_label, theme()->accent, 0);
	}
	if (scan_ok_button) {
		lv_obj_set_style_bg_color(scan_ok_button, theme()->accent, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(scan_ok_button, 0), accent_ink(), 0);
	}
}

// Opens the Chapters tab focused on `book_path`.
void audiobook_open_detail(const char *book_path) {
	if (!book_path || !book_path[0]) {
		return;
	}
	snprintf(selected_book_path, sizeof(selected_book_path), "%s", book_path);
	select_tab(AB_TAB_CHAPTERS, true);
	if (lv_screen_active() != audiobooks_screen) {
		switch_screen(audiobooks_screen);
	}
}

static lv_obj_t *make_nav_pill(lv_obj_t *parent, const char *text, bool grow, lv_event_cb_t cb, void *ud, lv_obj_t **lbl_out) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, grow ? 0 : LV_SIZE_CONTENT, 44);
	if (grow) {
		lv_obj_set_flex_grow(btn, 1);
		lv_obj_set_style_pad_hor(btn, 6, 0);
	} else {
		lv_obj_set_style_pad_hor(btn, 14, 0);
	}
	lv_obj_set_style_pad_ver(btn, 0, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);

	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, text);
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_font(lbl, &font_ui_18, 0);
	lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
	if (grow) {
		lv_obj_set_width(lbl, lv_pct(100));
	}
	lv_obj_center(lbl);

	if (lbl_out) {
		*lbl_out = lbl;
	}
	return btn;
}

static void build_hub_page(gui_config_t *cfg) {
	saved_cfg = cfg;
	for (int s = 0; s < SLOT_POOL_COUNT; s++) {
		slot_owner[s] = -1;
	}

	lv_obj_add_style(audiobooks_screen, &theme_style_screen, 0);

	// Centered header title between the back button (left) and Settings gear (right).
	int side_reserved = cfg->padding + 56 + 10;
	header_title = lv_label_create(audiobooks_screen);
	lv_label_set_text(header_title, tr("audiobook_library"));
	lv_label_set_long_mode(header_title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(header_title, (int)cfg->screen_width - 2 * side_reserved);
	lv_obj_set_height(header_title, lv_font_get_line_height(&font_ui_32));
	lv_obj_set_style_text_align(header_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(header_title, &font_ui_32, 0);
	lv_obj_set_style_text_color(header_title, heading_color(), 0);
	lv_obj_align(header_title, LV_ALIGN_TOP_MID, 0, cfg->top_bar_height + cfg->padding + 12);

	corner_button(audiobooks_screen, cfg, 0, &icon_music_settings, settings_click_cb);

	int content_top = settingsrow_content_top(cfg) - 6;
	int content_h = (int)cfg->screen_height - content_top - BOTTOM_BAR_H;

	// Empty state when no audiobooks have been scanned yet.
	section_empty = gridpage_empty_panel(audiobooks_screen, cfg, &icon_book_headphones, "audiobook_no_database", section_scan_cb);

	// ---- View 0: Library ----
	view_library = lv_obj_create(audiobooks_screen);
	lv_obj_set_size(view_library, lv_pct(100), content_h);
	lv_obj_align(view_library, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(view_library, 0, 0);
	lv_obj_set_style_border_width(view_library, 0, 0);
	lv_obj_set_style_pad_hor(view_library, cfg->padding, 0);
	lv_obj_set_style_pad_top(view_library, 2, 0);
	lv_obj_set_style_pad_bottom(view_library, 16, 0);
	lv_obj_set_style_pad_row(view_library, 14, 0);
	lv_obj_set_scroll_dir(view_library, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(view_library, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_flex_flow(view_library, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(view_library, true);
	lv_obj_add_event_cb(view_library, library_scroll_cb, LV_EVENT_SCROLL, NULL);
	player_sheet_attach_drag(view_library, true);
	switcher_attach_back_gesture(view_library);

	lib_continue_heading = lv_label_create(view_library);
	lv_label_set_text(lib_continue_heading, tr("audiobook_continue_reading"));
	lv_obj_set_style_text_font(lib_continue_heading, &font_ui_24, 0);
	lv_obj_set_style_text_color(lib_continue_heading, heading_color(), 0);

	// Continue Listening Hero Card
	lib_hero_card = lv_btn_create(view_library);
	lv_obj_set_size(lib_hero_card, lv_pct(100), 96);
	lv_obj_add_style(lib_hero_card, &theme_style_card, 0);
	lv_obj_add_style(lib_hero_card, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(lib_hero_card, 20, 0);
	lv_obj_set_style_border_width(lib_hero_card, 0, 0);
	lv_obj_set_style_shadow_width(lib_hero_card, 0, 0);
	lv_obj_set_style_pad_all(lib_hero_card, 12, 0);
	lv_obj_set_style_pad_column(lib_hero_card, 14, 0);
	lv_obj_set_scrollable(lib_hero_card, false);
	lv_obj_set_event_bubble(lib_hero_card, true);
	lv_obj_set_flex_flow(lib_hero_card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(lib_hero_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(lib_hero_card, hero_card_cb, LV_EVENT_CLICKED, NULL);

	lib_hero_cover_box = lv_obj_create(lib_hero_card);
	lv_obj_set_size(lib_hero_cover_box, HERO_COVER_SIZE, HERO_COVER_SIZE);
	lv_obj_set_style_bg_color(lib_hero_cover_box, theme()->cover_bg, 0);
	lv_obj_set_style_bg_opa(lib_hero_cover_box, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(lib_hero_cover_box, 12, 0);
	lv_obj_set_style_clip_corner(lib_hero_cover_box, true, 0);
	lv_obj_set_style_border_width(lib_hero_cover_box, 0, 0);
	lv_obj_set_style_pad_all(lib_hero_cover_box, 0, 0);
	lv_obj_set_scrollable(lib_hero_cover_box, false);
	lv_obj_set_clickable(lib_hero_cover_box, false);
	lv_obj_set_event_bubble(lib_hero_cover_box, true);

	lib_hero_img = lv_image_create(lib_hero_cover_box);
	lv_obj_set_size(lib_hero_img, HERO_COVER_SIZE, HERO_COVER_SIZE);
	lv_image_set_src(lib_hero_img, &icon_book_headphones_row);
	lv_image_set_inner_align(lib_hero_img, LV_IMAGE_ALIGN_CENTER);
	lv_obj_add_style(lib_hero_img, &theme_style_icon, 0);
	lv_obj_set_clickable(lib_hero_img, false);
	lv_obj_set_event_bubble(lib_hero_img, true);

	lv_obj_t *hero_col = lv_obj_create(lib_hero_card);
	lv_obj_set_size(hero_col, 0, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(hero_col, 1);
	lv_obj_set_style_bg_opa(hero_col, 0, 0);
	lv_obj_set_style_border_width(hero_col, 0, 0);
	lv_obj_set_style_pad_all(hero_col, 0, 0);
	lv_obj_set_style_pad_row(hero_col, 4, 0);
	lv_obj_set_scrollable(hero_col, false);
	lv_obj_set_clickable(hero_col, false);
	lv_obj_set_event_bubble(hero_col, true);
	lv_obj_set_flex_flow(hero_col, LV_FLEX_FLOW_COLUMN);

	lib_hero_title = lv_label_create(hero_col);
	lv_label_set_long_mode(lib_hero_title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lib_hero_title, lv_pct(100));
	lv_obj_set_height(lib_hero_title, lv_font_get_line_height(&font_ui_24));
	lv_obj_set_style_text_font(lib_hero_title, &font_ui_24, 0);
	lv_obj_set_style_text_color(lib_hero_title, heading_color(), 0);
	lv_obj_set_event_bubble(lib_hero_title, true);

	lib_hero_sub = lv_label_create(hero_col);
	lv_label_set_long_mode(lib_hero_sub, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lib_hero_sub, lv_pct(100));
	lv_obj_set_height(lib_hero_sub, lv_font_get_line_height(&font_ui_18));
	lv_obj_add_style(lib_hero_sub, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(lib_hero_sub, &font_ui_18, 0);
	lv_obj_set_event_bubble(lib_hero_sub, true);

	lib_hero_play_btn = lv_btn_create(lib_hero_card);
	lv_obj_set_size(lib_hero_play_btn, 52, 52);
	lv_obj_set_style_radius(lib_hero_play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(lib_hero_play_btn, theme()->accent, 0);
	lv_obj_set_style_bg_opa(lib_hero_play_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(lib_hero_play_btn, 0, 0);
	lv_obj_set_style_shadow_width(lib_hero_play_btn, 0, 0);
	lv_obj_set_style_pad_all(lib_hero_play_btn, 0, 0);
	lv_obj_set_style_opa(lib_hero_play_btn, LV_OPA_70, LV_STATE_PRESSED);
	lv_obj_add_event_cb(lib_hero_play_btn, hero_play_cb, LV_EVENT_CLICKED, NULL);

	lib_hero_play_icon = lv_image_create(lib_hero_play_btn);
	lv_image_set_src(lib_hero_play_icon, &icon_play_small);
	lv_obj_set_style_image_recolor(lib_hero_play_icon, accent_ink(), 0);
	lv_obj_set_style_image_recolor_opa(lib_hero_play_icon, LV_OPA_COVER, 0);
	lv_obj_center(lib_hero_play_icon);

	// 2-column Book Grid container
	lib_grid_cont = lv_obj_create(view_library);
	lv_obj_set_size(lib_grid_cont, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(lib_grid_cont, 0, 0);
	lv_obj_set_style_border_width(lib_grid_cont, 0, 0);
	lv_obj_set_style_pad_all(lib_grid_cont, 0, 0);
	lv_obj_set_style_pad_top(lib_grid_cont, 4, 0);
	lv_obj_set_style_pad_column(lib_grid_cont, GRID_GAP, 0);
	lv_obj_set_style_pad_row(lib_grid_cont, 18, 0);
	lv_obj_set_scrollable(lib_grid_cont, false);
	lv_obj_set_event_bubble(lib_grid_cont, true);
	lv_obj_set_flex_flow(lib_grid_cont, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(lib_grid_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	// ---- View 1: Chapters ----
	view_chapters = lv_obj_create(audiobooks_screen);
	lv_obj_set_size(view_chapters, lv_pct(100), content_h);
	lv_obj_align(view_chapters, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(view_chapters, 0, 0);
	lv_obj_set_style_border_width(view_chapters, 0, 0);
	lv_obj_set_style_pad_hor(view_chapters, cfg->padding, 0);
	lv_obj_set_style_pad_ver(view_chapters, 0, 0);
	lv_obj_set_style_pad_row(view_chapters, 10, 0);
	lv_obj_set_scrollable(view_chapters, false);
	lv_obj_set_flex_flow(view_chapters, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_hidden(view_chapters, true);

	chap_subtitle = lv_label_create(view_chapters);
	lv_label_set_text(chap_subtitle, "");
	lv_label_set_long_mode(chap_subtitle, LV_LABEL_LONG_DOT);
	lv_obj_set_width(chap_subtitle, lv_pct(100));
	lv_obj_set_height(chap_subtitle, lv_font_get_line_height(&font_ui_20));
	lv_obj_add_style(chap_subtitle, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(chap_subtitle, &font_ui_20, 0);
	lv_obj_set_style_text_align(chap_subtitle, LV_TEXT_ALIGN_CENTER, 0);

	chap_list = lv_obj_create(view_chapters);
	lv_obj_set_size(chap_list, lv_pct(100), 0);
	lv_obj_set_flex_grow(chap_list, 1);
	lv_obj_set_style_bg_opa(chap_list, 0, 0);
	lv_obj_set_style_border_width(chap_list, 0, 0);
	lv_obj_set_style_pad_all(chap_list, 0, 0);
	lv_obj_set_style_pad_bottom(chap_list, 14, 0);
	lv_obj_set_style_pad_row(chap_list, 10, 0);
	lv_obj_set_scroll_dir(chap_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(chap_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_flex_flow(chap_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(chap_list, true);
	player_sheet_attach_drag(chap_list, true);
	switcher_attach_back_gesture(chap_list);

	// ---- View 2: Bookmarks ----
	view_bookmarks = lv_obj_create(audiobooks_screen);
	lv_obj_set_size(view_bookmarks, lv_pct(100), content_h);
	lv_obj_align(view_bookmarks, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(view_bookmarks, 0, 0);
	lv_obj_set_style_border_width(view_bookmarks, 0, 0);
	lv_obj_set_style_pad_hor(view_bookmarks, cfg->padding, 0);
	lv_obj_set_style_pad_ver(view_bookmarks, 2, 0);
	lv_obj_set_style_pad_row(view_bookmarks, 14, 0);
	lv_obj_set_scrollable(view_bookmarks, false);
	lv_obj_set_flex_flow(view_bookmarks, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_hidden(view_bookmarks, true);

	bm_add_btn = lv_btn_create(view_bookmarks);
	lv_obj_set_size(bm_add_btn, lv_pct(100), 58);
	lv_obj_set_style_radius(bm_add_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(bm_add_btn, theme()->accent, 0);
	lv_obj_set_style_bg_opa(bm_add_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(bm_add_btn, 0, 0);
	lv_obj_set_style_shadow_width(bm_add_btn, 0, 0);
	lv_obj_set_style_opa(bm_add_btn, LV_OPA_70, LV_STATE_PRESSED);
	lv_obj_add_event_cb(bm_add_btn, bm_add_click_cb, LV_EVENT_CLICKED, NULL);

	bm_add_label = lv_label_create(bm_add_btn);
	lv_label_set_long_mode(bm_add_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_font(bm_add_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(bm_add_label, accent_ink(), 0);
	lv_obj_center(bm_add_label);

	bm_list = lv_obj_create(view_bookmarks);
	lv_obj_set_size(bm_list, lv_pct(100), 0);
	lv_obj_set_flex_grow(bm_list, 1);
	lv_obj_set_style_bg_opa(bm_list, 0, 0);
	lv_obj_set_style_border_width(bm_list, 0, 0);
	lv_obj_set_style_pad_all(bm_list, 0, 0);
	lv_obj_set_style_pad_bottom(bm_list, 14, 0);
	lv_obj_set_style_pad_row(bm_list, 10, 0);
	lv_obj_set_scroll_dir(bm_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(bm_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_flex_flow(bm_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(bm_list, true);
	player_sheet_attach_drag(bm_list, true);
	switcher_attach_back_gesture(bm_list);

	// ---- Persistent Bottom Pill Navigation Bar ----
	bottom_bar = lv_obj_create(audiobooks_screen);
	lv_obj_set_size(bottom_bar, lv_pct(100), BOTTOM_BAR_H);
	lv_obj_align(bottom_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_add_style(bottom_bar, &theme_style_screen, 0);
	lv_obj_set_style_border_width(bottom_bar, 0, 0);
	lv_obj_set_style_radius(bottom_bar, 0, 0);
	lv_obj_set_style_pad_hor(bottom_bar, 12, 0);
	lv_obj_set_style_pad_ver(bottom_bar, 8, 0);
	lv_obj_set_style_pad_column(bottom_bar, 8, 0);
	lv_obj_set_scrollable(bottom_bar, false);
	lv_obj_set_flex_flow(bottom_bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(bottom_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	nav_pills[AB_TAB_LIBRARY]   = make_nav_pill(bottom_bar, tr("audiobook_library"), true, nav_pill_click_cb, (void *)(intptr_t)AB_TAB_LIBRARY, &nav_labels[AB_TAB_LIBRARY]);
	nav_pills[AB_TAB_CHAPTERS]  = make_nav_pill(bottom_bar, tr("chapters"),          true, nav_pill_click_cb, (void *)(intptr_t)AB_TAB_CHAPTERS, &nav_labels[AB_TAB_CHAPTERS]);
	nav_pills[AB_TAB_BOOKMARKS] = make_nav_pill(bottom_bar, tr("bookmarks"),         true, nav_pill_click_cb, (void *)(intptr_t)AB_TAB_BOOKMARKS, &nav_labels[AB_TAB_BOOKMARKS]);
	speed_pill = make_nav_pill(bottom_bar, "1.0x", false, speed_pill_click_cb, NULL, &speed_label);

	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);

	refresh_bottom_bar();

	player_sheet_attach_drag(audiobooks_screen, true);
	switcher_attach_back_gesture(audiobooks_screen);
	lv_obj_add_event_cb(audiobooks_screen, section_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(audiobooks_screen, section_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}

// ---------------------------------------------------------------------------
// Settings Page & Controls Page (audiobooksettings_screen / audiobookcontrols_screen)
// ---------------------------------------------------------------------------

static void scan_row_cb(lv_event_t *e);

static void option_note(lv_obj_t *parent, const char *text) {
	lv_obj_t *note = lv_label_create(parent);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, lv_pct(100));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_22, 0);
	lv_label_set_text(note, tr(text));
}

typedef struct {
	lv_obj_t *card;
	lv_obj_t *toggle;
	lv_obj_t *pills;
} toggle_pills_t;

static void toggle_pills_expanded(const toggle_pills_t *tp, bool expanded) {
	if (!tp || !tp->pills) {
		return;
	}
	lv_obj_set_hidden(tp->pills, !expanded);
}

static void build_toggle_pills(lv_obj_t *parent, const char *title, lv_event_cb_t toggle_cb, toggle_pills_t *out) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_row(card, 18, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *head = lv_obj_create(card);
	lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(head, 0, 0);
	lv_obj_set_style_border_width(head, 0, 0);
	lv_obj_set_style_pad_all(head, 0, 0);
	lv_obj_set_scrollable(head, false);
	lv_obj_set_event_bubble(head, true);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *name = lv_label_create(head);
	lv_label_set_text(name, tr(title));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *toggle = settingsrow_switch(head, toggle_cb);

	lv_obj_t *pills = lv_obj_create(card);
	lv_obj_set_size(pills, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(pills, 0, 0);
	lv_obj_set_style_border_width(pills, 0, 0);
	lv_obj_set_style_pad_all(pills, 0, 0);
	lv_obj_set_style_pad_gap(pills, 10, 0);
	lv_obj_set_scrollable(pills, false);
	lv_obj_set_event_bubble(pills, true);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

	out->card = card;
	out->toggle = toggle;
	out->pills = pills;
}

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, int value, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(btn, 18, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)value);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_22, 0);
	lv_obj_center(label);

	return btn;
}

#define REWIND_CHOICES 4

static lv_obj_t *stop_chapter_switch;
static lv_obj_t *duration_total_pill;
static lv_obj_t *duration_chapter_pill;
static settingsrow_duration_t sleep_row;
static toggle_pills_t rewind_card;
static lv_obj_t *rewind_pill[REWIND_CHOICES];
static const int REWIND_VALUES[REWIND_CHOICES] = {3, 5, 7, 10};

static void paint_choice(lv_obj_t *btn, bool on) {
	if (!btn) {
		return;
	}
	lv_obj_set_style_bg_color(btn, on ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), on ? accent_ink() : theme()->text_primary, 0);
}

static void refresh_settings_page(void) {
	if (!stop_chapter_switch) {
		return;
	}

	bool per_chapter = audiobook_duration_per_chapter();
	settingsrow_pill_active(duration_total_pill, !per_chapter);
	settingsrow_pill_active(duration_chapter_pill, per_chapter);

	if (audiobook_stop_at_chapter_end()) {
		lv_obj_add_state(stop_chapter_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(stop_chapter_switch, LV_STATE_CHECKED);
	}

	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_AUDIOBOOK));
	settingsrow_duration_expanded(&sleep_row, sleeptimer_enabled(SLEEPTIMER_AUDIOBOOK));
	settingsrow_duration_repaint(&sleep_row);

	bool rewind_on = audiobook_rewind_enabled();
	if (rewind_on) {
		lv_obj_add_state(rewind_card.toggle, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(rewind_card.toggle, LV_STATE_CHECKED);
	}
	toggle_pills_expanded(&rewind_card, rewind_on);

	int seconds = audiobook_rewind_seconds();
	for (int i = 0; i < REWIND_CHOICES; i++) {
		paint_choice(rewind_pill[i], REWIND_VALUES[i] == seconds);
	}
}

static void duration_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_duration_per_chapter(lv_event_get_user_data(e) != NULL);
	config_save();
	refresh_settings_page();
}

static void stop_chapter_cb(lv_event_t *e) {
	audiobook_set_stop_at_chapter_end(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void sleep_toggle_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_enabled(SLEEPTIMER_AUDIOBOOK, lv_obj_has_state(sleep_row.toggle, LV_STATE_CHECKED));
	refresh_settings_page();
}

static void sleep_wheel_cb(lv_event_t *e) {
	(void)e;
	sleeptimer_set_minutes(SLEEPTIMER_AUDIOBOOK, settingsrow_duration_minutes(&sleep_row));
}

static void rewind_toggle_cb(lv_event_t *e) {
	audiobook_set_rewind_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	refresh_settings_page();
}

static void rewind_pick_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_rewind_seconds((int)(intptr_t)lv_event_get_user_data(e));
	refresh_settings_page();
}

static void settings_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_settings_page();
}

static void build_settings_page(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(audiobooksettings_screen, cfg, "audiobooks");
	settingsrow_add(container, "change_controls", NULL, switch_screen_cb, audiobookcontrols_screen);

	lv_obj_t *duration_pills;
	settingsrow_pills(container, "audiobook_show_duration", &duration_pills);
	duration_total_pill = settingsrow_pill(duration_pills, "audiobook_duration_total", 0, duration_pick_cb);
	duration_chapter_pill = settingsrow_pill(duration_pills, "audiobook_duration_chapter", 1, duration_pick_cb);

	settingsrow_toggle(container, "audiobook_stop_at_end_of_chapter", &stop_chapter_switch, stop_chapter_cb);

	settingsrow_toggle_duration(container, "sleep_timer", sleep_toggle_cb, sleep_wheel_cb, &sleep_row);
	settingsrow_duration_set_minutes(&sleep_row, sleeptimer_minutes(SLEEPTIMER_AUDIOBOOK));

	build_toggle_pills(container, "audiobook_rewind_after_pause", rewind_toggle_cb, &rewind_card);
	static const char *const REWIND_LABELS[REWIND_CHOICES] = {"audiobook_3_sec", "audiobook_5_sec", "audiobook_7_sec", "audiobook_10_sec"};
	for (int i = 0; i < REWIND_CHOICES; i++) {
		rewind_pill[i] = make_pill(rewind_card.pills, REWIND_LABELS[i], REWIND_VALUES[i], rewind_pick_cb);
	}

	settingsrow_action(container, "audiobook_scan_audiobooks_2", scan_row_cb, NULL);
	option_note(container, "audiobook_folder_note");

	refresh_settings_page();
	lv_obj_add_event_cb(audiobooksettings_screen, settings_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(audiobooksettings_screen, settings_loaded_cb, LV_EVENT_REFRESH, NULL);
}

// ---------------------------------------------------------------------------
// Controls Page (audiobookcontrols_screen)
// ---------------------------------------------------------------------------

#define SKIP_CHOICES 3

static lv_obj_t *back_choice[SKIP_CHOICES];
static lv_obj_t *forward_choice[SKIP_CHOICES];
static const int SKIP_VALUES[SKIP_CHOICES] = {AUDIOBOOK_SKIP_SHORT, AUDIOBOOK_SKIP_LONG, AUDIOBOOK_SKIP_HUGE};

static void refresh_control_buttons(void) {
	int back = audiobook_skip_back();
	int forward = audiobook_skip_forward();

	for (int i = 0; i < SKIP_CHOICES; i++) {
		paint_choice(back_choice[i], SKIP_VALUES[i] == back);
		paint_choice(forward_choice[i], SKIP_VALUES[i] == forward);
	}
}

static void pick_back_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_skip_back((int)(intptr_t)lv_event_get_user_data(e));
	refresh_control_buttons();
	player_refresh_now_playing();
}

static void pick_forward_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) {
		return;
	}
	audiobook_set_skip_forward((int)(intptr_t)lv_event_get_user_data(e));
	refresh_control_buttons();
	player_refresh_now_playing();
}

static lv_obj_t *make_skip_choice(lv_obj_t *parent, const char *text, int seconds, lv_event_cb_t cb) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 64);
	lv_obj_set_style_pad_hor(btn, 22, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(intptr_t)seconds);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_center(label);

	return btn;
}

static lv_obj_t *make_control_card(lv_obj_t *parent, const char *title) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, 12, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_all(card, 20, 0);
	lv_obj_set_style_pad_gap(card, 18, 0);
	lv_obj_set_scrollable(card, false);
	lv_obj_set_event_bubble(card, true);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, tr(title));
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);

	lv_obj_t *row = lv_obj_create(card);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_gap(row, 14, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_event_bubble(row, true);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	return row;
}

static void controls_loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_control_buttons();
}

static void build_controls_page(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(audiobookcontrols_screen, cfg, "change_controls");

	lv_obj_t *back_row = make_control_card(container, "back_button");
	back_choice[0] = make_skip_choice(back_row, "-10", AUDIOBOOK_SKIP_SHORT, pick_back_cb);
	back_choice[1] = make_skip_choice(back_row, "-30", AUDIOBOOK_SKIP_LONG, pick_back_cb);
	back_choice[2] = make_skip_choice(back_row, "-60", AUDIOBOOK_SKIP_HUGE, pick_back_cb);

	lv_obj_t *forward_row = make_control_card(container, "forward_button");
	forward_choice[0] = make_skip_choice(forward_row, "+10", AUDIOBOOK_SKIP_SHORT, pick_forward_cb);
	forward_choice[1] = make_skip_choice(forward_row, "+30", AUDIOBOOK_SKIP_LONG, pick_forward_cb);
	forward_choice[2] = make_skip_choice(forward_row, "+60", AUDIOBOOK_SKIP_HUGE, pick_forward_cb);

	refresh_control_buttons();
	lv_obj_add_event_cb(audiobookcontrols_screen, controls_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// Scan Page (audiobookscan_screen)
// ---------------------------------------------------------------------------

#define SCAN_POLL_MS 200

static lv_obj_t *scan_status_label;
static lv_obj_t *scan_cancel_button;
static lv_timer_t *scan_poll_timer;

static char scan_root[512];

static void scan_show_finished(int found) {
	lv_label_set_text_fmt(scan_count_label, "%d", found);
	lv_label_set_text(scan_status_label, found == 1 ? tr("audiobook_found") : tr("audiobook_found_count"));

	lv_obj_set_hidden(scan_cancel_button, true);
	lv_obj_set_hidden(scan_ok_button, false);
	power_hold_screen_on(false);
}

static void scan_poll_cb(lv_timer_t *timer) {
	int found = audiobookdb_scan_found();
	lv_label_set_text_fmt(scan_count_label, "%d", found);

	if (audiobookdb_scan_running()) {
		return;
	}

	lv_timer_pause(timer);
	scan_show_finished(found);
}

static void scan_ok_cb(lv_event_t *e) {
	(void)e;
	switch_screen(audiobooks_screen);
	screen_history_reset();
}

static void scan_cancel_cb(lv_event_t *e) {
	(void)e;
	audiobookdb_scan_stop();
	lv_timer_pause(scan_poll_timer);
	power_hold_screen_on(false);
	switch_screen(audiobooks_screen);
	screen_history_reset();
}

static void scan_begin(void) {
	lv_label_set_text(scan_count_label, "0");
	lv_label_set_text(scan_status_label, tr("audiobook_found_count"));
	lv_obj_set_hidden(scan_ok_button, true);
	lv_obj_set_hidden(scan_cancel_button, false);

	if (!audiobookdb_scan_start(scan_root)) {
		lv_label_set_text(scan_status_label, tr("no_card_to_scan"));
		lv_obj_set_hidden(scan_cancel_button, true);
		lv_obj_set_hidden(scan_ok_button, false);
		return;
	}

	power_hold_screen_on(true);
	lv_timer_reset(scan_poll_timer);
	lv_timer_resume(scan_poll_timer);
}

static void start_scan(void *user) {
	(void)user;
	switch_screen(audiobookscan_screen);
	scan_begin();
}

static void scan_row_cb(lv_event_t *e) {
	(void)e;
	confirm_show("audiobook_scan_audiobooks",
				 "audiobook_rescan_confirm_note",
				 "scan", start_scan, NULL);
}

static lv_obj_t *scan_make_button(const char *text, lv_color_t colour, lv_event_cb_t cb, gui_config_t *cfg) {
	lv_obj_t *button = lv_btn_create(audiobookscan_screen);
	lv_obj_set_size(button, 240, 68);
	lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -(cfg->padding * 2));
	lv_obj_add_style(button, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(button, colour, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *label = lv_label_create(button);
	lv_label_set_text(label, tr(text));
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_style_text_color(label, color_ink(colour), 0);
	lv_obj_center(label);

	return button;
}

static void build_scan_page(gui_config_t *cfg) {
	snprintf(scan_root, sizeof(scan_root), "%s/%s", cfg->sd_root_path ? cfg->sd_root_path : "", AUDIOBOOKDB_FOLDER);

	lv_obj_add_style(audiobookscan_screen, &theme_style_screen, 0);

	lv_obj_t *title = settingsrow_title(audiobookscan_screen, cfg, "scan_2");
	lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

	int content_top = settingsrow_content_top(cfg);

	lv_obj_t *container = lv_obj_create(audiobookscan_screen);
	lv_obj_set_size(container, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_all(container, cfg->padding, 0);
	lv_obj_set_style_pad_gap(container, 10, 0);
	lv_obj_set_scrollable(container, false);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *book = lv_image_create(container);
	lv_image_set_src(book, &icon_book_headphones);
	lv_obj_add_style(book, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(book, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_bottom(book, 16, 0);

	scan_count_label = lv_label_create(container);
	lv_label_set_text(scan_count_label, "0");
	lv_obj_set_style_text_color(scan_count_label, theme()->accent, 0);
	lv_obj_set_style_text_font(scan_count_label, &font_ui_32, 0);

	scan_status_label = lv_label_create(container);
	lv_label_set_text(scan_status_label, tr("audiobook_found_count"));
	lv_obj_add_style(scan_status_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(scan_status_label, &font_ui_26, 0);

	scan_cancel_button = scan_make_button("cancel", lv_color_make(210, 66, 58), scan_cancel_cb, cfg);
	scan_ok_button = scan_make_button("ok", theme()->accent, scan_ok_cb, cfg);
	lv_obj_set_hidden(scan_ok_button, true);

	scan_poll_timer = lv_timer_create(scan_poll_cb, SCAN_POLL_MS, NULL);
	lv_timer_pause(scan_poll_timer);
}

// ---------------------------------------------------------------------------
// Module Initialization
// ---------------------------------------------------------------------------

void audiobooks_init(gui_config_t *cfg) {
	if (!audiobooks_screen) audiobooks_screen = lv_obj_create(NULL);
	if (!audiobooklibrary_screen) audiobooklibrary_screen = lv_obj_create(NULL);
	if (!book_detail_screen) book_detail_screen = lv_obj_create(NULL);

	lv_obj_add_style(audiobooklibrary_screen, &theme_style_screen, 0);
	lv_obj_add_style(book_detail_screen, &theme_style_screen, 0);

	build_hub_page(cfg);
	build_settings_page(cfg);
	build_controls_page(cfg);
	build_scan_page(cfg);

	theme_register_refresh(audiobooks_refresh_theme);
}
