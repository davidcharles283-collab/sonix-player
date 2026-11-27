#include "podcast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "src/system/core/config.h"
#include "src/system/net/http.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/streaming/podcastrss.h"
#include "src/system/streaming/podcastsubs.h"

// The two endpoints, both Apple's and both free, with no key and no signature.
// Configurable so a fake server can stand in for them in a test.
//
//   search_base: the iTunes Search API, for search and lookup by id.
//   chart_base:  Apple's marketing RSS, for the per-country top chart.
#define ITUNES_SEARCH_DEFAULT "https://itunes.apple.com"
#define ITUNES_CHART_DEFAULT "https://rss.marketingtools.apple.com/api/v2"

// A search page of 200 podcasts with their artwork addresses stays far under
// a megabyte; this only cuts off something that is not an answer.
#define PODCAST_BODY_MAX (2 * 1024 * 1024)
#define PODCAST_TIMEOUT_SECS 15

// The most the Search API returns for one query: it has no offset, so a list
// scrolled past this has simply reached the end.
#define ITUNES_SEARCH_MAX 200
// The most the chart lists.
#define ITUNES_CHART_MAX 100

static __thread char last_error[256];

const char *podcast_last_error(void) { return last_error; }

static void set_error(const char *what) { snprintf(last_error, sizeof(last_error), "%s", what ? what : ""); }

void podcast_set_last_error(const char *what) { set_error(what); }

// The catalogue needs no keys, so it is always there; only the network can be
// missing, and the pages check that themselves.
bool podcast_configured(void) { return true; }

static const char *search_base(void) { return config_get("podcast", "search_base", ITUNES_SEARCH_DEFAULT); }
static const char *chart_base(void) { return config_get("podcast", "chart_base", ITUNES_CHART_DEFAULT); }

// The store country, which decides the chart and ranks the search results.
// Taken from the interface language, so a player in Italian gets the Italian
// chart; [podcast] country (a two-letter code) overrides it for, say, a
// Spanish speaker in Mexico.
//
// The table lives here rather than in lang.c because this is its only caller:
// lang.c works in language names, which is what a menu needs.
static const char *store_country(void) {
	const char *forced = config_get("podcast", "country", "");
	if (forced && strlen(forced) == 2) {
		return forced;
	}
	static const struct {
		const char *name;
		const char *country;
	} MAP[] = {
		{"Italiano", "it"},
		{"English", "us"},
		{"Deutsch", "de"},
		{"Fran\xC3\xA7\x61is", "fr"},
		{"Espa\xC3\xB1ol", "es"},
		{"Japanese", "jp"},
		{"Nederlands", "nl"},
		{"Polski", "pl"},
		{"Russian", "ru"},
		{"Chinese", "cn"},
		{"Portugu\xC3\xAAs BR", "br"},
		{"\xC4\x8C"
		 "e\xC5\xA1tina",
		 "cz"},
	};
	const char *current = lang_current();
	for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
		if (current && strcmp(current, MAP[i].name) == 0) {
			return MAP[i].country;
		}
	}
	return "us";
}

// ---------------------------------------------------------------------------
// the request
// ---------------------------------------------------------------------------

// Fetches `url` and returns the body for the caller to free. NULL on error,
// with last_error already written.
static char *api_get(const char *url) {
	char *body = NULL;
	size_t len = 0;
	int status = 0;
	if (!http_get_ex(url, "User-Agent: " HTTP_USER_AGENT "\r\n", true, &body, &len, PODCAST_BODY_MAX,
					 PODCAST_TIMEOUT_SECS, &status)) {
		if (status >= 500) {
			set_error(tr("podcast_the_podcast_directory_is_not_answering"));
		} else if (status > 0) {
			snprintf(last_error, sizeof(last_error), tr("podcast_directory_status"), status);
		} else {
			const char *why = http_last_error();
			set_error(why && why[0] ? why : tr("podcast_i_cannot_reach_the_podcast_directory"));
		}
		free(body);
		return NULL;
	}
	// want_error_body is true, so a 4xx arrives here with its body.
	if (status >= 400) {
		snprintf(last_error, sizeof(last_error), tr("podcast_directory_status"), status);
		free(body);
		return NULL;
	}
	last_error[0] = '\0';
	return body;
}

