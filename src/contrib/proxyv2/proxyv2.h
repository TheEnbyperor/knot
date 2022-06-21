/*  Copyright (C) 2021 Fastly, Inc.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <sys/socket.h>
#include <stddef.h>

#include "libknot/mm_ctx.h"
#include "libknot/packet/pkt.h"
#include "knot/include/module.h"
#include "contrib/sockaddr.h"

int proxyv2_header_offset(void *base, size_t len_base);
int proxyv2_sockaddr_store(void *base, size_t len_base, struct sockaddr_storage *ss);

// int proxyv2_decapsulate(void *base,
// 			size_t len_base,
// 			knot_pkt_t **query,
// 			knotd_qdata_params_t *params,
// 			struct sockaddr_storage *client,
// 			knot_mm_t *mm);
