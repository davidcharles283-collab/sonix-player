#include "audiobooks.h"

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

static lv_obj_t *names_screen;

static void start_scan(void *user);

// ---------------------------------------------------------------------------
// Design Color Palette & Constants
// ---------------------------------------------------------------------------

#define AUDIOBOOK_ACCENT lv_color_hex(0xF15C3C)
#define AUDIOBOOK_CARD_BG lv_color_hex(0x1C1C1E)
#define AUDIOBOOK_CARD_PRESSED lv_color_hex(0x2C2C2E)
#define AUDIOBOOK_BAR_TRACK lv_color_hex(0x333336)
#define AUDIOBOOK_GREEN lv_color_hex(0x34C759)
#define AUDIOBOOK_TEXT_DIM lv_color_hex(0x8E8E93)
#define AUDIOBOOK_ACTIVE_BG lv_color_hex(0x341C0E)

#define ROW_HEIGHT 104
#define ROW_GAP 8
#define ROW_PITCH (ROW_HEIGHT + ROW_GAP)
#define ROW_RADIUS 14
#define ROW_PAD 12
#define THUMB_SIZE 64
#define ROW_POOL 12
#define WINDOW_ROWS 48
#define THUMB_POLL_MS 150

#define SLOT_BOOKS_BASE (COVERLOADER_AUDIOBOOKS_BASE + 0)     // 12 slots: 0..11
#define SLOT_HOME_HERO (COVERLOADER_AUDIOBOOKS_BASE + 12)     // 1 slot: 12
#define SLOT_HOME_CONTINUE (COVERLOADER_AUDIOBOOKS_BASE + 13) // 6 slots: 13..18
#define SLOT_DETAIL_HERO (COVERLOADER_AUDIOBOOKS_BASE + 19)   // 1 slot: 19
#define MAX_HOME_CONTINUE 6

// ---------------------------------------------------------------------------
// Lists, Sorts & Filter Choices
// ---------------------------------------------------------------------------

typedef enum {
	LIST_LIBRARY,
	LIST_CONTINUE,
	LIST_FINISHED,
	LIST_AUTHOR_BOOKS,
	LIST_SERIES_BOOKS,
	LIST_AUTHORS,
	LIST_SERIES,
	LIST_COUNT,
} list_id_t;

typedef enum {
	FILTER_ALL,
	FILTER_READ,
	FILTER_UNREAD,
} books_filter_t;

typedef enum {
	SORT_AZ,
	SORT_ZA,
	SORT_NEW,
	SORT_OLD,
	SORT_PLAYED,
	SORT_SERIES,
} sort_choice_t;

typedef struct {
	const char *config_key;
	sort_choice_t fallback;
	int choice_count;
	sort_choice_t choices[6];
} list_sorts_t;

static const list_sorts_t LIST_SORTS[LIST_COUNT] = {
	[LIST_LIBRARY] = {"sort_library", SORT_AZ, 5, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD, SORT_PLAYED}},
	[LIST_CONTINUE] = {"sort_continue", SORT_PLAYED, 3, {SORT_PLAYED, SORT_AZ, SORT_ZA}},
	[LIST_FINISHED] = {"sort_finished", SORT_NEW, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}},
	[LIST_AUTHOR_BOOKS] = {"sort_author_books", SORT_AZ, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}},
	[LIST_SERIES_BOOKS] = {"sort_series_books", SORT_SERIES, 5, {SORT_SERIES, SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}},
	[LIST_AUTHORS] = {"sort_authors", SORT_AZ, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}},
	[LIST_SERIES] = {"sort_series", SORT_AZ, 4, {SORT_AZ, SORT_ZA, SORT_NEW, SORT_OLD}},
};

static const char *const SORT_LABELS[] = {
	[SORT_AZ] = "medialist_sort_name_az",
	[SORT_ZA] = "medialist_sort_name_za",
	[SORT_NEW] = "medialist_sort_added_new",
	[SORT_OLD] = "medialist_sort_added_old",
	[SORT_PLAYED] = "audiobook_sort_played",
	[SORT_SERIES] = "audiobook_sort_series",
};

static sort_choice_t list_sort(list_id_t list) {
	const list_sorts_t *sorts = &LIST_SORTS[list];
	long value = config_get_int("audiobook", sorts->config_key, sorts->fallback);
	for (int i = 0; i < sorts->choice_count; i++) {
		if (sorts->choices[i] == value) {
			return (sort_choice_t)value;
		}
	}
	return sorts->fallback;
}

static const lv_image_dsc_t *sort_icon(sort_choice_t choice) {
	switch (choice) {
	case SORT_ZA:
		return &icon_sort_za;
	case SORT_NEW:
		return &icon_sort_date_new;
	case SORT_OLD:
		return &icon_sort_date_old;
	case SORT_PLAYED:
		return &icon_sort_played;
	case SORT_SERIES:
		return &icon_sort_series;
	default:
		return &icon_sort_az;
	}
}

static void sort_to_order(sort_choice_t choice, audiobook_order_t *order, bool *desc) {
	*desc = choice == SORT_ZA || choice == SORT_NEW || choice == SORT_PLAYED;
	*order = choice == SORT_NEW || choice == SORT_OLD ? AUDIOBOOK_ORDER_ADDED :
			 choice == SORT_PLAYED ? AUDIOBOOK_ORDER_PLAYED :
			 choice == SORT_SERIES ? AUDIOBOOK_ORDER_SERIES : AUDIOBOOK_ORDER_NAME;
}

static list_id_t sort_menu_list;
static void (*sort_menu_reload)(void);

static void sort_picked(void *user) {
	config_set_int("audiobook", LIST_SORTS[sort_menu_list].config_key, (long)(intptr_t)user);
	config_save();
	if (sort_menu_reload) {
		sort_menu_reload();
	}
}

