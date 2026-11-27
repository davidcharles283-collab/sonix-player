#include <assert.h>
#include <stdio.h>

#include "src/system/db/sqlite3.h"
#include "src/system/device/system.h"
#include "src/system/streaming/podcast.h"
#include "src/system/streaming/podcaststate.h"

static const char *card_root;
const char *storage_sd_root(void) { return card_root; }

static bool has_lookup_ids(const podcaststate_t *state, void *user) {
	(void)user;
	return podcast_episode_has_lookup_ids(state->feed_id, state->episode_id);
}

static void skips_unavailable_history(const char *card) {
	podcaststate_t episode;
	podcaststate_set_root(card);
	assert(podcaststate_in_progress(&episode, 1, has_lookup_ids, NULL) == 0);

	char path[512];
	snprintf(path, sizeof(path), "%s/.local/podcast.db", card);
	sqlite3 *db = NULL;
	assert(sqlite3_open(path, &db) == SQLITE_OK);
	assert(sqlite3_exec(db,
			"INSERT INTO PODCAST_EPISODES(key,feed_id,episode_id,position,touched)"
			" VALUES ('rss',4611686018427387905,4611686018427387906,40,1);"
			"WITH RECURSIVE recent(i) AS (VALUES(2) UNION ALL SELECT i+1 FROM recent WHERE i<10)"
			" INSERT INTO PODCAST_EPISODES(key,feed_id,episode_id,position,touched)"
			" SELECT 'legacy:'||i,1234,i,40,i FROM recent;", NULL, NULL, NULL) == SQLITE_OK);
	assert(sqlite3_close(db) == SQLITE_OK);

	assert(podcaststate_in_progress(&episode, 1, has_lookup_ids, NULL) == 1);
	assert(episode.episode_id == 0x4000000000000002LL);
	assert(podcaststate_in_progress(&episode, 1, NULL, NULL) == 1);
	assert(episode.episode_id == 10);
	podcaststate_set_root(NULL);
}

int main(int argc, char **argv) {
	assert(argc == 2);
	card_root = argv[1];
	const long long legacy_feed = 1234;
	const long long legacy_episode = 12345678901LL;
	const long long rss_feed = 0x4000000000000001LL;
	const long long rss_episode = 0x4000000000000002LL;

	assert(podcast_episode_has_lookup_ids(rss_feed, rss_episode));
	assert(!podcast_episode_has_lookup_ids(legacy_feed, legacy_episode));
	assert(!podcast_episode_has_lookup_ids(legacy_feed, rss_episode));
	assert(!podcast_episode_has_lookup_ids(rss_feed, legacy_episode));
	skips_unavailable_history(argv[1]);
	return 0;
}
