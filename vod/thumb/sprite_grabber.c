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
	SPRITE_STATE_DECODE_FRAMES,
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

	// keyframe-snap sampling: each tile is mapped to the nearest keyframe so we
	// only decode keyframes (cheap, ~one decode per keyframe) instead of every
	// frame. Keyframes are decoded independently (intra), and the result is
	// reused for consecutive tiles that snap to the same keyframe.
	uint32_t timescale;
	uint32_t tiles_in_page;
	uint32_t cur_tile;             // tile being produced (0-based within page)
	input_frame_t** snap_frame;    // array[tiles_in_page] keyframe per tile (NULL = none)
	frame_list_part_t** snap_part; // array[tiles_in_page] the keyframe's frame list part
	uint64_t* snap_dts;            // array[tiles_in_page] keyframe dts (timescale units)

	input_frame_t* last_decoded_frame;  // last keyframe we attempted to decode
	int has_frame;                       // working_frame holds a valid decoded frame
	bool_t frame_started;
	u_char* frame_buffer;
	uint32_t cur_frame_pos;
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

#if (VOD_HAVE_LIB_SW_SCALE)
static vod_status_t sprite_grabber_resize_and_place(
	sprite_grabber_state_t* state, AVFrame* input_frame, uint32_t col, uint32_t row);
#endif // VOD_HAVE_LIB_SW_SCALE

// place the current working_frame into the given tile of the grid
static vod_status_t
sprite_grabber_place_tile(sprite_grabber_state_t* state, uint32_t tile)
{
	uint32_t col = tile % state->cols;
	uint32_t row = tile / state->cols;

	if (!state->has_frame ||
		!sprite_grabber_validate_frame(
			state->working_frame, state->request_context->log, tile))
	{
		// no decoded frame yet (e.g. first keyframe failed): leave tile black
		return VOD_OK;
	}

#if (VOD_HAVE_LIB_SW_SCALE)
	{
		vod_status_t rc = sprite_grabber_resize_and_place(
			state, state->working_frame, col, row);
		if (rc != VOD_OK)
		{
			vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
				"sprite_grabber_place_tile: resize_and_place failed at tile %uD", tile);
		}
	}
#else
	vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
		"sprite_grabber_place_tile: libswscale is required for sprite generation");
	return VOD_BAD_REQUEST;
#endif

	return VOD_OK;
}

// decode the keyframe snapped to the current tile, independently (it is intra,
// so a flush + single packet is enough). On any failure we keep the previous
// working_frame so the tile reuses the last good keyframe instead of going
// black or crashing.
static vod_status_t
sprite_grabber_decode_keyframe(sprite_grabber_state_t* state)
{
	input_frame_t* frame = state->snap_frame[state->cur_tile];
	AVPacket* input_packet;
	int avrc;
	vod_status_t rc;

	// mark this keyframe as attempted so a failure is not retried by tiles that
	// snap to the same keyframe
	state->last_decoded_frame = frame;

	if (frame == NULL || frame->size == 0 ||
		state->cur_frame_pos != frame->size)
	{
		return VOD_OK;  // reuse previous working_frame
	}

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
	input_packet->dts = state->snap_dts[state->cur_tile];
	input_packet->pts = state->snap_dts[state->cur_tile] + frame->pts_delay;
	input_packet->duration = frame->duration;
	input_packet->flags = AV_PKT_FLAG_KEY;

	av_frame_unref(state->decoded_frame);

	avrc = avcodec_send_packet(state->decoder, input_packet);
	av_packet_free(&input_packet);
	if (avrc < 0)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"sprite_grabber_decode_keyframe: send_packet failed %d, reusing previous", avrc);
		return VOD_OK;
	}

	avrc = avcodec_receive_frame(state->decoder, state->decoded_frame);
	if (avrc == AVERROR(EAGAIN))
	{
		// single packet: drain to flush the frame out
		avrc = avcodec_send_packet(state->decoder, NULL);
		if (avrc >= 0)
		{
			avrc = avcodec_receive_frame(state->decoder, state->decoded_frame);
		}
	}

	if (avrc < 0)
	{
		vod_log_debug1(VOD_LOG_DEBUG_LEVEL, state->request_context->log, 0,
			"sprite_grabber_decode_keyframe: receive_frame failed %d, reusing previous", avrc);
		return VOD_OK;
	}

	if (!sprite_grabber_validate_frame(
		state->decoded_frame, state->request_context->log, state->cur_tile))
	{
		return VOD_OK;
	}

	rc = sprite_grabber_copy_to_working_frame(state);
	if (rc != VOD_OK)
	{
		return rc;
	}
	state->has_frame = 1;

	return VOD_OK;
}

