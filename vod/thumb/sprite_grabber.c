#include "sprite_grabber.h"
#include "../media_set.h"
#include "../codec_config.h"

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>

#if (VOD_HAVE_LIB_SW_SCALE)
#include <libswscale/swscale.h>
#endif // VOD_HAVE_LIB_SW_SCALE

// state machine states
enum {
	SPRITE_STATE_READ_FRAME,
	SPRITE_STATE_DECODE,
	SPRITE_STATE_RESIZE_PLACE,
	SPRITE_STATE_ENCODE,
};

// typedefs
typedef struct
{
	// fixed
	request_context_t* request_context;
	write_callback_t write_callback;
	void* write_context;

	// grid settings
	uint32_t cols;
	uint32_t rows;
	uint32_t tile_width;
	uint32_t tile_height;
	uint32_t canvas_width;
	uint32_t canvas_height;
	uint32_t encode_height;
	uint32_t interval_ms;
	uint32_t page;
	uint32_t total_tiles;

	// canvas (YUV420P)
	uint8_t* canvas_data[4];
	int canvas_linesize[4];

	// libavcodec
	AVCodecContext *decoder;
	AVCodecContext *encoder;
	AVFrame *decoded_frame;
	AVFrame *working_frame;
	AVPacket *output_packet;

	// track info
	media_track_t* track;
	uint64_t video_duration_ms;

	// state machine
	int cur_state;
	uint32_t cur_tile;

	// frame reading (like thumb_grabber)
	frames_source_t* frames_source;
	void* frames_source_context;
	input_frame_t* cur_keyframe;
	uint64_t cur_keyframe_dts;
	input_frame_t* last_decoded_keyframe;
	uint64_t last_keyframe_dts;
	u_char* frame_buffer;
	uint32_t frame_buffer_size;
	uint32_t max_frame_size;

} sprite_grabber_state_t;

// codec mapping
typedef struct {
	uint32_t codec_id;
	enum AVCodecID av_codec_id;
	const char* name;
} sprite_codec_id_mapping_t;

// globals
static const AVCodec *sprite_decoder_codec[VOD_CODEC_ID_COUNT];
static const AVCodec *sprite_encoder_codec = NULL;

static sprite_codec_id_mapping_t sprite_codec_mappings[] = {
	{ VOD_CODEC_ID_AVC, AV_CODEC_ID_H264, "h264" },
	{ VOD_CODEC_ID_HEVC, AV_CODEC_ID_H265, "h265" },
	{ VOD_CODEC_ID_VP8, AV_CODEC_ID_VP8, "vp8" },
	{ VOD_CODEC_ID_VP9, AV_CODEC_ID_VP9, "vp9" },
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 89, 100)
	{ VOD_CODEC_ID_AV1, AV_CODEC_ID_AV1, "av1" },
#endif
};

void
sprite_grabber_process_init(vod_log_t* log)
{
	const AVCodec *cur_decoder;
	sprite_codec_id_mapping_t* mapping_cur;
	sprite_codec_id_mapping_t* mapping_end;

	vod_memzero(sprite_decoder_codec, sizeof(sprite_decoder_codec));

	sprite_encoder_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	if (sprite_encoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"sprite_grabber_process_init: failed to get jpeg encoder, sprite capture is disabled");
		return;
	}

	mapping_end = sprite_codec_mappings + vod_array_entries(sprite_codec_mappings);
	for (mapping_cur = sprite_codec_mappings; mapping_cur < mapping_end; mapping_cur++)
	{
		cur_decoder = avcodec_find_decoder(mapping_cur->av_codec_id);
		if (cur_decoder == NULL)
		{
			vod_log_error(VOD_LOG_WARN, log, 0,
				"sprite_grabber_process_init: failed to get %s decoder", mapping_cur->name);
			continue;
		}
		sprite_decoder_codec[mapping_cur->codec_id] = cur_decoder;
	}
}

static void
sprite_grabber_free_state(void* context)
{
	sprite_grabber_state_t* state = (sprite_grabber_state_t*)context;

	av_packet_free(&state->output_packet);
	av_frame_free(&state->working_frame);
	av_frame_free(&state->decoded_frame);
	if (state->encoder != NULL)
	{
		avcodec_close(state->encoder);
		av_free(state->encoder);
	}
	if (state->decoder != NULL)
	{
		avcodec_close(state->decoder);
		av_free(state->decoder);
	}
	if (state->canvas_data[0] != NULL)
	{
		av_freep(&state->canvas_data[0]);
	}
}

