/*******************************************************************************
* Copyright 2018-2019 Intel Corporation
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

#include <float.h>
#include <math.h>
#include <random>
#include <stdio.h>
#include <stdlib.h>

#include "dnnl.h"

#include "src/common/dnnl_thread.hpp"

#include "dnnl_common.hpp"
#include "dnnl_memory.hpp"
#include "norm.hpp"

#include "rnn/rnn.hpp"
#include "rnn/rnn_aux.hpp"

#define CALL_DNNL_RNN 1

#define COMPARE_DAT(rc, a, lay) \
    do { \
        dnn_mem_t CONCAT2(a, _dt_plain)(CONCAT2(a, _dt), fp, lay, engine_tgt); \
        rc |= compare_dat( \
                p, a, CONCAT2(a, _dt_plain), CONCAT2(a, _fp), r, true); \
    } while (0)

// Using hidden attr API for testing RNN
dnnl_status_t dnnl_primitive_attr_set_rnn_tparams(dnnl_primitive_attr_t attr,
        bool mode, dnnl_dim_t ngates, const float *scales, float cscale);

namespace rnn {

void create_dnnl_rnn_attr(const prb_t &p, dnnl_primitive_attr_t *dnnl_attr) {
    DNN_SAFE_V(dnnl_primitive_attr_create(dnnl_attr));

    if (p.skip_nonlinear)
        DNN_SAFE_V(dnnl_primitive_attr_set_rnn_tparams(*dnnl_attr, true,
                p.n_gates(), p.linear_scales, p.linear_cscale));

    if (p.scale_policy == policy_t::PER_OC) {
        DNN_SAFE_V(dnnl_primitive_attr_set_rnn_weights_qparams(
                *dnnl_attr, p.dic * p.n_gates(), 0x18, p.wei_oc_scales));
    } else if (p.scale_policy == policy_t::COMMON && p.wei_scale != 1.) {
        DNN_SAFE_V(dnnl_primitive_attr_set_rnn_weights_qparams(
                *dnnl_attr, 1, 0, &p.wei_scale));
    }

    if (p.data_scale != 1.0 || p.data_shift != 0.0) {
        DNN_SAFE_V(dnnl_primitive_attr_set_rnn_data_qparams(
                *dnnl_attr, p.data_scale, p.data_shift));
    }
}

int fill_memory(const prb_t &p, rnn_data_kind_t kind, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp, dnnl_data_type_t dt, float mean, float stddev,
        float min, float max, const_dnnl_primitive_attr_t attr = nullptr) {
#ifdef CALL_DNNL_RNN
    const auto nelems = mem_dt.nelems();
    assert(mem_dt.nelems() == mem_fp.nelems());
#else
    const auto nelems = mem_fp.nelems();
#endif

    // For non-int8 RNN the data is filled according to cfg directly.
    // However, for int8 RNN we have slightly obscure logic, at least for now:
    // 1. cfg describes the quantized data;
    // 2. We fill first f32 de-quantized data, by inverse-applying the scale
    //    and shift to the data generated by cfg distribution;
    // 3. We reorder the data for the DNNL RNN primitive
    // 4. Q10n of the data for reference benchdnn RNN:
    //    4.a. If the tensor is weights -- q10n it here;
    //    4.b. If the tensor is data -- reference benchdnn RNN will quantize it.

    // default, nothing to be done
    float scale = 1.f, shift = 0.f;
    bool need_recompute_scale = false;
    const_dnnl_primitive_attr_t reorder_attr = nullptr;

    if (p.is_int8()) {
        if (kind == weights_input || kind == weights_states) {
            need_recompute_scale = p.scale_policy == policy_t::PER_OC;
            if (!need_recompute_scale) scale = p.wei_scale;
        } else if (dt == dnnl_u8 && (kind == input || kind == states)) {
            scale = p.data_scale;
            shift = p.data_shift;
        }
        // pass rnn attributes to f32 -> int8 reorders
        if (dt != dnnl_f32) reorder_attr = attr;
    }

    /* Do fixed partitioning to have same filling for any number of threads */
    const int64_t n_chunks = 16;
    const int64_t chunk_size = div_up(nelems, n_chunks);
    dnnl::impl::parallel_nd(n_chunks, [&](int idx_chunk) {
        int64_t idx_start = idx_chunk * chunk_size;
        int64_t idx_end = MIN2(idx_start + chunk_size, nelems);
        std::minstd_rand msr;
        msr.seed(idx_start + kind);
        std::normal_distribution<float> gen(mean, stddev);
        for (int64_t idx = idx_start; idx < idx_end; ++idx) {
            float val = (dt == dnnl_f32) ? gen(msr) : round(gen(msr));
            val = MAX2(MIN2(val, max), min);

            const float current_scale = need_recompute_scale
                    ? p.wei_oc_scales[idx % (p.dic * p.n_gates())]
                    : scale;
            val = (val - shift) / current_scale; // change only int8-case

            mem_fp.set_elem(idx, val);
        }
    });

    mem_dt.reorder(mem_fp, {reorder_attr});

    // Bullet 4.a holds: quantize weights for int8 benchdnn reference RNN
    if (p.is_int8() && (kind == weights_input || kind == weights_states)) {
        dnnl::impl::parallel_nd(n_chunks, [&](int idx_chunk) {
            int64_t idx_start = idx_chunk * chunk_size;
            int64_t idx_end = MIN2(idx_start + chunk_size, nelems);
            for (int64_t idx = idx_start; idx < idx_end; ++idx) {
                const float current_scale = need_recompute_scale
                        ? p.wei_oc_scales[idx % (p.dic * p.n_gates())]
                        : scale;

                float val = ((float *)mem_fp)[idx];
                val = round(current_scale * val);

                mem_fp.set_elem(idx, MAX2(MIN2(val, max), min));
            }
        });
    }

    return OK;
}

