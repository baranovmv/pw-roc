#include <pipewire/log.h>
#include <roc/log.h>
#include <spa/param/audio/raw-types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/dict.h>
#include <spa/utils/json.h>
#include <spa/debug/types.h>

#include "common.h"

PW_LOG_TOPIC(roc_log_topic, "mod.roc.lib");

void pw_roc_log_init(void)
{
	roc_log_set_handler(pw_roc_log_handler, NULL);
	roc_log_set_level(pw_roc_log_level_pw_2_roc(roc_log_topic->has_custom_level ? roc_log_topic->level : pw_log_level));
}

void pw_roc_log_handler(const roc_log_message *message, void *argument)
{
	const enum spa_log_level log_level = pw_roc_log_level_roc_2_pw(message->level);
	if (SPA_UNLIKELY(pw_log_topic_enabled(log_level, roc_log_topic))) {
		pw_log_logt(log_level, roc_log_topic, message->file, message->line, message->module, message->text, "");
	}
}

static uint32_t channel_name2id(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (strcmp(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)) == 0)
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

// XXX: this duplicates function introduced in later versions of spa
int pw_roc_spa_audio_parse_position_n(const char *str, size_t len,
		uint32_t *position, uint32_t max_position, uint32_t *n_channels)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], str, strlen(str));
	if (spa_json_enter_array(&it[0], &it[1]) <= 0)
		spa_json_init(&it[1], str, strlen(str));

	size_t channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    channels < SPA_AUDIO_MAX_CHANNELS) {
		position[channels++] = channel_name2id(v);
	}
	if (n_channels) {
		*n_channels = channels;
	}
	return channels;
}