static vod_status_t
sprite_grabber_init_decoder(
	request_context_t* request_context,
	media_info_t* media_info,
	AVCodecContext **result)
{
	AVCodecContext *dec;
	int avrc;

	dec = avcodec_alloc_context3(sprite_decoder_codec[media_info->codec_id]);
	if (dec == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"sprite_grabber_init_decoder: avcodec_alloc_context3 failed");
		return VOD_ALLOC_FAILED;
	}

	*result = dec;

	dec->codec_tag = media_info->format;
	dec->time_base.num = 1;
	dec->time_base.den = media_info->frames_timescale;
	dec->pkt_timebase = dec->time_base;
	if (media_info->extra_data.len > 0)
	{
		dec->extradata = vod_alloc(
			request_context->pool,
			media_info->extra_data.len + AV_INPUT_BUFFER_PADDING_SIZE);
		if (dec->extradata == NULL)
		{
			vod_log_error(VOD_LOG_ERR, request_context->log, 0,
				"sprite_grabber_init_decoder: vod_alloc failed");
			return VOD_ALLOC_FAILED;
		}
		vod_memcpy(dec->extradata, media_info->extra_data.data, media_info->extra_data.len);
		dec->extradata_size = media_info->extra_data.len;
	}
	dec->width = media_info->u.video.width;
	dec->height = media_info->u.video.height;
	dec->thread_count = 1;

	avrc = avcodec_open2(dec, sprite_decoder_codec[media_info->codec_id], NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"sprite_grabber_init_decoder: avcodec_open2 failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	return VOD_OK;
}

static vod_status_t
sprite_grabber_find_keyframe_at(
	media_track_t* track,
	uint64_t target_time_ms,
	uint32_t timescale,
	frame_list_part_t** out_part,
	input_frame_t** out_frame,
	uint32_t* out_frame_size,
	uint64_t* out_keyframe_dts)
{
	frame_list_part_t* part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	input_frame_t* best_frame = NULL;
	frame_list_part_t* best_part = NULL;
	uint64_t best_dts = 0;
	uint64_t dts = 0;
	uint64_t target_ts;

	target_ts = (target_time_ms * timescale) / 1000;

	part = &track->frames;
	last_frame = part->last_frame;

	if (part->first_frame == NULL || part->first_frame >= last_frame)
	{
		return VOD_NOT_FOUND;
	}

	for (cur_frame = part->first_frame; ; cur_frame++)
	{
		if (cur_frame >= last_frame)
		{
			if (part->next == NULL)
			{
				break;
			}
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
			if (part->first_frame == NULL || part->first_frame >= last_frame)
			{
				break;
			}
		}

		if (cur_frame->key_frame)
		{
			if (dts > target_ts && best_frame != NULL)
			{
				break;  // we've gone past, use previous keyframe
			}
			best_frame = cur_frame;
			best_part = part;
			best_dts = dts;
		}

		dts += cur_frame->duration;
	}

	if (best_frame == NULL)
	{
		return VOD_NOT_FOUND;
	}

	*out_part = best_part;
	*out_frame = best_frame;
	if (out_frame_size != NULL)
	{
		*out_frame_size = best_frame->size;
	}
	if (out_keyframe_dts != NULL)
	{
		*out_keyframe_dts = best_dts;
	}

	return VOD_OK;
}

static uint64_t
sprite_grabber_get_duration_ms(media_track_t* track, uint32_t timescale)
{
	frame_list_part_t* part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint64_t total_duration = 0;

	part = &track->frames;
	last_frame = part->last_frame;

	if (part->first_frame == NULL || part->first_frame >= last_frame)
	{
		return 0;
	}

	for (cur_frame = part->first_frame; ; cur_frame++)
	{
		if (cur_frame >= last_frame)
		{
			if (part->next == NULL)
			{
				break;
			}
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
			if (part->first_frame == NULL || part->first_frame >= last_frame)
			{
				break;
			}
		}
		total_duration += cur_frame->duration;
	}

	if (timescale == 0)
	{
		return 0;
	}

	return (total_duration * 1000) / timescale;
}

uint64_t
sprite_grabber_get_track_duration_ms(media_track_t* track)
{
	uint64_t duration_ms;
	uint32_t timescale;

	if (track == NULL)
	{
		return 0;
	}

	timescale = track->media_info.frames_timescale;
	duration_ms = sprite_grabber_get_duration_ms(track, timescale);

	if (duration_ms == 0 && track->media_info.duration_millis != 0)
	{
		duration_ms = track->media_info.duration_millis;
	}

	if (duration_ms == 0 && track->media_info.duration != 0 && track->media_info.timescale != 0)
	{
		duration_ms = (track->media_info.duration * 1000) / track->media_info.timescale;
	}

	if (duration_ms == 0 && track->media_info.full_duration != 0 && track->media_info.timescale != 0)
	{
		duration_ms = (track->media_info.full_duration * 1000) / track->media_info.timescale;
	}

	if (duration_ms == 0 && track->total_frames_duration != 0 && timescale != 0)
	{
		duration_ms = (track->total_frames_duration * 1000) / timescale;
	}

	return duration_ms;
}

