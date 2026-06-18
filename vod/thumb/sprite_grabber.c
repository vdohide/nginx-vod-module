#include "sprite_grabber.h"
#include "../media_set.h"

#include <libavcodec/avcodec.h>

#if (VOD_HAVE_LIB_SW_SCALE)
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#endif // VOD_HAVE_LIB_SW_SCALE

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
	AVPacket *output_packet;

	// track info
	media_track_t* track;
	uint64_t video_duration_ms;

	// tile processing state
	uint32_t cur_tile;
	bool_t initialized;

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
	dec->extradata = media_info->extra_data.data;
	dec->extradata_size = media_info->extra_data.len;
	dec->width = media_info->u.video.width;
	dec->height = media_info->u.video.height;

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
	uint32_t* out_frame_size)
{
	frame_list_part_t* part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	input_frame_t* best_frame = NULL;
	frame_list_part_t* best_part = NULL;
	uint64_t dts = 0;
	uint64_t target_ts;

	target_ts = (target_time_ms * timescale) / 1000;

	part = &track->frames;
	last_frame = part->last_frame;

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
		}

		if (cur_frame->key_frame)
		{
			if (dts > target_ts && best_frame != NULL)
			{
				break;  // we've gone past, use previous keyframe
			}
			best_frame = cur_frame;
			best_part = part;
		}

		dts += cur_frame->duration;
	}

	if (best_frame == NULL)
	{
		return VOD_NOT_FOUND;
	}

	*out_part = best_part;
	*out_frame = best_frame;
	*out_frame_size = best_frame->size;

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
		}
		total_duration += cur_frame->duration;
	}

	return (total_duration * 1000) / timescale;
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
	int avrc;

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

	// calculate tile dimensions
	if (tile_height == 0 && track->media_info.u.video.width > 0)
	{
		tile_height = ((uint64_t)track->media_info.u.video.height * tile_width) /
			track->media_info.u.video.width;
	}

	// ensure dimensions are even (required for YUV420P)
	tile_width = (tile_width + 1) & ~1;
	tile_height = (tile_height + 1) & ~1;

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
	state->initialized = FALSE;

	// get video duration
	state->video_duration_ms = sprite_grabber_get_duration_ms(track, 
		track->media_info.frames_timescale);

	// init decoder
	vod_status_t rc = sprite_grabber_init_decoder(request_context, &track->media_info, &state->decoder);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// alloc decoded frame
	state->decoded_frame = av_frame_alloc();
	if (state->decoded_frame == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

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

	// init encoder for full canvas
	state->encoder = avcodec_alloc_context3(sprite_encoder_codec);
	if (state->encoder == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	state->encoder->width = state->canvas_width;
	state->encoder->height = state->canvas_height;
	state->encoder->time_base = (AVRational){ 1, 1 };
	state->encoder->pix_fmt = AV_PIX_FMT_YUVJ420P;

	avrc = avcodec_open2(state->encoder, sprite_encoder_codec, NULL);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, request_context->log, 0,
			"sprite_grabber_init_state: avcodec_open2 encoder failed %d", avrc);
		return VOD_UNEXPECTED;
	}

	*result = state;
	return VOD_OK;
}

static vod_status_t
sprite_grabber_decode_keyframe(
	sprite_grabber_state_t* state,
	uint64_t target_time_ms)
{
	frame_list_part_t* part;
	input_frame_t* frame;
	uint32_t frame_size;
	uint8_t* buffer;
	uint8_t* frame_end;
	uint8_t original_pad[VOD_BUFFER_PADDING_SIZE];
	AVPacket* input_packet;
	int avrc;
	vod_status_t rc;

	rc = sprite_grabber_find_keyframe_at(
		state->track,
		target_time_ms,
		state->track->media_info.frames_timescale,
		&part,
		&frame,
		&frame_size);

	if (rc != VOD_OK)
	{
		return rc;
	}

	// read the frame data
	rc = part->frames_source->start_frame(
		part->frames_source_context,
		frame,
		NULL);
	if (rc != VOD_OK)
	{
		return rc;
	}

	// allocate buffer for frame data
	buffer = vod_alloc(state->request_context->pool, frame_size + VOD_BUFFER_PADDING_SIZE);
	if (buffer == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	// read frame data
	uint32_t total_read = 0;
	for (;;)
	{
		uint8_t* read_buffer;
		uint32_t read_size;
		bool_t frame_done;

		rc = part->frames_source->read(
			part->frames_source_context,
			&read_buffer,
			&read_size,
			&frame_done);
		if (rc != VOD_OK)
		{
			if (rc == VOD_AGAIN)
			{
				continue;
			}
			return rc;
		}

		vod_memcpy(buffer + total_read, read_buffer, read_size);
		total_read += read_size;

		if (frame_done)
		{
			break;
		}
	}

	// decode frame
	av_frame_unref(state->decoded_frame);

	frame_end = buffer + frame_size;
	vod_memcpy(original_pad, frame_end, sizeof(original_pad));
	vod_memzero(frame_end, sizeof(original_pad));

	input_packet = av_packet_alloc();
	if (input_packet == NULL)
	{
		return VOD_ALLOC_FAILED;
	}

	input_packet->data = buffer;
	input_packet->size = frame_size;
	input_packet->flags = AV_PKT_FLAG_KEY;

	avrc = avcodec_send_packet(state->decoder, input_packet);
	av_packet_free(&input_packet);
	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"sprite_grabber_decode_keyframe: avcodec_send_packet failed %d", avrc);
		vod_memcpy(frame_end, original_pad, sizeof(original_pad));
		return VOD_BAD_DATA;
	}

	avrc = avcodec_receive_frame(state->decoder, state->decoded_frame);
	vod_memcpy(frame_end, original_pad, sizeof(original_pad));

	if (avrc < 0)
	{
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"sprite_grabber_decode_keyframe: avcodec_receive_frame failed %d", avrc);
		return VOD_BAD_DATA;
	}

	return VOD_OK;
}

