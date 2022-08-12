#include "knot/include/module.h"
#include "knot/conf/schema.h"

const yp_item_t queryignoretsig_conf[] = {
	{ NULL }
};

int queryignoretsig_conf_check(knotd_conf_check_args_t *args)
{
	return KNOT_EOK;
}

static knotd_state_t queryignoretsig_handle(knotd_state_t state, knot_pkt_t *pkt,
                                  knotd_qdata_t *qdata, knotd_mod_t *mod)
{
	assert(pkt && qdata && mod);

	if (qdata->type != KNOTD_QUERY_TYPE_NORMAL) {
	    return state;
	}

	qdata->query->tsig_rr = NULL;
	return state;
}

int queryignoretsig_load(knotd_mod_t *mod)
{
	return knotd_mod_hook(mod, KNOTD_STAGE_BEGIN, queryignoretsig_handle);
}

void queryignoretsig_unload(knotd_mod_t *mod)
{
}

KNOTD_MOD_API(queryignoretsig, KNOTD_MOD_FLAG_SCOPE_ZONE,
              queryignoretsig_load, queryignoretsig_unload, queryignoretsig_conf, queryignoretsig_conf_check);