void
sprite_grabber_calc_tile_dims(
	uint32_t video_width,
	uint32_t video_height,
	uint32_t* tile_width,
	uint32_t* tile_height)
{
	uint32_t tw = *tile_width;
	uint32_t th = *tile_height;

	// height-primary: when only the height is known, derive the width from the
	// source aspect ratio so portrait/landscape videos share the same tile
	// height (width varies). Width-primary is kept as a fallback for explicit
	// width requests.
	if (th > 0 && tw == 0 && video_height > 0)
	{
		tw = (uint32_t)(((uint64_t)video_width * th) / video_height);
	}
	else if (tw > 0 && th == 0 && video_width > 0)
	{
		th = (uint32_t)(((uint64_t)video_height * tw) / video_width);
	}

	if (tw == 0)
	{
		tw = SPRITE_DEFAULT_TILE_WIDTH;
	}
	if (th == 0)
	{
		th = SPRITE_DEFAULT_TILE_HEIGHT;
	}

	// dimensions must be even for YUV420P
	*tile_width = (tw + 1) & ~1u;
	*tile_height = (th + 1) & ~1u;
}

uint32_t
sprite_grabber_get_total_content_tiles(
	uint64_t duration_ms,
	uint32_t interval_ms)
{
	if (interval_ms == 0 || duration_ms == 0)
	{
		return 0;
	}

	return (uint32_t)((duration_ms + interval_ms - 1) / interval_ms);
}

bool_t
sprite_grabber_is_valid_page(
	uint64_t duration_ms,
	uint32_t interval_ms,
	uint32_t cols,
	uint32_t rows,
	uint32_t page)
{
	uint32_t tiles_per_page;
	uint32_t total_content_tiles;
	uint32_t page_start_tile;

	if (interval_ms == 0)
	{
		return FALSE;
	}

	tiles_per_page = cols * rows;
	if (tiles_per_page == 0)
	{
		return FALSE;
	}

	total_content_tiles = sprite_grabber_get_total_content_tiles(duration_ms, interval_ms);
	if (total_content_tiles == 0)
	{
		return TRUE;
	}

	page_start_tile = page * tiles_per_page;

	return page_start_tile < total_content_tiles;
}

static vod_status_t
sprite_grabber_calc_encode_height(
	uint64_t duration_ms,
	uint32_t interval_ms,
	uint32_t cols,
	uint32_t rows,
	uint32_t page,
	uint32_t tile_height,
	uint32_t* encode_height)
{
	uint32_t tiles_per_page;
	uint32_t total_content_tiles;
	uint32_t page_start_tile;
	uint32_t tiles_in_page;
	uint32_t used_rows;

	if (interval_ms == 0 || cols == 0 || rows == 0)
	{
		return VOD_BAD_DATA;
	}

	tiles_per_page = cols * rows;
	total_content_tiles = sprite_grabber_get_total_content_tiles(duration_ms, interval_ms);
	page_start_tile = page * tiles_per_page;

	if (total_content_tiles == 0 || page_start_tile >= total_content_tiles)
	{
		return VOD_BAD_DATA;
	}

	tiles_in_page = total_content_tiles - page_start_tile;
	if (tiles_in_page > tiles_per_page)
	{
		tiles_in_page = tiles_per_page;
	}

	used_rows = (tiles_in_page + cols - 1) / cols;
	*encode_height = used_rows * tile_height;
	if (*encode_height == 0)
	{
		*encode_height = tile_height;
	}
	return VOD_OK;
}

static uint32_t
sprite_grabber_get_nal_length_size(
	request_context_t* request_context,
	media_info_t* media_info)
{
	vod_status_t rc;
	uint32_t nal_length_size = 4;
	vod_str_t dummy;

	switch (media_info->codec_id)
	{
	case VOD_CODEC_ID_AVC:
		rc = codec_config_avcc_get_nal_units(
			request_context, &media_info->extra_data, TRUE, &nal_length_size, &dummy);
		break;

	case VOD_CODEC_ID_HEVC:
		rc = codec_config_hevc_get_nal_units(
			request_context, &media_info->extra_data, TRUE, &nal_length_size, &dummy);
		break;

	default:
		return 4;
	}

	if (rc != VOD_OK || nal_length_size == 0)
	{
		return 4;
	}

	return nal_length_size;
}

static uint32_t
sprite_grabber_read_nal_size(u_char* p, uint32_t nal_length_size)
{
	uint32_t result = 0;
	uint32_t i;

	for (i = 0; i < nal_length_size; i++)
	{
		result = (result << 8) | p[i];
	}

	return result;
}

