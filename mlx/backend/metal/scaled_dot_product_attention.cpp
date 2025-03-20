// Copyright © 2024 Apple Inc.
#include <sstream>

#include "mlx/backend/common/compiled.h"
#include "mlx/backend/metal/copy.h"
#include "mlx/backend/metal/device.h"

#include "mlx/backend/metal/kernels/steel/attn/params.h"
#include "mlx/backend/metal/utils.h"
#include "mlx/fast_primitives.h"
#include "mlx/utils.h"

namespace mlx::core::fast {

namespace {
void sdpa_full_self_attention_metal(
    const Stream& s,
    metal::Device& d,
    const array& q,
    const array& k,
    const array& v,
    const float scale,
    array& o,
    bool do_causal_ = false,
    const std::optional<array>& mask = std::nullopt) {
  using namespace mlx::steel;

  int wm = 4;
  int wn = 1;

  int bd = q.shape(-1);
  int bq = 32;
  int bk = bd < 128 ? 32 : 16;

  int B = q.shape(0);
  int H = q.shape(1);
  int D = q.shape(3);
  int gqa_factor = q.shape(1) / k.shape(1);

  int qL = q.shape(2);
  int kL = k.shape(2);

  const bool align_Q = (qL % bq) == 0;
  const bool align_K = (kL % bk) == 0;
  const bool has_mask = !!mask;
  const bool do_causal = do_causal_;

  metal::MTLFCList func_consts = {
      {&align_Q, MTL::DataType::DataTypeBool, 200},
      {&align_K, MTL::DataType::DataTypeBool, 201},
      {&has_mask, MTL::DataType::DataTypeBool, 300},
      {&do_causal, MTL::DataType::DataTypeBool, 301}};

  std::ostringstream kname;
  // clang-format off
  kname << "steel_attention_"
        << type_to_name(q)
        << "_bq" << bq
        << "_bk" << bk
        << "_bd" << bd
        << "_wm" << wm
        << "_wn" << wn
        << "_mask" << (type_to_name(has_mask ? *mask : q)); // clang-format on

  std::string base_name = kname.str();

  // clang-format off
  kname << "_align_Q_" << (align_Q ? 't' : 'n')
        << "_align_K_" << (align_K ? 't' : 'n')
        << "_has_mask_" << (has_mask ? 't' : 'n')
        << "_do_causal_" << (do_causal ? 't' : 'n'); // clang-format on

  std::string hash_name = kname.str();

  auto& compute_encoder = d.get_command_encoder(s.index);
  auto kernel = d.get_kernel(base_name, "mlx", hash_name, func_consts);
  compute_encoder.set_compute_pipeline_state(kernel);

  const int NQ = (qL + bq - 1) / bq;
  const int NK = (kL + bk - 1) / bk;

  const int NQ_aligned = qL / bq;
  const int NK_aligned = kL / bk;

  AttnParams params{
      /* int B = */ B,
      /* int H = */ H,
      /* int D = */ D,

      /* int qL = */ qL,
      /* int kL = */ kL,

      /* int gqa_factor = */ gqa_factor,
      /* float scale = */ scale,

      /* int NQ = */ NQ,
      /* int NK = */ NK,

      /* int NQ_aligned = */ NQ_aligned,
      /* int NK_aligned = */ NK_aligned,

      /* int qL_rem = */ (qL - NQ_aligned * bq),
      /* int kL_rem = */ (kL - NK_aligned * bk),
      /* int qL_off = */ (kL - qL),

      /* int64_t Q_strides[3] = */ {q.strides(0), q.strides(1), q.strides(2)},
      /* int64_t K_strides[3] = */ {k.strides(0), k.strides(1), k.strides(2)},
      /* int64_t V_strides[3] = */ {v.strides(0), v.strides(1), v.strides(2)},
      /* int64_t O_strides[3] = */ {o.strides(0), o.strides(1), o.strides(2)}};

  compute_encoder.set_input_array(q, 0);
  compute_encoder.set_input_array(k, 1);
  compute_encoder.set_input_array(v, 2);
  compute_encoder.set_output_array(o, 3);
  compute_encoder.set_bytes(params, 4);

  if (mask) {
    auto m = *mask;

    AttnMaskParams mask_params{/* int64_t M_strides[3] = */ {
        m.strides(0), m.strides(1), m.strides(2)}};

    compute_encoder.set_bytes(mask_params, 5);
    compute_encoder.set_input_array(m, 6);
  }

  MTL::Size grid_dims = MTL::Size(NQ, H, B);
  MTL::Size group_dims = MTL::Size(32, wm, wn);

  compute_encoder.dispatch_threadgroups(grid_dims, group_dims);
}

void sdpa_vector(
    const Stream& s,
    metal::Device& d,
    const array& q,
    const array& k,
    const array& v,
    array& out,
    float scale,
    const std::optional<array>& mask) {
  // Set the kernel name
  std::string kname;
  kname.reserve(64);
  kname += "sdpa_vector_";
  kname += get_type_string(q.dtype());
  kname += "_";
  kname += std::to_string(q.shape(-1));
  kname += "_";
  kname += std::to_string(v.shape(-1));

  // Compute the necessary sizes
  int gqa_factor = q.shape(1) / k.shape(1);
  int N = k.shape(2);
  int B = q.shape(0) * q.shape(1);
  size_t k_head_stride = k.strides()[1];
  size_t k_seq_stride = k.strides()[2];
  size_t v_head_stride = v.strides()[1];
  size_t v_seq_stride = v.strides()[2];

  MTL::Size group_dims(1024, 1, 1);
  MTL::Size grid_dims(B, q.shape(2), 1);

  bool has_mask = mask.has_value();
  bool query_transposed = !q.flags().row_contiguous;
  metal::MTLFCList func_consts = {
      {&has_mask, MTL::DataType::DataTypeBool, 20},
      {&query_transposed, MTL::DataType::DataTypeBool, 21},
  };
  std::string hash_name = kname;
  hash_name += has_mask ? "_mask" : "_nomask";
  hash_name += query_transposed ? "_qt" : "_qnt";

  // Get the kernel
  auto& compute_encoder = d.get_command_encoder(s.index);
  auto kernel = d.get_kernel(kname, "mlx", hash_name, func_consts);
  compute_encoder.set_compute_pipeline_state(kernel);

  // Set its arguments
  compute_encoder.set_input_array(q, 0);
  compute_encoder.set_input_array(k, 1);
  compute_encoder.set_input_array(v, 2);
  compute_encoder.set_output_array(out, 3);
  compute_encoder.set_bytes(gqa_factor, 4);
  compute_encoder.set_bytes(N, 5);
  compute_encoder.set_bytes(k_head_stride, 6);
  compute_encoder.set_bytes(k_seq_stride, 7);
  compute_encoder.set_bytes(v_head_stride, 8);
  compute_encoder.set_bytes(v_seq_stride, 9);

  compute_encoder.set_bytes(scale, 10);
  if (has_mask) {
    auto& m = *mask;
    compute_encoder.set_input_array(m, 11);
    auto nd = m.ndim();
    int32_t kv_seq_stride =
        nd >= 1 && m.shape(-1) > 1 ? m.strides()[nd - 1] : 0;
    int32_t q_seq_stride = nd >= 2 && m.shape(-2) > 1 ? m.strides()[nd - 2] : 0;
    int32_t head_stride = nd >= 3 && m.shape(-3) > 1 ? m.strides()[nd - 3] : 0;
    compute_encoder.set_bytes(kv_seq_stride, 12);
    compute_encoder.set_bytes(q_seq_stride, 13);
    compute_encoder.set_bytes(head_stride, 14);
  }

  // Launch
  compute_encoder.dispatch_threadgroups(grid_dims, group_dims);
}

void sdpa_vector_2pass(
    const Stream& s,
    metal::Device& d,
    const array& q,
    const array& k,
    const array& v,
    array& out,
    float scale,
    const std::optional<array>& mask) {
  // Set the kernel name
  std::string kname;
  kname.reserve(64);
  kname += "sdpa_vector_2pass_1_";
  kname += get_type_string(q.dtype());
  kname += "_";
  kname += std::to_string(q.shape(-1));
  kname += "_";
  kname += std::to_string(v.shape(-1));

  // Compute the necessary sizes
  int gqa_factor = q.shape(1) / k.shape(1);
  int N = k.shape(2);
  int blocks = 32;
  int B = q.shape(0) * q.shape(1);
  size_t k_head_stride = k.strides()[1];
  size_t k_seq_stride = k.strides()[2];
  size_t v_head_stride = v.strides()[1];
  size_t v_seq_stride = v.strides()[2];
  MTL::Size group_dims(8 * 32, 1, 1);
  MTL::Size grid_dims(B, q.shape(2), blocks);

  // Allocate the intermediates
  Shape intermediate_shape;
  intermediate_shape.reserve(out.ndim() + 1);
  intermediate_shape.insert(
      intermediate_shape.end(), out.shape().begin(), out.shape().end() - 1);
  intermediate_shape.push_back(blocks);
  intermediate_shape.push_back(out.shape().back());
  array intermediate(intermediate_shape, float32, nullptr, {});
  intermediate_shape.pop_back();
  array sums(intermediate_shape, float32, nullptr, {});
  array maxs(std::move(intermediate_shape), float32, nullptr, {});
  intermediate.set_data(allocator::malloc(intermediate.nbytes()));
  sums.set_data(allocator::malloc(sums.nbytes()));
  maxs.set_data(allocator::malloc(maxs.nbytes()));
  d.add_temporary(intermediate, s.index);
  d.add_temporary(sums, s.index);
  d.add_temporary(maxs, s.index);

  bool has_mask = mask.has_value();
  bool query_transposed = !q.flags().row_contiguous;
  metal::MTLFCList func_consts = {
      {&has_mask, MTL::DataType::DataTypeBool, 20},
      {&query_transposed, MTL::DataType::DataTypeBool, 21},
  };
  std::string hash_name = kname;
  hash_name += has_mask ? "_mask" : "_nomask";
  hash_name += query_transposed ? "_qt" : "_qnt";

  // Get the kernel
  auto& compute_encoder = d.get_command_encoder(s.index);
  auto kernel = d.get_kernel(kname, "mlx", hash_name, func_consts);

  compute_encoder.set_compute_pipeline_state(kernel);

  // Set its arguments
  compute_encoder.set_input_array(q, 0);
  compute_encoder.set_input_array(k, 1);
  compute_encoder.set_input_array(v, 2);
  compute_encoder.set_output_array(intermediate, 3);
  compute_encoder.set_output_array(sums, 4);
  compute_encoder.set_output_array(maxs, 5);
  compute_encoder.set_bytes(gqa_factor, 6);
  compute_encoder.set_bytes(N, 7);
  compute_encoder.set_bytes(k_head_stride, 8);
  compute_encoder.set_bytes(k_seq_stride, 9);
  compute_encoder.set_bytes(v_head_stride, 10);
  compute_encoder.set_bytes(v_seq_stride, 11);
  compute_encoder.set_bytes(scale, 12);
  if (has_mask) {
    auto& m = *mask;
    compute_encoder.set_input_array(m, 13);
    auto nd = m.ndim();
    int32_t kv_seq_stride =
        nd >= 1 && m.shape(-1) > 1 ? m.strides()[nd - 1] : 0;
    int32_t q_seq_stride = nd >= 2 && m.shape(-2) > 1 ? m.strides()[nd - 2] : 0;
    int32_t head_stride = nd >= 3 && m.shape(-3) > 1 ? m.strides()[nd - 3] : 0;
    compute_encoder.set_bytes(kv_seq_stride, 14);
    compute_encoder.set_bytes(q_seq_stride, 15);
    compute_encoder.set_bytes(head_stride, 16);
  }

  // Launch
  compute_encoder.dispatch_threadgroups(grid_dims, group_dims);

  // Final pass
  kname.clear();
  kname += "sdpa_vector_2pass_2_";
  kname += get_type_string(q.dtype());
  kname += "_";
  kname += std::to_string(v.shape(-1));

  // Get the kernel
  kernel = d.get_kernel(kname);
  compute_encoder.set_compute_pipeline_state(kernel);

  // Set its arguments
  compute_encoder.set_input_array(intermediate, 0);
  compute_encoder.set_input_array(sums, 1);
  compute_encoder.set_input_array(maxs, 2);
  compute_encoder.set_output_array(out, 3);

  // Launch
  group_dims = MTL::Size(1024, 1, 1);
  grid_dims = MTL::Size(B, q.shape(2), 1);
  compute_encoder.dispatch_threadgroups(grid_dims, group_dims);
}

} // namespace

void ScaledDotProductAttention::eval_gpu(
    const std::vector<array>& inputs,
    array& out) {
  auto& s = stream();
  auto& d = metal::device(s.device);

  auto& q_pre = inputs[0];
  auto& k_pre = inputs[1];
  auto& v_pre = inputs[2];
  auto& o = out;

  std::vector<array> copies;

  // Define some copy functions to ensure the layout of the inputs is as
  // expected.
  copies.reserve(3);
  auto copy_unless = [&copies, &s](
                         auto predicate, const array& arr) -> const array& {
    if (!predicate(arr)) {
      array arr_copy(arr.shape(), arr.dtype(), nullptr, {});
      copy_gpu(arr, arr_copy, CopyType::General, s);
      copies.push_back(std::move(arr_copy));
      return copies.back();
    } else {
      return arr;
    }
  };

  // Checks if arr is row contiguous or the sequence and head dimension are
  // transposed
  auto is_contiguous_or_head_seq_transposed = [](const array& arr) {
    if (arr.flags().row_contiguous) {
      return true;
    }
    auto& strides = arr.strides();
    auto& shape = arr.shape();
    return (strides[3] == 1) && (strides[2] == shape[3] * shape[1]) &&
        (strides[1] == shape[3]) && (strides[0] == strides[2] * shape[2]);
  };

  // Checks that the headdim dimension has stride 1.
  auto is_matrix_contiguous = [](const array& arr) {
    return arr.strides(-1) == 1;
  };

  // We are in vector mode ie single query
  if (q_pre.shape(2) <= 8) {
    const auto& q = copy_unless(is_contiguous_or_head_seq_transposed, q_pre);
    const auto& k = copy_unless(is_matrix_contiguous, k_pre);
    const auto& v = copy_unless(is_matrix_contiguous, v_pre);

    // Donate the query if possible
    if (q.is_donatable() && (q.shape(2) == 1 || !q.flags().row_contiguous) &&
        q.size() == o.size()) {
      o.copy_shared_buffer(q);
    } else {
      if (o.shape(2) == 1) {
        o.set_data(allocator::malloc(o.nbytes()));
      } else {
        auto strides = o.strides();
        strides[2] = o.shape(1) * o.shape(3);
        strides[1] = o.shape(3);
        auto flags = q.flags();
        flags.row_contiguous = q.shape(1) == 1;
        o.set_data(
            allocator::malloc(o.nbytes()), o.size(), std::move(strides), flags);
      }
    }

    auto mask =
        inputs.size() > 3 ? std::optional<array>{inputs[3]} : std::nullopt;

    // We route to the 2 pass fused attention if
    // - The device is large and the sequence length long
    // - The sequence length is even longer and we have gqa
    char devc = d.get_architecture().back();
    if ((devc == 'd' && k.shape(2) >= 1024) ||
        (k.shape(1) < q.shape(1) && k.shape(2) >= 4096)) {
      sdpa_vector_2pass(s, d, q, k, v, o, scale_, mask);
    } else {
      sdpa_vector(s, d, q, k, v, o, scale_, mask);
    }
  }

  // Full attention mode
  else {
    const auto& q = copy_unless(is_matrix_contiguous, q_pre);
    const auto& k = copy_unless(is_matrix_contiguous, k_pre);
    const auto& v = copy_unless(is_matrix_contiguous, v_pre);

    int64_t str_oD = 1;
    int64_t str_oH = o.shape(3);
    int64_t str_oL = o.shape(1) * str_oH;
    int64_t str_oB = o.shape(2) * str_oL;
    size_t data_size = o.shape(0) * str_oB;

    array::Flags flags{
        /* bool contiguous = */ 1,
        /* bool row_contiguous = */ 0,
        /* bool col_contiguous = */ 0,
    };

    o.set_data(
        allocator::malloc(o.nbytes()),
        data_size,
        {str_oB, str_oH, str_oL, str_oD},
        flags);

    auto mask = inputs.size() > 3
        ? std::optional<array>{copy_unless(is_matrix_contiguous, inputs[3])}
        : std::nullopt;

    sdpa_full_self_attention_metal(s, d, q, k, v, scale_, o, do_causal_, mask);
  }

  d.add_temporaries(std::move(copies), s.index);
}

} // namespace mlx::core::fast