// ---------------------------------------------------------------------------
// from iTunes JSON to podcast_feed_t
// ---------------------------------------------------------------------------

// One podcast of a Search API answer. False when it is no use: a podcast
// without a feed address (Apple lists some that are Apple-only) cannot be
// played here.
//
// The id is NOT the iTunes collectionId. It is the one podcastrss.c makes
// from the feed address, so every podcast found in the catalogue is from then
// on exactly a podcast read from its feed: its episodes come from the RSS
// (complete, where the Search API stops at 200), and the cache, the downloads,
// the resume points and the follows all work by the same id and address.
static bool read_feed(const json_doc_t *doc, int obj, podcast_feed_t *out) {
	memset(out, 0, sizeof(*out));
	if (!json_obj_str(doc, obj, "feedUrl", out->url, sizeof(out->url)) || !out->url[0]) {
		return false;
	}
	json_obj_str(doc, obj, "collectionName", out->title, sizeof(out->title));
	if (!out->title[0]) {
		return false;
	}
	json_obj_str(doc, obj, "artistName", out->author, sizeof(out->author));
	// 600 px is the largest square Apple gives here; the list thumbnails and the
	// cover both come from it.
	if (!json_obj_str(doc, obj, "artworkUrl600", out->image, sizeof(out->image)) || !out->image[0]) {
		json_obj_str(doc, obj, "artworkUrl100", out->image, sizeof(out->image));
	}
	out->episode_count = (int)json_obj_long(doc, obj, "trackCount", 0);
	out->id = podcastrss_feed_id(out->url);
	// So opening it finds the feed without asking anyone again.
	podcastrss_remember(out->id, out->url);
	return true;
}

static __thread int last_raw_count;

int podcast_last_raw_count(void) { return last_raw_count; }

// Fetches a Search API answer ({"resultCount":N, "results":[...]}) and fills
// `out` in the order Apple gives. last_raw_count is how many Apple sent, before
// the podcasts without a feed were dropped: that says whether the answer filled
// the limit asked for.
static int fetch_feeds(const char *url, podcast_feed_t *out, int max) {
	last_raw_count = 0;
	char *body = api_get(url);
	if (!body) {
		return -1;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return -1;
	}
	int arr = json_get(&doc, json_root(&doc), "results");
	int len = json_len(&doc, arr);
	last_raw_count = len;
	int count = 0;
	for (int i = 0; i < len && count < max; i++) {
		int item = json_at(&doc, arr, i);
		if (item < 0 || !read_feed(&doc, item, &out[count])) {
			continue;
		}
		// The same feed listed twice (a show and its rerun channel can share
		// one) would be two rows doing the same thing.
		bool dup = false;
		for (int k = 0; k < count && !dup; k++) {
			dup = out[k].id == out[count].id;
		}
		if (!dup) {
			count++;
		}
	}
	json_free(&doc);
	free(body);
	return count;
}

// ---------------------------------------------------------------------------
// the calls
// ---------------------------------------------------------------------------

// Whether a query is the address of a feed rather than words to search for.
static bool looks_like_address(const char *query) {
	return strncasecmp(query, "http://", 7) == 0 || strncasecmp(query, "https://", 8) == 0;
}

int podcast_search(const char *query, podcast_feed_t *out, int max) {
	if (!query || !query[0] || !out || max <= 0) {
		return -1;
	}
	// An address is read as the feed it is.
	if (looks_like_address(query)) {
		last_raw_count = 1;
		if (!podcastrss_read_feed(query, out)) {
			return -1;
		}
		podcastsubs_keep_address(out->id, query);
		return 1;
	}
	char encoded[512];
	http_url_encode(query, encoded, sizeof(encoded));

	// No explicit filter: hiding explicit podcasts is not the player's choice
	// to make for the listener.
	char url[800];
	snprintf(url, sizeof(url), "%s/search?media=podcast&entity=podcast&term=%s&limit=%d&country=%s",
			 search_base(), encoded, max > ITUNES_SEARCH_MAX ? ITUNES_SEARCH_MAX : max, store_country());
	return fetch_feeds(url, out, max);
}

