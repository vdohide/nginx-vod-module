#ifndef _NGX_HTTP_VOD_SPRITE_CONF_H_INCLUDED_
#define _NGX_HTTP_VOD_SPRITE_CONF_H_INCLUDED_

// includes
#include <ngx_http.h>

// typedefs
typedef struct
{
	ngx_str_t file_name_prefix;
	ngx_uint_t cols;
	ngx_uint_t rows;
	ngx_uint_t interval;        // ms between frames
	ngx_uint_t tile_width;      // px, height auto-calculated
} ngx_http_vod_sprite_loc_conf_t;

#endif // _NGX_HTTP_VOD_SPRITE_CONF_H_INCLUDED_
