//
// Copyright 2022 NanoMQ Team, Inc. <jaylin@emqx.io>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "include/nng_proxy.h"

#include <nng/mqtt/mqtt_client.h>
#include <nng/nng.h>
#include <nng/protocol/bus0/bus.h>
#include <nng/protocol/pair0/pair.h>
#include <nng/protocol/pair1/pair.h>
#include <nng/protocol/pipeline0/pull.h>
#include <nng/protocol/pipeline0/push.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/reqrep0/rep.h>
#include <nng/protocol/reqrep0/req.h>
#include <nng/protocol/survey0/respond.h>
#include <nng/protocol/survey0/survey.h>
#include <nng/supplemental/tls/tls.h>
#include <nng/supplemental/util/options.h>
#include <nng/supplemental/util/platform.h>

#if defined(SUPP_CLIENT)

#ifdef NNG_SUPP_TLS
#include <nng/supplemental/tls/tls.h>
static int init_dialer_tls(nng_dialer d, const char *cacert, const char *cert,
    const char *key, const char *pass);
#endif

static void loadfile(const char *path, void **datap, size_t *lenp);
static void fatal(const char *msg, ...);

#define ASSERT_NULL(p, fmt, ...)           \
	if ((p) != NULL) {                 \
		fatal(fmt, ##__VA_ARGS__); \
	}

struct topic {
	struct topic *next;
	char *        val;
};

struct nng_proxy_opts {
	enum nng_proto   type;
	bool             verbose;
	size_t           parallel;
	atomic_ulong     msg_count;	// caculate how many msg has been conveyed
	size_t           interval;
	uint8_t          version;
	char            *nng_url;
        char            *mqtt_url;
	size_t           clients;
	struct topic *   topic;
	size_t           topic_count;
	uint8_t          qos;
	bool             retain;
	char *           user;
	char *           passwd;
	char *           client_id;
	uint16_t         keepalive;
	bool             clean_session;
	uint8_t *        msg;
	size_t           msg_len;
	uint8_t *        will_msg;
	size_t           will_msg_len;
	uint8_t          will_qos;
	bool             will_retain;
	char *           will_topic;
	bool             enable_ssl;
	char *           cacert;
	size_t           cacert_len;
	char *           cert;
	size_t           cert_len;
	char *           key;
	size_t           key_len;
	char *           keypass;
};

nng_proxy_opts *nng_opts = NULL;

enum options {
	OPT_HELP = 1,
	OPT_VERBOSE,
	OPT_PARALLEL,
	OPT_MSGCOUNT,
	OPT_CLIENTS,
	OPT_INTERVAL,
	OPT_VERSION,
	OPT_MQTT_URL,
        OPT_NNG_URL,
	OPT_PUB,
	OPT_SUB,
	OPT_TOPIC,
	OPT_QOS,
	OPT_RETAIN,
	OPT_USER,
	OPT_PASSWD,
	OPT_CLIENTID,
	OPT_KEEPALIVE,
	OPT_CLEAN_SESSION,
	OPT_WILL_MSG,
	OPT_WILL_QOS,
	OPT_WILL_RETAIN,
	OPT_WILL_TOPIC,
	OPT_SECURE,
	OPT_CACERT,
	OPT_CERTFILE,
	OPT_KEYFILE,
	OPT_KEYPASS,
	OPT_MSG,
	OPT_FILE,
};

static nng_optspec cmd_opts[] = {
	{ .o_name = "help", .o_short = 'h', .o_val = OPT_HELP },
	{ .o_name = "verbose", .o_short = 'v', .o_val = OPT_VERBOSE },
	{ .o_name    = "parallel",
	    .o_short = 'n',
	    .o_val   = OPT_PARALLEL,
	    .o_arg   = true },
	{ .o_name    = "interval",
	    .o_short = 'i',
	    .o_val   = OPT_INTERVAL,
	    .o_arg   = true },
	{ .o_name    = "limit",
	    .o_short = 'L',
	    .o_val   = OPT_MSGCOUNT,
	    .o_arg   = true },
	{ .o_name    = "count",
	    .o_short = 'C',
	    .o_val   = OPT_CLIENTS,
	    .o_arg   = true },
	{ .o_name = "version", .o_short = 'V', .o_val = OPT_VERSION },
	{ .o_name = "mqtt_url", .o_val = OPT_MQTT_URL, .o_arg = true },
        { .o_name = "nng_url", .o_val = OPT_NNG_URL, .o_arg = true },
	{ .o_name    = "topic",
	    .o_short = 't',
	    .o_val   = OPT_TOPIC,
	    .o_arg   = true },
	{ .o_name = "qos", .o_short = 'q', .o_val = OPT_QOS, .o_arg = true },
	{ .o_name = "retain", .o_short = 'r', .o_val = OPT_RETAIN },
	{ .o_name = "user", .o_short = 'u', .o_val = OPT_USER, .o_arg = true },
	{ .o_name    = "password",
	    .o_short = 'p',
	    .o_val   = OPT_PASSWD,
	    .o_arg   = true },
	{ .o_name    = "id",
	    .o_short = 'I',
	    .o_val   = OPT_CLIENTID,
	    .o_arg   = true },
	{ .o_name    = "keepalive",
	    .o_short = 'k',
	    .o_val   = OPT_KEEPALIVE,
	    .o_arg   = true },
	{ .o_name    = "clean_session",
	    .o_short = 'c',
	    .o_val   = OPT_CLEAN_SESSION,
	    .o_arg   = true },
	{ .o_name = "will-msg", .o_val = OPT_WILL_MSG, .o_arg = true },
	{ .o_name = "will-qos", .o_val = OPT_WILL_QOS, .o_arg = true },
	{ .o_name = "will-retain", .o_val = OPT_WILL_RETAIN },
	{ .o_name = "will-topic", .o_val = OPT_WILL_TOPIC, .o_arg = true },
	{ .o_name = "secure", .o_short = 's', .o_val = OPT_SECURE },
	{ .o_name = "cacert", .o_val = OPT_CACERT, .o_arg = true },
	{ .o_name = "key", .o_val = OPT_KEYFILE, .o_arg = true },
	{ .o_name = "keypass", .o_val = OPT_KEYPASS, .o_arg = true },
	{
	    .o_name  = "cert",
	    .o_short = 'E',
	    .o_val   = OPT_CERTFILE,
	    .o_arg   = true,
	},

	{ .o_name = "msg", .o_short = 'm', .o_val = OPT_MSG, .o_arg = true },
	{ .o_name = "file", .o_short = 'f', .o_val = OPT_FILE, .o_arg = true },

	{ .o_name = NULL, .o_val = 0 },
};

struct work {
	enum { INIT, RECV, RECV_MQTT, RECV_NNG, SEND_MQTT, SEND_NNG } state;
	nng_aio *    aio;
	nng_msg *    msg;
	nng_ctx      ctx;
	nng_ctx	     proxy_ctx;
	nng_socket   nsocket;
	nng_proxy_opts *nng_opts;
};

static atomic_bool exit_signal = false;

static void
fatal(const char *msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	vfprintf(stderr, msg, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static void
nng_fatal(const char *msg, int rv)
{
	fatal("%s:%s", msg, nng_strerror(rv));
}

static void
help(enum nng_proto type)
{
	switch (type) {
	case PUB0:
		printf("Usage: nanomq pub { start | stop } <addr> "
		       "[<topic>...] [<nng_opts>...] [<src>]\n\n");
		break;
	case SUB0:
		printf("Usage: nanomq sub { start | stop } <addr> "
		       "[<topic>...] [<nng_opts>...]\n\n");
		break;
	case CONN:
		printf("Usage: nanomq conn { start | stop } <addr> "
		       "[<nng_opts>...]\n\n");
		break;

	default:
		break;
	}

	printf("<addr> must be one or more of:\n");
	printf("  --url <url>                      The url for mqtt broker "
	       "('mqtt-tcp://host:port' or 'tls+mqtt-tcp://host:port') \n");
	printf("                                   [default: "
	       "mqtt-tcp://127.0.0.1:1883]\n");

	if (type == PUB0 || type == SUB0) {
		printf("\n<topic> must be set:\n");
		printf(
		    "  -t, --topic <topic>              Topic for publish or "
		    "subscribe\n");
	}

	printf("\n<nng_opts> may be any of:\n");
	printf("  -V, --version <version: 3|4|5>   The MQTT version used by "
	       "the client [default: 4]\n");
	printf("  -n, --parallel             	   The number of parallel for "
	       "client [default: 1]\n");
	printf("  -v, --verbose              	   Enable verbose mode\n");
	printf("  -u, --user <user>                The username for "
	       "authentication\n");
	printf("  -p, --password <password>        The password for "
	       "authentication\n");
	printf("  -k, --keepalive <keepalive>      A keep alive of the client "
	       "(in seconds) [default: 60]\n");
	if (type == PUB0) {
		printf("  -m, --msg <message>              The message to "
		       "publish\n");
		printf("  -L, --limit <num>                Max count of "
		       "publishing "
		       "message [default: 1]\n");
		printf("  -i, --interval <ms>              Interval of "
		       "publishing "
		       "message (ms) [default: 10]\n");
		printf(
		    "  -I, --identifier <identifier>    The client identifier "
		    "UTF-8 String (default randomly generated string)\n");
	} else {
		printf("  -i, --interval <ms>              Interval of "
		       "establishing connection "
		       "(ms) [default: 10]\n");
	}
	printf("  -C, --count <num>                Num of client \n");
	printf("  -q, --qos <qos>                  Quality of service for the "
	       "corresponding topic [default: 0]\n");
	printf("  -r, --retain                     The message will be "
	       "retained [default: false]\n");
	printf("  -c, --clean_session <true|false> Define a clean start for "
	       "the connection [default: true]\n");
	printf("  --will-qos <qos>                 Quality of service level "
	       "for the will message [default: 0]\n");
	printf("  --will-msg <message>             The payload of the will "
	       "message\n");
	printf("  --will-topic <topic>             The topic of the will "
	       "message\n");
	printf("  --will-retain                    Will message as retained "
	       "message [default: false]\n");

	printf("  -s, --secure                     Enable TLS/SSL mode\n");
	printf(
	    "      --cacert <file>              CA certificates file path\n");
	printf("      -E, --cert <file>            Certificate file path\n");
	printf("      --key <file>                 Private key file path\n");
	printf("      --keypass <key password>     Private key password\n");

	if (type == PUB0) {
		printf("\n<src> may be one of:\n");
		printf("  -m, --msg  <data>                \n");
		printf("  -f, --file <file>                \n");
	}
}

static int
intarg(const char *val, int maxv)
{
	int v = 0;

	if (val[0] == '\0') {
		fatal("Empty integer argument.");
	}
	while (*val != '\0') {
		if (!isdigit(*val)) {
			fatal("Integer argument expected.");
		}
		v *= 10;
		v += ((*val) - '0');
		val++;
		if (v > maxv) {
			fatal("Integer argument too large.");
		}
	}
	if (v < 0) {
		fatal("Integer argument overflow.");
	}
	return (v);
}

// struct topic **
// addtopic(struct topic **endp, const char *s)
// {
// 	struct topic *t;

// 	if (((t = malloc(sizeof(*t))) == NULL) ||
// 	    ((t->val = malloc(strlen(s) + 1)) == NULL)) {
// 		fatal("Out of memory.");
// 	}
// 	memcpy(t->val, s, strlen(s) + 1);
// 	t->next = NULL;
// 	*endp   = t;
// 	return (&t->next);
// }

// void
// freetopic(struct topic *endp)
// {
// 	struct topic *t = endp;

// 	for (struct topic *t = endp; t != NULL; t = t->next) {
// 		if (t->val) {
// 			free(t->val);
// 			t->val = NULL;
// 		}
// 	}
// 	free(t);
// }

int
nng_client_parse_opts(int argc, char **argv, nng_proxy_opts *nng_opts)
{
	int    idx = 0;
	char * arg;
	int    val;
	int    rv;
	size_t filelen = 0;

	struct topic **topicend;
	topicend = &nng_opts->topic;

	while ((rv = nng_opts_parse(argc, argv, cmd_opts, &val, &arg, &idx)) ==
	    0) {
		switch (val) {
		case OPT_HELP:
			help(nng_opts->type);
			exit(0);
			break;
		case OPT_VERBOSE:
			nng_opts->verbose = true;
			break;
		case OPT_PARALLEL:
			nng_opts->parallel = intarg(arg, 1024000);
			break;
                //TODO tasq number
		case OPT_VERSION:
			nng_opts->version = intarg(arg, 4);
			break;
		case OPT_MQTT_URL:
			ASSERT_NULL(nng_opts->mqtt_url,
			    "URL (--url) may be specified "
			    "only once.");
			nng_opts->mqtt_url = nng_strdup(arg);
			break;
		case OPT_NNG_URL:
			ASSERT_NULL(nng_opts->nng_url,
			    "URL (--url) may be specified "
			    "only once.");
			nng_opts->nng_url = nng_strdup(arg);
			break;
		case OPT_TOPIC:
			topicend = addtopic(topicend, arg);
			nng_opts->topic_count++;
			break;
		case OPT_QOS:
			nng_opts->qos = intarg(arg, 2);
			break;
		case OPT_RETAIN:
			nng_opts->retain = true;
			break;
		case OPT_USER:
			ASSERT_NULL(nng_opts->user,
			    "User (-u, --user) may be specified "
			    "only "
			    "once.");
			nng_opts->user = nng_strdup(arg);
			break;
		case OPT_PASSWD:
			ASSERT_NULL(nng_opts->passwd,
			    "Password (-p, --password) may be "
			    "specified "
			    "only "
			    "once.");
			nng_opts->passwd = nng_strdup(arg);
			break;
		case OPT_CLIENTID:
			ASSERT_NULL(nng_opts->client_id,
			    "Identifier (-I, --identifier) may be "
			    "specified "
			    "only "
			    "once.");
			nng_opts->client_id = nng_strdup(arg);
			break;
		case OPT_KEEPALIVE:
			nng_opts->keepalive = intarg(arg, 65535);
			break;
		case OPT_CLEAN_SESSION:
			nng_opts->clean_session = strcasecmp(arg, "true") == 0;
			break;
		case OPT_WILL_MSG:
			ASSERT_NULL(nng_opts->will_msg,
			    "Will_msg (--will-msg) may be specified "
			    "only "
			    "once.");
			nng_opts->will_msg     = nng_strdup(arg);
			nng_opts->will_msg_len = strlen(arg);
			break;
		case OPT_WILL_QOS:
			nng_opts->will_qos = intarg(arg, 2);
			break;
		case OPT_WILL_RETAIN:
			nng_opts->retain = true;
			break;
		case OPT_WILL_TOPIC:
			ASSERT_NULL(nng_opts->will_topic,
			    "Will_topic (--will-topic) may be "
			    "specified "
			    "only "
			    "once.");
			nng_opts->will_topic = nng_strdup(arg);
			break;
		case OPT_SECURE:
			nng_opts->enable_ssl = true;
			break;
		case OPT_CACERT:
			ASSERT_NULL(nng_opts->cacert,
			    "CA Certificate (--cacert) may be "
			    "specified only once.");
			loadfile(
			    arg, (void **) &nng_opts->cacert, &nng_opts->cacert_len);
			break;
		case OPT_CERTFILE:
			ASSERT_NULL(nng_opts->cert,
			    "Cert (--cert) may be specified "
			    "only "
			    "once.");
			loadfile(arg, (void **) &nng_opts->cert, &nng_opts->cert_len);
			break;
		case OPT_KEYFILE:
			ASSERT_NULL(nng_opts->key,
			    "Key (--key) may be specified only once.");
			loadfile(arg, (void **) &nng_opts->key, &nng_opts->key_len);
			break;
		case OPT_KEYPASS:
			ASSERT_NULL(nng_opts->keypass,
			    "Key Password (--keypass) may be specified only "
			    "once.");
			nng_opts->keypass = nng_strdup(arg);
			break;
		case OPT_MSG:
			ASSERT_NULL(nng_opts->msg,
			    "Data (--file, --data) may be "
			    "specified "
			    "only once.");
			nng_opts->msg     = nng_strdup(arg);
			nng_opts->msg_len = strlen(arg);
			break;
		case OPT_FILE:
			ASSERT_NULL(nng_opts->msg,
			    "Data (--file, --data) may be "
			    "specified "
			    "only once.");
			loadfile(arg, (void **) &nng_opts->msg, &nng_opts->msg_len);
			break;
		}
	}
	switch (rv) {
	case NNG_EINVAL:
		fatal("Option %s is invalid.", argv[idx]);
		break;
	case NNG_EAMBIGUOUS:
		fatal("Option %s is ambiguous (specify in full).", argv[idx]);
		break;
	case NNG_ENOARG:
		fatal("Option %s requires argument.", argv[idx]);
		break;
	default:
		break;
	}

	if (!nng_opts->mqtt_url) {
		nng_opts->mqtt_url = nng_strdup("mqtt-tcp://127.0.0.1:1883");
	}
        if (!nng_opts->nng_url) {
                //FIXME error url
		return;
	}

	switch (nng_opts->type) {
	case PUB0:
		if (nng_opts->topic_count == 0) {
			fatal("Missing required option: '(-t, --topic) "
			      "PUB0 convey mqtt msg from -t to --nng_url"
			      "<topic>'\n Need an MQTT topic for subscribing msg");
		}
		break;
	case SUB0:
		if (nng_opts->topic_count == 0) {
			fatal("Missing required option: '(-t, --topic) "
			      "SUB0 convey nng msg from --nng_url( all topics ) "
			      "to --mqtt_url and topic -t"
			      "<topic>'\n Need an MQTT topic for publishing msg");
		}
		/* code */
		break;
	case CONN:
		/* code */
		break;

	default:
		break;
	}

	return rv;
}

static void
set_default_conf(nng_proxy_opts *nng_opts)
{
	nng_opts->msg_count     = 0;
	nng_opts->interval      = 10;
	nng_opts->qos           = 0;
	nng_opts->retain        = false;
	nng_opts->parallel      = 1;
	nng_opts->version       = 4;
	nng_opts->keepalive     = 60;
	nng_opts->clean_session = true;
	nng_opts->enable_ssl    = false;
	nng_opts->verbose       = false;
	nng_opts->topic_count   = 0;
	nng_opts->clients       = 1;
}

// This reads a file into memory.  Care is taken to ensure that
// the buffer is one byte larger and contains a terminating
// NUL. (Useful for key files and such.)
static void
loadfile(const char *path, void **datap, size_t *lenp)
{
	FILE * f;
	size_t total_read      = 0;
	size_t allocation_size = BUFSIZ;
	char * fdata;
	char * realloc_result;

	if (strcmp(path, "-") == 0) {
		f = stdin;
	} else {
		if ((f = fopen(path, "rb")) == NULL) {
			fatal(
			    "Cannot open file %s: %s", path, strerror(errno));
		}
	}

	if ((fdata = malloc(allocation_size + 1)) == NULL) {
		fatal("Out of memory.");
	}

	while (1) {
		total_read += fread(
		    fdata + total_read, 1, allocation_size - total_read, f);
		if (ferror(f)) {
			if (errno == EINTR) {
				continue;
			}
			fatal(
			    "Read from %s failed: %s", path, strerror(errno));
		}
		if (feof(f)) {
			break;
		}
		if (total_read == allocation_size) {
			if (allocation_size > SIZE_MAX / 2) {
				fatal("Out of memory.");
			}
			allocation_size *= 2;
			if ((realloc_result = realloc(
			         fdata, allocation_size + 1)) == NULL) {
				free(fdata);
				fatal("Out of memory.");
			}
			fdata = realloc_result;
		}
	}
	if (f != stdin) {
		fclose(f);
	}
	fdata[total_read] = '\0';
	*datap            = fdata;
	*lenp             = total_read;
}

#ifdef NNG_SUPP_TLS
static int
init_dialer_tls(nng_dialer d, const char *cacert, const char *cert,
    const char *key, const char *pass)
{
	nng_tls_config *cfg;
	int             rv;

	if ((rv = nng_tls_config_alloc(&cfg, NNG_TLS_MODE_CLIENT)) != 0) {
		return (rv);
	}

	if (cert != NULL && key != NULL) {
		nng_tls_config_auth_mode(cfg, NNG_TLS_AUTH_MODE_REQUIRED);
		if ((rv = nng_tls_config_own_cert(cfg, cert, key, pass)) !=
		    0) {
			goto out;
		}
	} else {
		nng_tls_config_auth_mode(cfg, NNG_TLS_AUTH_MODE_NONE);
	}

	if (cacert != NULL) {
		if ((rv = nng_tls_config_ca_chain(cfg, cacert, NULL)) != 0) {
			goto out;
		}
	}

	rv = nng_dialer_set_ptr(d, NNG_OPT_TLS_CONFIG, cfg);

out:
	nng_tls_config_free(cfg);
	return (rv);
}

#endif

nng_msg *
nng_publish_msg(nng_proxy_opts *nng_opts, nng_msg *msg)
{
	// create a PUBLISH message
	nng_msg *pubmsg;
	nng_mqtt_msg_alloc(&pubmsg, 0);
	nng_mqtt_msg_set_packet_type(pubmsg, NNG_MQTT_PUBLISH);
	nng_mqtt_msg_set_publish_qos(pubmsg, nng_opts->qos);
	nng_mqtt_msg_set_publish_retain(pubmsg, nng_opts->retain);
	nng_mqtt_msg_set_publish_payload(pubmsg, nng_msg_body(msg), nng_msg_len(msg));
	nng_mqtt_msg_set_publish_topic(pubmsg, nng_opts->topic->val);
	return pubmsg;
}

void
nng_client_cb(void *arg)
{
	struct work *work = arg;
	nng_msg *    msg  = NULL;
	int          rv;

	switch (work->state) {
	case INIT:
		switch (work->nng_opts->type) {
		case PUB0:
			work->state = RECV_MQTT;
			nng_ctx_recv(work->ctx, work->aio);
			break;
		case SUB0:
		case CONN:
			printf("init\n");
			work->state = RECV_NNG;
			nng_ctx_recv(work->proxy_ctx, work->aio);
			break;
		}
		break;

	case RECV_NNG:
		if ((rv = nng_aio_result(work->aio)) != 0) {
			nng_fatal("nng_recv_aio", rv);
			work->state = RECV;
			nng_ctx_recv(work->proxy_ctx, work->aio);
			break;
		}
		printf("recv %d\n", work->ctx.id);
		work->msg   = nng_aio_get_msg(work->aio);
		printf("!here msg : %s %d\n", nng_msg_body(work->msg), nng_msg_len(work->msg));
		msg = nng_publish_msg(work->nng_opts, work->msg);
		nng_msg_free(work->msg);
		work->msg = msg;
		nng_aio_set_msg(work->aio, work->msg);
		nng_ctx_send(work->ctx, work->aio);
		work->state = SEND_MQTT;
		break;

	case RECV_MQTT:
		if ((rv = nng_aio_result(work->aio)) != 0) {
			nng_fatal("nng_recv_aio", rv);
			work->state = RECV;
			nng_ctx_recv(work->ctx, work->aio);
			break;
		}
		work->msg   = nng_aio_get_msg(work->aio);
		msg = work->msg;
		work->msg = NULL;
		uint32_t payload_len;
		uint8_t *payload =
		    nng_mqtt_msg_get_publish_payload(msg, &payload_len);
		uint32_t    topic_len;
		const char *recv_topic =
		    nng_mqtt_msg_get_publish_topic(msg, &topic_len);

		printf("%.*s: %.*s\n", topic_len, recv_topic, payload_len,
		    (char *) payload);

		if (((rv = nng_msg_alloc(&work->msg, 0)) != 0) ||
		    ((rv = nng_msg_append(work->msg, payload, payload_len)) != 0)) {
			fatal(nng_strerror(rv));
		}
		nng_msg_free(msg);

		work->state = SEND_NNG;
		nng_aio_set_msg(work->aio, work->msg);
		nng_send_aio(work->nsocket, work->aio);
		// nng_ctx_send(work->proxy_ctx, work->aio);
		break;

	case SEND_MQTT:
		if ((rv = nng_aio_result(work->aio)) != 0) {
			nng_msg_free(work->msg);
			nng_fatal("nng_send_aio", rv);
		}
		printf("send %d\n", work->ctx.id);
		work->state = RECV_NNG;
		nng_ctx_recv(work->proxy_ctx, work->aio);
		break;

	case SEND_NNG:
		//recv next mqtt msg
		work->state = RECV_MQTT;
		nng_ctx_recv(work->ctx, work->aio);
		break;

	default:
		nng_fatal("bad state!", NNG_ESTATE);
		break;
	}
	return;

out:
	exit_signal = true;
}

static struct work *
alloc_work(nng_socket sock, nng_proxy_opts *nng_opts)
{
	struct work *w;
	int          rv;

	if ((w = nng_alloc(sizeof(*w))) == NULL) {
		nng_fatal("nng_alloc", NNG_ENOMEM);
	}
	if ((rv = nng_aio_alloc(&w->aio, nng_client_cb, w)) != 0) {
		nng_fatal("nng_aio_alloc", rv);
	}
	if ((rv = nng_ctx_open(&w->ctx, sock)) != 0) {
		nng_fatal("nng_ctx_open", rv);
	}
	w->nng_opts  = nng_opts;
	w->state = INIT;
	return (w);
}

static nng_msg *
connect_msg(nng_proxy_opts *nng_opts)
{
	nng_msg *msg;
	nng_mqtt_msg_alloc(&msg, 0);
	nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_CONNECT);
	nng_mqtt_msg_set_connect_proto_version(msg, nng_opts->version);
	nng_mqtt_msg_set_connect_keep_alive(msg, nng_opts->keepalive);
	nng_mqtt_msg_set_connect_clean_session(msg, nng_opts->clean_session);

	if (nng_opts->client_id) {
		nng_mqtt_msg_set_connect_client_id(msg, nng_opts->client_id);
	}
	if (nng_opts->user) {
		nng_mqtt_msg_set_connect_user_name(msg, nng_opts->user);
	}
	if (nng_opts->passwd) {
		nng_mqtt_msg_set_connect_password(msg, nng_opts->passwd);
	}
	if (nng_opts->will_topic) {
		nng_mqtt_msg_set_connect_will_topic(msg, nng_opts->will_topic);
	}
	if (nng_opts->will_qos) {
		nng_mqtt_msg_set_connect_will_qos(msg, nng_opts->will_qos);
	}
	if (nng_opts->will_msg) {
		nng_mqtt_msg_set_connect_will_msg(
		    msg, nng_opts->will_msg, nng_opts->will_msg_len);
	}
	if (nng_opts->will_retain) {
		nng_mqtt_msg_set_connect_will_retain(msg, nng_opts->will_retain);
	}

	return msg;
}

struct connect_param {
	nng_socket * sock;
	nng_proxy_opts *nng_opts;
	size_t       id;
};

static void
connect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	printf("%s: connected!\n", __FUNCTION__);
	struct connect_param *param = arg;

	if (param->nng_opts->type == PUB0 && param->nng_opts->topic_count > 0) {
		nng_msg *msg;
		nng_mqtt_msg_alloc(&msg, 0);
		nng_mqtt_msg_set_packet_type(msg, NNG_MQTT_SUBSCRIBE);

		nng_mqtt_topic_qos *topics_qos =
		    nng_mqtt_topic_qos_array_create(param->nng_opts->topic_count);

		size_t i = 0;
		for (struct topic *tp = param->nng_opts->topic;
		     tp != NULL && i < param->nng_opts->topic_count;
		     tp = tp->next, i++) {
			nng_mqtt_topic_qos_array_set(
			    topics_qos, i, tp->val, param->nng_opts->qos);
		}

		nng_mqtt_msg_set_subscribe_topics(
		    msg, topics_qos, param->nng_opts->topic_count);

		nng_mqtt_topic_qos_array_free(
		    topics_qos, param->nng_opts->topic_count);

		// Send subscribe message
		nng_sendmsg(*param->sock, msg, NNG_FLAG_NONBLOCK);
	}
}

// Disconnect message callback function
static void
disconnect_cb(nng_pipe p, nng_pipe_ev ev, void *arg)
{
	printf("disconnected\n");
}

static struct work *
nng_alloc_work(nng_socket sock, nng_socket psock, nng_proxy_opts *nng_opts)
{
	struct work *w;
	int          rv;

	if ((w = nng_alloc(sizeof(*w))) == NULL) {
		nng_fatal("nng_alloc", NNG_ENOMEM);
	}
	if ((rv = nng_aio_alloc(&w->aio, nng_client_cb, w)) != 0) {
		nng_fatal("nng_aio_alloc", rv);
	}
	if ((rv = nng_ctx_open(&w->ctx, sock)) != 0) {
		nng_fatal("nng_ctx_open", rv);
	}
	switch (nng_opts->type) {
	case SUB0:
		if ((rv = nng_ctx_open(&w->proxy_ctx, psock)) != 0) {
			nng_fatal("nng_ctx_open", rv);
		}
		nng_ctx_setopt(w->proxy_ctx, NNG_OPT_SUB_SUBSCRIBE, "", 0);
		break;
	case PUB0:
		w->nsocket = psock;
		// nng_ctx_setopt(w->proxy_ctx, NNG_OPT_SUB_SUBSCRIBE, "", 0);
		break;
	case PAIR0:
		nng_ctx_setopt(w->proxy_ctx, NNG_OPT_SUB_SUBSCRIBE, "", 0);
		break;
	default:
		break;
	}

	w->nng_opts  = nng_opts;
	w->state = INIT;
	return (w);
}

static void
create_client(nng_socket *sock, nng_socket psock, struct work **works,
    size_t nwork, struct connect_param *param)
{
	int        rv;
	nng_dialer dialer;
	nng_socket s;

	if ((rv = nng_mqtt_client_open(sock)) != 0) {
		nng_fatal("nng_socket", rv);
	}

	for (size_t i = 0; i < nng_opts->parallel; i++) {
		works[i] = nng_alloc_work(*sock, psock, nng_opts);
	}

	nng_msg *msg = connect_msg(nng_opts);

	if ((rv = nng_dialer_create(&dialer, *sock, nng_opts->mqtt_url)) != 0) {
		nng_fatal("nng_dialer_create", rv);
	}

#ifdef NNG_SUPP_TLS
	if (nng_opts->enable_ssl) {
		if ((rv = init_dialer_tls(dialer, nng_opts->cacert, nng_opts->cert,
		         nng_opts->key, nng_opts->keypass)) != 0) {
			fatal("init_dialer_tls", rv);
		}
	}
#endif

	nng_dialer_set_ptr(dialer, NNG_OPT_MQTT_CONNMSG, msg);

	param->sock = sock;
	param->nng_opts = nng_opts;

	nng_mqtt_set_connect_cb(*sock, connect_cb, param);
	nng_mqtt_set_disconnect_cb(*sock, disconnect_cb, msg);

	nng_dialer_start(dialer, NNG_FLAG_NONBLOCK);

	for (size_t i = 0; i < nng_opts->parallel; i++) {
		nng_client_cb(works[i]);
	}
}

static void
nng_proxy_client(int argc, char **argv, enum nng_proto type)
{
	int rv;
	nng_socket   s;		//nng socket
	nng_listener l;
	nng_dialer   d;
	nng_opts = nng_zalloc(sizeof(nng_proxy_opts));
	set_default_conf(nng_opts);
	nng_opts->type = type;

	nng_client_parse_opts(argc, argv, nng_opts);
	struct connect_param *param = nng_zalloc(sizeof(struct connect_param *));
	//mqtt socket
	nng_socket *socket = nng_zalloc(sizeof(nng_socket *));
	switch (nng_opts->type) {
	case SUB0:
		if ((rv = nng_sub0_open(&s)) != 0) {
			nng_fatal("nng_socket", rv);
		}

		rv = nng_listener_create(&l, s, nng_opts->nng_url);
		nng_listener_start(l, 0);
		// nng_listener_get(l, nng_opts->nng_url, NULL, 0);
		break;
	case PUB0:
		if ((rv = nng_pub0_open(&s)) != 0) {
			nng_fatal("nng_socket", rv);
		}

		rv = nng_dialer_create(&d, s, nng_opts->nng_url);
		rv  = nng_dialer_start(d, 0);
		printf("Connected to: %d\n", rv);
		break;
	default:
		break;
	}
	struct work *works[nng_opts->parallel];

	param  = nng_zalloc(sizeof(struct connect_param));
	socket = nng_zalloc(sizeof(nng_socket));
	create_client(socket, s, works, nng_opts->parallel, param);

	while (!exit_signal) {
		nng_msleep(1000);
	}

		nng_free(param, sizeof(struct connect_param));
		nng_free(socket, sizeof(nng_socket));

		for (size_t k = 0; k < nng_opts->parallel; k++) {
			nng_aio_free(works[k]->aio);
			if (works[k]->msg) {
				nng_msg_free(works[k]->msg);
				works[k]->msg = NULL;
			}

			nng_free(works[k], sizeof(struct work));
		}

	nng_free(param, sizeof(struct connect_param *));
	nng_free(socket, sizeof(nng_socket *));

	nng_client0_stop(argc, argv);
}

int
nng_pub0_start(int argc, char **argv)
{
	nng_proxy_client(argc, argv, PUB0);
	return 0;
}

int
nng_sub0_start(int argc, char **argv)
{
	nng_proxy_client(argc, argv, SUB0);
	return 0;
}

int
nng_proxy_start(int argc, char **argv)
{
	if (strncmp(argv[0], "sub0", 3) == 0)
		nng_proxy_client(argc-1, argv+1, SUB0);
	else if (strncmp(argv[0], "pub0", 3) == 0)
		nng_proxy_client(argc-1, argv+1, PUB0);
	// else if (strncmp(argv[0], "req0", 3) == 0)
	// 	nng_proxy_client(argc-1, argv+1, SUB0);
	else
		help(SUB0);
	return 0;
}

int
nng_pub0_dflt(int argc, char **argv)
{
	help(PUB0);
	return 0;
}

int
nng_sub0_dflt(int argc, char **argv)
{
	help(SUB0);
	return 0;
}

int
nng_client0_stop(int argc, char **argv)
{
	if (nng_opts) {
		if (nng_opts->mqtt_url) {
			nng_strfree(nng_opts->mqtt_url);
		}
		if (nng_opts->topic) {
			freetopic(nng_opts->topic);
		}
		if (nng_opts->user) {
			nng_strfree(nng_opts->user);
		}
		if (nng_opts->passwd) {
			nng_strfree(nng_opts->passwd);
		}
		if (nng_opts->client_id) {
			nng_strfree(nng_opts->client_id);
		}
		if (nng_opts->msg) {
			nng_free(nng_opts->msg, nng_opts->msg_len);
		}
		if (nng_opts->will_msg) {
			nng_free(nng_opts->will_msg, nng_opts->will_msg_len);
		}
		if (nng_opts->will_topic) {
			nng_strfree(nng_opts->will_topic);
		}
		if (nng_opts->cacert) {
			nng_free(nng_opts->cacert, nng_opts->cacert_len);
		}
		if (nng_opts->cert) {
			nng_free(nng_opts->cert, nng_opts->cert_len);
		}
		if (nng_opts->key) {
			nng_free(nng_opts->key, nng_opts->key_len);
		}
		if (nng_opts->keypass) {
			nng_strfree(nng_opts->keypass);
		}

		free(nng_opts);
	}

	return 0;
}

#endif