static void sort_menu_show(lv_obj_t *anchor, list_id_t list, void (*reload)(void)) {
	const list_sorts_t *sorts = &LIST_SORTS[list];
	sort_choice_t current = list_sort(list);
	popover_item_t items[6];
	for (int i = 0; i < sorts->choice_count; i++) {
		sort_choice_t choice = sorts->choices[i];
		items[i] = (popover_item_t){SORT_LABELS[choice], sort_picked, (void *)(intptr_t)choice, choice == current};
	}
	sort_menu_list = list;
	sort_menu_reload = reload;
	popover_show(anchor, items, sorts->choice_count);
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

static void search_click_cb(lv_event_t *e) {
	(void)e;
	search_open();
}

static void settings_click_cb(lv_event_t *e) {
	(void)e;
	switch_screen(audiobooksettings_screen);
}

// ---------------------------------------------------------------------------
// Book Progress & Metadata Helper
// ---------------------------------------------------------------------------

typedef struct {
	char title[256];
	char author[256];
	bool finished;
	int current_chapter;
	int total_chapters;
	int percent;
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
	audiobookdb_book_info(path, info->title, sizeof(info->title), info->author, sizeof(info->author), NULL);
	info->finished = audiobookdb_is_finished(path);
	if (info->finished) {
		info->percent = 100;
		info->current_chapter = 1;
		info->total_chapters = 1;
		return;
	}
	bool has_pos = audiobookdb_get_position(path, info->resume_file, sizeof(info->resume_file), &info->resume_pos);
	bool is_folder = audiobookdb_is_folder_book(path);

	if (is_folder) {
		folder_part_counter_t ctx = {info->resume_file, 0, 0};
		audiobookdb_parts_for_each(path, count_parts_cb, &ctx);
		info->total_chapters = ctx.count > 0 ? ctx.count : 1;
		if (has_pos && ctx.found_idx > 0) {
			info->current_chapter = ctx.found_idx;
			info->percent = (ctx.found_idx * 100) / info->total_chapters;
		} else {
			info->current_chapter = 0;
			info->percent = 0;
		}
	} else {
		audiobook_chapter_entry_t *entries = NULL;
		int count = audiobook_get_file_chapters(path, &entries);
		if (count > 0) {
			info->total_chapters = count;
			if (has_pos && info->resume_pos > 1.0) {
				int idx = 0;
				for (int i = 0; i < count; i++) {
					double next_start = (i + 1 < count) ? entries[i + 1].start : 1e9;
					if (info->resume_pos >= entries[i].start && info->resume_pos < next_start) {
						idx = i + 1;
						break;
					}
				}
				info->current_chapter = idx > 0 ? idx : 1;
				info->percent = (info->current_chapter * 100) / count;
			} else {
				info->current_chapter = 0;
				info->percent = 0;
			}
			audiobook_free_file_chapters(entries);
		} else {
			info->total_chapters = 1;
			if (has_pos && info->resume_pos > 1.0) {
				info->current_chapter = 1;
				info->percent = 50;
			} else {
				info->current_chapter = 0;
				info->percent = 0;
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

// ---------------------------------------------------------------------------
// Screen 2: "Biblioteca" (audiobooklibrary_screen)
// ---------------------------------------------------------------------------

typedef struct {
	char name[256];
	char path[512];
} winrow_t;

typedef struct {
	lv_obj_t *button;
	lv_obj_t *icon;
	lv_obj_t *title_label;
	lv_obj_t *author_label;
	lv_obj_t *status_label;
	lv_obj_t *bar;
	int index;

	bool has_thumb;
	bool thumb_requested;
	bool thumb_settled;
	cover_image_t thumb;
} row_t;

static lv_obj_t *books_title;
static lv_obj_t *books_sort_icon;
static lv_obj_t *book_list;
static lv_obj_t *book_body;
static lv_obj_t *empty_label;
static lv_timer_t *thumb_timer;

static row_t rows[ROW_POOL];
static audiobookdb_index_t *book_index;
static int entry_count;
static winrow_t *window_rows;
static int window_first = -1;
static int window_count;
static int row_width;

static list_id_t books_list = LIST_LIBRARY;
static books_filter_t current_filter = FILTER_ALL;
static char books_value[256];

static lv_obj_t *filter_pill_all;
static lv_obj_t *filter_pill_read;
static lv_obj_t *filter_pill_unread;

static int row_slot(const row_t *row) { return SLOT_BOOKS_BASE + (int)(row - rows); }
static bool row_at(int index, const char **name_out, const char **path_out);

static audiobook_list_t books_kind(void) {
	if (books_list == LIST_LIBRARY) {
		if (current_filter == FILTER_READ) return AUDIOBOOK_LIST_FINISHED;
		if (current_filter == FILTER_UNREAD) return AUDIOBOOK_LIST_UNREAD;
		return AUDIOBOOK_LIST_ALL;
	}
	switch (books_list) {
	case LIST_FINISHED:
		return AUDIOBOOK_LIST_FINISHED;
	case LIST_CONTINUE:
		return AUDIOBOOK_LIST_CONTINUE;
	case LIST_AUTHOR_BOOKS:
		return AUDIOBOOK_LIST_AUTHOR;
	case LIST_SERIES_BOOKS:
		return AUDIOBOOK_LIST_SERIES;
	default:
		return AUDIOBOOK_LIST_ALL;
	}
}

static audiobookdb_index_t *books_index_open(void) {
	audiobook_order_t order;
	bool desc;
	sort_to_order(list_sort(books_list), &order, &desc);
	return audiobookdb_index_open(books_kind(), books_value, order, desc);
}

static void row_show_glyph(row_t *row) {
	lv_image_set_src(row->icon, &icon_book_headphones_row);
	lv_obj_add_style(row->icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_COVER, 0);
}

static void row_show_cover(row_t *row, const lv_image_dsc_t *dsc) {
	lv_image_set_src(row->icon, dsc);
	lv_obj_set_style_image_recolor_opa(row->icon, LV_OPA_TRANSP, 0);
}

static void row_drop_thumb(row_t *row) {
	coverloader_release(row_slot(row));
	if (row->has_thumb) {
		row_show_glyph(row);
		cover_free(&row->thumb);
		row->has_thumb = false;
	}
	row->thumb_requested = false;
	row->thumb_settled = false;
}

static void row_bind(row_t *row, int index) {
	if (row->index == index) {
		return;
	}
	row_drop_thumb(row);
	row->index = index;

	if (index < 0 || index >= entry_count) {
		lv_obj_set_hidden(row->button, true);
		return;
	}

	const char *name = NULL;
	const char *path = NULL;
	if (!row_at(index, &name, &path)) {
		lv_obj_set_hidden(row->button, true);
		return;
	}

	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * ROW_PITCH);

	book_progress_info_t info;
	get_book_progress_info(path, &info);

	lv_label_set_text(row->title_label, info.title[0] ? info.title : (name ? name : ""));
	lv_label_set_text(row->author_label, info.author[0] ? info.author : tr("audiobook_unknown_author"));

	char status_str[128];
	lv_color_t ind_col;
	if (info.finished) {
		snprintf(status_str, sizeof(status_str), "%s · 100%%", tr("audiobook_completed"));
		ind_col = AUDIOBOOK_GREEN;
	} else if (info.percent > 0) {
		snprintf(status_str, sizeof(status_str), "Cap. %d de %d · %d%%",
				 info.current_chapter > 0 ? info.current_chapter : 1,
				 info.total_chapters > 0 ? info.total_chapters : 1,
				 info.percent);
		ind_col = AUDIOBOOK_ACCENT;
	} else {
		snprintf(status_str, sizeof(status_str), "%s · 0%%", tr("audiobook_not_started"));
		ind_col = AUDIOBOOK_BAR_TRACK;
	}

	lv_label_set_text(row->status_label, status_str);
	lv_bar_set_value(row->bar, info.percent, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(row->bar, ind_col, LV_PART_INDICATOR);

	row_show_glyph(row);
}

static void books_thumbs_update(void) {
	bool pending = false;
	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];
		if (row->index < 0 || row->index >= entry_count || row->thumb_settled) {
			continue;
		}

		const char *source = NULL;
		if (!row_at(row->index, NULL, &source) || !source[0]) {
			row->thumb_settled = true;
			continue;
		}

		if (!row->thumb_requested) {
			char first[512];
			coverloader_request(row_slot(row), cover_source(source, first, sizeof(first)), THUMB_SIZE);
			row->thumb_requested = true;
		}

		bool finished = false;
		cover_image_t image;
		if (coverloader_take(row_slot(row), &image, &finished)) {
			row->thumb = image;
			row->has_thumb = true;
			row->thumb_settled = true;
			row_show_cover(row, &row->thumb.dsc);
		} else if (finished) {
			row->thumb_settled = true;
		} else {
			pending = true;
		}
	}

	if (pending) {
		lv_timer_resume(thumb_timer);
	}
}

static void show_empty(lv_obj_t *label, int count) {
	if (count == 0) {
		lv_obj_set_hidden(label, false);
	} else {
		lv_obj_set_hidden(label, true);
	}
}

static void refresh_stale(void) {
	if (!book_index || !audiobookdb_index_stale(book_index)) {
		return;
	}
	audiobookdb_index_t *fresh = books_index_open();
	audiobookdb_index_close(book_index);
	book_index = fresh;
	entry_count = audiobookdb_index_count(fresh);
	window_first = -1;
	window_count = 0;

	for (int i = 0; i < ROW_POOL; i++) {
		row_drop_thumb(&rows[i]);
		rows[i].index = -2;
	}
	lv_obj_set_height(book_body, entry_count > 0 ? entry_count * ROW_PITCH : 1);
	show_empty(empty_label, entry_count);
}

static void window_update(void) {
	refresh_stale();
	if (entry_count == 0) {
		for (int i = 0; i < ROW_POOL; i++) {
			row_bind(&rows[i], -1);
		}
		return;
	}

	int scroll = lv_obj_get_scroll_y(book_list);
	if (scroll < 0) {
		scroll = 0;
	}

	int first = (scroll / ROW_PITCH) - 1;
	if (first + ROW_POOL > entry_count) {
		first = entry_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}

	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		row_bind(&rows[index % ROW_POOL], index < entry_count ? index : -1);
	}

	books_thumbs_update();
}

static void books_scroll_cb(lv_event_t *e) {
	(void)e;
	window_update();
}

static bool window_fill_cb(const char *name, const char *path, void *user) {
	int *filled = user;
	if (*filled >= WINDOW_ROWS) {
		return false;
	}
	winrow_t *row = &window_rows[*filled];
	snprintf(row->name, sizeof(row->name), "%s", name ? name : "");
	snprintf(row->path, sizeof(row->path), "%s", path ? path : "");
	(*filled)++;
	return true;
}

static bool window_cover(int index) {
	if (!book_index || index < 0 || index >= entry_count) {
		return false;
	}
	if (!window_rows) {
		window_rows = calloc(WINDOW_ROWS, sizeof(*window_rows));
		if (!window_rows) {
			return false;
		}
	}
	if (window_first >= 0 && index >= window_first && index < window_first + window_count) {
		return true;
	}

	int first = index - ROW_POOL;
	if (first < 0) {
		first = 0;
	}
	if (first + WINDOW_ROWS > entry_count) {
		first = entry_count - WINDOW_ROWS;
	}
	if (first < 0) {
		first = 0;
	}

	int filled = 0;
	audiobookdb_index_window(book_index, first, WINDOW_ROWS, window_fill_cb, &filled);
	window_first = first;
	window_count = filled;
	return index >= first && index < first + filled;
}

static bool row_at(int index, const char **name_out, const char **path_out) {
	if (index < 0 || index >= entry_count || !window_cover(index)) {
		return false;
	}
	const winrow_t *row = &window_rows[index - window_first];
	if (name_out) {
		*name_out = row->name;
	}
	if (path_out) {
		*path_out = row->path;
	}
	return true;
}

static const char *books_empty_text(void) {
	if (current_filter == FILTER_READ) return tr("audiobook_finished_empty");
	if (current_filter == FILTER_UNREAD) return tr("audiobook_list_empty");
	return tr("audiobook_empty_note");
}

static void reload_books(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		row_bind(&rows[i], -1);
		rows[i].index = -1;
	}

	audiobookdb_index_close(book_index);
	book_index = books_index_open();
	entry_count = audiobookdb_index_count(book_index);
	window_first = -1;
	window_count = 0;

	lv_obj_set_height(book_body, entry_count > 0 ? entry_count * ROW_PITCH : 1);
	lv_obj_scroll_to_y(book_list, 0, LV_ANIM_OFF);

	lv_label_set_text(empty_label, books_empty_text());
	show_empty(empty_label, entry_count);
	lv_image_set_src(books_sort_icon, sort_icon(list_sort(books_list)));

	window_update();
}

static void update_filter_pills_ui(void) {
	if (!filter_pill_all || !filter_pill_read || !filter_pill_unread) return;

	lv_obj_set_style_bg_color(filter_pill_all, current_filter == FILTER_ALL ? AUDIOBOOK_ACCENT : AUDIOBOOK_CARD_PRESSED, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(filter_pill_all, 0), current_filter == FILTER_ALL ? lv_color_white() : AUDIOBOOK_TEXT_DIM, 0);

	lv_obj_set_style_bg_color(filter_pill_read, current_filter == FILTER_READ ? AUDIOBOOK_ACCENT : AUDIOBOOK_CARD_PRESSED, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(filter_pill_read, 0), current_filter == FILTER_READ ? lv_color_white() : AUDIOBOOK_TEXT_DIM, 0);

	lv_obj_set_style_bg_color(filter_pill_unread, current_filter == FILTER_UNREAD ? AUDIOBOOK_ACCENT : AUDIOBOOK_CARD_PRESSED, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(filter_pill_unread, 0), current_filter == FILTER_UNREAD ? lv_color_white() : AUDIOBOOK_TEXT_DIM, 0);
}

static void filter_pill_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) return;
	books_filter_t f = (books_filter_t)(intptr_t)lv_event_get_user_data(e);
	if (current_filter != f) {
		current_filter = f;
		update_filter_pills_ui();
		reload_books();
	}
}

static void books_open(list_id_t list, const char *value, const char *title) {
	books_list = list;
	snprintf(books_value, sizeof(books_value), "%s", value ? value : "");
	lv_label_set_text(books_title, title);
	update_filter_pills_ui();
	reload_books();
	switch_screen(audiobooklibrary_screen);
}

static void book_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}

	lv_obj_t *button = lv_event_get_current_target(e);
	int index = -1;
	for (int i = 0; i < ROW_POOL; i++) {
		if (rows[i].button == button) {
			index = rows[i].index;
			break;
		}
	}
	const char *found = NULL;
	if (!row_at(index, NULL, &found) || !found[0]) {
		return;
	}
	audiobook_open_detail(found);
}

static void books_sort_cb(lv_event_t *e) {
	sort_menu_show(lv_event_get_current_target(e), books_list, reload_books);
}

static void books_loaded_cb(lv_event_t *e) {
	(void)e;
	for (int i = 0; i < ROW_POOL; i++) {
		rows[i].index = -2;
	}
	refresh_stale();
	window_update();
}

static lv_obj_t *create_pill_button(lv_obj_t *parent, const char *text, books_filter_t filter) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, LV_SIZE_CONTENT, 38);
	lv_obj_set_style_pad_hor(btn, 18, 0);
	lv_obj_set_style_pad_ver(btn, 0, 0);
	lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_style_bg_color(btn, AUDIOBOOK_CARD_PRESSED, 0);
	lv_obj_add_event_cb(btn, filter_pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)filter);

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, text);
	lv_obj_set_style_text_font(label, &font_ui_20, 0);
	lv_obj_set_style_text_color(label, AUDIOBOOK_TEXT_DIM, 0);
	lv_obj_center(label);

	return btn;
}