static bool_t
sprite_grabber_nal_is_vcl(uint32_t codec_id, uint8_t nal_type)
{
	switch (codec_id)
	{
	case VOD_CODEC_ID_AVC:
		return nal_type >= 1 && nal_type <= 5;

	case VOD_CODEC_ID_HEVC:
		return nal_type <= 31;

	default:
		return FALSE;
	}
}

static bool_t
sprite_grabber_vcl_is_safe_to_decode(uint32_t codec_id, uint8_t nal_type)
{
	switch (codec_id)
	{
	case VOD_CODEC_ID_AVC:
		return nal_type == 5;

	case VOD_CODEC_ID_HEVC:
		return nal_type == 19 || nal_type == 20;

	default:
		return TRUE;
	}
}

static bool_t
sprite_grabber_frame_is_safe_to_decode(
	request_context_t* request_context,
	media_info_t* media_info,
	u_char* buffer,
	uint32_t size)
{
	uint32_t nal_length_size;
	uint32_t cur = 0;
	uint32_t nal_size;
	uint8_t nal_type;

	switch (media_info->codec_id)
	{
	case VOD_CODEC_ID_AVC:
	case VOD_CODEC_ID_HEVC:
		break;

	default:
		return TRUE;
	}

	nal_length_size = sprite_grabber_get_nal_length_size(request_context, media_info);

	if (nal_length_size == 0 || nal_length_size > 4)
	{
		return TRUE;
	}

	while (cur + nal_length_size <= size)
	{
		nal_size = sprite_grabber_read_nal_size(buffer + cur, nal_length_size);
		cur += nal_length_size;

		// guard against integer overflow / truncated NAL framing
		if (nal_size == 0 || nal_size > size || cur > size - nal_size)
		{
			break;
		}

		switch (media_info->codec_id)
		{
		case VOD_CODEC_ID_AVC:
			nal_type = buffer[cur] & 0x1f;
			break;

		case VOD_CODEC_ID_HEVC:
			nal_type = (buffer[cur] >> 1) & 0x3f;
			break;

		default:
			nal_type = 0;
			break;
		}

		if (sprite_grabber_nal_is_vcl(media_info->codec_id, nal_type))
		{
			return sprite_grabber_vcl_is_safe_to_decode(
				media_info->codec_id, nal_type);
		}

		cur += nal_size;
	}

	return FALSE;
}

static bool_t
sprite_grabber_validate_frame(AVFrame* frame, vod_log_t* log, uint32_t cur_tile)
{
	if (frame == NULL ||
		frame->width <= 0 || frame->height <= 0 ||
		frame->width > 8192 || frame->height > 8192 ||
		frame->data[0] == NULL || frame->linesize[0] < frame->width)
	{
		vod_log_error(VOD_LOG_WARN, log, 0,
			"sprite_grabber_validate_frame: invalid frame %dx%d at tile %uD",
			frame != NULL ? frame->width : 0,
			frame != NULL ? frame->height : 0,
			cur_tile);
		return FALSE;
	}

	if (frame->format == AV_PIX_FMT_YUV420P ||
		frame->format == AV_PIX_FMT_YUVJ420P)
	{
		if (frame->data[1] == NULL || frame->data[2] == NULL ||
			frame->linesize[1] <= 0 || frame->linesize[2] <= 0)
		{
			vod_log_error(VOD_LOG_WARN, log, 0,
				"sprite_grabber_validate_frame: missing YUV planes at tile %uD",
				cur_tile);
			return FALSE;
		}
	}

	return TRUE;
}

static vod_status_t
sprite_grabber_copy_to_working_frame(sprite_grabber_state_t* state)
{
	AVFrame* src = state->decoded_frame;
	AVFrame* dst = state->working_frame;
	int avrc;

	av_frame_unref(dst);

	dst->format = src->format;
	dst->width = src->width;
	dst->height = src->height;

	avrc = av_frame_get_buffer(dst, 32);
	if (avrc < 0)
	{
		return VOD_ALLOC_FAILED;
	}

	avrc = av_frame_copy(dst, src);
	if (avrc < 0)
	{
		return VOD_UNEXPECTED;
	}

	av_frame_copy_props(dst, src);

	return VOD_OK;
}