#if (VOD_HAVE_LIB_SW_SCALE)
static vod_status_t
sprite_grabber_resize_and_place(
	sprite_grabber_state_t* state,
	uint32_t col,
	uint32_t row)
{
	struct SwsContext *sws_ctx;
	AVFrame* input_frame = state->decoded_frame;
	uint8_t* tile_data[4];
	int tile_linesize[4];
	uint32_t x_offset, y_offset;

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
	int avrc = av_image_alloc(tile_data, tile_linesize,
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
	x_offset = col * state->tile_width;
	y_offset = row * state->tile_height;

	// Y plane
	for (uint32_t y = 0; y < state->tile_height; y++)
	{
		memcpy(
			state->canvas_data[0] + (y_offset + y) * state->canvas_linesize[0] + x_offset,
			tile_data[0] + y * tile_linesize[0],
			state->tile_width);
	}

	// U plane (half resolution)
	for (uint32_t y = 0; y < state->tile_height / 2; y++)
	{
		memcpy(
			state->canvas_data[1] + (y_offset / 2 + y) * state->canvas_linesize[1] + x_offset / 2,
			tile_data[1] + y * tile_linesize[1],
			state->tile_width / 2);
	}

	// V plane (half resolution)
	for (uint32_t y = 0; y < state->tile_height / 2; y++)
	{
		memcpy(
			state->canvas_data[2] + (y_offset / 2 + y) * state->canvas_linesize[2] + x_offset / 2,
			tile_data[2] + y * tile_linesize[2],
			state->tile_width / 2);
	}

	av_freep(&tile_data[0]);

	return VOD_OK;
}
#endif // VOD_HAVE_LIB_SW_SCALE

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
	canvas_frame->height = state->canvas_height;
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
sprite_grabber_process(void* context)
{
	sprite_grabber_state_t* state = (sprite_grabber_state_t*)context;
	vod_status_t rc;
	uint32_t base_offset_ms;
	uint32_t frame_offset_ms;
	uint32_t col, row;

	base_offset_ms = state->page * state->total_tiles * state->interval_ms;

	// process each tile
	while (state->cur_tile < state->total_tiles)
	{
		frame_offset_ms = base_offset_ms + state->cur_tile * state->interval_ms;

		// skip tiles beyond video duration
		if (frame_offset_ms >= state->video_duration_ms)
		{
			state->cur_tile++;
			continue;
		}

		col = state->cur_tile % state->cols;
		row = state->cur_tile / state->cols;

		// decode the nearest keyframe
		rc = sprite_grabber_decode_keyframe(state, frame_offset_ms);
		if (rc != VOD_OK)
		{
			if (rc == VOD_AGAIN)
			{
				return VOD_AGAIN;
			}
			// skip failed frames
			vod_log_error(VOD_LOG_WARN, state->request_context->log, 0,
				"sprite_grabber_process: failed to decode frame at %uD ms, skipping", frame_offset_ms);
			state->cur_tile++;
			continue;
		}

#if (VOD_HAVE_LIB_SW_SCALE)
		// resize and place on canvas
		rc = sprite_grabber_resize_and_place(state, col, row);
		if (rc != VOD_OK)
		{
			state->cur_tile++;
			continue;
		}
#else
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"sprite_grabber_process: libswscale is required for sprite generation");
		return VOD_BAD_REQUEST;
#endif

		// flush decoder for next frame
		avcodec_flush_buffers(state->decoder);

		state->cur_tile++;
	}

	// all tiles processed, encode the canvas
	return sprite_grabber_encode_canvas(state);
}
