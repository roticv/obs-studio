#include "rush-output.h"

#include <obs-avc.h>
#include <util/array-serializer.h>
#include <util/platform.h>

static const char *rush_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("RUSHOutput");
}

static void *rush_output_create(obs_data_t *settings, obs_output_t *output)
{
	struct rush_output *stream = bzalloc(sizeof(struct rush_output));
	stream->output = output;

	UNUSED_PARAMETER(settings);
	return stream;
}

static void rush_output_destroy(void *data)
{
	struct rush_output *stream = data;

	pthread_mutex_destroy(&stream->mutex);
	bfree(stream);
}

static bool rush_output_start(void *data)
{
	struct rush_output *stream = data;
	stream->file = os_fopen("/tmp/test.fbvp", "wb");
	stream->rush_id = 1;
	stream->sent_headers = false;
	os_atomic_set_bool(&stream->stopping, false);
	pthread_mutex_init(&stream->mutex, NULL);

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;
	/* start capture */
	obs_output_begin_data_capture(stream->output, 0);

	return true;
}

static inline bool stopping(struct rush_output *stream)
{
	return os_atomic_load_bool(&stream->stopping);
}

static void rush_output_stop(void *data, uint64_t ts)
{
	struct rush_output *stream = data;

	/* copied from flv_output so it got to be correct */
	stream->stop_ts = ts / 1000;
	os_atomic_set_bool(&stream->stopping, true);
}

static void rush_output_actual_stop(struct rush_output *stream, int code)
{
	if (code) {
		obs_output_signal_stop(stream->output, code);
	} else {
		obs_output_end_data_capture(stream->output);
	}

	blog(LOG_INFO, "RUSH file output complete");
}

static void write_headers(struct rush_output *stream)
{
	/* Connect payload */
	struct array_output_data output;
	struct serializer s;

	array_output_serializer_init(&s, &output);
	/*
	 * 8byte len, 8byte id, 1byte frame type, 1byte version, 2 byte video
	 * timescale, 2byte audio timescale, 8byte broadcast id, and 0byte payload
	 */
	int64_t len = 8 + 8 + 1 + 1 + 2 + 2 + 8 + 2;
	s_wl64(&s, len);
	s_wl64(&s, stream->rush_id);
	++stream->rush_id;
	/* connect frame = 0x0 */
	s_w8(&s, 0);
	/* version default value is 0 */
	s_w8(&s, 0);

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		/* Well, lets use some default value (which might not make sense) */
		stream->video_timescale = 1000;
	} else {
		/* timebase_den is based on fps_num */
		stream->video_timescale = ovi.fps_num;
	}

	/* need to deal with multiple audio stream?? */
	struct obs_audio_info oai;
	if (!obs_get_audio_info(&oai)) {
		/* Once again, we use some default. I like 44.1khz */
		stream->audio_timescale = 44100;
	} else {
		/* This is how audio_output_get_sample_rate is defined */
		stream->audio_timescale = oai.samples_per_sec;
	}

	s_wl16(&s, stream->video_timescale);
	s_wl16(&s, stream->audio_timescale);

	/* broadcast id 0 */
	s_wl64(&s, 0);
	s_write(&s, "{}", 2);

	/* write to file */
	fwrite(output.bytes.array, 1, output.bytes.num, stream->file);

	array_output_serializer_free(&output);
}

/* May overflow, but meh */
static int64_t convert_ts(int64_t ts, int64_t timebase_num,
				int64_t timebase_den, int64_t timescale)
{
	return ts * timescale * timebase_num / timebase_den;
}

static void write_aac_packet(struct rush_output *stream,
				struct encoder_packet *packet)
{
	struct array_output_data output;
	struct serializer s;

	array_output_serializer_init(&s, &output);

	obs_output_t *context = stream->output;
	obs_encoder_t *aencoder = obs_output_get_audio_encoder(context, 0);
	struct encoder_packet header_packet = {.type = OBS_ENCODER_AUDIO,
					.timebase_den = 1};
	obs_encoder_get_extra_data(
		aencoder, &header_packet.data, &header_packet.size);