static vod_status_t
sprite_grabber_decode_tile(sprite_grabber_state_t* state)
{
	input_frame_t* frame = state->cur_keyframe;
	media_info_t* media_info = &state->track->media_info;
	AVPacket* input_packet;
	int avrc;
	vod_status_t rc;
	uint32_t tile_offset_ms;

	if (frame == NULL || frame->size == 0)
	{
		return VOD_UNEXPECTED;
	}

	tile_offset_ms = state->page * state->total_tiles * state->interval_ms +
		state->cur_tile * state->interval_ms;

	if (state->frame_buffer_size != frame->size)
	{
		vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
			"sprite_grabber_decode_tile: read size %uD != frame size %uD at tile %uD",
			state->frame_buffer_size, frame->size, state->cur_tile);
		return VOD_UNEXPECTED;
	}

	if (state->cur_keyframe == state->last_decoded_keyframe &&
		sprite_grabber_validate_frame(
			state->working_frame, state->request_context->log, state->cur_tile))
	{
		return VOD_OK;
	}

	if (frame->key_frame &&
		!sprite_grabber_frame_is_safe_to_decode(
			state->request_context, media_info,
			state->frame_buffer, frame->size))
	{
		vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
			"sprite_grabber_decode_tile: unsafe keyframe at tile %uD offset %uD ms, skipping",
			state->cur_tile, tile_offset_ms);
		return VOD_UNEXPECTED;
	}

	// reset decoder state before decoding this independent keyframe
	avcodec_flush_buffers(state->decoder);

	input_packet = av_packet_alloc();
	if (input_packet == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	avrc = av_new_packet(input_packet, frame->size);
	if (avrc < 0)
	{
		av_packet_free(&input_packet);
		return VOD_ALLOC_FAILED;
	}

	vod_memcpy(input_packet->data, state->frame_buffer, frame->size);

	input_packet->dts = state->cur_keyframe_dts;
	input_packet->pts = state->cur_keyframe_dts + frame->pts_delay;
	input_packet->duration = frame->duration;
	input_packet->flags = frame->key_frame ? AV_PKT_FLAG_KEY : 0;

	av_frame_unref(state->decoded_frame);

	avrc = avcodec_send_packet(state->decoder, input_packet);
	av_packet_free(&input_packet);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
			"sprite_grabber_decode_tile: avcodec_send_packet failed %d at tile %uD offset %uD ms",
			avrc, state->cur_tile, tile_offset_ms);
		return VOD_UNEXPECTED;
	}

	avrc = avcodec_receive_frame(state->decoder, state->decoded_frame);
	if (avrc == AVERROR(EAGAIN))
	{
		// single keyframe packet: drain the decoder to flush out the frame
		avrc = avcodec_send_packet(state->decoder, NULL);
		if (avrc < 0)
		{
			vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
				"sprite_grabber_decode_tile: drain send_packet failed %d at tile %uD",
				avrc, state->cur_tile);
			return VOD_UNEXPECTED;
		}

		avrc = avcodec_receive_frame(state->decoder, state->decoded_frame);
	}

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
			"sprite_grabber_decode_tile: avcodec_receive_frame failed %d at tile %uD",
			avrc, state->cur_tile);
		return VOD_UNEXPECTED;
	}

	if (!sprite_grabber_validate_frame(
		state->decoded_frame, state->request_context->log, state->cur_tile))
	{
		return VOD_UNEXPECTED;
	}

	rc = sprite_grabber_copy_to_working_frame(state);
	if (rc != VOD_OK)
	{
		vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
			"sprite_grabber_decode_tile: copy_to_working_frame failed at tile %uD",
			state->cur_tile);
		return rc;
	}

	state->last_decoded_keyframe = state->cur_keyframe;
	state->last_keyframe_dts = state->cur_keyframe_dts;

	return VOD_OK;
}

// advance to the next valid tile, find its keyframe, and start reading it
static vod_status_t
sprite_grabber_start_next_tile(sprite_grabber_state_t* state)
{
	uint32_t base_offset_ms;
	uint32_t frame_offset_ms;
	frame_list_part_t* part;
	input_frame_t* frame;
	uint64_t keyframe_dts;
	vod_status_t rc;

	base_offset_ms = state->page * state->total_tiles * state->interval_ms;

	for (;;)
	{
		if (state->cur_tile >= state->total_tiles)
		{
			// no more tiles, go to encode
			state->cur_state = SPRITE_STATE_ENCODE;
			return VOD_OK;
		}

		frame_offset_ms = base_offset_ms + state->cur_tile * state->interval_ms;

		if (frame_offset_ms >= state->video_duration_ms)
		{
			// beyond video, skip
			state->cur_tile++;
			continue;
		}

		rc = sprite_grabber_find_keyframe_at(
			state->track,
			frame_offset_ms,
			state->track->media_info.frames_timescale,
			&part,
			&frame,
			NULL,
			&keyframe_dts);
		if (rc != VOD_OK)
		{
			state->cur_tile++;
			continue;
		}

		state->cur_keyframe = frame;
		state->cur_keyframe_dts = keyframe_dts;

		// set up reading from this frame's source
		state->frames_source = part->frames_source;
		state->frames_source_context = part->frames_source_context;

		rc = state->frames_source->start_frame(
			state->frames_source_context,
			frame,
			NULL);
		if (rc != VOD_OK)
		{
			vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
				"sprite_grabber_start_next_tile: start_frame failed %i at tile %uD", rc, state->cur_tile);
			state->cur_tile++;
			continue;
		}

		state->frame_buffer_size = 0;
		state->cur_state = SPRITE_STATE_READ_FRAME;
		return VOD_OK;
	}
}