#if (VOD_HAVE_LIB_SW_SCALE)
static vod_status_t
sprite_grabber_resize_and_place(
	sprite_grabber_state_t* state,
	AVFrame* input_frame,
	uint32_t col,
	uint32_t row)
{
	struct SwsContext *sws_ctx;
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
	state->timescale = track->media_info.frames_timescale;

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

	// allocate frame buffer (reused for accumulating multi-chunk frames)
	state->frame_buffer = vod_alloc(request_context->pool,
		max_frame_size + VOD_BUFFER_PADDING_SIZE);
	if (state->frame_buffer == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	// snap every tile to its nearest preceding keyframe, so we only decode
	// keyframes (cheap) instead of every frame in the page span
	{
		uint32_t total_content_tiles;
		uint32_t page_first_tile;
		uint64_t base_offset_ms;
		uint32_t i;

		total_content_tiles = sprite_grabber_get_total_content_tiles(
			state->video_duration_ms, interval_ms);
		page_first_tile = page * state->total_tiles;

		if (total_content_tiles > page_first_tile)
		{
			state->tiles_in_page = total_content_tiles - page_first_tile;
			if (state->tiles_in_page > state->total_tiles)
			{
				state->tiles_in_page = state->total_tiles;
			}
		}
		else
		{
			state->tiles_in_page = 0;
		}

		base_offset_ms = (uint64_t)page * state->total_tiles * interval_ms;

		if (state->tiles_in_page == 0)
		{
			// nothing to decode (e.g. empty page), just emit a black canvas
			state->cur_state = SPRITE_STATE_ENCODE;
			*result = state;
			return VOD_OK;
		}

		state->snap_frame = vod_alloc(request_context->pool,
			state->tiles_in_page * sizeof(state->snap_frame[0]));
		state->snap_part = vod_alloc(request_context->pool,
			state->tiles_in_page * sizeof(state->snap_part[0]));
		state->snap_dts = vod_alloc(request_context->pool,
			state->tiles_in_page * sizeof(state->snap_dts[0]));
		if (state->snap_frame == NULL || state->snap_part == NULL ||
			state->snap_dts == NULL)
		{
			return VOD_ALLOC_FAILED;
		}

		for (i = 0; i < state->tiles_in_page; i++)
		{
			uint64_t tile_ms = base_offset_ms + (uint64_t)i * interval_ms;
			frame_list_part_t* kf_part;
			input_frame_t* kf_frame;
			uint64_t kf_dts;

			rc = sprite_grabber_find_keyframe_at(
				track,
				tile_ms,
				state->timescale,
				&kf_part,
				&kf_frame,
				NULL,
				&kf_dts);
			if (rc != VOD_OK)
			{
				// no keyframe for this tile: mark empty (reuses previous frame)
				state->snap_frame[i] = NULL;
				state->snap_part[i] = NULL;
				state->snap_dts[i] = 0;
				continue;
			}

			state->snap_frame[i] = kf_frame;
			state->snap_part[i] = kf_part;
			state->snap_dts[i] = kf_dts;
		}

		state->cur_tile = 0;
		state->last_decoded_frame = NULL;
		state->has_frame = 0;
		state->frame_started = FALSE;
		state->cur_frame_pos = 0;
		state->cur_state = SPRITE_STATE_DECODE_FRAMES;
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
	input_frame_t* kf;

	for (;;)
	{
		if (state->cur_state == SPRITE_STATE_ENCODE)
		{
			return sprite_grabber_encode_canvas(state);
		}

		// SPRITE_STATE_DECODE_FRAMES: produce one tile at a time. Each tile is
		// snapped to a keyframe; we only decode that keyframe (intra), and reuse
		// the result for consecutive tiles that snap to the same keyframe.

		// all tiles produced -> encode
		if (state->cur_tile >= state->tiles_in_page)
		{
			state->cur_state = SPRITE_STATE_ENCODE;
			continue;
		}

		kf = state->snap_frame[state->cur_tile];

		// reuse the last decoded keyframe (same snap target, or no keyframe for
		// this tile) without re-reading/decoding
		if (kf == NULL || kf == state->last_decoded_frame)
		{
			rc = sprite_grabber_place_tile(state, state->cur_tile);
			if (rc != VOD_OK)
			{
				return rc;
			}
			state->cur_tile++;
			continue;
		}

		// start reading the keyframe if not started yet
		if (!state->frame_started)
		{
			rc = state->snap_part[state->cur_tile]->frames_source->start_frame(
				state->snap_part[state->cur_tile]->frames_source_context,
				kf,
				NULL);
			if (rc != VOD_OK)
			{
				return rc;
			}

			state->frame_started = TRUE;
			state->cur_frame_pos = 0;
		}

		// read a chunk of the keyframe
		rc = state->snap_part[state->cur_tile]->frames_source->read(
			state->snap_part[state->cur_tile]->frames_source_context,
			&read_buffer,
			&read_size,
			&frame_done);
		if (rc != VOD_OK)
		{
			if (rc != VOD_AGAIN)
			{
				return rc;
			}
			return VOD_AGAIN;  // let the framework read more data
		}

		// accumulate into the frame buffer
		if (state->cur_frame_pos + read_size >
			state->max_frame_size + VOD_BUFFER_PADDING_SIZE)
		{
			vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
				"sprite_grabber_process: keyframe too large (%uD bytes)",
				state->cur_frame_pos + read_size);
			return VOD_BAD_DATA;
		}

		vod_memcpy(state->frame_buffer + state->cur_frame_pos,
			read_buffer, read_size);
		state->cur_frame_pos += read_size;

		if (!frame_done)
		{
			continue;  // keyframe spans multiple cache buffers
		}

		// decode the keyframe (independently) into working_frame
		rc = sprite_grabber_decode_keyframe(state);
		if (rc != VOD_OK)
		{
			return rc;
		}

		state->frame_started = FALSE;

		// place into the current tile and advance
		rc = sprite_grabber_place_tile(state, state->cur_tile);
		if (rc != VOD_OK)
		{
			return rc;
		}
		state->cur_tile++;
	}
}
