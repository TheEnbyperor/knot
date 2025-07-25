#include "knot/include/module.h"
#include "knot/conf/schema.h"
#include "knot/query/requestor.h"
#include "knot/query/capture.h"

#define MOD_REMOTE		"\x06""remote"
#define MOD_TIMEOUT		"\x07""timeout"

const yp_item_t updateproxy_conf[] = {
	{ MOD_REMOTE,         YP_TREF,  YP_VREF = { C_RMT }, YP_FNONE, { knotd_conf_check_ref } },
	{ MOD_TIMEOUT,        YP_TINT,  YP_VINT = { 0, INT32_MAX, 500 } },
	{ NULL }
};

int updateproxy_conf_check(knotd_conf_check_args_t *args)
{
	knotd_conf_t rmt = knotd_conf_check_item(args, MOD_REMOTE);
	if (rmt.count == 0) {
		args->err_str = "no remote server specified";
		return KNOT_EINVAL;
	}

	return KNOT_EOK;
}

typedef struct {
	struct sockaddr_storage remote;
	struct sockaddr_storage via;
	int timeout;
} updateproxy_t;

static knotd_state_t updateproxy_fwd(knotd_state_t state, knot_pkt_t *pkt,
                                  knotd_qdata_t *qdata, knotd_mod_t *mod)
{
	assert(pkt && qdata && mod);

	updateproxy_t *proxy = knotd_mod_ctx(mod);

	if (qdata->type != KNOTD_QUERY_TYPE_UPDATE) {
	    return state;
	}

	if (qdata->query->tsig_rr != NULL) {
	int ret = knot_tsig_append(qdata->query->wire, &qdata->query->size, 65535, qdata->query->tsig_rr);
        if (ret != KNOT_EOK) {
            knotd_mod_log(mod, LOG_ERR, "Failed to add TSIG to update request (%s)", knot_strerror(ret));
            qdata->rcode = KNOT_RCODE_SERVFAIL;
	    return KNOTD_STATE_FAIL;
        }
	}

	const knot_layer_api_t *capture = query_capture_api();
	struct capture_param capture_param = {
		.sink = pkt
	};

	knot_requestor_t re;
	int ret = knot_requestor_init(&re, capture, &capture_param, qdata->mm);
	if (ret != KNOT_EOK) {
		return state;
	}

	knot_request_flag_t flags = KNOT_REQUEST_NONE;
	const struct sockaddr_storage *dst = &proxy->remote;
	const struct sockaddr_storage *src = &proxy->via;
	knot_request_t *req = knot_request_make_generic(re.mm, dst, src, qdata->query, NULL, NULL, NULL, NULL, 0, flags);
	if (req == NULL) {
		knot_requestor_clear(&re);
		return state;
	}

	ret = knot_requestor_exec(&re, req, proxy->timeout);

	knot_request_free(req, re.mm);
	knot_requestor_clear(&re);

	if (ret != KNOT_EOK) {
	    knotd_mod_log(mod, LOG_ERR, "Failed to forward update request (%s)", knot_strerror(ret));
		qdata->rcode = KNOT_RCODE_SERVFAIL;
		return KNOTD_STATE_FAIL;
	} else {
		qdata->rcode = knot_pkt_ext_rcode(pkt);
		if (pkt->tsig_rr != NULL) {
            knot_tsig_append(pkt->wire, &pkt->size, pkt->max_size, pkt->tsig_rr);
        }

		return KNOTD_STATE_FINAL;
	}
}

int updateproxy_load(knotd_mod_t *mod)
{
	updateproxy_t *proxy = calloc(1, sizeof(*proxy));
	if (proxy == NULL) {
		return KNOT_ENOMEM;
	}

	knotd_conf_t remote = knotd_conf_mod(mod, MOD_REMOTE);
	knotd_conf_t conf = knotd_conf(mod, C_RMT, C_ADDR, &remote);
	if (conf.count > 0) {
		proxy->remote = conf.multi[0].addr;
		knotd_conf_free(&conf);
	}
	conf = knotd_conf(mod, C_RMT, C_VIA, &remote);
	if (conf.count > 0) {
		proxy->via = conf.multi[0].addr;
		knotd_conf_free(&conf);
	}

	conf = knotd_conf_mod(mod, MOD_TIMEOUT);
	proxy->timeout = conf.single.integer;

	knotd_mod_ctx_set(mod, proxy);

	return knotd_mod_hook(mod, KNOTD_STAGE_BEGIN, updateproxy_fwd);
}

void updateproxy_unload(knotd_mod_t *mod)
{
	updateproxy_t *ctx = knotd_mod_ctx(mod);
	free(ctx);
}

KNOTD_MOD_API(updateproxy, KNOTD_MOD_FLAG_SCOPE_ZONE,
              updateproxy_load, updateproxy_unload, updateproxy_conf, updateproxy_conf_check);