int fill_memory(const prb_t &p, rnn_data_kind_t kind, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp, const_dnnl_primitive_attr_t attr = nullptr) {
    dt_conf_t c = p.cfg[kind];
    return fill_memory(p, kind, mem_dt, mem_fp, c.dt, c.f_mean, c.f_stddev,
            c.f_min, c.f_max, attr);
}

int fill_c_states(const prb_t &p, dnn_mem_t &mem_dt, dnn_mem_t &mem_fp,
        const_dnnl_primitive_attr_t attr = nullptr) {
    const bool special_case = p.prop == dnnl_backward && p.skip_nonlinear;
    if (!special_case) return fill_memory(p, c_states, mem_dt, mem_fp, attr);

    // The scaling factors in tparams when testing backward are common for
    // for forward and backward passes, and computed as 1 over maximum of
    // the accumulation chain:
    // - ~n_gates on FWD
    // - ~dic * n_gates on BWD_D
    // - ~mb * n_gates on BWD_W
    //
    // This makes tparam relatively small for the forward pass (compare to
    // the forward pass when we test forward only). This in turn, makes
    // c_states converge relatively fast to the value ~ i_gate * c_gate,
    // which is (typically) way smaller than the original distribution for
    // c_states.
    //
    // TODO: use different tparams for forward and
    //       backward passes when testing BWD_DW.
    //
    // The problem appears on backward pass. Consider diff_f_gate that
    // contributes to backward weights when batch or number of iterations
    // is big:
    //   diff_f_gate[iter] = src_c_state[iter] * diff_dst[iter].
    //   diff_weights += ~diff_f_gate[iter].
    //
    // Assume, that diff_dst[iter] is always about the same for every iter.
    // Since src_c_state[0] >> src_c_state[iter] for iter > 0, this makes the
    // diff_weight be highly dependent on the order of accumulating the
    // diff_f_gate[iter].
    //
    // Originally we had something like:
    // diff_weights = v + v * 10^-5 + ... + v * 10^-5 (n_iter * MB summands).
    // Depending on the order of summation the difference might exceed the
    // typical bound approximation: coefficient * log(number_of_summands).
    //
    // Anyways, the algorithm below tries to put the first src_c_state[iter = 0]
    // in the same ballpark as all the subsequent src_c_state[iter > 0].
    //
    // The estimation is based on the following rough assumptions:
    //   c_state[iter+1] = f_gate * c_state[iter] + i_gate * c_gate
    //                  ~= f_gate * small_value   + i_gate * c_gate
    //                  ~=                          i_gate * c_gate.
    //   i_gate ~= tparams[i_gate] * (
    //              1 / ngates * mean_src_layer +
    //              1 / ngates * mean_src_iter  +
    //              mean_bias);
    //
    // Same for c_gate.
    // The (1 / ngates) factor is taken from fill_weights().

    float expect_gemm_output = (1.f / p.n_gates()) * p.cfg[input].f_mean
            + (1.f / p.n_gates()) * p.cfg[states].f_mean + p.cfg[bias].f_mean;
    float expect_i_gate = (float)p.linear_scales[LSTM_I] * expect_gemm_output;
    float expect_c_gate = (float)p.linear_scales[LSTM_C] * expect_gemm_output;
    float expect_c_state_mean = expect_i_gate * expect_c_gate;

    float adjust_factor = 1;

    const bool need_adjust = expect_c_state_mean < p.cfg[c_states].f_mean
            && p.cfg[c_states].f_mean != 0;
    if (need_adjust)
        adjust_factor = expect_c_state_mean / p.cfg[c_states].f_mean;

    dt_conf_t c = p.cfg[c_states];
    return fill_memory(p, c_states, mem_dt, mem_fp, c.dt,
            c.f_mean * adjust_factor, c.f_stddev * adjust_factor,
            c.f_min * adjust_factor, c.f_max * adjust_factor, attr);
}

int fill_weights(const prb_t &p, rnn_data_kind_t kind, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp, const_dnnl_primitive_attr_t attr = nullptr) {

    dt_conf_t c = p.cfg[kind];
    if (c.dt == dnnl_s8) return fill_memory(p, kind, mem_dt, mem_fp, attr);

    auto dims = mem_fp.md_.dims;
    auto L = dims[0];
    auto D = dims[1];
    auto I = dims[2];
    auto G = dims[3];
    auto O = dims[4];

    for (int64_t i = 0; i < mem_dt.nelems(); i++)
        mem_fp.set_elem(i, 0.0f);
    for (int64_t l = 0; l < L; l++)
        for (int64_t d = 0; d < D; d++)
            for (int64_t g = 0; g < G; g++)
                for (int64_t o = 0; o < O; o++) {
                    auto i_off = ((o + g * 7 + d * 11 + l * 13) % I);
                    mem_fp.set_elem(l * D * I * G * O + d * I * G * O
                                    + i_off * G * O + g * O + o,
                            1.0f / p.n_gates());
                }
    mem_dt.reorder(mem_fp);
    return OK;
}