static void build_books_page(gui_config_t *cfg) {
	lv_obj_add_style(audiobooklibrary_screen, &theme_style_screen, 0);
	books_title = settingsrow_title(audiobooklibrary_screen, cfg, "audiobook_library");
	settingsrow_title_corner_slots(books_title, cfg, 3);

	corner_button(audiobooklibrary_screen, cfg, 0, &icon_music_settings, settings_click_cb);
	corner_button(audiobooklibrary_screen, cfg, 1, &icon_search, search_click_cb);
	lv_obj_t *sort_btn = corner_button(audiobooklibrary_screen, cfg, 2, &icon_sort_az, books_sort_cb);
	books_sort_icon = lv_obj_get_child(sort_btn, 0);

	int content_top = settingsrow_content_top(cfg);

	// Filter pills container
	lv_obj_t *pill_row = lv_obj_create(audiobooklibrary_screen);
	lv_obj_set_size(pill_row, lv_pct(100), 46);
	lv_obj_align(pill_row, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(pill_row, 0, 0);
	lv_obj_set_style_border_width(pill_row, 0, 0);
	lv_obj_set_style_pad_hor(pill_row, cfg->padding, 0);
	lv_obj_set_style_pad_ver(pill_row, 0, 0);
	lv_obj_set_style_pad_gap(pill_row, 10, 0);
	lv_obj_set_scrollable(pill_row, false);
	lv_obj_set_flex_flow(pill_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(pill_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	filter_pill_all = create_pill_button(pill_row, tr("audiobook_filter_all"), FILTER_ALL);
	filter_pill_read = create_pill_button(pill_row, tr("audiobook_filter_read"), FILTER_READ);
	filter_pill_unread = create_pill_button(pill_row, tr("audiobook_filter_unread"), FILTER_UNREAD);

	int list_top = content_top + 48;
	row_width = cfg->screen_width - 2 * cfg->padding;

	book_list = lv_obj_create(audiobooklibrary_screen);
	lv_obj_set_size(book_list, lv_pct(100), cfg->screen_height - list_top);
	lv_obj_align(book_list, LV_ALIGN_TOP_LEFT, 0, list_top);
	lv_obj_set_style_bg_opa(book_list, 0, 0);
	lv_obj_set_style_border_width(book_list, 0, 0);
	lv_obj_set_style_radius(book_list, 0, 0);
	lv_obj_set_style_pad_hor(book_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(book_list, 0, 0);
	lv_obj_set_scroll_dir(book_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(book_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(book_list, books_scroll_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_set_event_bubble(book_list, true);

	book_body = lv_obj_create(book_list);
	lv_obj_set_width(book_body, row_width);
	lv_obj_set_height(book_body, 1);
	lv_obj_set_pos(book_body, 0, 0);
	lv_obj_set_style_bg_opa(book_body, 0, 0);
	lv_obj_set_style_border_width(book_body, 0, 0);
	lv_obj_set_style_pad_all(book_body, 0, 0);
	lv_obj_set_scrollable(book_body, false);
	lv_obj_set_event_bubble(book_body, true);

	empty_label = lv_label_create(book_list);
	lv_obj_set_width(empty_label, row_width);
	lv_label_set_long_mode(empty_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(empty_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(empty_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(empty_label, &font_ui_24, 0);
	lv_obj_align(empty_label, LV_ALIGN_TOP_MID, 0, 90);
	lv_obj_set_hidden(empty_label, true);

	player_sheet_attach_drag(book_list, true);
	switcher_attach_back_gesture(book_list);

	for (int i = 0; i < ROW_POOL; i++) {
		row_t *row = &rows[i];

		row->button = lv_btn_create(book_body);
		lv_obj_set_size(row->button, row_width, ROW_HEIGHT);
		lv_obj_set_x(row->button, 0);
		lv_obj_set_style_bg_color(row->button, AUDIOBOOK_CARD_BG, 0);
		lv_obj_set_style_bg_color(row->button, AUDIOBOOK_CARD_PRESSED, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_all(row->button, ROW_PAD, 0);
		lv_obj_set_style_pad_gap(row->button, 14, 0);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_event_bubble(row->button, true);
		lv_obj_add_event_cb(row->button, book_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		row->icon = lv_image_create(row->button);
		lv_obj_set_size(row->icon, THUMB_SIZE, THUMB_SIZE);
		lv_obj_set_style_radius(row->icon, 8, 0);
		lv_obj_set_style_clip_corner(row->icon, true, 0);
		lv_image_set_inner_align(row->icon, LV_IMAGE_ALIGN_CENTER);

		lv_obj_t *col = lv_obj_create(row->button);
		lv_obj_set_size(col, 0, LV_SIZE_CONTENT);
		lv_obj_set_flex_grow(col, 1);
		lv_obj_set_style_bg_opa(col, 0, 0);
		lv_obj_set_style_border_width(col, 0, 0);
		lv_obj_set_style_pad_all(col, 0, 0);
		lv_obj_set_style_pad_gap(col, 3, 0);
		lv_obj_set_scrollable(col, false);
		lv_obj_set_event_bubble(col, true);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

		row->title_label = lv_label_create(col);
		lv_label_set_long_mode(row->title_label, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->title_label, lv_pct(100));
		lv_obj_set_style_text_color(row->title_label, lv_color_white(), 0);
		lv_obj_set_style_text_font(row->title_label, &font_ui_22, 0);
		lv_obj_set_style_max_height(row->title_label, lv_font_get_line_height(&font_ui_22), 0);

		row->author_label = lv_label_create(col);
		lv_label_set_long_mode(row->author_label, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->author_label, lv_pct(100));
		lv_obj_set_style_text_color(row->author_label, AUDIOBOOK_TEXT_DIM, 0);
		lv_obj_set_style_text_font(row->author_label, &font_ui_18, 0);
		lv_obj_set_style_max_height(row->author_label, lv_font_get_line_height(&font_ui_18), 0);

		row->status_label = lv_label_create(col);
		lv_label_set_long_mode(row->status_label, LV_LABEL_LONG_DOT);
		lv_obj_set_width(row->status_label, lv_pct(100));
		lv_obj_set_style_text_color(row->status_label, AUDIOBOOK_TEXT_DIM, 0);
		lv_obj_set_style_text_font(row->status_label, &font_ui_16, 0);

		row->bar = lv_bar_create(col);
		lv_obj_set_size(row->bar, lv_pct(100), 4);
		lv_bar_set_range(row->bar, 0, 100);
		lv_obj_set_style_bg_color(row->bar, AUDIOBOOK_BAR_TRACK, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(row->bar, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_bg_color(row->bar, AUDIOBOOK_ACCENT, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(row->bar, LV_OPA_COVER, LV_PART_INDICATOR);
		lv_obj_set_style_radius(row->bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_radius(row->bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
		lv_obj_set_event_bubble(row->bar, true);

		row->index = -1;
		row->has_thumb = false;
		row->thumb_requested = false;
		row->thumb_settled = false;
	}

	player_sheet_attach_drag(audiobooklibrary_screen, true);
	switcher_attach_back_gesture(audiobooklibrary_screen);
	lv_obj_add_event_cb(audiobooklibrary_screen, books_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// Screen 3 & 4: "Book Detail" (book_detail_screen)
// ---------------------------------------------------------------------------

typedef struct {
	char title[192];
	char path[512];
	double start;
	double duration;
} detail_chap_t;

static detail_chap_t *detail_chapters = NULL;
static int detail_chapter_count = 0;
static int detail_active_idx = -1;

typedef struct {
	char file[512];
	char label[128];
	double seconds;
} detail_mark_t;

static detail_mark_t *detail_bookmarks = NULL;
static int detail_bookmark_count = 0;

static char detail_book_path[512];
static lv_obj_t *detail_title;
static lv_obj_t *detail_scroll;
static lv_obj_t *detail_hero_img;
static lv_obj_t *detail_hero_title;
static lv_obj_t *detail_hero_author;
static lv_obj_t *detail_hero_meta;
static lv_obj_t *detail_resume_btn;
static lv_obj_t *detail_resume_label;

static lv_obj_t *tab_btn_chapters;
static lv_obj_t *tab_btn_bookmarks;
static lv_obj_t *tab_line_chapters;
static lv_obj_t *tab_line_bookmarks;
static lv_obj_t *detail_tab_body;

static int detail_tab_index = 0; // 0 = chapters, 1 = bookmarks
static cover_image_t detail_hero_thumb;
static bool detail_hero_has_thumb;
static bool detail_hero_thumb_requested;
static bool detail_hero_thumb_settled;

static void detail_hero_drop_thumb(void) {
	coverloader_release(SLOT_DETAIL_HERO);
	if (detail_hero_has_thumb) {
		lv_image_set_src(detail_hero_img, &icon_book_headphones_row);
		lv_obj_set_style_image_recolor_opa(detail_hero_img, LV_OPA_COVER, 0);
		cover_free(&detail_hero_thumb);
		detail_hero_has_thumb = false;
	}
	detail_hero_thumb_requested = false;
	detail_hero_thumb_settled = false;
}

static void detail_thumbs_update(void) {
	if (!detail_hero_img || detail_hero_thumb_settled || !detail_book_path[0]) return;

	if (!detail_hero_thumb_requested) {
		char first[512];
		coverloader_request(SLOT_DETAIL_HERO, cover_source(detail_book_path, first, sizeof(first)), 72);
		detail_hero_thumb_requested = true;
	}

	bool finished = false;
	cover_image_t image;
	if (coverloader_take(SLOT_DETAIL_HERO, &image, &finished)) {
		detail_hero_thumb = image;
		detail_hero_has_thumb = true;
		detail_hero_thumb_settled = true;
		lv_image_set_src(detail_hero_img, &detail_hero_thumb.dsc);
		lv_obj_set_style_image_recolor_opa(detail_hero_img, LV_OPA_TRANSP, 0);
	} else if (finished) {
		detail_hero_thumb_settled = true;
	} else {
		lv_timer_resume(thumb_timer);
	}
}

static void detail_resume_cb(lv_event_t *e) {
	(void)e;
	if (!detail_book_path[0]) return;
	play_book(detail_book_path);
}

static void chapter_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) return;
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (!detail_chapters || i < 0 || i >= detail_chapter_count) return;

	if (audiobookdb_is_folder_book(detail_book_path)) {
		audiobooks_play_at(detail_book_path, detail_chapters[i].path, 0.0);
	} else {
		audiobooks_play_at(detail_book_path, detail_book_path, detail_chapters[i].start);
	}
	player_sheet_open(true);
}

static void bookmark_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) return;
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (!detail_bookmarks || i < 0 || i >= detail_bookmark_count) return;

	audiobooks_play_at(detail_book_path, detail_bookmarks[i].file, detail_bookmarks[i].seconds);
	player_sheet_open(true);
}

static void format_clock(double seconds, char *out, size_t size) {
	if (seconds < 0) seconds = 0;
	long total = (long)seconds;
	long hours = total / 3600;
	long minutes = (total % 3600) / 60;
	long secs = total % 60;
	if (hours > 0) {
		snprintf(out, size, "%ld:%02ld:%02ld", hours, minutes, secs);
	} else {
		snprintf(out, size, "%ld:%02ld", minutes, secs);
	}
}

static bool collect_detail_parts_cb(const char *path, const char *title, void *user) {
	(void)user;
	int grown = detail_chapter_count + 1;
	detail_chap_t *bigger = realloc(detail_chapters, (size_t)grown * sizeof(*bigger));
	if (!bigger) return false;
	detail_chapters = bigger;

	detail_chap_t *c = &detail_chapters[detail_chapter_count];
	snprintf(c->path, sizeof(c->path), "%s", path ? path : "");
	if (title && title[0]) {
		snprintf(c->title, sizeof(c->title), "%s", title);
	} else {
		snprintf(c->title, sizeof(c->title), "%s %d", tr("audiobook_chapter_prefix"), detail_chapter_count + 1);
	}
	c->start = 0;
	c->duration = 0;
	detail_chapter_count++;
	return true;
}

static bool collect_detail_bookmarks_cb(long long id, const char *file, double seconds, const char *label, void *user) {
	(void)id;
	(void)user;
	int grown = detail_bookmark_count + 1;
	detail_mark_t *bigger = realloc(detail_bookmarks, (size_t)grown * sizeof(*bigger));
	if (!bigger) return false;
	detail_bookmarks = bigger;

	detail_mark_t *m = &detail_bookmarks[detail_bookmark_count];
	snprintf(m->file, sizeof(m->file), "%s", file ? file : "");
	snprintf(m->label, sizeof(m->label), "%s", label ? label : "");
	m->seconds = seconds;
	detail_bookmark_count++;
	return true;
}

static void load_detail_data(const char *book_path) {
	free(detail_chapters);
	detail_chapters = NULL;
	detail_chapter_count = 0;
	detail_active_idx = -1;

	free(detail_bookmarks);
	detail_bookmarks = NULL;
	detail_bookmark_count = 0;

	if (!book_path || !book_path[0]) return;

	char resume_file[512] = "";
	double resume_pos = 0;
	bool has_pos = audiobookdb_get_position(book_path, resume_file, sizeof(resume_file), &resume_pos);
	bool finished = audiobookdb_is_finished(book_path);

	bool is_folder = audiobookdb_is_folder_book(book_path);
	if (is_folder) {
		audiobookdb_parts_for_each(book_path, collect_detail_parts_cb, NULL);
		if (!finished && has_pos && resume_file[0]) {
			for (int i = 0; i < detail_chapter_count; i++) {
				if (strcmp(detail_chapters[i].path, resume_file) == 0) {
					detail_active_idx = i;
					break;
				}
			}
		}
	} else {
		audiobook_chapter_entry_t *entries = NULL;
		int count = audiobook_get_file_chapters(book_path, &entries);
		if (count > 0 && entries) {
			detail_chapters = calloc((size_t)count, sizeof(*detail_chapters));
			if (detail_chapters) {
				detail_chapter_count = count;
				for (int i = 0; i < count; i++) {
					snprintf(detail_chapters[i].title, sizeof(detail_chapters[i].title), "%s", entries[i].title);
					snprintf(detail_chapters[i].path, sizeof(detail_chapters[i].path), "%s", book_path);
					detail_chapters[i].start = entries[i].start;
					if (i + 1 < count) {
						detail_chapters[i].duration = entries[i + 1].start - entries[i].start;
					}
				}
			}
			audiobook_free_file_chapters(entries);

			if (!finished && has_pos && resume_pos > 1.0) {
				for (int i = 0; i < detail_chapter_count; i++) {
					double next_start = (i + 1 < detail_chapter_count) ? detail_chapters[i + 1].start : 1e9;
					if (resume_pos >= detail_chapters[i].start && resume_pos < next_start) {
						detail_active_idx = i;
						break;
					}
				}
			}
		} else {
			detail_chapters = calloc(1, sizeof(*detail_chapters));
			if (detail_chapters) {
				detail_chapter_count = 1;
				snprintf(detail_chapters[0].title, sizeof(detail_chapters[0].title), "%s 1", tr("audiobook_chapter_prefix"));
				snprintf(detail_chapters[0].path, sizeof(detail_chapters[0].path), "%s", book_path);
				detail_chapters[0].start = 0;
			}
			if (!finished && has_pos && resume_pos > 1.0) {
				detail_active_idx = 0;
			}
		}
	}

	audiobookdb_bookmarks_for_each(book_path, collect_detail_bookmarks_cb, NULL);
}

static void rebuild_detail_tab_content(void) {
	lv_obj_clean(detail_tab_body);

	if (detail_tab_index == 0) {
		// Chapters Tab
		bool finished = audiobookdb_is_finished(detail_book_path);
		char resume_file[512] = "";
		double resume_pos = 0;
		audiobookdb_get_position(detail_book_path, resume_file, sizeof(resume_file), &resume_pos);

		for (int i = 0; i < detail_chapter_count; i++) {
			bool is_playing = (i == detail_active_idx);
			bool is_completed = finished || (detail_active_idx >= 0 && i < detail_active_idx);

			lv_obj_t *card = lv_btn_create(detail_tab_body);
			lv_obj_set_size(card, lv_pct(100), is_playing ? 70 : 58);
			lv_obj_set_style_radius(card, 12, 0);
			lv_obj_set_style_pad_hor(card, 16, 0);
			lv_obj_set_style_pad_ver(card, 0, 0);
			lv_obj_set_style_shadow_width(card, 0, 0);
			lv_obj_set_style_border_width(card, is_playing ? 2 : 0, 0);
			lv_obj_set_style_border_color(card, AUDIOBOOK_ACCENT, 0);
			lv_obj_set_style_bg_color(card, is_playing ? AUDIOBOOK_ACTIVE_BG : AUDIOBOOK_CARD_BG, 0);
			lv_obj_set_style_bg_color(card, AUDIOBOOK_CARD_PRESSED, LV_STATE_PRESSED);
			lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
			lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
			lv_obj_set_style_pad_gap(card, 12, 0);
			lv_obj_add_event_cb(card, chapter_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

			if (is_playing) {
				lv_obj_t *play_circle = lv_obj_create(card);
				lv_obj_set_size(play_circle, 32, 32);
				lv_obj_set_style_radius(play_circle, LV_RADIUS_CIRCLE, 0);
				lv_obj_set_style_bg_color(play_circle, AUDIOBOOK_ACCENT, 0);
				lv_obj_set_style_border_width(play_circle, 0, 0);
				lv_obj_set_style_pad_all(play_circle, 0, 0);
				lv_obj_set_scrollable(play_circle, false);

				lv_obj_t *play_icon = lv_image_create(play_circle);
				lv_image_set_src(play_icon, &icon_play_small);
				lv_obj_add_style(play_icon, &theme_style_icon, 0);
				lv_obj_set_style_image_recolor_opa(play_icon, LV_OPA_COVER, 0);
				lv_obj_set_style_image_recolor(play_icon, lv_color_white(), 0);
				lv_obj_center(play_icon);

				lv_obj_t *col = lv_obj_create(card);
				lv_obj_set_size(col, 0, LV_SIZE_CONTENT);
				lv_obj_set_flex_grow(col, 1);
				lv_obj_set_style_bg_opa(col, 0, 0);
				lv_obj_set_style_border_width(col, 0, 0);
				lv_obj_set_style_pad_all(col, 0, 0);
				lv_obj_set_style_pad_gap(col, 2, 0);
				lv_obj_set_scrollable(col, false);
				lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

				lv_obj_t *lbl = lv_label_create(col);
				lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
				lv_obj_set_width(lbl, lv_pct(100));
				lv_label_set_text_fmt(lbl, "%s %s", detail_chapters[i].title, tr("audiobook_now_playing_tag"));
				lv_obj_set_style_text_color(lbl, AUDIOBOOK_ACCENT, 0);
				lv_obj_set_style_text_font(lbl, &font_ui_20, 0);

				char time_str[64];
				char dur_str[32];
				double chap_pos = (resume_pos >= detail_chapters[i].start) ? (resume_pos - detail_chapters[i].start) : resume_pos;
				format_clock(chap_pos, time_str, sizeof(time_str));
				if (detail_chapters[i].duration > 0) {
					format_clock(detail_chapters[i].duration, dur_str, sizeof(dur_str));
					snprintf(time_str + strlen(time_str), sizeof(time_str) - strlen(time_str), " / %s", dur_str);
				}
				lv_obj_t *time_lbl = lv_label_create(col);
				lv_label_set_text(time_lbl, time_str);
				lv_obj_set_style_text_color(time_lbl, AUDIOBOOK_TEXT_DIM, 0);
				lv_obj_set_style_text_font(time_lbl, &font_ui_16, 0);
			} else {
				lv_obj_t *lbl = lv_label_create(card);
				lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
				lv_obj_set_flex_grow(lbl, 1);
				lv_label_set_text(lbl, detail_chapters[i].title);
				lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
				lv_obj_set_style_text_font(lbl, &font_ui_22, 0);

				if (is_completed) {
					lv_obj_t *chk = lv_image_create(card);
					lv_image_set_src(chk, &icon_check_small);
					lv_obj_add_style(chk, &theme_style_icon, 0);
					lv_obj_set_style_image_recolor_opa(chk, LV_OPA_COVER, 0);
					lv_obj_set_style_image_recolor(chk, AUDIOBOOK_TEXT_DIM, 0);
				}
			}
		}
	} else {
		// Bookmarks Tab
		for (int i = 0; i < detail_bookmark_count; i++) {
			lv_obj_t *card = lv_btn_create(detail_tab_body);
			lv_obj_set_size(card, lv_pct(100), 58);
			lv_obj_set_style_radius(card, 12, 0);
			lv_obj_set_style_pad_hor(card, 16, 0);
			lv_obj_set_style_shadow_width(card, 0, 0);
			lv_obj_set_style_border_width(card, 0, 0);
			lv_obj_set_style_bg_color(card, AUDIOBOOK_CARD_BG, 0);
			lv_obj_set_style_bg_color(card, AUDIOBOOK_CARD_PRESSED, LV_STATE_PRESSED);
			lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
			lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
			lv_obj_add_event_cb(card, bookmark_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

			char clock[32];
			format_clock(detail_bookmarks[i].seconds, clock, sizeof(clock));

			lv_obj_t *lbl = lv_label_create(card);
			lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
			lv_obj_set_flex_grow(lbl, 1);
			if (detail_bookmarks[i].label[0]) {
				lv_label_set_text_fmt(lbl, "%s · %s", detail_bookmarks[i].label, clock);
			} else {
				lv_label_set_text_fmt(lbl, "%s · %s", tr("audiobook_chapter_prefix"), clock);
			}
			lv_obj_add_style(lbl, &theme_style_text, 0);
			lv_obj_set_style_text_font(lbl, &font_ui_22, 0);
		}

		if (detail_bookmark_count == 0) {
			lv_obj_t *empty = lv_label_create(detail_tab_body);
			lv_label_set_text(empty, tr("audiobook_no_bookmarks"));
			lv_obj_set_style_text_color(empty, AUDIOBOOK_TEXT_DIM, 0);
			lv_obj_set_style_text_font(empty, &font_ui_20, 0);
			lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_set_width(empty, lv_pct(100));
			lv_obj_set_style_pad_top(empty, 40, 0);
		}
	}
}

static void update_detail_tabs_ui(void) {
	if (!tab_btn_chapters || !tab_btn_bookmarks) return;

	lv_obj_t *lbl_chap = lv_obj_get_child(tab_btn_chapters, 0);
	lv_obj_t *lbl_mark = lv_obj_get_child(tab_btn_bookmarks, 0);

	if (detail_tab_index == 0) {
		lv_obj_set_style_text_color(lbl_chap, AUDIOBOOK_ACCENT, 0);
		lv_obj_set_hidden(tab_line_chapters, false);

		lv_obj_set_style_text_color(lbl_mark, AUDIOBOOK_TEXT_DIM, 0);
		lv_obj_set_hidden(tab_line_bookmarks, true);
	} else {
		lv_obj_set_style_text_color(lbl_chap, AUDIOBOOK_TEXT_DIM, 0);
		lv_obj_set_hidden(tab_line_chapters, true);

		lv_obj_set_style_text_color(lbl_mark, AUDIOBOOK_ACCENT, 0);
		lv_obj_set_hidden(tab_line_bookmarks, false);
	}
	rebuild_detail_tab_content();
}

static void detail_tab_cb(lv_event_t *e) {
	if (switcher_back_drag_active()) return;
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	if (detail_tab_index != idx) {
		detail_tab_index = idx;
		update_detail_tabs_ui();
	}
}

void audiobook_open_detail(const char *book_path) {
	if (!book_path || !book_path[0]) return;
	snprintf(detail_book_path, sizeof(detail_book_path), "%s", book_path);

	load_detail_data(detail_book_path);

	book_progress_info_t info;
	get_book_progress_info(detail_book_path, &info);

	lv_label_set_text(detail_title, info.title[0] ? info.title : tr("audiobooks"));
	lv_label_set_text(detail_hero_title, info.title[0] ? info.title : tr("audiobooks"));
	lv_label_set_text(detail_hero_author, info.author[0] ? info.author : tr("audiobook_unknown_author"));

	char meta_str[128];
	int chap_display_count = detail_chapter_count > 0 ? detail_chapter_count : (info.total_chapters > 0 ? info.total_chapters : 1);
	snprintf(meta_str, sizeof(meta_str), "%d %s", chap_display_count, tr("audiobook_chapters_count"));
	lv_label_set_text(detail_hero_meta, meta_str);

	if (info.finished || info.percent == 0) {
		lv_label_set_text(detail_resume_label, tr("audiobook_start"));
	} else {
		lv_label_set_text(detail_resume_label, tr("audiobook_resume"));
	}

	detail_hero_drop_thumb();
	detail_thumbs_update();

	detail_tab_index = 0;
	update_detail_tabs_ui();

	switch_screen(book_detail_screen);
}

static void detail_loaded_cb(lv_event_t *e) {
	(void)e;
	if (detail_book_path[0]) {
		load_detail_data(detail_book_path);
		rebuild_detail_tab_content();
	}
	detail_thumbs_update();
}

static void build_detail_page(gui_config_t *cfg) {
	lv_obj_add_style(book_detail_screen, &theme_style_screen, 0);
	detail_title = settingsrow_title(book_detail_screen, cfg, "");
	settingsrow_title_corner_slots(detail_title, cfg, 1);
	corner_button(book_detail_screen, cfg, 0, &icon_music_settings, settings_click_cb);

	int content_top = settingsrow_content_top(cfg);

	detail_scroll = lv_obj_create(book_detail_screen);
	lv_obj_set_size(detail_scroll, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(detail_scroll, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(detail_scroll, 0, 0);
	lv_obj_set_style_border_width(detail_scroll, 0, 0);
	lv_obj_set_style_pad_hor(detail_scroll, cfg->padding, 0);
	lv_obj_set_style_pad_ver(detail_scroll, 0, 0);
	lv_obj_set_style_pad_gap(detail_scroll, 14, 0);
	lv_obj_set_scroll_dir(detail_scroll, LV_DIR_VER);
	lv_obj_set_flex_flow(detail_scroll, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(detail_scroll, true);
	player_sheet_attach_drag(detail_scroll, true);
	switcher_attach_back_gesture(detail_scroll);

	// Hero Card
	lv_obj_t *hero = lv_obj_create(detail_scroll);
	lv_obj_set_size(hero, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(hero, AUDIOBOOK_CARD_BG, 0);
	lv_obj_set_style_radius(hero, 14, 0);
	lv_obj_set_style_border_width(hero, 0, 0);
	lv_obj_set_style_shadow_width(hero, 0, 0);
	lv_obj_set_style_pad_all(hero, 14, 0);
	lv_obj_set_style_pad_gap(hero, 12, 0);
	lv_obj_set_scrollable(hero, false);
	lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	detail_hero_img = lv_image_create(hero);
	lv_obj_set_size(detail_hero_img, 72, 72);
	lv_obj_set_style_radius(detail_hero_img, 8, 0);
	lv_obj_set_style_clip_corner(detail_hero_img, true, 0);
	lv_image_set_src(detail_hero_img, &icon_book_headphones_row);
	lv_obj_add_style(detail_hero_img, &theme_style_icon, 0);

	lv_obj_t *hero_col = lv_obj_create(hero);
	lv_obj_set_size(hero_col, 0, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(hero_col, 1);
	lv_obj_set_style_bg_opa(hero_col, 0, 0);
	lv_obj_set_style_border_width(hero_col, 0, 0);
	lv_obj_set_style_pad_all(hero_col, 0, 0);
	lv_obj_set_style_pad_gap(hero_col, 3, 0);
	lv_obj_set_scrollable(hero_col, false);
	lv_obj_set_flex_flow(hero_col, LV_FLEX_FLOW_COLUMN);

	detail_hero_title = lv_label_create(hero_col);
	lv_label_set_long_mode(detail_hero_title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(detail_hero_title, lv_pct(100));
	lv_obj_set_style_text_color(detail_hero_title, lv_color_white(), 0);
	lv_obj_set_style_text_font(detail_hero_title, &font_ui_22, 0);
	lv_obj_set_style_max_height(detail_hero_title, lv_font_get_line_height(&font_ui_22), 0);

	detail_hero_author = lv_label_create(hero_col);
	lv_label_set_long_mode(detail_hero_author, LV_LABEL_LONG_DOT);
	lv_obj_set_width(detail_hero_author, lv_pct(100));
	lv_obj_set_style_text_color(detail_hero_author, AUDIOBOOK_TEXT_DIM, 0);
	lv_obj_set_style_text_font(detail_hero_author, &font_ui_18, 0);
	lv_obj_set_style_max_height(detail_hero_author, lv_font_get_line_height(&font_ui_18), 0);

	detail_hero_meta = lv_label_create(hero_col);
	lv_label_set_long_mode(detail_hero_meta, LV_LABEL_LONG_DOT);
	lv_obj_set_width(detail_hero_meta, lv_pct(100));
	lv_obj_set_style_text_color(detail_hero_meta, AUDIOBOOK_TEXT_DIM, 0);
	lv_obj_set_style_text_font(detail_hero_meta, &font_ui_16, 0);

	detail_resume_btn = lv_btn_create(hero);
	lv_obj_set_size(detail_resume_btn, LV_SIZE_CONTENT, 40);
	lv_obj_set_style_pad_hor(detail_resume_btn, 16, 0);
	lv_obj_set_style_radius(detail_resume_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(detail_resume_btn, AUDIOBOOK_ACCENT, 0);
	lv_obj_set_style_shadow_width(detail_resume_btn, 0, 0);
	lv_obj_set_style_border_width(detail_resume_btn, 0, 0);
	lv_obj_add_event_cb(detail_resume_btn, detail_resume_cb, LV_EVENT_CLICKED, NULL);

	detail_resume_label = lv_label_create(detail_resume_btn);
	lv_label_set_text(detail_resume_label, tr("audiobook_resume"));
	lv_obj_set_style_text_color(detail_resume_label, lv_color_white(), 0);
	lv_obj_set_style_text_font(detail_resume_label, &font_ui_18, 0);
	lv_obj_center(detail_resume_label);

	// Tab Bar
	lv_obj_t *tab_bar = lv_obj_create(detail_scroll);
	lv_obj_set_size(tab_bar, lv_pct(100), 44);
	lv_obj_set_style_bg_opa(tab_bar, 0, 0);
	lv_obj_set_style_border_width(tab_bar, 0, 0);
	lv_obj_set_style_pad_all(tab_bar, 0, 0);
	lv_obj_set_style_pad_gap(tab_bar, 24, 0);
	lv_obj_set_scrollable(tab_bar, false);
	lv_obj_set_flex_flow(tab_bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tab_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// Tab Chapters
	tab_btn_chapters = lv_btn_create(tab_bar);
	lv_obj_set_size(tab_btn_chapters, LV_SIZE_CONTENT, 44);
	lv_obj_set_style_bg_opa(tab_btn_chapters, 0, 0);
	lv_obj_set_style_shadow_width(tab_btn_chapters, 0, 0);
	lv_obj_set_style_border_width(tab_btn_chapters, 0, 0);
	lv_obj_set_style_pad_all(tab_btn_chapters, 0, 0);
	lv_obj_set_flex_flow(tab_btn_chapters, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tab_btn_chapters, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(tab_btn_chapters, detail_tab_cb, LV_EVENT_CLICKED, (void *)0);

	lv_obj_t *l_chap = lv_label_create(tab_btn_chapters);
	lv_label_set_text(l_chap, tr("chapters"));
	lv_obj_set_style_text_font(l_chap, &font_ui_22, 0);

	tab_line_chapters = lv_obj_create(tab_btn_chapters);
	lv_obj_set_size(tab_line_chapters, lv_pct(100), 3);
	lv_obj_set_style_bg_color(tab_line_chapters, AUDIOBOOK_ACCENT, 0);
	lv_obj_set_style_radius(tab_line_chapters, 2, 0);
	lv_obj_set_style_border_width(tab_line_chapters, 0, 0);

	// Tab Bookmarks
	tab_btn_bookmarks = lv_btn_create(tab_bar);
	lv_obj_set_size(tab_btn_bookmarks, LV_SIZE_CONTENT, 44);
	lv_obj_set_style_bg_opa(tab_btn_bookmarks, 0, 0);
	lv_obj_set_style_shadow_width(tab_btn_bookmarks, 0, 0);
	lv_obj_set_style_border_width(tab_btn_bookmarks, 0, 0);
	lv_obj_set_style_pad_all(tab_btn_bookmarks, 0, 0);
	lv_obj_set_flex_flow(tab_btn_bookmarks, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tab_btn_bookmarks, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(tab_btn_bookmarks, detail_tab_cb, LV_EVENT_CLICKED, (void *)1);

	lv_obj_t *l_mark = lv_label_create(tab_btn_bookmarks);
	lv_label_set_text(l_mark, tr("bookmarks"));
	lv_obj_set_style_text_font(l_mark, &font_ui_22, 0);

	tab_line_bookmarks = lv_obj_create(tab_btn_bookmarks);
	lv_obj_set_size(tab_line_bookmarks, lv_pct(100), 3);
	lv_obj_set_style_bg_color(tab_line_bookmarks, AUDIOBOOK_ACCENT, 0);
	lv_obj_set_style_radius(tab_line_bookmarks, 2, 0);
	lv_obj_set_style_border_width(tab_line_bookmarks, 0, 0);
	lv_obj_set_hidden(tab_line_bookmarks, true);

	// Tab content container
	detail_tab_body = lv_obj_create(detail_scroll);
	lv_obj_set_size(detail_tab_body, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(detail_tab_body, 0, 0);
	lv_obj_set_style_border_width(detail_tab_body, 0, 0);
	lv_obj_set_style_pad_all(detail_tab_body, 0, 0);
	lv_obj_set_style_pad_gap(detail_tab_body, 8, 0);
	lv_obj_set_scrollable(detail_tab_body, false);
	lv_obj_set_flex_flow(detail_tab_body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(detail_tab_body, true);

	player_sheet_attach_drag(book_detail_screen, true);
	switcher_attach_back_gesture(book_detail_screen);
	lv_obj_add_event_cb(book_detail_screen, detail_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// Screen 1: "Inicio" (audiobooks_screen)
// ---------------------------------------------------------------------------

static lv_obj_t *home_scroll;
static lv_obj_t *section_empty;

static lv_obj_t *home_hero_card;
static lv_obj_t *home_hero_img;
static lv_obj_t *home_hero_title;
static lv_obj_t *home_hero_author;
static lv_obj_t *home_hero_bar;
static lv_obj_t *home_hero_pct;
static lv_obj_t *home_hero_play_btn;
static char home_hero_path[512];

static lv_obj_t *home_continue_container;

typedef struct {
	char path[512];
	lv_obj_t *card;
	lv_obj_t *img;
	bool has_thumb;
	bool thumb_requested;
	bool thumb_settled;
	cover_image_t thumb;
} home_continue_item_t;

static home_continue_item_t home_continue_items[MAX_HOME_CONTINUE];
static int home_continue_count = 0;

static cover_image_t home_hero_thumb;
static bool home_hero_has_thumb;
static bool home_hero_thumb_requested;
static bool home_hero_thumb_settled;

static void home_hero_drop_thumb(void) {
	coverloader_release(SLOT_HOME_HERO);
	if (home_hero_has_thumb) {
		lv_image_set_src(home_hero_img, &icon_book_headphones_row);
		lv_obj_set_style_image_recolor_opa(home_hero_img, LV_OPA_COVER, 0);
		cover_free(&home_hero_thumb);
		home_hero_has_thumb = false;
	}
	home_hero_thumb_requested = false;
	home_hero_thumb_settled = false;
}

static void home_continue_drop_thumbs(void) {
	for (int i = 0; i < MAX_HOME_CONTINUE; i++) {
		coverloader_release(SLOT_HOME_CONTINUE + i);
		if (home_continue_items[i].has_thumb) {
			cover_free(&home_continue_items[i].thumb);
			home_continue_items[i].has_thumb = false;
		}
		home_continue_items[i].thumb_requested = false;
		home_continue_items[i].thumb_settled = false;
	}
}

static void home_thumbs_update(void) {
	bool pending = false;

	if (home_hero_path[0] && !home_hero_thumb_settled) {
		if (!home_hero_thumb_requested) {
			char first[512];
			coverloader_request(SLOT_HOME_HERO, cover_source(home_hero_path, first, sizeof(first)), 80);
			home_hero_thumb_requested = true;
		}

		bool finished = false;
		cover_image_t image;
		if (coverloader_take(SLOT_HOME_HERO, &image, &finished)) {
			home_hero_thumb = image;
			home_hero_has_thumb = true;
			home_hero_thumb_settled = true;
			lv_image_set_src(home_hero_img, &home_hero_thumb.dsc);
			lv_obj_set_style_image_recolor_opa(home_hero_img, LV_OPA_TRANSP, 0);
		} else if (finished) {
			home_hero_thumb_settled = true;
		} else {
			pending = true;
		}
	}

	for (int i = 0; i < home_continue_count; i++) {
		home_continue_item_t *item = &home_continue_items[i];
		if (!item->path[0] || item->thumb_settled) continue;

		int slot = SLOT_HOME_CONTINUE + i;
		if (!item->thumb_requested) {
			char first[512];
			coverloader_request(slot, cover_source(item->path, first, sizeof(first)), 56);
			item->thumb_requested = true;
		}

		bool finished = false;
		cover_image_t image;
		if (coverloader_take(slot, &image, &finished)) {
			item->thumb = image;
			item->has_thumb = true;
			item->thumb_settled = true;
			lv_image_set_src(item->img, &item->thumb.dsc);
			lv_obj_set_style_image_recolor_opa(item->img, LV_OPA_TRANSP, 0);
		} else if (finished) {
			item->thumb_settled = true;
		} else {
			pending = true;
		}
	}

	if (pending) {
		lv_timer_resume(thumb_timer);
	}
}

static void thumb_timer_cb(lv_timer_t *timer) {
	lv_timer_pause(timer);
	lv_obj_t *active = lv_screen_active();
	if (active == audiobooklibrary_screen) {
		books_thumbs_update();
	} else if (active == audiobooks_screen) {
		home_thumbs_update();
	} else if (active == book_detail_screen) {
		detail_thumbs_update();
	}
}

static void open_library(void) {
	books_open(LIST_LIBRARY, NULL, tr("audiobook_library"));
}

static void open_library_banner_cb(lv_event_t *e) {
	(void)e;
	if (switcher_back_drag_active()) return;
	open_library();
}

static void home_hero_play_cb(lv_event_t *e) {
	(void)e;
	if (home_hero_path[0]) {
		play_book(home_hero_path);
	}
}

static void home_hero_card_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) return;
	lv_obj_t *target = lv_event_get_target(e);
	if (target == home_hero_play_btn || (target && lv_obj_get_parent(target) == home_hero_play_btn)) {
		return;
	}
	if (home_hero_path[0]) {
		audiobook_open_detail(home_hero_path);
	}
}

static void home_continue_click_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) return;
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	if (idx >= 0 && idx < home_continue_count) {
		audiobook_open_detail(home_continue_items[idx].path);
	}
}

typedef struct {
	int count;
	char paths[MAX_HOME_CONTINUE + 1][512];
} continue_collect_t;

static bool collect_continue_cb(const char *name, const char *path, void *user) {
	(void)name;
	continue_collect_t *c = user;
	if (c->count < MAX_HOME_CONTINUE + 1) {
		snprintf(c->paths[c->count++], 512, "%s", path ? path : "");
	}
	return c->count < MAX_HOME_CONTINUE + 1;
}

static void reload_home_page(void) {
	if (audiobookdb_count() == 0) {
		lv_obj_set_hidden(home_scroll, true);
		lv_obj_set_hidden(section_empty, false);
		return;
	}
	lv_obj_set_hidden(home_scroll, false);
	lv_obj_set_hidden(section_empty, true);

	home_hero_drop_thumb();
	home_continue_drop_thumbs();

	// Fetch continue list
	audiobookdb_index_t *c_idx = audiobookdb_index_open(AUDIOBOOK_LIST_CONTINUE, NULL, AUDIOBOOK_ORDER_PLAYED, true);
	continue_collect_t coll;
	memset(&coll, 0, sizeof(coll));
	if (c_idx) {
		int total = audiobookdb_index_count(c_idx);
		if (total > 0) {
			audiobookdb_index_window(c_idx, 0, MAX_HOME_CONTINUE + 1, collect_continue_cb, &coll);
		}
		audiobookdb_index_close(c_idx);
	}

	// If no continue books, fallback hero to first library book
	if (coll.count == 0) {
		audiobookdb_index_t *all_idx = audiobookdb_index_open(AUDIOBOOK_LIST_ALL, NULL, AUDIOBOOK_ORDER_NAME, false);
		if (all_idx) {
			int total = audiobookdb_index_count(all_idx);
			if (total > 0) {
				audiobookdb_index_window(all_idx, 0, 1, collect_continue_cb, &coll);
			}
			audiobookdb_index_close(all_idx);
		}
	}

	if (coll.count > 0) {
		snprintf(home_hero_path, sizeof(home_hero_path), "%s", coll.paths[0]);
		book_progress_info_t info;
		get_book_progress_info(home_hero_path, &info);

		lv_label_set_text(home_hero_title, info.title[0] ? info.title : tr("audiobooks"));
		lv_label_set_text(home_hero_author, info.author[0] ? info.author : tr("audiobook_unknown_author"));
		lv_bar_set_value(home_hero_bar, info.percent, LV_ANIM_OFF);
		lv_label_set_text_fmt(home_hero_pct, "%d%%", info.percent);

		lv_obj_set_hidden(home_hero_card, false);
	} else {
		home_hero_path[0] = '\0';
		lv_obj_set_hidden(home_hero_card, true);
	}

	// Populate "En curso" list: show remaining books, never duplicate hero book!
	lv_obj_clean(home_continue_container);
	home_continue_count = 0;

	for (int i = 1; i < coll.count && home_continue_count < MAX_HOME_CONTINUE; i++) {
		const char *path = coll.paths[i];
		book_progress_info_t info;
		get_book_progress_info(path, &info);

		int item_idx = home_continue_count++;
		home_continue_item_t *item = &home_continue_items[item_idx];
		snprintf(item->path, sizeof(item->path), "%s", path);
		item->has_thumb = false;
		item->thumb_requested = false;
		item->thumb_settled = false;

		item->card = lv_btn_create(home_continue_container);
		lv_obj_set_size(item->card, lv_pct(100), 78);
		lv_obj_set_style_bg_color(item->card, AUDIOBOOK_CARD_BG, 0);
		lv_obj_set_style_bg_color(item->card, AUDIOBOOK_CARD_PRESSED, LV_STATE_PRESSED);
		lv_obj_set_style_radius(item->card, 12, 0);
		lv_obj_set_style_border_width(item->card, 0, 0);
		lv_obj_set_style_shadow_width(item->card, 0, 0);
		lv_obj_set_style_pad_all(item->card, 10, 0);
		lv_obj_set_style_pad_gap(item->card, 12, 0);
		lv_obj_set_flex_flow(item->card, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(item->card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_add_event_cb(item->card, home_continue_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)item_idx);

		item->img = lv_image_create(item->card);
		lv_obj_set_size(item->img, 56, 56);
		lv_obj_set_style_radius(item->img, 6, 0);
		lv_obj_set_style_clip_corner(item->img, true, 0);
		lv_image_set_src(item->img, &icon_book_headphones_row);
		lv_obj_add_style(item->img, &theme_style_icon, 0);

		lv_obj_t *col = lv_obj_create(item->card);
		lv_obj_set_size(col, 0, LV_SIZE_CONTENT);
		lv_obj_set_flex_grow(col, 1);
		lv_obj_set_style_bg_opa(col, 0, 0);
		lv_obj_set_style_border_width(col, 0, 0);
		lv_obj_set_style_pad_all(col, 0, 0);
		lv_obj_set_style_pad_gap(col, 2, 0);
		lv_obj_set_scrollable(col, false);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

		lv_obj_t *lbl_t = lv_label_create(col);
		lv_label_set_long_mode(lbl_t, LV_LABEL_LONG_DOT);
		lv_obj_set_width(lbl_t, lv_pct(100));
		lv_label_set_text(lbl_t, info.title[0] ? info.title : tr("audiobooks"));
		lv_obj_set_style_text_color(lbl_t, lv_color_white(), 0);
		lv_obj_set_style_text_font(lbl_t, &font_ui_20, 0);
		lv_obj_set_style_max_height(lbl_t, lv_font_get_line_height(&font_ui_20), 0);

		lv_obj_t *lbl_a = lv_label_create(col);
		lv_label_set_long_mode(lbl_a, LV_LABEL_LONG_DOT);
		lv_obj_set_width(lbl_a, lv_pct(100));
		lv_label_set_text(lbl_a, info.author[0] ? info.author : tr("audiobook_unknown_author"));
		lv_obj_set_style_text_color(lbl_a, AUDIOBOOK_TEXT_DIM, 0);
		lv_obj_set_style_text_font(lbl_a, &font_ui_16, 0);
		lv_obj_set_style_max_height(lbl_a, lv_font_get_line_height(&font_ui_16), 0);

		lv_obj_t *lbl_s = lv_label_create(col);
		lv_label_set_text_fmt(lbl_s, "Cap. %d · %d%%", info.current_chapter > 0 ? info.current_chapter : 1, info.percent);
		lv_obj_set_style_text_color(lbl_s, AUDIOBOOK_TEXT_DIM, 0);
		lv_obj_set_style_text_font(lbl_s, &font_ui_14, 0);

		lv_obj_t *bar = lv_bar_create(col);
		lv_obj_set_size(bar, lv_pct(100), 4);
		lv_bar_set_range(bar, 0, 100);
		lv_bar_set_value(bar, info.percent, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(bar, AUDIOBOOK_BAR_TRACK, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_bg_color(bar, AUDIOBOOK_ACCENT, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
		lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
	}

	if (home_continue_count == 0) {
		lv_obj_t *empty = lv_label_create(home_continue_container);
		lv_label_set_text(empty, tr("audiobook_continue_empty"));
		lv_obj_set_style_text_color(empty, AUDIOBOOK_TEXT_DIM, 0);
		lv_obj_set_style_text_font(empty, &font_ui_18, 0);
		lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(empty, lv_pct(100));
		lv_obj_set_style_pad_ver(empty, 20, 0);
	}

	home_thumbs_update();
}

static void section_loaded_cb(lv_event_t *e) {
	(void)e;
	reload_home_page();
}

static void section_scan_cb(lv_event_t *e) {
	(void)e;
	start_scan(NULL);
}

static void build_section_page(gui_config_t *cfg) {
	lv_obj_add_style(audiobooks_screen, &theme_style_screen, 0);
	lv_obj_t *title = settingsrow_title(audiobooks_screen, cfg, "audiobook_home");
	settingsrow_title_corner_slots(title, cfg, 2);

	corner_button(audiobooks_screen, cfg, 0, &icon_music_settings, settings_click_cb);
	corner_button(audiobooks_screen, cfg, 1, &icon_search, search_click_cb);

	int content_top = settingsrow_content_top(cfg);

	// Empty state when 0 books exist in database
	section_empty = gridpage_empty_panel(audiobooks_screen, cfg, &icon_book_headphones, "audiobook_no_database", section_scan_cb);

	// Home scrollable container
	home_scroll = lv_obj_create(audiobooks_screen);
	lv_obj_set_size(home_scroll, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(home_scroll, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(home_scroll, 0, 0);
	lv_obj_set_style_border_width(home_scroll, 0, 0);
	lv_obj_set_style_pad_hor(home_scroll, cfg->padding, 0);
	lv_obj_set_style_pad_ver(home_scroll, 10, 0);
	lv_obj_set_style_pad_gap(home_scroll, 12, 0);
	lv_obj_set_style_pad_bottom(home_scroll, 30, 0);
	lv_obj_set_scroll_dir(home_scroll, LV_DIR_VER);
	lv_obj_set_flex_flow(home_scroll, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(home_scroll, true);

	player_sheet_attach_drag(home_scroll, true);
	switcher_attach_back_gesture(home_scroll);

	// Top Banner: "Ir a la Biblioteca >"
	lv_obj_t *banner = lv_btn_create(home_scroll);
	lv_obj_set_size(banner, lv_pct(100), 56);
	lv_obj_set_style_bg_color(banner, AUDIOBOOK_CARD_BG, 0);
	lv_obj_set_style_bg_color(banner, AUDIOBOOK_CARD_PRESSED, LV_STATE_PRESSED);
	lv_obj_set_style_border_color(banner, AUDIOBOOK_ACCENT, 0);
	lv_obj_set_style_border_width(banner, 2, 0);
	lv_obj_set_style_radius(banner, 14, 0);
	lv_obj_set_style_shadow_width(banner, 0, 0);
	lv_obj_set_style_pad_hor(banner, 16, 0);
	lv_obj_set_style_pad_gap(banner, 12, 0);
	lv_obj_set_scrollable(banner, false);
	lv_obj_set_flex_flow(banner, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(banner, open_library_banner_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *b_icon = lv_image_create(banner);
	lv_image_set_src(b_icon, &icon_library_books);
	lv_obj_add_style(b_icon, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(b_icon, LV_OPA_COVER, 0);
	lv_obj_set_style_image_recolor(b_icon, lv_color_white(), 0);

	lv_obj_t *b_lbl = lv_label_create(banner);
	lv_label_set_long_mode(b_lbl, LV_LABEL_LONG_DOT);
	lv_label_set_text(b_lbl, tr("audiobook_go_to_library"));
	lv_obj_set_style_text_color(b_lbl, lv_color_white(), 0);
	lv_obj_set_style_text_font(b_lbl, &font_ui_22, 0);
	lv_obj_set_flex_grow(b_lbl, 1);

	lv_obj_t *b_chev = lv_image_create(banner);
	lv_image_set_src(b_chev, &icon_chevron_right);
	lv_obj_add_style(b_chev, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(b_chev, LV_OPA_COVER, 0);
	lv_obj_set_style_image_recolor(b_chev, AUDIOBOOK_ACCENT, 0);

	// Section 1: "Continuar leyendo"
	lv_obj_t *sec1_lbl = lv_label_create(home_scroll);
	lv_label_set_text(sec1_lbl, tr("audiobook_continue_reading"));
	lv_obj_set_style_text_color(sec1_lbl, lv_color_white(), 0);
	lv_obj_set_style_text_font(sec1_lbl, &font_ui_24, 0);
	lv_obj_set_style_pad_top(sec1_lbl, 8, 0);

	home_hero_card = lv_btn_create(home_scroll);
	lv_obj_set_size(home_hero_card, lv_pct(100), 108);
	lv_obj_set_style_bg_color(home_hero_card, AUDIOBOOK_CARD_BG, 0);
	lv_obj_set_style_bg_color(home_hero_card, AUDIOBOOK_CARD_PRESSED, LV_STATE_PRESSED);
	lv_obj_set_style_radius(home_hero_card, 14, 0);
	lv_obj_set_style_border_width(home_hero_card, 0, 0);
	lv_obj_set_style_shadow_width(home_hero_card, 0, 0);
	lv_obj_set_style_pad_all(home_hero_card, 14, 0);
	lv_obj_set_style_pad_gap(home_hero_card, 14, 0);
	lv_obj_set_flex_flow(home_hero_card, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(home_hero_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_event_cb(home_hero_card, home_hero_card_cb, LV_EVENT_CLICKED, NULL);

	home_hero_img = lv_image_create(home_hero_card);
	lv_obj_set_size(home_hero_img, 76, 76);
	lv_obj_set_style_radius(home_hero_img, 8, 0);
	lv_obj_set_style_clip_corner(home_hero_img, true, 0);
	lv_image_set_src(home_hero_img, &icon_book_headphones_row);
	lv_obj_add_style(home_hero_img, &theme_style_icon, 0);

	lv_obj_t *hero_mid = lv_obj_create(home_hero_card);
	lv_obj_set_size(hero_mid, 0, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(hero_mid, 1);
	lv_obj_set_style_bg_opa(hero_mid, 0, 0);
	lv_obj_set_style_border_width(hero_mid, 0, 0);
	lv_obj_set_style_pad_all(hero_mid, 0, 0);
	lv_obj_set_style_pad_gap(hero_mid, 4, 0);
	lv_obj_set_scrollable(hero_mid, false);
	lv_obj_set_flex_flow(hero_mid, LV_FLEX_FLOW_COLUMN);

	home_hero_title = lv_label_create(hero_mid);
	lv_label_set_long_mode(home_hero_title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(home_hero_title, lv_pct(100));
	lv_obj_set_style_text_color(home_hero_title, lv_color_white(), 0);
	lv_obj_set_style_text_font(home_hero_title, &font_ui_22, 0);
	lv_obj_set_style_max_height(home_hero_title, lv_font_get_line_height(&font_ui_22), 0);

	home_hero_author = lv_label_create(hero_mid);
	lv_label_set_long_mode(home_hero_author, LV_LABEL_LONG_DOT);
	lv_obj_set_width(home_hero_author, lv_pct(100));
	lv_obj_set_style_text_color(home_hero_author, AUDIOBOOK_TEXT_DIM, 0);
	lv_obj_set_style_text_font(home_hero_author, &font_ui_18, 0);
	lv_obj_set_style_max_height(home_hero_author, lv_font_get_line_height(&font_ui_18), 0);

	lv_obj_t *p_row = lv_obj_create(hero_mid);
	lv_obj_set_size(p_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(p_row, 0, 0);
	lv_obj_set_style_border_width(p_row, 0, 0);
	lv_obj_set_style_pad_all(p_row, 0, 0);
	lv_obj_set_style_pad_gap(p_row, 10, 0);
	lv_obj_set_scrollable(p_row, false);
	lv_obj_set_flex_flow(p_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(p_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	home_hero_bar = lv_bar_create(p_row);
	lv_obj_set_size(home_hero_bar, 0, 6);
	lv_obj_set_flex_grow(home_hero_bar, 1);
	lv_bar_set_range(home_hero_bar, 0, 100);
	lv_obj_set_style_bg_color(home_hero_bar, AUDIOBOOK_BAR_TRACK, LV_PART_MAIN);
	lv_obj_set_style_bg_opa(home_hero_bar, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color(home_hero_bar, AUDIOBOOK_ACCENT, LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(home_hero_bar, LV_OPA_COVER, LV_PART_INDICATOR);
	lv_obj_set_style_radius(home_hero_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_radius(home_hero_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

	home_hero_pct = lv_label_create(p_row);
	lv_label_set_text(home_hero_pct, "0%");
	lv_obj_set_style_text_color(home_hero_pct, AUDIOBOOK_TEXT_DIM, 0);
	lv_obj_set_style_text_font(home_hero_pct, &font_ui_16, 0);

	home_hero_play_btn = lv_btn_create(home_hero_card);
	lv_obj_set_size(home_hero_play_btn, 44, 44);
	lv_obj_set_style_radius(home_hero_play_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(home_hero_play_btn, AUDIOBOOK_ACCENT, 0);
	lv_obj_set_style_shadow_width(home_hero_play_btn, 0, 0);
	lv_obj_set_style_border_width(home_hero_play_btn, 0, 0);
	lv_obj_set_style_pad_all(home_hero_play_btn, 0, 0);
	lv_obj_add_event_cb(home_hero_play_btn, home_hero_play_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *play_img = lv_image_create(home_hero_play_btn);
	lv_image_set_src(play_img, &icon_play_small);
	lv_obj_add_style(play_img, &theme_style_icon, 0);
	lv_obj_set_style_image_recolor_opa(play_img, LV_OPA_COVER, 0);
	lv_obj_set_style_image_recolor(play_img, lv_color_white(), 0);
	lv_obj_center(play_img);

	// Section 2: "En curso"
	lv_obj_t *sec2_lbl = lv_label_create(home_scroll);
	lv_label_set_text(sec2_lbl, tr("audiobook_in_progress"));
	lv_obj_set_style_text_color(sec2_lbl, lv_color_white(), 0);
	lv_obj_set_style_text_font(sec2_lbl, &font_ui_24, 0);
	lv_obj_set_style_pad_top(sec2_lbl, 8, 0);

	home_continue_container = lv_obj_create(home_scroll);
	lv_obj_set_size(home_continue_container, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(home_continue_container, 0, 0);
	lv_obj_set_style_border_width(home_continue_container, 0, 0);
	lv_obj_set_style_pad_all(home_continue_container, 0, 0);
	lv_obj_set_style_pad_gap(home_continue_container, 8, 0);
	lv_obj_set_scrollable(home_continue_container, false);
	lv_obj_set_flex_flow(home_continue_container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_event_bubble(home_continue_container, true);

	thumb_timer = lv_timer_create(thumb_timer_cb, THUMB_POLL_MS, NULL);
	lv_timer_pause(thumb_timer);

	player_sheet_attach_drag(audiobooks_screen, true);
	switcher_attach_back_gesture(audiobooks_screen);
	lv_obj_add_event_cb(audiobooks_screen, section_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// Authors and Series Page (names_screen)
// ---------------------------------------------------------------------------

#define NAME_ROW_HEIGHT 88
#define NAME_ROW_PITCH (NAME_ROW_HEIGHT + ROW_GAP)

typedef struct {
	lv_obj_t *button;
	lv_obj_t *label;
	lv_obj_t *count;
	int index;
} name_row_t;

typedef struct {
	char *name;
	int books;
} name_entry_t;

static lv_obj_t *names_title;
static lv_obj_t *names_sort_icon;
static lv_obj_t *names_list;
static lv_obj_t *names_body;
static lv_obj_t *names_empty;
static name_row_t name_rows[ROW_POOL];
static name_entry_t *names;
static int name_count;
static int name_capacity;
static list_id_t names_kind = LIST_AUTHORS;

static void names_clear(void) {
	for (int i = 0; i < name_count; i++) {
		free(names[i].name);
	}
	name_count = 0;
}

static bool names_add_cb(const char *name, int books, void *user) {
	(void)user;
	if (name_count == name_capacity) {
		int grown = name_capacity ? name_capacity * 2 : 64;
		name_entry_t *bigger = realloc(names, (size_t)grown * sizeof(*bigger));
		if (!bigger) {
			return false;
		}
		names = bigger;
		name_capacity = grown;
	}
	names[name_count].name = strdup(name);
	if (!names[name_count].name) {
		return false;
	}
	names[name_count].books = books;
	name_count++;
	return true;
}

static const char *shown_name(const char *name) { return name[0] ? name : tr("audiobook_unknown_author"); }

static void name_row_bind(name_row_t *row, int index) {
	if (row->index == index) {
		return;
	}
	row->index = index;
	if (index < 0 || index >= name_count) {
		lv_obj_set_hidden(row->button, true);
		return;
	}
	lv_obj_set_hidden(row->button, false);
	lv_obj_set_y(row->button, index * NAME_ROW_PITCH);
	lv_label_set_text(row->label, shown_name(names[index].name));
	lv_label_set_text_fmt(row->count, "%d", names[index].books);
}

static void names_window_update(void) {
	int scroll = lv_obj_get_scroll_y(names_list);
	if (scroll < 0) {
		scroll = 0;
	}
	int first = (scroll / NAME_ROW_PITCH) - 1;
	if (first + ROW_POOL > name_count) {
		first = name_count - ROW_POOL;
	}
	if (first < 0) {
		first = 0;
	}
	for (int i = 0; i < ROW_POOL; i++) {
		int index = first + i;
		name_row_bind(&name_rows[index % ROW_POOL], index < name_count ? index : -1);
	}
}

static void names_scroll_cb(lv_event_t *e) {
	(void)e;
	names_window_update();
}

static void reload_names(void) {
	for (int i = 0; i < ROW_POOL; i++) {
		name_row_bind(&name_rows[i], -1);
		name_rows[i].index = -1;
	}
	names_clear();

	sort_choice_t choice = list_sort(names_kind);
	bool by_added = choice == SORT_NEW || choice == SORT_OLD;
	bool desc = choice == SORT_ZA || choice == SORT_NEW;
	audiobookdb_names_for_each(names_kind == LIST_SERIES ? AUDIOBOOK_NAMES_SERIES : AUDIOBOOK_NAMES_AUTHORS, by_added, desc, names_add_cb, NULL);

	lv_obj_set_height(names_body, name_count > 0 ? name_count * NAME_ROW_PITCH : 1);
	lv_obj_scroll_to_y(names_list, 0, LV_ANIM_OFF);
	lv_label_set_text(names_empty, tr(names_kind == LIST_SERIES ? "audiobook_series_empty" : "audiobook_empty_note"));
	show_empty(names_empty, name_count);
	lv_image_set_src(names_sort_icon, sort_icon(choice));
	names_window_update();
}

static void name_clicked_cb(lv_event_t *e) {
	if (player_sheet_drag_active() || switcher_back_drag_active()) {
		return;
	}
	lv_obj_t *button = lv_event_get_current_target(e);
	for (int i = 0; i < ROW_POOL; i++) {
		if (name_rows[i].button != button) {
			continue;
		}
		int index = name_rows[i].index;
		if (index < 0 || index >= name_count) {
			return;
		}
		char value[256];
		snprintf(value, sizeof(value), "%s", names[index].name);
		books_open(names_kind == LIST_SERIES ? LIST_SERIES_BOOKS : LIST_AUTHOR_BOOKS, value, shown_name(value));
		return;
	}
}

static void names_sort_cb(lv_event_t *e) { sort_menu_show(lv_event_get_current_target(e), names_kind, reload_names); }

static void names_loaded_cb(lv_event_t *e) {
	(void)e;
	names_window_update();
}

static void build_names_page(gui_config_t *cfg) {
	lv_obj_add_style(names_screen, &theme_style_screen, 0);
	names_title = settingsrow_title(names_screen, cfg, "audiobook_authors");
	settingsrow_title_corner_slots(names_title, cfg, 1);
	lv_obj_t *sort_btn = corner_button(names_screen, cfg, 0, &icon_sort_az, names_sort_cb);
	names_sort_icon = lv_obj_get_child(sort_btn, 0);

	int width = cfg->screen_width - 2 * cfg->padding;
	int content_top = settingsrow_content_top(cfg);

	names_list = lv_obj_create(names_screen);
	lv_obj_set_size(names_list, lv_pct(100), cfg->screen_height - content_top);
	lv_obj_align(names_list, LV_ALIGN_TOP_LEFT, 0, content_top);
	lv_obj_set_style_bg_opa(names_list, 0, 0);
	lv_obj_set_style_border_width(names_list, 0, 0);
	lv_obj_set_style_pad_hor(names_list, cfg->padding, 0);
	lv_obj_set_style_pad_ver(names_list, 0, 0);
	lv_obj_set_scroll_dir(names_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(names_list, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_add_event_cb(names_list, names_scroll_cb, LV_EVENT_SCROLL, NULL);
	lv_obj_set_event_bubble(names_list, true);

	names_body = lv_obj_create(names_list);
	lv_obj_set_width(names_body, width);
	lv_obj_set_height(names_body, 1);
	lv_obj_set_style_bg_opa(names_body, 0, 0);
	lv_obj_set_style_border_width(names_body, 0, 0);
	lv_obj_set_style_pad_all(names_body, 0, 0);
	lv_obj_set_scrollable(names_body, false);
	lv_obj_set_event_bubble(names_body, true);

	names_empty = lv_label_create(names_list);
	lv_obj_set_width(names_empty, width);
	lv_label_set_long_mode(names_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(names_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_add_style(names_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(names_empty, &font_ui_24, 0);
	lv_obj_align(names_empty, LV_ALIGN_TOP_MID, 0, 90);
	lv_obj_set_hidden(names_empty, true);

	for (int i = 0; i < ROW_POOL; i++) {
		name_row_t *row = &name_rows[i];
		row->button = lv_btn_create(names_body);
		lv_obj_set_size(row->button, width, NAME_ROW_HEIGHT);
		lv_obj_set_style_bg_color(row->button, AUDIOBOOK_CARD_BG, 0);
		lv_obj_set_style_bg_color(row->button, AUDIOBOOK_CARD_PRESSED, LV_STATE_PRESSED);
		lv_obj_set_style_radius(row->button, ROW_RADIUS, 0);
		lv_obj_set_style_border_width(row->button, 0, 0);
		lv_obj_set_style_shadow_width(row->button, 0, 0);
		lv_obj_set_style_pad_hor(row->button, 20, 0);
		lv_obj_set_style_pad_gap(row->button, 12, 0);
		lv_obj_set_hidden(row->button, true);
		lv_obj_set_event_bubble(row->button, true);
		lv_obj_add_event_cb(row->button, name_clicked_cb, LV_EVENT_CLICKED, NULL);
		lv_obj_set_flex_flow(row->button, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row->button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		row->label = lv_label_create(row->button);
		lv_label_set_long_mode(row->label, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(row->label, 1);
		lv_obj_set_height(row->label, lv_font_get_line_height(&font_ui_24));
		lv_obj_add_style(row->label, &theme_style_text, 0);
		lv_obj_set_style_text_font(row->label, &font_ui_24, 0);

		row->count = lv_label_create(row->button);
		lv_obj_add_style(row->count, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(row->count, &font_ui_22, 0);

		row->index = -1;
	}

	player_sheet_attach_drag(names_screen, true);
	switcher_attach_back_gesture(names_screen);
	lv_obj_add_event_cb(names_screen, names_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// Settings Page & Controls Page
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
	if (expanded) {
		lv_obj_set_hidden(tp->pills, false);
	} else {
		lv_obj_set_hidden(tp->pills, true);
	}
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
	lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), on ? lv_color_white() : theme()->text_primary, 0);
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
	theme_register_refresh(refresh_settings_page);
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
	theme_register_refresh(refresh_control_buttons);
	lv_obj_add_event_cb(audiobookcontrols_screen, controls_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}

// ---------------------------------------------------------------------------
// Scan Page (audiobookscan_screen)
// ---------------------------------------------------------------------------

#define SCAN_POLL_MS 200

static lv_obj_t *scan_count_label;
static lv_obj_t *scan_status_label;
static lv_obj_t *scan_ok_button;
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
	lv_obj_set_style_text_color(label, lv_color_white(), 0);
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
	if (!names_screen) names_screen = lv_obj_create(NULL);

	build_section_page(cfg);
	build_books_page(cfg);
	build_detail_page(cfg);
	build_names_page(cfg);
	build_settings_page(cfg);
	build_controls_page(cfg);
	build_scan_page(cfg);
}
