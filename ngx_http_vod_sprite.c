#include <ngx_http.h>
#include "ngx_http_vod_submodule.h"
#include "ngx_http_vod_utils.h"
#include "vod/thumb/sprite_grabber.h"
#include "vod/manifest_utils.h"
#include "vod/parse_utils.h"

#define SPRITE_TIMESCALE (1000)

static const u_char jpg_file_ext[] = ".jpg";
static const u_char vtt_file_ext[] = ".vtt";
static u_char jpeg_content_type[] = "image/jpeg";
static u_char vtt_content_type[] = "text/vtt; charset=utf-8";

// ──── WebVTT generation (metadata request) ────

static ngx_int_t
ngx_http_vod_sprite_handle_metadata(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* response,
	ngx_str_t* content_type)
{
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	request_context_t* request_context = &submodule_context->request_context;
	media_track_t* track;
	frame_list_part_t* part;
	input_frame_t* cur_frame;
	input_frame_t* last_frame;
	uint64_t total_duration = 0;
	uint64_t duration_ms;
	uint32_t cols = conf->sprite.cols;
	uint32_t rows = conf->sprite.rows;
	uint32_t interval_ms = conf->sprite.interval;
	uint32_t tile_width = conf->sprite.tile_width;
	uint32_t tile_height;
	uint32_t tiles_per_page = cols * rows;
	uint32_t total_tiles;
	uint32_t tile_idx;
	uint32_t page, col, row;
	u_char* p;
	size_t alloc_size;
	uint32_t start_sec, start_ms;
	uint32_t end_sec, end_ms;

	// find video track
	track = submodule_context->media_set.filtered_tracks;
	if (track == NULL)
	{
		return NGX_HTTP_NOT_FOUND;
	}

	// calculate duration
	part = &track->frames;
	last_frame = part->last_frame;
	for (cur_frame = part->first_frame; ; cur_frame++)
	{
		if (cur_frame >= last_frame)
		{
			if (part->next == NULL) break;
			part = part->next;
			cur_frame = part->first_frame;
			last_frame = part->last_frame;
		}
		total_duration += cur_frame->duration;
	}
	duration_ms = (total_duration * 1000) / track->media_info.frames_timescale;

	// calculate tile height from aspect ratio
	if (track->media_info.u.video.width > 0)
	{
		tile_height = ((uint64_t)track->media_info.u.video.height * tile_width) /
			track->media_info.u.video.width;
		tile_height = (tile_height + 1) & ~1;
	}
	else
	{
		tile_height = 90;
	}

	total_tiles = (uint32_t)((duration_ms + interval_ms - 1) / interval_ms);

	// estimate size: "WEBVTT\n\n" + per tile ~120 bytes
	alloc_size = 16 + total_tiles * 150;
	p = vod_alloc(request_context->pool, alloc_size);
	if (p == NULL)
	{
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_ALLOC_FAILED);
	}

	response->data = p;

	p = vod_copy(p, "WEBVTT\n\n", 8);

	for (tile_idx = 0; tile_idx < total_tiles; tile_idx++)
	{
		uint32_t start_time_ms = tile_idx * interval_ms;
		uint32_t end_time_ms = start_time_ms + interval_ms;
		if (end_time_ms > duration_ms)
		{
			end_time_ms = (uint32_t)duration_ms;
		}

		page = tile_idx / tiles_per_page;
		col = (tile_idx % tiles_per_page) % cols;
		row = (tile_idx % tiles_per_page) / cols;

		start_sec = start_time_ms / 1000;
		start_ms = start_time_ms % 1000;
		end_sec = end_time_ms / 1000;
		end_ms = end_time_ms % 1000;

		p = vod_sprintf(p,
			"%02uD:%02uD:%02uD.%03uD --> %02uD:%02uD:%02uD.%03uD\n"
			"%V-%uD.jpg#xywh=%uD,%uD,%uD,%uD\n\n",
			start_sec / 3600, (start_sec % 3600) / 60, start_sec % 60, start_ms,
			end_sec / 3600, (end_sec % 3600) / 60, end_sec % 60, end_ms,
			&conf->sprite.file_name_prefix, page,
			col * tile_width, row * tile_height,
			tile_width, tile_height);
	}

	response->len = p - response->data;

	content_type->len = sizeof(vtt_content_type) - 1;
	content_type->data = (u_char *)vtt_content_type;

	return NGX_OK;
}

