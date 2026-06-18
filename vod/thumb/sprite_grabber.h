#ifndef __SPRITE_GRABBER_H__
#define __SPRITE_GRABBER_H__

// includes
#include "../media_format.h"
#include "../media_set.h"

// constants
#define SPRITE_DEFAULT_COLS       (6)
#define SPRITE_DEFAULT_ROWS       (6)
#define SPRITE_DEFAULT_INTERVAL   (1000)    // ms between frames
#define SPRITE_DEFAULT_TILE_WIDTH (160)
#define SPRITE_DEFAULT_QUALITY    (75)

// functions
void sprite_grabber_process_init(vod_log_t* log);

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
