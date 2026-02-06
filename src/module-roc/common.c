#include <pipewire/log.h>
#include <pipewire/properties.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-types.h>
#include <spa/utils/dict.h>
#include <spa/utils/json.h>
#include <spa/debug/types.h>

#include "common.h"

#include <roc/log.h>
PW_LOG_TOPIC_EXTERN(mod_topic);
#define PW_LOG_TOPIC_DEFAULT mod_topic

void pw_roc_log_handler(const roc_log_message* message, void* argument)
{
    const enum spa_log_level log_level = pw_roc_log_level_roc_2_pw(message->level);
    if (SPA_UNLIKELY(pw_log_topic_enabled(log_level, mod_topic))) {
        pw_log_logt(log_level, mod_topic, message->file, message->line, message->module, message->text, "");
    }
}

void pw_roc_parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	info->format = SPA_AUDIO_FORMAT_F32;
	info->rate = PW_ROC_DEFAULT_RATE;
	info->channels = 2;
	info->position[0] = SPA_AUDIO_CHANNEL_FL;
	info->position[1] = SPA_AUDIO_CHANNEL_FR;

	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_FORMAT)) != NULL) {
		uint32_t fmt = spa_debug_type_find_type_short(spa_type_audio_format, str);
		if (fmt != SPA_ID_INVALID)
			info->format = fmt;
	}
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_RATE)) != NULL) {
		info->rate = atoi(str);
	}
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_CHANNELS)) != NULL) {
		info->channels = atoi(str);
	}
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL) {
		struct spa_json it[2];
		char v[256];
		uint32_t i = 0;

		spa_json_init(&it[0], str, strlen(str));
		if (spa_json_enter_array(&it[0], &it[1]) <= 0)
			spa_json_init(&it[1], str, strlen(str));

		while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
		    i < SPA_AUDIO_MAX_CHANNELS) {
			uint32_t ch = spa_debug_type_find_type_short(spa_type_audio_channel, v);
			if (ch != SPA_ID_INVALID)
				info->position[i++] = ch;
		}
		if (i > 0)
			info->channels = i;
	}
}