// ──── Frame processor (JPEG sprite) ────

static ngx_int_t
ngx_http_vod_sprite_init_frame_processor(
	ngx_http_vod_submodule_context_t* submodule_context,
	segment_writer_t* segment_writer,
	ngx_http_vod_frame_processor_t* frame_processor,
	void** frame_processor_state,
	ngx_str_t* output_buffer,
	size_t* response_size,
	ngx_str_t* content_type)
{
	ngx_http_vod_loc_conf_t* conf = submodule_context->conf;
	request_params_t* request_params = &submodule_context->request_params;
	media_track_t* track = submodule_context->media_set.filtered_tracks;
	uint32_t tile_width;
	uint32_t tile_height = 0;
	vod_status_t rc;

	tile_width = request_params->width > 0 ? request_params->width : conf->sprite.tile_width;
	if (request_params->height > 0)
	{
		tile_height = request_params->height;
	}

	rc = sprite_grabber_init_state(
		&submodule_context->request_context,
		track,
		request_params->segment_index,    // page number
		tile_width,
		tile_height,
		conf->sprite.cols,
		conf->sprite.rows,
		conf->sprite.interval,
		segment_writer->write_tail,
		segment_writer->context,
		frame_processor_state);
	if (rc != VOD_OK)
	{
		return ngx_http_vod_status_to_ngx_error(submodule_context->r, rc);
	}

	*frame_processor = (ngx_http_vod_frame_processor_t)sprite_grabber_process;

	content_type->len = sizeof(jpeg_content_type) - 1;
	content_type->data = (u_char *)jpeg_content_type;

	return NGX_OK;
}

// ──── Request definitions ────

static const ngx_http_vod_request_t sprite_jpg_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL | PARSE_FLAG_EXTRA_DATA,
	REQUEST_CLASS_THUMB,
	VOD_CODEC_FLAG(AVC) | VOD_CODEC_FLAG(HEVC) | VOD_CODEC_FLAG(VP8) | VOD_CODEC_FLAG(VP9) | VOD_CODEC_FLAG(AV1),
	SPRITE_TIMESCALE,
	NULL,
	ngx_http_vod_sprite_init_frame_processor,
};

static const ngx_http_vod_request_t sprite_vtt_request = {
	REQUEST_FLAG_SINGLE_TRACK,
	PARSE_FLAG_FRAMES_ALL,
	REQUEST_CLASS_OTHER,
	VOD_CODEC_FLAG(AVC) | VOD_CODEC_FLAG(HEVC) | VOD_CODEC_FLAG(VP8) | VOD_CODEC_FLAG(VP9) | VOD_CODEC_FLAG(AV1),
	SPRITE_TIMESCALE,
	ngx_http_vod_sprite_handle_metadata,
	NULL,
};

// ──── Submodule callbacks ────

static void
ngx_http_vod_sprite_create_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_sprite_loc_conf_t *conf)
{
	conf->cols = NGX_CONF_UNSET_UINT;
	conf->rows = NGX_CONF_UNSET_UINT;
	conf->interval = NGX_CONF_UNSET_UINT;
	conf->tile_width = NGX_CONF_UNSET_UINT;
}

static char *
ngx_http_vod_sprite_merge_loc_conf(
	ngx_conf_t *cf,
	ngx_http_vod_loc_conf_t *base,
	ngx_http_vod_sprite_loc_conf_t *conf,
	ngx_http_vod_sprite_loc_conf_t *prev)
{
	ngx_conf_merge_str_value(conf->file_name_prefix, prev->file_name_prefix, "sprite");
	ngx_conf_merge_uint_value(conf->cols, prev->cols, SPRITE_DEFAULT_COLS);
	ngx_conf_merge_uint_value(conf->rows, prev->rows, SPRITE_DEFAULT_ROWS);
	ngx_conf_merge_uint_value(conf->interval, prev->interval, SPRITE_DEFAULT_INTERVAL);
	ngx_conf_merge_uint_value(conf->tile_width, prev->tile_width, SPRITE_DEFAULT_TILE_WIDTH);
	return NGX_CONF_OK;
}