#if (VOD_HAVE_LIB_SW_SCALE)
static vod_status_t
sprite_grabber_resize_and_place(
	sprite_grabber_state_t* state,
	uint32_t col,
	uint32_t row)
{
	struct SwsContext *sws_ctx;
	AVFrame* input_frame = state->working_frame;
	uint8_t* tile_data[4];
	int tile_linesize[4];
	uint32_t x_offset, y_offset;

	x_offset = col * state->tile_width;
	y_offset = row * state->tile_height;

	if (x_offset + state->tile_width > state->canvas_width ||
		y_offset + state->tile_height > state->canvas_height)
	{
		vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
			"sprite_grabber_resize_and_place: tile %uD at (%uD,%uD) exceeds canvas %ux%u",
			state->cur_tile, col, row, state->canvas_width, state->canvas_height);
		return VOD_UNEXPECTED;
	}

	if (input_frame->width <= 0 || input_frame->height <= 0)
	{
		vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
			"sprite_grabber_resize_and_place: invalid decoded frame size at tile %uD",
			state->cur_tile);
		return VOD_UNEXPECTED;
	}

	sws_ctx = sws_getContext(
		input_frame->width, input_frame->height, input_frame->format,
		state->tile_width, state->tile_height, AV_PIX_FMT_YUV420P,
		SWS_FAST_BILINEAR, NULL, NULL, NULL);
	if (sws_ctx == NULL)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"sprite_grabber_resize_and_place: sws_getContext failed");
		return VOD_UNEXPECTED;
	}

	// allocate temp tile buffer
	{
		int avrc;
		uint32_t y;

		avrc = av_image_alloc(tile_data, tile_linesize,
			state->tile_width, state->tile_height, AV_PIX_FMT_YUV420P, 16);
		if (avrc < 0)
		{
			sws_freeContext(sws_ctx);
			return VOD_ALLOC_FAILED;
		}

		// scale
		sws_scale(sws_ctx,
			(const uint8_t* const*)input_frame->data, input_frame->linesize,
			0, input_frame->height,
			tile_data, tile_linesize);

		sws_freeContext(sws_ctx);

		// copy tile pixels into canvas at (col, row) position
		// Y plane
		for (y = 0; y < state->tile_height; y++)
		{
			memcpy(
				state->canvas_data[0] + (y_offset + y) * state->canvas_linesize[0] + x_offset,
				tile_data[0] + y * tile_linesize[0],
				state->tile_width);
		}

		// U plane (half resolution)
		for (y = 0; y < state->tile_height / 2; y++)
		{
			memcpy(
				state->canvas_data[1] + (y_offset / 2 + y) * state->canvas_linesize[1] + x_offset / 2,
				tile_data[1] + y * tile_linesize[1],
				state->tile_width / 2);
		}

		// V plane (half resolution)
		for (y = 0; y < state->tile_height / 2; y++)
		{
			memcpy(
				state->canvas_data[2] + (y_offset / 2 + y) * state->canvas_linesize[2] + x_offset / 2,
				tile_data[2] + y * tile_linesize[2],
				state->tile_width / 2);
		}

		av_freep(&tile_data[0]);
	}

	return VOD_OK;
}

#endif // VOD_HAVE_LIB_SW_SCALE

static uint32_t
sprite_grabber_get_max_frame_size(media_track_t* track)
{
	frame_list_part_t* part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint32_t max_frame_size = 0;

	part = &track->frames;
	last_frame = part->last_frame;

	if (part->first_frame == NULL || part->first_frame >= last_frame)
	{
		return 0;
	}

	for (cur_frame = part->first_frame; ; cur_frame++)
	{
		if (cur_frame >= last_frame)
		{
			if (part->next == NULL)
			{
				break;
			}
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
			if (part->first_frame == NULL || part->first_frame >= last_frame)
			{
				break;
			}
		}

		if (cur_frame->size > max_frame_size)
		{
			max_frame_size = cur_frame->size;
		}
	}

	return max_frame_size;
}