// The chart of `country`: Apple's list gives only ids and names, so a second
// call looks them all up at once for their feed addresses. Results keep the
// chart's order.
static int chart(const char *country, podcast_feed_t *out, int max) {
	last_raw_count = 0;
	int want = max > ITUNES_CHART_MAX ? ITUNES_CHART_MAX : max;

	char url[256];
	snprintf(url, sizeof(url), "%s/%s/podcasts/top/%d/podcasts.json", chart_base(), country, want);
	char *body = api_get(url);
	if (!body) {
		return -1;
	}
	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return -1;
	}
	int arr = json_path(&doc, json_root(&doc), "feed.results");
	int len = json_len(&doc, arr);

	// The ids, in chart order, joined for one lookup. An iTunes id is at most
	// ten digits; 100 of them and their commas fit easily.
	long long ids[ITUNES_CHART_MAX];
	int n_ids = 0;
	char joined[ITUNES_CHART_MAX * 12 + 1] = "";
	size_t used = 0;
	for (int i = 0; i < len && n_ids < want; i++) {
		char id_text[24] = "";
		int item = json_at(&doc, arr, i);
		if (item < 0 || !json_obj_str(&doc, item, "id", id_text, sizeof(id_text))) {
			continue;
		}
		long long id = strtoll(id_text, NULL, 10);
		if (id <= 0) {
			continue;
		}
		ids[n_ids++] = id;
		used += (size_t)snprintf(joined + used, sizeof(joined) - used, "%s%lld", used ? "," : "", id);
	}
	json_free(&doc);
	free(body);
	if (n_ids == 0) {
		return 0;
	}

	char lookup[sizeof(joined) + 256];
	snprintf(lookup, sizeof(lookup), "%s/lookup?entity=podcast&country=%s&id=%s", search_base(), country, joined);
	body = api_get(lookup);
	if (!body) {
		return -1;
	}
	if (!json_parse(body, &doc)) {
		set_error(tr("podcast_directory_unparsable"));
		free(body);
		return -1;
	}
	// The lookup answers in its own order; put it back in the chart's.
	arr = json_get(&doc, json_root(&doc), "results");
	len = json_len(&doc, arr);
	int count = 0;
	for (int c = 0; c < n_ids && count < max; c++) {
		for (int i = 0; i < len; i++) {
			int item = json_at(&doc, arr, i);
			if (item < 0 || json_obj_llong(&doc, item, "collectionId", 0) != ids[c]) {
				continue;
			}
			if (read_feed(&doc, item, &out[count])) {
				count++;
			}
			break;
		}
	}
	json_free(&doc);
	free(body);
	// The chart's length, not the lookup's: this is the list the page scrolls.
	last_raw_count = n_ids;
	return count;
}

int podcast_trending(podcast_feed_t *out, int max) {
	if (!out || max <= 0) {
		return -1;
	}
	// A country without a chart of its own (Apple has none for a few) falls
	// back to the US one: a foreign chart beats an empty page.
	int n = chart(store_country(), out, max);
	if (n <= 0 && strcmp(store_country(), "us") != 0) {
		n = chart("us", out, max);
	}
	return n;
}

// A podcast read from its feed is found by its address, which the reader
// holds only for the feeds met since startup: the others are asked of the
// database it was kept in.
static void rss_find(long long feed_id) {
	char url[PODCAST_FEED_URL_MAX];
	if (!podcastrss_url_for(feed_id, url, sizeof(url)) && podcastsubs_address(feed_id, url, sizeof(url))) {
		podcastrss_remember(feed_id, url);
	}
}

// Every id the section hands out now is a feed id: an id from elsewhere is
// one Podcast Index gave, which nothing here can look up any more.
static bool legacy_id(long long id) {
	if (podcastrss_is_id(id)) {
		return false;
	}
	set_error(tr("podcast_gone"));
	return true;
}

