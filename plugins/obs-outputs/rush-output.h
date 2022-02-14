#include <obs-module.h>
#include <util/threading.h>

struct rush_output {
	obs_output_t *output;

	FILE *file;
	int64_t rush_id;
	int64_t stop_ts;
	int64_t video_timescale;
	int64_t audio_timescale;
	bool sent_headers;
	volatile bool stopping;

	pthread_mutex_t mutex;
};
