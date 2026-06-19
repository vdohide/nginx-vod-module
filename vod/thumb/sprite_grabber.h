#ifndef __SPRITE_GRABBER_H__
#define __SPRITE_GRABBER_H__

// includes
#include "../media_format.h"
#include "../media_set.h"

// constants
#define SPRITE_DEFAULT_COLS        (6)
#define SPRITE_DEFAULT_ROWS        (6)
#define SPRITE_DEFAULT_INTERVAL    (1000)    // ms between frames
#define SPRITE_DEFAULT_TILE_WIDTH  (160)     // fallback only (height-primary by default)
#define SPRITE_DEFAULT_TILE_HEIGHT (160)     // fixed tile height; width derived from aspect ratio
#define SPRITE_DEFAULT_QUALITY     (90)

// functions
void sprite_grabber_process_init(vod_log_t* log);

uint64_t sprite_grabber_get_track_duration_ms(media_track_t* track);

void sprite_grabber_calc_tile_dims(
	uint32_t video_width,
	uint32_t video_height,
	uint32_t* tile_width,
	uint32_t* tile_height);

uint32_t sprite_grabber_get_total_content_tiles(
	uint64_t duration_ms,
	uint32_t interval_ms);

bool_t sprite_grabber_is_valid_page(
	uint64_t duration_ms,
	uint32_t interval_ms,
	uint32_t cols,
	uint32_t rows,
	uint32_t page);

vod_status_t sprite_grabber_init_state(
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
	void** result);

vod_status_t sprite_grabber_process(void* context);

#endif //__SPRITE_GRABBER_H__
