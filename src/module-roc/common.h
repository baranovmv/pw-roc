#ifndef MODULE_ROC_COMMON_H
#define MODULE_ROC_COMMON_H

#include <roc/config.h>
#include <roc/endpoint.h>
#include <roc/log.h>

#include <spa/utils/string.h>
#include <spa/support/log.h>
#include <spa/param/audio/raw.h>

#define PW_ROC_DEFAULT_IP "0.0.0.0"
#define PW_ROC_DEFAULT_SOURCE_PORT 10001
#define PW_ROC_DEFAULT_REPAIR_PORT 10002
#define PW_ROC_DEFAULT_CONTROL_PORT 10003
#define PW_ROC_DEFAULT_SESS_LATENCY 200
#define PW_ROC_DEFAULT_RATE 44100
#define PW_ROC_DEFAULT_CONTROL_PROTO ROC_PROTO_RTCP

void pw_roc_log_handler(const roc_log_message* message, void* argument);

static inline int pw_roc_parse_fec_encoding(roc_fec_encoding *out, const char *str)
{
	if (!str || !*str)
		*out = ROC_FEC_ENCODING_DEFAULT;
	else if (spa_streq(str, "disable"))
		*out = ROC_FEC_ENCODING_DISABLE;
	else if (spa_streq(str, "rs8m"))
		*out = ROC_FEC_ENCODING_RS8M;
	else if (spa_streq(str, "ldpc"))
		*out = ROC_FEC_ENCODING_LDPC_STAIRCASE;
	else
		return -EINVAL;
	return 0;
}

static inline int pw_roc_parse_resampler_profile(roc_resampler_profile *out, const char *str)
{
	if (!str || !*str)
		*out = ROC_RESAMPLER_PROFILE_DEFAULT;
	else if (spa_streq(str, "high"))
		*out = ROC_RESAMPLER_PROFILE_HIGH;
	else if (spa_streq(str, "medium"))
		*out = ROC_RESAMPLER_PROFILE_MEDIUM;
	else if (spa_streq(str, "low"))
		*out = ROC_RESAMPLER_PROFILE_LOW;
	else
		return -EINVAL;
	return 0;
}

static inline int pw_roc_create_endpoint(roc_endpoint **result, roc_protocol protocol, const char *ip, int port)
{
	roc_endpoint *endpoint;

	if (roc_endpoint_allocate(&endpoint))
		return -ENOMEM;

	if (roc_endpoint_set_protocol(endpoint, protocol))
		goto out_error_free_ep;

	if (roc_endpoint_set_host(endpoint, ip))
		goto out_error_free_ep;

	if (roc_endpoint_set_port(endpoint, port))
		goto out_error_free_ep;

	*result = endpoint;
	return 0;

out_error_free_ep:
	(void) roc_endpoint_deallocate(endpoint);
	return -EINVAL;
}

static inline void pw_roc_fec_encoding_to_proto(roc_fec_encoding fec_code, roc_protocol *audio, roc_protocol *repair)
{
	switch (fec_code) {
	case ROC_FEC_ENCODING_DEFAULT:
	case ROC_FEC_ENCODING_RS8M:
		*audio = ROC_PROTO_RTP_RS8M_SOURCE;
		*repair = ROC_PROTO_RS8M_REPAIR;
		break;
	case ROC_FEC_ENCODING_LDPC_STAIRCASE:
		*audio = ROC_PROTO_RTP_LDPC_SOURCE;
		*repair = ROC_PROTO_LDPC_REPAIR;
		break;
	default:
		*audio = ROC_PROTO_RTP;
		*repair = 0;
		break;
	}
}

static inline roc_log_level pw_roc_log_level_pw_2_roc(const enum spa_log_level pw_log_level)
{
	if (pw_log_level == SPA_LOG_LEVEL_NONE)
		return ROC_LOG_NONE;
	else if (pw_log_level == SPA_LOG_LEVEL_ERROR)
		return ROC_LOG_ERROR;
	else if (pw_log_level == SPA_LOG_LEVEL_WARN)
		return ROC_LOG_ERROR;
	else if (pw_log_level == SPA_LOG_LEVEL_INFO)
		return ROC_LOG_INFO;
	else if (pw_log_level == SPA_LOG_LEVEL_DEBUG)
		return ROC_LOG_DEBUG;
	else if (pw_log_level == SPA_LOG_LEVEL_TRACE)
		return ROC_LOG_TRACE;
	else
		return ROC_LOG_NONE;
}

static inline enum spa_log_level pw_roc_log_level_roc_2_pw(const roc_log_level roc_log_level)
{
	if (roc_log_level == ROC_LOG_NONE)
		return SPA_LOG_LEVEL_NONE;
	else if (roc_log_level == ROC_LOG_ERROR)
		return SPA_LOG_LEVEL_ERROR;
	else if (roc_log_level == ROC_LOG_INFO)
		return SPA_LOG_LEVEL_INFO;
	else if (roc_log_level == ROC_LOG_DEBUG)
		return SPA_LOG_LEVEL_DEBUG;
	else if (roc_log_level == ROC_LOG_TRACE)
		return SPA_LOG_LEVEL_TRACE;
	else
		return SPA_LOG_LEVEL_NONE;
}

static inline int pw_roc_parse_log_level(roc_log_level *loglevel, const char *str,
																				 roc_log_level default_level)
{
	if (spa_streq(str, "DEFAULT"))
		*loglevel = default_level;
	else if (spa_streq(str, "NONE"))
		*loglevel = ROC_LOG_NONE;
	else if (spa_streq(str, "ERROR"))
		*loglevel = ROC_LOG_ERROR;
	else if (spa_streq(str, "INFO"))
		*loglevel = ROC_LOG_INFO;
	else if (spa_streq(str, "DEBUG"))
		*loglevel = ROC_LOG_DEBUG;
	else if (spa_streq(str, "TRACE"))
		*loglevel = ROC_LOG_TRACE;
	else
		return -EINVAL;
	return 0;
}

#endif /* MODULE_ROC_COMMON_H */