int fill_bias(const prb_t &p, rnn_data_kind_t kind, dnn_mem_t &mem_dt,
        dnn_mem_t &mem_fp) {
    // To reduce likelihood of cancellation happening in bwd by bias,
    // (especially for GRU), we want diff_bias to be sparse
    auto dims = mem_fp.md_.dims;
    auto L = dims[0];
    auto D = dims[1];
    auto G = dims[2];
    auto O = dims[3];

    std::minstd_rand msr;
    std::normal_distribution<float> gen(
            p.cfg[kind].f_mean, p.cfg[kind].f_stddev);
    msr.seed(kind);

    for_(int64_t l = 0; l < L; l++)
    for_(int64_t d = 0; d < D; d++)
    for_(int64_t g = 0; g < G; g++)
    for (int64_t o = 0; o < O; o++) {
        auto idx = l * D * G * O + d * G * O + g * O + o;
        auto val = gen(msr) * flip_coin(idx, 0.05f);
        mem_fp.set_elem(idx, val);
    }
    mem_dt.reorder(mem_fp);
    return OK;
}

inline int init_pd(const prb_t &p, dnnl_rnn_desc_t rd[2],
        dnnl_primitive_desc_t rpd[2], res_t *r) {
    dnnl_prop_kind_t fwd_prop = dnnl_prop_kind_undef;
    bool is_bwd = false;
    switch (p.prop) {
        case dnnl_forward: fwd_prop = dnnl_forward_inference; break;
        case dnnl_backward:
            // If we are testing backward, we have to run forward training first
            // in order to generate a valid workspace.
            fwd_prop = dnnl_forward_training;
            is_bwd = true;
            break;
        default: DNN_SAFE(dnnl_invalid_arguments, CRIT);
    }

    const bool is_gru_lbr = p.alg == LBR_GRU;
    // Enable testing non trivial strides in correctness mode and non-int8
    // FIXME: enable the stride testing back when the corresponding missing
    //        reorder added to the library;
    // TODO:  testing with non-trivial stride should be a testing option!
    int the_stride = (bench_mode == CORR && !p.is_int8()) ? 1 : 0;
    /// @todo we need to add stride support for diff_* tensors too
    dnnl_memory_desc_t input_d, states_d, c_states_d, weights_input_d,
            weights_states_d, bias_d, dst_last_layer_d, dst_last_iteration_d,
            dst_c_last_iteration_d, diff_input_d, diff_states_d,
            diff_c_states_d, diff_weights_input_d, diff_weights_states_d,
            diff_bias_d, diff_last_layer_d, diff_last_iteration_d,
            diff_c_last_iteration_d;

    // dimensions with ref
    dnnl_dims_t input_dims = {p.n_iter, p.mb, p.slc};
    // bidirectional = 2, s for lstm = 2, for all other = 1
    dnnl_dims_t weights_input_dims
            = {p.n_layer, p.n_dir(), p.slc, p.n_gates(), p.dic};
    dnnl_dims_t weights_states_dims
            = {p.n_layer, p.n_dir(), p.sic, p.n_gates(), p.dic};
    dnnl_dims_t bias_dims
            = {p.n_layer, p.n_dir(), p.n_gates() + is_gru_lbr, p.dic};
    // dnnl_tnc
    int64_t lastlay_dlc
            = (p.direction == dnnl_bidirectional_concat) ? 2 * p.dlc : p.dlc;
    dnnl_dims_t dst_last_layer_dims = {p.n_iter, p.mb, lastlay_dlc};

    DNN_SAFE(dnnl_memory_desc_init_by_tag(
                     &input_d, 3, input_dims, p.cfg[input].dt, dnnl_tnc),
            WARN);
    input_d.format_desc.blocking.strides[0] += the_stride;

    dnnl_dims_t states_dims = {p.n_layer, p.n_dir(), p.mb, p.sic};
    DNN_SAFE(dnnl_memory_desc_init_by_tag(
                     &states_d, 4, states_dims, p.cfg[states].dt, dnnl_ldnc),
            WARN);
    states_d.format_desc.blocking.strides[2] = p.sic + the_stride;
    for (int d = 1; d >= 0; --d)
        states_d.format_desc.blocking.strides[d]
                = states_d.format_desc.blocking.strides[d + 1]
                * states_d.dims[d + 1];

    dnnl_dims_t c_states_dims = {p.n_layer, p.n_dir(), p.mb, p.dic};
    DNN_SAFE(dnnl_memory_desc_init_by_tag(&c_states_d, 4, c_states_dims,
                     p.cfg[c_states].dt, dnnl_ldnc),
            WARN);
    c_states_d.format_desc.blocking.strides[2] = p.dic + the_stride;
    for (int d = 1; d >= 0; --d)
        c_states_d.format_desc.blocking.strides[d]
                = c_states_d.format_desc.blocking.strides[d + 1]
                * c_states_d.dims[d + 1];

    DNN_SAFE(dnnl_memory_desc_init_by_tag(&weights_input_d, 5,
                     weights_input_dims, p.cfg[weights_input].dt,
                     dnnl_format_tag_any),
            WARN);

    DNN_SAFE(dnnl_memory_desc_init_by_tag(&weights_states_d, 5,
                     weights_states_dims, p.cfg[weights_states].dt,
                     dnnl_format_tag_any),
            WARN);

    DNN_SAFE(dnnl_memory_desc_init_by_tag(&bias_d, 4, bias_dims, p.cfg[bias].dt,
                     dnnl_format_tag_any),
            WARN);

    DNN_SAFE(dnnl_memory_desc_init_by_tag(&dst_last_layer_d, 3,
                     dst_last_layer_dims, p.cfg[dst_last_layer].dt, dnnl_tnc),
            WARN);
    dst_last_layer_d.format_desc.blocking.strides[0] += the_stride;

    dnnl_dims_t dst_last_iteration_dims = {p.n_layer, p.n_dir(), p.mb, p.dic};
    DNN_SAFE(dnnl_memory_desc_init_by_tag(&dst_last_iteration_d, 4,
                     dst_last_iteration_dims, p.cfg[dst_last_iteration].dt,
                     dnnl_ldnc),
            WARN);

    dst_last_iteration_d.format_desc.blocking.strides[2] = p.dic + the_stride;
    for (int d = 1; d >= 0; --d)
        dst_last_iteration_d.format_desc.blocking.strides[d]
                = dst_last_iteration_d.format_desc.blocking.strides[d + 1]
                * dst_last_iteration_d.dims[d + 1];

    dnnl_dims_t dst_c_last_iteration_dims = {p.n_layer, p.n_dir(), p.mb, p.dic};
    DNN_SAFE(dnnl_memory_desc_init_by_tag(&dst_c_last_iteration_d, 4,
                     dst_c_last_iteration_dims, p.cfg[dst_c_last_iteration].dt,
                     dnnl_ldnc),
            WARN);

    dst_last_iteration_d.format_desc.blocking.strides[2] = p.dic + the_stride;
    for (int d = 1; d >= 0; --d)
        dst_last_iteration_d.format_desc.blocking.strides[d]
                = dst_last_iteration_d.format_desc.blocking.strides[d + 1]
                * dst_last_iteration_d.dims[d + 1];

    // Initializing the forward pass
    // When inference, we use forward_inference
    // When training, we use forward_training
    {
        dnnl_status_t init_status = init_rnn_fwd_desc(rd, p, fwd_prop, &input_d,
                &states_d, &c_states_d, &weights_input_d, &weights_states_d,
                &bias_d, &dst_last_layer_d, &dst_last_iteration_d,
                &dst_c_last_iteration_d);
        if (init_status == dnnl_unimplemented)
            return r->state = UNIMPLEMENTED, OK;
        else
            DNN_SAFE(init_status, WARN);
    }

    if (is_bwd) {
        DNN_SAFE(dnnl_memory_desc_init_by_tag(&diff_input_d, 3, input_dims,
                         p.cfg[dst_diff_input].dt, dnnl_format_tag_any),
                WARN);
        DNN_SAFE(dnnl_memory_desc_init_by_tag(&diff_states_d, 4, states_dims,
                         p.cfg[dst_diff_states].dt, dnnl_format_tag_any),
                WARN);
        DNN_SAFE(
                dnnl_memory_desc_init_by_tag(&diff_c_states_d, 4, c_states_dims,
                        p.cfg[dst_diff_c_states].dt, dnnl_format_tag_any),
                WARN);
        DNN_SAFE(dnnl_memory_desc_init_by_tag(&diff_weights_input_d, 5,
                         weights_input_dims, p.cfg[dst_diff_weights_input].dt,
                         dnnl_format_tag_any),
                WARN);
        DNN_SAFE(dnnl_memory_desc_init_by_tag(&diff_weights_states_d, 5,
                         weights_states_dims, p.cfg[dst_diff_weights_states].dt,
                         dnnl_format_tag_any),
                WARN);
        DNN_SAFE(dnnl_memory_desc_init_by_tag(&diff_bias_d, 4, bias_dims,
                         p.cfg[dst_diff_bias].dt, dnnl_format_tag_any),
                WARN);
        DNN_SAFE(dnnl_memory_desc_init_by_tag(&diff_last_layer_d, 3,
                         dst_last_layer_dims, p.cfg[diff_last_layer].dt,
                         dnnl_format_tag_any),
                WARN);
        DNN_SAFE(dnnl_memory_desc_init_by_tag(&diff_last_iteration_d, 4,
                         dst_last_iteration_dims, p.cfg[diff_last_iteration].dt,
                         dnnl_format_tag_any),
                WARN);
        DNN_SAFE(dnnl_memory_desc_init_by_tag(&diff_c_last_iteration_d, 4,
                         dst_c_last_iteration_dims,
                         p.cfg[diff_c_last_iteration].dt, dnnl_format_tag_any),
                WARN);
        DNN_SAFE(init_rnn_bwd_desc(rd + 1, p, p.prop, &input_d, &states_d,
                         &c_states_d, &weights_input_d, &weights_states_d,
                         &bias_d, &dst_last_layer_d, &dst_last_iteration_d,
                         &dst_c_last_iteration_d, &diff_input_d, &diff_states_d,
                         &diff_c_states_d, &diff_weights_input_d,
                         &diff_weights_states_d, &diff_bias_d,
                         &diff_last_layer_d, &diff_last_iteration_d,
                         &diff_c_last_iteration_d),
                WARN);
    }
    dnnl_primitive_attr_t dnnl_attr;
    create_dnnl_rnn_attr(p, &dnnl_attr);
    dnnl_status_t init_status = dnnl_success;
    for (int i = 0; i < 1 + (int)is_bwd; i++) {
        init_status = dnnl_primitive_desc_create(
                &(rpd[i]), &(rd[i]), dnnl_attr, engine_tgt, NULL);
        if (init_status == dnnl_unimplemented)
            return r->state = UNIMPLEMENTED, OK;
        else
            SAFE(init_status, WARN);

        const char *impl_str = query_impl_info(rpd[i]);
        print(5, "dnnl implementation: %s\n", impl_str);
    }
    dnnl_primitive_attr_destroy(dnnl_attr);

    auto q = [=](dnnl_query_t query, int rpd_idx, int index = 0) {
        return *dnnl_primitive_desc_query_md(rpd[rpd_idx], query, index);
    };

    for (int i = 0; i < 1 + (int)is_bwd; i++) {
        rd[i].src_layer_desc = q(dnnl_query_src_md, i);
        rd[i].src_iter_desc = q(dnnl_query_src_md, i, 1);
        if (p.alg == VANILLA_LSTM)
            rd[i].src_iter_c_desc = q(dnnl_query_src_md, i, 2);
        rd[i].weights_layer_desc = q(dnnl_query_weights_md, i);
        rd[i].weights_iter_desc = q(dnnl_query_weights_md, i, 1);
        rd[i].bias_desc = q(dnnl_query_weights_md, i, 2);
        rd[i].dst_layer_desc = q(dnnl_query_dst_md, i);
        rd[i].dst_iter_desc = q(dnnl_query_dst_md, i, 1);
        if (p.alg == VANILLA_LSTM)
            rd[i].dst_iter_c_desc = q(dnnl_query_dst_md, i, 2);
    }
    if (is_bwd) {
        rd[1].diff_src_layer_desc = q(dnnl_query_diff_src_md, 1);
        rd[1].diff_src_iter_desc = q(dnnl_query_diff_src_md, 1, 1);
        if (p.alg == VANILLA_LSTM)
            rd[1].diff_src_iter_c_desc = q(dnnl_query_diff_src_md, 1, 2);
        rd[1].diff_weights_layer_desc = q(dnnl_query_diff_weights_md, 1);
        rd[1].diff_weights_iter_desc = q(dnnl_query_diff_weights_md, 1, 1);
        rd[1].diff_bias_desc = q(dnnl_query_diff_weights_md, 1, 2);
        rd[1].diff_dst_layer_desc = q(dnnl_query_diff_dst_md, 1);
        rd[1].diff_dst_iter_desc = q(dnnl_query_diff_dst_md, 1, 1);
        if (p.alg == VANILLA_LSTM)
            rd[1].diff_dst_iter_c_desc = q(dnnl_query_diff_dst_md, 1, 2);
    }

    return OK;
}

