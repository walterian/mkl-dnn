/*******************************************************************************
* Copyright 2019 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include <sstream>

#include "dnnl.h"

#include "src/common/dnnl_thread.hpp"

#include "dnnl_common.hpp"
#include "dnnl_memory.hpp"
#include "norm.hpp"

#include "resampling/resampling.hpp"

namespace resampling {

inline int compare_dat(const prb_t *p, data_kind_t kind, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp, res_t *r) {
    const auto nelems = mem_dt.nelems();
    r->errors = 0;
    r->total = nelems;
    const float trh
            = p->alg == nearest ? 0 : (p->dt == dnnl_bf16 ? 1e-2 : 1e-6);

    for (int64_t i = 0; i < nelems; ++i) {
        const float dt = mem_dt.get_elem(i);
        const float fp0 = mem_fp.get_elem(i);
        const float fp = maybe_saturate(p->dt, fp0);

        const float diff = fabsf(fp - dt);
        const float rel_diff = diff / (fabsf(fp) > FLT_MIN ? fabsf(fp) : 1);
        const bool ok = (fabsf(fp) > 1e-5 ? rel_diff : diff) <= trh;

        r->errors += !ok;

        if ((!ok && (r->errors < 10 || verbose >= 10))
                || (verbose >= 50 && i < 30)) {
            int64_t mb = 0, ic = 0, d = 0, h = 0, w = 0;
            switch (kind) {
                case SRC: inv_src_off_f(p, i, mb, ic, d, h, w); break;
                case DST: inv_dst_off_f(p, i, mb, ic, d, h, w); break;
            }
            print(0,
                    "[%4ld][" IFMT "," IFMT "," IFMT "," IFMT "," IFMT
                    "] "
                    "fp:%8g fp0:%8g dt:%8g diff:%8g rdiff:%8g\n",
                    (long)i, mb, ic, d, h, w, fp, fp0, dt, diff, rel_diff);
        }
    }

    if (r->errors) r->state = FAILED;

    if (r->state == UNTESTED) r->state = PASSED; /* optimism */

    return r->state == FAILED ? FAIL : OK;
}

int compare_src(
        const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *r) {
    return compare_dat(p, SRC, mem_dt, mem_fp, r);
}

int compare_dst(
        const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *r) {
    return compare_dat(p, DST, mem_dt, mem_fp, r);
}

int fill_dat(const prb_t *p, data_kind_t kind, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp, res_t *r) {
    const auto nelems = mem_fp.nelems();
    const auto dt = p->dt;
    const int range = 16;
    const int f_min = 0;

    dnnl::impl::parallel_nd(nelems, [&](int64_t i) {
        const int gen = ((97 * i) - 17 * kind + 101) % (range + 1);
        const float value = (dt == dnnl_bf16 || dt == dnnl_f16)
                ? (f_min + gen) / range
                : (f_min + gen) * (1.0f + 4.0f / range);
        mem_fp.set_elem(i, maybe_saturate(dt, value));
    });

    SAFE(mem_dt.reorder(mem_fp), WARN);

    return OK;
}

int fill_src(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *r) {
    return fill_dat(p, SRC, mem_dt, mem_fp, r);
}

int fill_dst(const prb_t *p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp, res_t *r) {
    return fill_dat(p, DST, mem_dt, mem_fp, r);
}

