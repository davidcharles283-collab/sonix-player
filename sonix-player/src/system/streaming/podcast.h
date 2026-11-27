#ifndef PODCAST_H
#define PODCAST_H

#include <stdbool.h>
#include <stddef.h>

// The catalogue behind the Podcasts section: Apple's iTunes Search API, which
// is free and needs no key, no account and no signature.
//
// A podcast is an RSS feed, yet the catalogue comes first, for search: without
// one, the only way to find a podcast is to know its URL by heart and type it
// on an on-screen keyboard. The catalogue answers "which podcasts match these
// words" and "what is in this country's chart", and for each podcast gives its
// feed address.
//
// From there on everything is the feed: a podcast's id is the one
// podcastrss.c makes from that address, and its episodes are read from the RSS
// (podcastrss.h). The Search API lists at most 200 episodes and the feed has
// them all; and with one kind of id, a podcast found by search and one typed
// as an address are the same thing to the cache, the downloads, the resume
// points and the follows.
//
// Ids from before this (Podcast Index numbers, below PODCASTRSS_ID_BASE) can no
// longer be looked up. podcastsubs.c moves the followed ones to their feed id
// at startup when it knows their address; the calls below answer "gone" for
// any other.
//
// Catalogue and feed requests block. Call them only from the page's worker
// thread, never from the UI thread.

#define PODCAST_TITLE_MAX 160
#define PODCAST_AUTHOR_MAX 120
#define PODCAST_URL_MAX 512
#define PODCAST_PAGE_LIMIT 60
// The feed's own address, the RSS. Shorter than the other addresses: it is
// kept for every followed podcast and in every list of podcasts, and a feed
// address is rarely a third of this.
#define PODCAST_FEED_URL_MAX 384

// The ids are 64-bit, and not out of generosity: on this device `long` is
// 32-bit, and the ids are 62-bit hashes of the feed address and the episode
// guid (podcastrss.h). With a 32-bit `long` strtol does not get such a number
// wrong, it saturates -- every episode collapses onto 2147483647, sharing one
// cache file and one sidecar row.

// A podcast: the feed, not an episode.
typedef struct {
	long long id; // podcastrss_feed_id(url), which is how it is found again
	char title[PODCAST_TITLE_MAX];
	char author[PODCAST_AUTHOR_MAX];
	char image[PODCAST_URL_MAX];
	char url[PODCAST_FEED_URL_MAX]; // the RSS feed; "" when not known
	int episode_count;				 // 0 when the catalogue does not say
} podcast_feed_t;

typedef struct {
	long long id;
	long long feed_id;
	char title[PODCAST_TITLE_MAX];
	char feed_title[PODCAST_TITLE_MAX];
	char image[PODCAST_URL_MAX];
	char enclosure[PODCAST_URL_MAX]; // the audio itself
	char mime[64];					 // the declared type, for the extension
	long published;					 // unix time, 0 when unknown
	int duration_secs;				 // 0 when unknown
} podcast_episode_t;

// Whether the catalogue can be used. Always true: it needs no keys. Kept so the
// pages keep one place to ask; the network they check themselves.
bool podcast_configured(void);

// Why the last call failed, already translated for display.
const char *podcast_last_error(void);

// How many entries the catalogue sent in the last response, before the
// unusable ones were discarded: this is the number that says whether the
// response filled the requested limit, and hence whether asking for more is
// worthwhile.
int podcast_last_raw_count(void);

// Searches podcasts by name. Returns how many it put in `out`, -1 on error.
// A query that is an address (http://, https://) finds that podcast: the
// catalogue's when it has it, otherwise the feed read directly
// (podcastrss.h).
int podcast_search(const char *query, podcast_feed_t *out, int max);

// The current chart.
int podcast_trending(podcast_feed_t *out, int max);

// A podcast's episodes, most recent first.
int podcast_episodes(long long feed_id, podcast_episode_t *out, int max);

// A single podcast, by id. Needed by "My podcasts", which stores ids rather
// than titles, so a podcast that is renamed stays the right one.
bool podcast_feed(long long feed_id, podcast_feed_t *out);

// A podcast found by the address of its feed, the way an OPML file names it.
// 1 when found, 0 when the catalogue does not have it, -1 when the catalogue
// could not be asked. The iTunes catalogue cannot be searched by address, so
// this answers 0 and the caller reads the feed (podcastrss_read_feed).
int podcast_feed_by_url(const char *url, podcast_feed_t *out);

// A single episode by its id alone. Episodes are only known through their
// feed, so this always fails: use podcast_episode_in_feed().
bool podcast_episode(long long episode_id, podcast_episode_t *out);

// Whether these saved IDs can be used for an RSS lookup. Does not fetch the
// feed or check whether it still carries the episode.
bool podcast_episode_has_lookup_ids(long long feed_id, long long episode_id);

// The same when the podcast is known too, which is what an episode read from a
// feed needs: there is no catalogue to ask by its id alone.
bool podcast_episode_in_feed(long long feed_id, long long episode_id, podcast_episode_t *out);

// For podcastrss.c, which reports its failures through podcast_last_error().
void podcast_set_last_error(const char *what);

// ---------------------------------------------------------------------------
// The two skip buttons, which on podcasts are their own and not the audiobook
// ones.
//
// The two cases look alike -- back because a sentence was missed, forward to
// jump an advert -- but are not listened to the same way: in an audiobook the
// back skip picks up the thread of a story, in a podcast the forward skip jumps
// a half-minute sponsor. Wanting -10/+30 on podcasts and -30/+10 on books
// should not be a choice: they are two settings, in two different sections of
// the configuration file.
#define PODCAST_SKIP_SHORT 10
#define PODCAST_SKIP_LONG 30
#define PODCAST_SKIP_HUGE 60

int podcast_skip_back(void);
int podcast_skip_forward(void);
void podcast_set_skip_back(int seconds);
void podcast_set_skip_forward(int seconds);

// Playback speed for episodes, in thousandths (1000 is normal), apart from the
// audiobook one in audiobook.h. Same range, [podcast] speed in the config.
int podcast_speed_permille(void);
void podcast_set_speed_permille(int permille);
double podcast_speed(void);

// Stop at the end of the episode: when this one finishes, the queue stays where
// it is instead of starting the next. The audiobook's chapter-end switch under
// another name, and for the same listener -- somebody following one thing at a
// time, who does not want the next hour starting on its own.
bool podcast_stop_at_episode_end(void);
void podcast_set_stop_at_episode_end(bool on);

#endif /* PODCAST_H */
