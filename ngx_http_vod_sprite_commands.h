#define BASE_OFFSET offsetof(ngx_http_vod_loc_conf_t, sprite)

	{ ngx_string("vod_sprite_file_name_prefix"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_str_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_sprite_loc_conf_t, file_name_prefix),
	NULL },

	{ ngx_string("vod_sprite_cols"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_sprite_loc_conf_t, cols),
	NULL },

	{ ngx_string("vod_sprite_rows"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_sprite_loc_conf_t, rows),
	NULL },

	{ ngx_string("vod_sprite_interval"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_sprite_loc_conf_t, interval),
	NULL },

	{ ngx_string("vod_sprite_tile_width"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_sprite_loc_conf_t, tile_width),
	NULL },

	{ ngx_string("vod_sprite_tile_height"),
	NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
	ngx_conf_set_num_slot,
	NGX_HTTP_LOC_CONF_OFFSET,
	BASE_OFFSET + offsetof(ngx_http_vod_sprite_loc_conf_t, tile_height),
	NULL },

#undef BASE_OFFSET