static int
ngx_http_vod_sprite_get_file_path_components(ngx_str_t* uri)
{
	return 1;
}

static ngx_int_t
ngx_http_vod_sprite_parse_uri_file_name(
	ngx_http_request_t *r,
	ngx_http_vod_loc_conf_t *conf,
	u_char* start_pos,
	u_char* end_pos,
	request_params_t* request_params,
	const ngx_http_vod_request_t** request)
{
	ngx_int_t rc;

	// check for sprite.vtt
	if ((size_t)(end_pos - start_pos) == conf->sprite.file_name_prefix.len + sizeof(vtt_file_ext) - 1 &&
		ngx_memcmp(start_pos, conf->sprite.file_name_prefix.data, conf->sprite.file_name_prefix.len) == 0 &&
		ngx_memcmp(start_pos + conf->sprite.file_name_prefix.len, vtt_file_ext, sizeof(vtt_file_ext) - 1) == 0)
	{
		*request = &sprite_vtt_request;

		// parse the required tracks string (initializes tracks_mask)
		rc = ngx_http_vod_parse_uri_file_name(r, end_pos, end_pos, 0, request_params);
		if (rc != NGX_OK)
		{
			return rc;
		}

		vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_AUDIO]);
		vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_SUBTITLE]);
		return NGX_OK;
	}

	// check for sprite-{page}[-w{width}[-h{height}]].jpg
	if (ngx_http_vod_match_prefix_postfix(start_pos, end_pos, &conf->sprite.file_name_prefix, jpg_file_ext))
	{
		start_pos += conf->sprite.file_name_prefix.len;
		end_pos -= (sizeof(jpg_file_ext) - 1);
		*request = &sprite_jpg_request;
	}
	else
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_sprite_parse_uri_file_name: unidentified request");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	// parse page number after the dash
	if (start_pos < end_pos && *start_pos == '-')
	{
		start_pos++;
	}

	if (start_pos >= end_pos || *start_pos < '0' || *start_pos > '9')
	{
		ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
			"ngx_http_vod_sprite_parse_uri_file_name: failed to parse page number");
		return ngx_http_vod_status_to_ngx_error(r, VOD_BAD_REQUEST);
	}

	{
		uint32_t page = 0;
		while (start_pos < end_pos && *start_pos >= '0' && *start_pos <= '9')
		{
			page = page * 10 + (*start_pos++ - '0');
		}

		request_params->segment_index = page;  // reuse segment_index for page
	}

	// parse optional width/height: -w{width}[-h{height}]
	request_params->width = 0;
	request_params->height = 0;

	if (start_pos < end_pos && *start_pos == '-')
	{
		start_pos++;

		if (start_pos < end_pos && *start_pos == 'w')
		{
			start_pos++;
			while (start_pos < end_pos && *start_pos >= '0' && *start_pos <= '9')
			{
				request_params->width = request_params->width * 10 + (*start_pos++ - '0');
			}

			if (start_pos < end_pos && *start_pos == '-')
			{
				start_pos++;
			}
		}

		if (start_pos < end_pos && *start_pos == 'h')
		{
			start_pos++;
			while (start_pos < end_pos && *start_pos >= '0' && *start_pos <= '9')
			{
				request_params->height = request_params->height * 10 + (*start_pos++ - '0');
			}
		}
	}

	// parse the required tracks string (initializes tracks_mask)
	rc = ngx_http_vod_parse_uri_file_name(r, start_pos, end_pos, 0, request_params);
	if (rc != NGX_OK)
	{
		return rc;
	}

	vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_AUDIO]);
	vod_track_mask_reset_all_bits(request_params->tracks_mask[MEDIA_TYPE_SUBTITLE]);

	return NGX_OK;
}

static ngx_int_t
ngx_http_vod_sprite_parse_drm_info(
	ngx_http_vod_submodule_context_t* submodule_context,
	ngx_str_t* drm_info,
	void** output)
{
	ngx_log_error(NGX_LOG_ERR, submodule_context->request_context.log, 0,
		"ngx_http_vod_sprite_parse_drm_info: unexpected - drm on sprite request");
	return ngx_http_vod_status_to_ngx_error(submodule_context->r, VOD_BAD_REQUEST);
}

DEFINE_SUBMODULE(sprite);