int init_pd(const prb_t *p, dnnl_primitive_desc_t &rpd, res_t *r) {
    dnnl_memory_desc_t src_d, dst_d;

    dnnl_dims_t src_1d_dims = {p->mb, p->ic, p->iw};
    dnnl_dims_t src_2d_dims = {p->mb, p->ic, p->ih, p->iw};
    dnnl_dims_t src_3d_dims = {p->mb, p->ic, p->id, p->ih, p->iw};
    dnnl_dim_t *src_dims = p->ndims == 5
            ? src_3d_dims
            : p->ndims == 4 ? src_2d_dims : src_1d_dims;

    dnnl_dims_t dst_1d_dims = {p->mb, p->ic, p->ow};
    dnnl_dims_t dst_2d_dims = {p->mb, p->ic, p->oh, p->ow};
    dnnl_dims_t dst_3d_dims = {p->mb, p->ic, p->od, p->oh, p->ow};
    dnnl_dim_t *dst_dims = p->ndims == 5
            ? dst_3d_dims
            : p->ndims == 4 ? dst_2d_dims : dst_1d_dims;

    dnnl_format_tag_t tag_src
            = (p->dir & FLAG_FWD) ? p->tag : dnnl_format_tag_any;
    dnnl_format_tag_t tag_dst
            = (p->dir & FLAG_BWD) ? p->tag : dnnl_format_tag_any;

    DNN_SAFE(dnnl_memory_desc_init_by_tag(
                     &src_d, p->ndims, src_dims, p->dt, tag_src),
            WARN);

    DNN_SAFE(dnnl_memory_desc_init_by_tag(
                     &dst_d, p->ndims, dst_dims, p->dt, tag_dst),
            WARN);

    dnnl_alg_kind_t alg = alg2alg_kind(p->alg);
    dnnl_resampling_desc_t pd;

    if (p->dir & FLAG_FWD) {
        auto prop_kind = p->dir & FLAG_INF ? dnnl_forward_inference
                                           : dnnl_forward_training;
        DNN_SAFE(dnnl_resampling_forward_desc_init(
                         &pd, prop_kind, alg, nullptr, &src_d, &dst_d),
                WARN);
    } else {
        DNN_SAFE(dnnl_resampling_backward_desc_init(
                         &pd, alg, nullptr, &src_d, &dst_d),
                WARN);
    }

    dnnl_primitive_desc_t hint = NULL;
    if (p->dir & FLAG_BWD) {
        dnnl_resampling_desc_t rd_fwd;
        DNN_SAFE(dnnl_resampling_forward_desc_init(&rd_fwd,
                         dnnl_forward_training, alg, nullptr, &src_d, &dst_d),
                WARN);
        dnnl_status_t init_fwd_status = dnnl_primitive_desc_create(
                &hint, &rd_fwd, NULL, engine_tgt, NULL);
        if (init_fwd_status == dnnl_unimplemented)
            return r->state = UNIMPLEMENTED, OK;
        else
            SAFE(init_fwd_status, WARN);
    }

    dnnl_status_t init_status = dnnl_success;
    init_status = dnnl_primitive_desc_create(&rpd, &pd, NULL, engine_tgt, hint);

    if (init_status == dnnl_unimplemented)
        return r->state = UNIMPLEMENTED, OK;
    else
        SAFE(init_status, WARN);

    const char *impl_str = query_impl_info(rpd);
    if (maybe_skip(skip_impl, impl_str)) {
        print(2, "SKIPPED: dnnl implementation: %s\n", impl_str);
        DNN_SAFE(dnnl_primitive_desc_destroy(rpd), WARN);
        return r->state = SKIPPED, OK;
    } else {
        print(5, "dnnl implementation: %s\n", impl_str);
    }

    return OK;
}

int doit(const prb_t *p, res_t *r) {
    if (bench_mode == LIST) return r->state = LISTED, OK;

    dnnl_primitive_desc_t rpd;
    dnnl_primitive_t rp;

    SAFE(init_pd(p, rpd, r), WARN);
    if (r->state == SKIPPED || r->state == UNIMPLEMENTED) { return OK; }

    DNN_SAFE(dnnl_primitive_create(&rp, rpd), WARN);

    auto q_md = [](const_dnnl_primitive_desc_t pd, dnnl_query_t what) {
        const dnnl_memory_desc_t *md
                = dnnl_primitive_desc_query_md(pd, what, 0);
        SAFE_V(md != nullptr ? OK : FAIL);
        return md;
    };

    const auto &src_desc = p->dir == BWD_D ? *q_md(rpd, dnnl_query_diff_src_md)
                                           : *q_md(rpd, dnnl_query_src_md);
    const auto &dst_desc = p->dir == BWD_D ? *q_md(rpd, dnnl_query_diff_dst_md)
                                           : *q_md(rpd, dnnl_query_dst_md);
    dnn_mem_t src_dt(src_desc, p->dt, engine_tgt);
    dnn_mem_t dst_dt(dst_desc, p->dt, engine_tgt);
    dnn_mem_t d_src_dt, d_dst_dt;

    const auto tag = get_default_tag(src_dt.md_.ndims);
    const auto fp = dnnl_f32;

    dnn_mem_t src_fp(src_desc, fp, tag, engine_tgt);
    dnn_mem_t dst_fp(dst_desc, fp, tag, engine_tgt);
    dnn_mem_t d_dst_fp, d_src_fp;

    SAFE(fill_src(p, src_dt, src_fp, r), WARN);
    SAFE(fill_dst(p, dst_dt, dst_fp, r), WARN);

    args_t args_fwd, args_bwd;
    args_t args;

    if (p->dir & FLAG_FWD) {
        args.set(DNNL_ARG_SRC, src_dt);
        args.set(DNNL_ARG_DST, dst_dt);
        DNN_SAFE(execute_and_wait(rp, stream_tgt, args), WARN);
        if (bench_mode & CORR) {
            compute_ref_fwd(p, src_fp, dst_fp);
            if (p->dir & FLAG_FWD) {
                dnn_mem_t dst(dst_dt, fp, tag, engine_tgt);
                SAFE(compare_dst(p, dst, dst_fp, r), WARN);
            }
        }
    } else {
        args.set(DNNL_ARG_DIFF_DST, dst_dt);
        args.set(DNNL_ARG_DIFF_SRC, src_dt);

        DNN_SAFE(execute_and_wait(rp, stream_tgt, args), WARN);

        if (bench_mode & CORR) {
            compute_ref_bwd(p, src_fp, dst_fp);
            dnn_mem_t diff_src(src_dt, fp, tag, engine_tgt);
            SAFE(compare_src(p, diff_src, src_fp, r), WARN);
        }
    }

    measure_perf(r->timer, rp, args);

    DNN_SAFE(dnnl_primitive_destroy(rp), CRIT);
    DNN_SAFE(dnnl_primitive_desc_destroy(rpd), CRIT);

    return OK;
}

} // namespace resampling