int doit(const prb_t &p, res_t *r) {
    if (bench_mode == LIST) return r->state = LISTED, OK;

    res_t res_zero {};
    *r = res_zero;

    const auto fp = dnnl_f32;

    if (p.alg != VANILLA_LSTM && p.alg != VANILLA_RNN && p.alg != VANILLA_GRU
            && p.alg != LBR_GRU) {
        printf("p.alg: %d\n", (int)p.alg);
        r->state = UNIMPLEMENTED;
        return OK;
    }

    dnn_mem_t input_dt;
    dnn_mem_t states_dt;
    dnn_mem_t c_states_dt;
    dnn_mem_t weights_input_dt;
    dnn_mem_t weights_states_dt;
    dnn_mem_t bias_dt;
    dnn_mem_t dst_last_layer_dt;
    dnn_mem_t dst_last_iteration_dt;
    dnn_mem_t dst_c_last_iteration_dt;

    dnn_mem_t bwd_weights_input_dt;
    dnn_mem_t bwd_weights_states_dt;
    dnn_mem_t dst_diff_input_dt;
    dnn_mem_t dst_diff_states_dt;
    dnn_mem_t dst_diff_c_states_dt;
    dnn_mem_t dst_diff_weights_input_dt;
    dnn_mem_t dst_diff_weights_states_dt;
    dnn_mem_t dst_diff_bias_dt;
    dnn_mem_t diff_last_layer_dt;
    dnn_mem_t diff_last_iteration_dt;
    dnn_mem_t diff_c_last_iteration_dt;

    dnn_mem_t input_fp;
    dnn_mem_t states_fp;
    dnn_mem_t c_states_fp;
    dnn_mem_t weights_input_fp;
    dnn_mem_t weights_states_fp;
    dnn_mem_t bias_fp;
    dnn_mem_t dst_last_layer_fp;
    dnn_mem_t dst_last_iteration_fp;
    dnn_mem_t dst_c_last_iteration_fp;

    dnn_mem_t dst_diff_input_fp;
    dnn_mem_t dst_diff_states_fp;
    dnn_mem_t dst_diff_c_states_fp;
    dnn_mem_t dst_diff_weights_input_fp;
    dnn_mem_t dst_diff_weights_states_fp;
    dnn_mem_t dst_diff_bias_fp;
    dnn_mem_t diff_last_layer_fp;
    dnn_mem_t diff_last_iteration_fp;
    dnn_mem_t diff_c_last_iteration_fp;

    dnn_mem_t workspace_dt;

    dnnl_rnn_desc_t rd[2];
    dnnl_primitive_desc_t rpd[2] = {nullptr};
    dnnl_primitive_t c = nullptr;

    auto cleanup = [&]() {
        DNN_SAFE(dnnl_primitive_destroy(c), CRIT);
        DNN_SAFE(dnnl_primitive_desc_destroy(rpd[0]), CRIT);
        DNN_SAFE(dnnl_primitive_desc_destroy(rpd[1]), CRIT);
        return OK;
    };

    SAFE(init_pd(p, rd, rpd, r), WARN);
    if (r->state == SKIPPED || r->state == UNIMPLEMENTED) return OK;

    auto &input_dt_d = rd[0].src_layer_desc;
    auto &states_dt_d = rd[0].src_iter_desc;
    auto &c_states_dt_d = rd[0].src_iter_c_desc;
    auto &weights_input_dt_d = rd[0].weights_layer_desc;
    auto &weights_states_dt_d = rd[0].weights_iter_desc;
    auto &bias_dt_d = rd[0].bias_desc;
    auto &dst_last_layer_dt_d = rd[0].dst_layer_desc;
    auto &dst_last_iteration_dt_d = rd[0].dst_iter_desc;
    auto &dst_c_last_iteration_dt_d = rd[0].dst_iter_c_desc;

    auto &bwd_weights_input_dt_d = rd[1].weights_layer_desc;
    auto &bwd_weights_states_dt_d = rd[1].weights_iter_desc;
    auto &diff_src_layer_dt_d = rd[1].diff_src_layer_desc;
    auto &diff_src_iter_dt_d = rd[1].diff_src_iter_desc;
    auto &diff_src_iter_c_dt_d = rd[1].diff_src_iter_c_desc;
    auto &diff_weights_layer_dt_d = rd[1].diff_weights_layer_desc;
    auto &diff_weights_iter_dt_d = rd[1].diff_weights_iter_desc;
    auto &diff_bias_dt_d = rd[1].diff_bias_desc;
    auto &diff_dst_layer_dt_d = rd[1].diff_dst_layer_desc;
    auto &diff_dst_iter_dt_d = rd[1].diff_dst_iter_desc;
    auto &diff_dst_iter_c_dt_d = rd[1].diff_dst_iter_c_desc;

    input_dt = dnn_mem_t(input_dt_d, p.cfg[input].dt, engine_tgt);
    states_dt = dnn_mem_t(states_dt_d, p.cfg[states].dt, engine_tgt);
    c_states_dt = dnn_mem_t(c_states_dt_d, p.cfg[c_states].dt, engine_tgt);
    weights_input_dt = dnn_mem_t(
            weights_input_dt_d, p.cfg[weights_input].dt, engine_tgt);
    weights_states_dt = dnn_mem_t(
            weights_states_dt_d, p.cfg[weights_states].dt, engine_tgt);
    bias_dt = dnn_mem_t(bias_dt_d, p.cfg[bias].dt, engine_tgt);
    dst_last_layer_dt = dnn_mem_t(
            dst_last_layer_dt_d, p.cfg[dst_last_layer].dt, engine_tgt);
    dst_last_iteration_dt = dnn_mem_t(
            dst_last_iteration_dt_d, p.cfg[dst_last_iteration].dt, engine_tgt);
    dst_c_last_iteration_dt = dnn_mem_t(dst_c_last_iteration_dt_d,
            p.cfg[dst_c_last_iteration].dt, engine_tgt);

    const bool is_bwd = p.prop == dnnl_backward;
    if (is_bwd) {
        bwd_weights_input_dt = dnn_mem_t(
                bwd_weights_input_dt_d, p.cfg[weights_input].dt, engine_tgt);
        bwd_weights_states_dt = dnn_mem_t(
                bwd_weights_states_dt_d, p.cfg[weights_states].dt, engine_tgt);
        dst_diff_input_dt = dnn_mem_t(
                diff_src_layer_dt_d, p.cfg[dst_diff_input].dt, engine_tgt);
        dst_diff_states_dt = dnn_mem_t(
                diff_src_iter_dt_d, p.cfg[dst_diff_states].dt, engine_tgt);
        dst_diff_c_states_dt = dnn_mem_t(
                diff_src_iter_c_dt_d, p.cfg[dst_diff_c_states].dt, engine_tgt);
        dst_diff_weights_input_dt = dnn_mem_t(diff_weights_layer_dt_d,
                p.cfg[dst_diff_weights_input].dt, engine_tgt);
        dst_diff_weights_states_dt = dnn_mem_t(diff_weights_iter_dt_d,
                p.cfg[dst_diff_weights_states].dt, engine_tgt);
        dst_diff_bias_dt = dnn_mem_t(
                diff_bias_dt_d, p.cfg[dst_diff_bias].dt, engine_tgt);
        diff_last_layer_dt = dnn_mem_t(
                diff_dst_layer_dt_d, p.cfg[diff_last_layer].dt, engine_tgt);
        diff_last_iteration_dt = dnn_mem_t(
                diff_dst_iter_dt_d, p.cfg[diff_last_iteration].dt, engine_tgt);
        diff_c_last_iteration_dt = dnn_mem_t(diff_dst_iter_c_dt_d,
                p.cfg[diff_c_last_iteration].dt, engine_tgt);
    }

    input_fp = dnn_mem_t(input_dt_d, fp, dnnl_tnc, engine_tgt);
    states_fp = dnn_mem_t(states_dt_d, fp, dnnl_ldnc, engine_tgt);
    c_states_fp = dnn_mem_t(c_states_dt_d, fp, dnnl_ldnc, engine_tgt);
    weights_input_fp
            = dnn_mem_t(weights_input_dt_d, fp, dnnl_ldigo, engine_tgt);
    weights_states_fp
            = dnn_mem_t(weights_states_dt_d, fp, dnnl_ldigo, engine_tgt);
    bias_fp = dnn_mem_t(bias_dt_d, fp, dnnl_ldgo, engine_tgt);
    dst_last_layer_fp
            = dnn_mem_t(dst_last_layer_dt_d, fp, dnnl_tnc, engine_tgt);
    dst_last_iteration_fp
            = dnn_mem_t(dst_last_iteration_dt_d, fp, dnnl_ldnc, engine_tgt);
    dst_c_last_iteration_fp
            = dnn_mem_t(dst_c_last_iteration_dt_d, fp, dnnl_ldnc, engine_tgt);

    if (is_bwd) {
        dst_diff_input_fp
                = dnn_mem_t(diff_src_layer_dt_d, fp, dnnl_tnc, engine_tgt);
        dst_diff_states_fp
                = dnn_mem_t(diff_src_iter_dt_d, fp, dnnl_ldnc, engine_tgt);
        dst_diff_c_states_fp
                = dnn_mem_t(diff_src_iter_c_dt_d, fp, dnnl_ldnc, engine_tgt);
        dst_diff_weights_input_fp = dnn_mem_t(
                diff_weights_layer_dt_d, fp, dnnl_ldigo, engine_tgt);
        dst_diff_weights_states_fp
                = dnn_mem_t(diff_weights_iter_dt_d, fp, dnnl_ldigo, engine_tgt);
        dst_diff_bias_fp = dnn_mem_t(diff_bias_dt_d, fp, dnnl_ldgo, engine_tgt);
        diff_last_layer_fp
                = dnn_mem_t(diff_dst_layer_dt_d, fp, dnnl_tnc, engine_tgt);
        diff_last_iteration_fp
                = dnn_mem_t(diff_dst_iter_dt_d, fp, dnnl_ldnc, engine_tgt);
        diff_c_last_iteration_fp
                = dnn_mem_t(diff_dst_iter_c_dt_d, fp, dnnl_ldnc, engine_tgt);

        const auto ws_md = dnnl_primitive_desc_query_md(
                rpd[0], dnnl_query_workspace_md, 0);
        SAFE(ws_md->ndims != 0 ? OK : FAIL, WARN);
        workspace_dt = dnn_mem_t(*ws_md, engine_tgt);
    }

    // for int8 RNN we need pass attributes for data q10n
    const_dnnl_primitive_attr_t rnn_attr;
    DNN_SAFE(dnnl_primitive_desc_get_attr(rpd[0], &rnn_attr), WARN);

    SAFE(fill_memory(p, input, input_dt, input_fp, rnn_attr), WARN);
    SAFE(fill_memory(p, states, states_dt, states_fp, rnn_attr), WARN);
    if (p.alg == VANILLA_LSTM)
        SAFE(fill_c_states(p, c_states_dt, c_states_fp, rnn_attr), WARN);
    SAFE(fill_weights(p, weights_input, weights_input_dt, weights_input_fp,
                 rnn_attr),
            WARN);
    SAFE(fill_weights(p, weights_states, weights_states_dt, weights_states_fp,
                 rnn_attr),
            WARN);
    SAFE(fill_memory(p, bias, bias_dt, bias_fp), WARN);
    SAFE(fill_memory(p, dst_last_layer, dst_last_layer_dt, dst_last_layer_fp),
            WARN);
    SAFE(fill_memory(p, dst_last_iteration, dst_last_iteration_dt,
                 dst_last_iteration_fp),
            WARN);
    if (p.alg == VANILLA_LSTM)
        SAFE(fill_memory(p, dst_c_last_iteration, dst_c_last_iteration_dt,
                     dst_c_last_iteration_fp),
                WARN);

    if (is_bwd) {
        SAFE(bwd_weights_states_dt.reorder(weights_states_dt), WARN);
        SAFE(bwd_weights_input_dt.reorder(weights_input_dt), WARN);
        SAFE(fill_memory(
                     p, dst_diff_input, dst_diff_input_dt, dst_diff_input_fp),
                WARN);
        SAFE(fill_memory(p, dst_diff_states, dst_diff_states_dt,
                     dst_diff_states_fp),
                WARN);
        if (p.alg == VANILLA_LSTM)
            SAFE(fill_memory(p, dst_diff_c_states, dst_diff_c_states_dt,
                         dst_diff_c_states_fp),
                    WARN);
        SAFE(fill_weights(p, dst_diff_weights_input, dst_diff_weights_input_dt,
                     dst_diff_weights_input_fp),
                WARN);
        SAFE(fill_weights(p, dst_diff_weights_states,
                     dst_diff_weights_states_dt, dst_diff_weights_states_fp),
                WARN);
        SAFE(fill_bias(p, dst_diff_bias, dst_diff_bias_dt, dst_diff_bias_fp),
                WARN);
        SAFE(fill_memory(p, diff_last_layer, diff_last_layer_dt,
                     diff_last_layer_fp),
                WARN);
        SAFE(fill_memory(p, diff_last_iteration, diff_last_iteration_dt,
                     diff_last_iteration_fp),
                WARN);
        if (p.alg == VANILLA_LSTM)
            SAFE(fill_memory(p, diff_c_last_iteration, diff_c_last_iteration_dt,
                         diff_c_last_iteration_fp),
                    WARN);
    }

    args_t args;

    // Running the forward pass
    {
        DNN_SAFE(dnnl_primitive_create(&c, rpd[0]), WARN);

        args.set(DNNL_ARG_SRC_LAYER, input_dt);
        args.set(DNNL_ARG_SRC_ITER, states_dt);
        if (p.alg == VANILLA_LSTM) args.set(DNNL_ARG_SRC_ITER_C, c_states_dt);
        args.set(DNNL_ARG_WEIGHTS_LAYER, weights_input_dt);
        args.set(DNNL_ARG_WEIGHTS_ITER, weights_states_dt);
        args.set(DNNL_ARG_BIAS, bias_dt);

        args.set(DNNL_ARG_DST_LAYER, dst_last_layer_dt);
        args.set(DNNL_ARG_DST_ITER, dst_last_iteration_dt);
        if (p.alg == VANILLA_LSTM)
            args.set(DNNL_ARG_DST_ITER_C, dst_c_last_iteration_dt);
        if (workspace_dt.md_.ndims != 0)
            args.set(DNNL_ARG_WORKSPACE, workspace_dt);

#ifdef CALL_DNNL_RNN
        DNN_SAFE_CLEAN(execute_and_wait(c, stream_tgt, args), WARN, cleanup);
#endif
        if ((p.prop == dnnl_forward) && (bench_mode & CORR)) {
            compute_ref_fwd(p, input_fp, states_fp, c_states_fp,
                    weights_input_fp, weights_states_fp, bias_fp,
                    dst_last_layer_fp, dst_last_iteration_fp,
                    dst_c_last_iteration_fp);

            int compare_status = OK;
            COMPARE_DAT(compare_status, dst_last_layer, dnnl_tnc);
            COMPARE_DAT(compare_status, dst_last_iteration, dnnl_ldnc);
            if (p.alg == VANILLA_LSTM)
                COMPARE_DAT(compare_status, dst_c_last_iteration, dnnl_ldnc);
            SAFE_CLEAN(compare_status, WARN, cleanup);
        }
    }

    if (is_bwd) {
        args.clear();
        DNN_SAFE(dnnl_primitive_destroy(c), CRIT);

        DNN_SAFE(dnnl_primitive_create(&c, rpd[1]), WARN);

        args.set(DNNL_ARG_SRC_LAYER, input_dt);
        args.set(DNNL_ARG_SRC_ITER, states_dt);
        if (p.alg == VANILLA_LSTM) args.set(DNNL_ARG_SRC_ITER_C, c_states_dt);
        args.set(DNNL_ARG_WEIGHTS_LAYER, bwd_weights_input_dt);
        args.set(DNNL_ARG_WEIGHTS_ITER, bwd_weights_states_dt);
        args.set(DNNL_ARG_BIAS, bias_dt);
        args.set(DNNL_ARG_DST_LAYER, dst_last_layer_dt);
        args.set(DNNL_ARG_DST_ITER, dst_last_iteration_dt);
        if (p.alg == VANILLA_LSTM)
            args.set(DNNL_ARG_DST_ITER_C, dst_c_last_iteration_dt);
        args.set(DNNL_ARG_DIFF_DST_LAYER, diff_last_layer_dt);
        args.set(DNNL_ARG_DIFF_DST_ITER, diff_last_iteration_dt);
        if (p.alg == VANILLA_LSTM)
            args.set(DNNL_ARG_DIFF_DST_ITER_C, diff_c_last_iteration_dt);
        args.set(DNNL_ARG_WORKSPACE, workspace_dt);

        args.set(DNNL_ARG_DIFF_SRC_LAYER, dst_diff_input_dt);
        args.set(DNNL_ARG_DIFF_SRC_ITER, dst_diff_states_dt);
        if (p.alg == VANILLA_LSTM)
            args.set(DNNL_ARG_DIFF_SRC_ITER_C, dst_diff_c_states_dt);
        args.set(DNNL_ARG_DIFF_WEIGHTS_LAYER, dst_diff_weights_input_dt);
        args.set(DNNL_ARG_DIFF_WEIGHTS_ITER, dst_diff_weights_states_dt);
        args.set(DNNL_ARG_DIFF_BIAS, dst_diff_bias_dt);

#ifdef CALL_DNNL_RNN
        DNN_SAFE_CLEAN(execute_and_wait(c, stream_tgt, args), WARN, cleanup);
#endif

        if (bench_mode & CORR) {
            compute_ref_bwd(p, input_fp, states_fp, c_states_fp,
                    diff_last_layer_fp, diff_last_iteration_fp,
                    diff_c_last_iteration_fp, weights_input_fp,
                    weights_states_fp, bias_fp, dst_last_layer_fp,
                    dst_last_iteration_fp, dst_c_last_iteration_fp,
                    dst_diff_input_fp, dst_diff_states_fp, dst_diff_c_states_fp,
                    dst_diff_weights_input_fp, dst_diff_weights_states_fp,
                    dst_diff_bias_fp);

            int compare_fwd_status = OK;
            COMPARE_DAT(compare_fwd_status, dst_last_layer, dnnl_tnc);
            COMPARE_DAT(compare_fwd_status, dst_last_iteration, dnnl_ldnc);
            if (p.alg == VANILLA_LSTM)
                COMPARE_DAT(
                        compare_fwd_status, dst_c_last_iteration, dnnl_ldnc);
            SAFE_CLEAN(compare_fwd_status, WARN, cleanup);

            int compare_bwd_data_status = OK;
            COMPARE_DAT(compare_bwd_data_status, dst_diff_input, dnnl_tnc);
            COMPARE_DAT(compare_bwd_data_status, dst_diff_states, dnnl_ldnc);
            if (p.alg == VANILLA_LSTM)
                COMPARE_DAT(
                        compare_bwd_data_status, dst_diff_c_states, dnnl_ldnc);
            SAFE_CLEAN(compare_bwd_data_status, WARN, cleanup);

            int compare_bwd_weights_status = OK;
            COMPARE_DAT(compare_bwd_weights_status, dst_diff_weights_input,
                    dnnl_ldigo);
            COMPARE_DAT(compare_bwd_weights_status, dst_diff_weights_states,
                    dnnl_ldigo);
            COMPARE_DAT(compare_bwd_weights_status, dst_diff_bias, dnnl_ldgo);
            SAFE_CLEAN(compare_bwd_weights_status, WARN, cleanup);
        }
    }

    measure_perf(r->timer, c, args);
    cleanup();

    return OK;
}
} // namespace rnn