int podcast_episodes(long long feed_id, podcast_episode_t *out, int max) {
	if (feed_id <= 0 || !out || max <= 0 || legacy_id(feed_id)) {
		return -1;
	}
	rss_find(feed_id);
	// A podcast opened from the search but not followed is known only for this
	// run. Its episodes can end up in the cache or the downloads, and after a
	// restart those still have to find their feed: keep the address.
	char url[PODCAST_FEED_URL_MAX];
	char kept[PODCAST_FEED_URL_MAX];
	if (podcastrss_url_for(feed_id, url, sizeof(url)) && !podcastsubs_address(feed_id, kept, sizeof(kept))) {
		podcastsubs_keep_address(feed_id, url);
	}
	return podcastrss_episodes(feed_id, out, max, &last_raw_count);
}

bool podcast_feed(long long feed_id, podcast_feed_t *out) {
	if (feed_id <= 0 || !out || legacy_id(feed_id)) {
		return false;
	}
	rss_find(feed_id);
	return podcastrss_feed(feed_id, out);
}

// The Search API cannot be asked by feed address, so the answer is always "not
// in the catalogue", without a request. The caller (the OPML import) then reads
// the feed itself, which gives the same podcast with the same id a search
// would.
int podcast_feed_by_url(const char *url, podcast_feed_t *out) {
	if (!url || !url[0] || !out) {
		return -1;
	}
	return 0;
}

// Episodes are only known through their feed (podcast_episode_in_feed).
bool podcast_episode(long long episode_id, podcast_episode_t *out) {
	(void)episode_id;
	(void)out;
	set_error(tr("podcast_episode_gone"));
	return false;
}

bool podcast_episode_has_lookup_ids(long long feed_id, long long episode_id) {
	return podcastrss_is_id(feed_id) && podcastrss_is_id(episode_id);
}

bool podcast_episode_in_feed(long long feed_id, long long episode_id, podcast_episode_t *out) {
	if (!out) {
		return false;
	}
	if (!podcast_episode_has_lookup_ids(feed_id, episode_id)) {
		set_error(podcastrss_is_id(feed_id) ? tr("podcast_episode_gone") : tr("podcast_gone"));
		return false;
	}
	rss_find(feed_id);
	return podcastrss_episode(feed_id, episode_id, out);
}

// ---------------------------------------------------------------------------
// the two skip intervals
// ---------------------------------------------------------------------------

// Only the three values the buttons can draw: a hand-edited config file must
// not be able to give the player a seven-hundred-second skip behind an icon
// that says ten.
static int clamp_skip(int value) {
	if (value == PODCAST_SKIP_LONG) {
		return PODCAST_SKIP_LONG;
	}
	if (value == PODCAST_SKIP_HUGE) {
		return PODCAST_SKIP_HUGE;
	}
	return PODCAST_SKIP_SHORT;
}

// The defaults are not the audiobook ones (-10/+10): on a podcast the forward
// skip is almost always used to jump a sponsor read, and thirty seconds is that
// length.
int podcast_skip_back(void) { return clamp_skip(config_get_int("podcast", "skip_back", PODCAST_SKIP_SHORT)); }

int podcast_skip_forward(void) { return clamp_skip(config_get_int("podcast", "skip_forward", PODCAST_SKIP_LONG)); }

void podcast_set_skip_back(int seconds) { config_set_int("podcast", "skip_back", clamp_skip(seconds)); }

void podcast_set_skip_forward(int seconds) { config_set_int("podcast", "skip_forward", clamp_skip(seconds)); }

int podcast_speed_permille(void) {
	long value = config_get_int("podcast", "speed", 1000);
	if (value < 250 || value > 4000) {
		value = 1000;
	}
	return (int)value;
}

void podcast_set_speed_permille(int permille) {
	if (permille < 250 || permille > 4000) {
		permille = 1000;
	}
	config_set_int("podcast", "speed", permille);
}

double podcast_speed(void) { return (double)podcast_speed_permille() / 1000.0; }

bool podcast_stop_at_episode_end(void) { return config_get_int("podcast", "stop_episode_end", 0) != 0; }

void podcast_set_stop_at_episode_end(bool on) { config_set_int("podcast", "stop_episode_end", on ? 1 : 0); }