	/*
	 * 8byte len, 8byte id, 1byte frame type, 1byte codec, 8byte timestamp
	 * 1byte track id, 2byte header len, header len, payload len
	 */
	int64_t len =
		8 + 8 + 1 + 1 + 8 + 1 + 2 + header_packet.size + packet->size;
	s_wl64(&s, len);
	s_wl64(&s, stream->rush_id);
	++stream->rush_id;
	/* audio data with header frame = 0x14 */
	s_w8(&s, 0x14);
	/* aac is "codec 1" */
	s_w8(&s, 1);
	s_wl64(&s, convert_ts(packet->dts, packet->timebase_num,
			packet->timebase_den, stream->audio_timescale));
	/* Hardcode track id = 1 for audio */
	s_w8(&s, 1);
	s_wl16(&s, header_packet.size);
	s_write(&s, header_packet.data, header_packet.size);
	s_write(&s, packet->data, packet->size);

	/* write to file */
	fwrite(output.bytes.array, 1, output.bytes.num, stream->file);

	array_output_serializer_free(&output);
}

/*
 * Logic to add sps/pps to prepend of each keyframe isn't implemented
 */
static void write_h264_packet(struct rush_output *stream,
				struct encoder_packet *packet)
{
	/* we need to convert from annexb to avcc: 
	 * avcc is stored in parsed_packet */
	struct encoder_packet parsed_packet;

	obs_parse_avc_packet(&parsed_packet, packet);

	struct array_output_data output;
	struct serializer s;

	array_output_serializer_init(&s, &output);
	/*
	 * 8byte len, 8byte id, 1byte frame type, 1byte codec, 8byte pts, 8byte dts,
	 * 1byte track id, 2byte required frame offset, payload bytes
	 */
	int64_t len = 8 + 8 + 1 + 1 + 8 + 8 + 1 + 2 + parsed_packet.size;
	s_wl64(&s, len);
	s_wl64(&s, stream->rush_id);
	++stream->rush_id;
	/* video with track frame = 0xd */
	s_w8(&s, 0xd);
	/* h264 is 0x1 */
	s_w8(&s, 0x1);
	s_wl64(&s, convert_ts(parsed_packet.pts, parsed_packet.timebase_num,
		parsed_packet.timebase_den, stream->video_timescale));
	s_wl64(&s, convert_ts(parsed_packet.dts, parsed_packet.timebase_num,
		parsed_packet.timebase_den, stream->video_timescale));
	/* Hardcode track id = 0 for video */
	s_w8(&s, 0);
	/* Set required frame offset to 0 for now; to be fixed in the future */
	s_wl16(&s, 0);
	s_write(&s, parsed_packet.data, parsed_packet.size);

	/* write to file */
	fwrite(output.bytes.array, 1, output.bytes.num, stream->file);

	array_output_serializer_free(&output);

	obs_encoder_packet_release(&parsed_packet);
}


static void rush_output_data(void *data, struct encoder_packet *packet)
{
	struct rush_output *stream = data;

	pthread_mutex_lock(&stream->mutex);

	if (stopping(stream)) {
		if (packet->sys_dts_usec >= (int64_t)stream->stop_ts) {
			rush_output_actual_stop(stream, 0);
			goto unlock;
		}
	}

	if (!stream->sent_headers) {
		write_headers(stream);
		stream->sent_headers = true;
	}

	if (packet->type == OBS_ENCODER_VIDEO) {
		write_h264_packet(stream, packet);
	} else {
		write_aac_packet(stream, packet);
	}

unlock:
	pthread_mutex_unlock(&stream->mutex);
}

struct obs_output_info rush_output_info = {
	.id = "rush_output",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE |
		 OBS_OUTPUT_MULTI_TRACK,
	.encoded_video_codecs = "h264",
	.encoded_audio_codecs = "aac",
	.get_name = rush_output_getname,
	.create = rush_output_create,
	.destroy = rush_output_destroy,
	.start = rush_output_start,
	.stop = rush_output_stop,
	.encoded_packet = rush_output_data,
};