static vod_status_t
sprite_grabber_encode_canvas(sprite_grabber_state_t* state)
{
	AVFrame* canvas_frame;
	int avrc;

	canvas_frame = av_frame_alloc();
	if (canvas_frame == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	canvas_frame->width = state->canvas_width;
	canvas_frame->height = state->encode_height;
	canvas_frame->format = AV_PIX_FMT_YUV420P;
	canvas_frame->data[0] = state->canvas_data[0];
	canvas_frame->data[1] = state->canvas_data[1];
	canvas_frame->data[2] = state->canvas_data[2];
	canvas_frame->linesize[0] = state->canvas_linesize[0];
	canvas_frame->linesize[1] = state->canvas_linesize[1];
	canvas_frame->linesize[2] = state->canvas_linesize[2];

	avrc = avcodec_send_frame(state->encoder, canvas_frame);
	av_frame_free(&canvas_frame);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"sprite_grabber_encode_canvas: avcodec_send_frame failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	avrc = avcodec_receive_packet(state->encoder, state->output_packet);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"sprite_grabber_encode_canvas: avcodec_receive_packet failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	return state->write_callback(state->write_context,
		state->output_packet->data, state->output_packet->size);
}

vod_status_t
sprite_grabber_init_state(
	request_context_t* request_context,
	media_track_t* track,
	uint32_t page,
	uint32_t tile_width,
	uint32_t tile_height,
	uint32_t cols,
	uint32_t rows,
	uint32_t interval_ms,
	write_callback_t write_callback,
	void* write_context,
	void** result)
{
	sprite_grabber_state_t* state;
	vod_pool_cleanup_t *cln;
	vod_status_t rc;
	int avrc;
	uint32_t max_frame_size;

	if (sprite_encoder_codec == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"sprite_grabber_init_state: jpeg encoder not available");
		return VOD_BAD_REQUEST;
	}

	if (sprite_decoder_codec[track->media_info.codec_id] == NULL)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"sprite_grabber_init_state: no decoder for codec %uD", track->media_info.codec_id);
		return VOD_BAD_REQUEST;
	}

	state = vod_alloc(request_context->pool, sizeof(*state));
	if (state == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	vod_memzero(state, sizeof(*state));

	// add cleanup handler
	cln = vod_pool_cleanup_add(request_context->pool, 0);
	if (cln == NULL)
	{
		return VOD_ALLOC_FAILED;
	}
	cln->handler = sprite_grabber_free_state;
	cln->data = state;

	// derive tile dimensions (height-primary: fixed height, width from aspect
	// ratio) and round to even values for YUV420P
	sprite_grabber_calc_tile_dims(
		track->media_info.u.video.width,
		track->media_info.u.video.height,
		&tile_width,
		&tile_height);

	state->request_context = request_context;
	state->write_callback = write_callback;
	state->write_context = write_context;
	state->track = track;
	state->cols = cols;
	state->rows = rows;
	state->tile_width = tile_width;
	state->tile_height = tile_height;
	state->canvas_width = tile_width * cols;
	state->canvas_height = tile_height * rows;
	state->interval_ms = interval_ms;
	state->page = page;
	state->total_tiles = cols * rows;
	state->cur_tile = 0;

	// get video duration
	state->video_duration_ms = sprite_grabber_get_track_duration_ms(track);

	rc = sprite_grabber_calc_encode_height(
		state->video_duration_ms,
		interval_ms,
		cols,
		rows,
		page,
		tile_height,
		&state->encode_height);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// init decoder
	rc = sprite_grabber_init_decoder(request_context, &track->media_info, &state->decoder);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// alloc decoded/working frames
	state->decoded_frame = av_frame_alloc();
	if (state->decoded_frame == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	state->working_frame = av_frame_alloc();
	if (state->working_frame == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	vod_log_debug6(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
		"sprite_grabber_init_state: page=%uD canvas_w=%uD canvas_h=%uD encode_height=%uD duration_ms=%uL codec=%uD",
		(uint32_t)(page + 1), state->canvas_width, state->canvas_height,
		state->encode_height, state->video_duration_ms,
		(uint32_t)track->media_info.codec_id);

	state->output_packet = av_packet_alloc();
	if (state->output_packet == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	// allocate canvas (YUV420P)
	avrc = av_image_alloc(
		state->canvas_data, state->canvas_linesize,
		state->canvas_width, state->canvas_height,
		AV_PIX_FMT_YUV420P, 16);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"sprite_grabber_init_state: av_image_alloc failed for canvas %ux%u",
			state->canvas_width, state->canvas_height);
		return VOD_ALLOC_FAILED;
	}

	// fill canvas with black (Y=0, U=128, V=128)
	memset(state->canvas_data[0], 0, state->canvas_linesize[0] * state->canvas_height);
	memset(state->canvas_data[1], 128, state->canvas_linesize[1] * (state->canvas_height / 2));
	memset(state->canvas_data[2], 128, state->canvas_linesize[2] * (state->canvas_height / 2));

	// init encoder (width = full canvas, height = used rows only)
	state->encoder = avcodec_alloc_context3(sprite_encoder_codec);
	if (state->encoder == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	state->encoder->width = state->canvas_width;
	state->encoder->height = state->encode_height;
	state->encoder->time_base = (AVRational){ 1, 1 };
	state->encoder->pix_fmt = AV_PIX_FMT_YUVJ420P;

	avrc = avcodec_open2(state->encoder, sprite_encoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"sprite_grabber_init_state: avcodec_open2 encoder failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	// find max frame size across the entire track for buffer allocation
	max_frame_size = sprite_grabber_get_max_frame_size(track);
	if (max_frame_size == 0)
	{
		max_frame_size = 256 * 1024;  // 256KB fallback
	}

	state->max_frame_size = max_frame_size;

	// allocate frame buffer (reused for each tile)
	state->frame_buffer = vod_alloc(request_context->pool,
		max_frame_size + VOD_BUFFER_PADDING_SIZE);
	if (state->frame_buffer == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	// start the first tile
	rc = sprite_grabber_start_next_tile(state);
	if (rc != VOD_OK)
	{
		return rc;
	}

	*result = state;
	return VOD_OK;
}

vod_status_t
sprite_grabber_process(void* context)
{
	sprite_grabber_state_t* state = (sprite_grabber_state_t*)context;
	u_char* read_buffer;
	uint32_t read_size;
	bool_t frame_done;
	vod_status_t rc;
	uint32_t col, row;

	for (;;)
	{
		switch (state->cur_state)
		{
		case SPRITE_STATE_READ_FRAME:
			// read frame data from the read cache (like thumb_grabber)
			rc = state->frames_source->read(
				state->frames_source_context,
				&read_buffer,
				&read_size,
				&frame_done);
			if (rc != VOD_OK)
			{
				if (rc == VOD_AGAIN)
				{
					return VOD_AGAIN;  // let framework read more data
				}
				// read error, skip this tile
				vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
					"sprite_grabber_process: read failed %i at tile %uD", rc, state->cur_tile);
				state->cur_tile++;
				rc = sprite_grabber_start_next_tile(state);
				if (rc != VOD_OK)
				{
					return rc;
				}
				continue;
			}

			// accumulate data in frame buffer
			if (state->frame_buffer_size + read_size > state->max_frame_size)
			{
				vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
					"sprite_grabber_process: frame too large %uD bytes (max %uD) at tile %uD",
					state->frame_buffer_size + read_size, state->max_frame_size, state->cur_tile);
				state->cur_tile++;
				rc = sprite_grabber_start_next_tile(state);
				if (rc != VOD_OK)
				{
					return rc;
				}
				continue;
			}

			vod_memcpy(state->frame_buffer + state->frame_buffer_size,
				read_buffer, read_size);
			state->frame_buffer_size += read_size;

			if (!frame_done)
			{
				// partial read: the frame spans multiple cache buffers,
				// keep reading the next chunk (like thumb_grabber). Only the
				// frames_source returning VOD_AGAIN should bubble up.
				continue;
			}

			state->cur_state = SPRITE_STATE_DECODE;
			/* fall through */

		case SPRITE_STATE_DECODE:
			rc = sprite_grabber_decode_tile(state);
			if (rc != VOD_OK)
			{
				vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
					"sprite_grabber_process: decode failed at tile %uD", state->cur_tile);
				state->cur_tile++;
				rc = sprite_grabber_start_next_tile(state);
				if (rc != VOD_OK)
				{
					return rc;
				}
				continue;
			}

			state->cur_state = SPRITE_STATE_RESIZE_PLACE;
			/* fall through */

		case SPRITE_STATE_RESIZE_PLACE:
			col = state->cur_tile % state->cols;
			row = state->cur_tile / state->cols;

#if (VOD_HAVE_LIB_SW_SCALE)
			rc = sprite_grabber_resize_and_place(state, col, row);
			if (rc != VOD_OK)
			{
				vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
					"sprite_grabber_process: resize_and_place failed at tile %uD", state->cur_tile);
			}
#else
			vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
				"sprite_grabber_process: libswscale is required for sprite generation");
			return VOD_BAD_REQUEST;
#endif

			// advance to next tile
			state->cur_tile++;
			rc = sprite_grabber_start_next_tile(state);
			if (rc != VOD_OK)
			{
				return rc;
			}
			continue;  // back to switch

		case SPRITE_STATE_ENCODE:
			return sprite_grabber_encode_canvas(state);
		}
	}
}
