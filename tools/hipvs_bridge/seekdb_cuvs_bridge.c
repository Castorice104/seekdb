/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tunables. Defaults keep the contest baseline behaviour; the environment
 * overrides exist so one binary can measure several graph-build candidates in
 * a single GPU window (the official run never sets them). */
static long env_long(const char* name, long dflt)
{
  const char* v = getenv(name);
  char* end = 0;
  long x = 0;
  if (!v || !*v) return dflt;
  x = strtol(v, &end, 10);
  if (end == v || x <= 0) return dflt;
  return x;
}


static double env_dbl(const char* name, double dflt)
{
  const char* v = getenv(name);
  char* end = 0;
  double x = 0;
  if (!v || !*v) return dflt;
  x = strtod(v, &end);
  if (end == v) return dflt;
  return x;
}

static const char* env_str(const char* name, const char* dflt)
{
  const char* v = getenv(name);
  return (v && *v) ? v : dflt;
}

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* query scratch, reused across searches (grown on demand) */
  float*    qd;      /* nq x dim device query buffer */
  long      qd_cap;  /* capacity in elements */
  unsigned* nd;      /* nq x topk device neighbour ids */
  long      nd_cap;
  float*    dd;      /* nq x topk device distances */
  long      dd_cap;
  cuvsCagraSearchParams_t sp;
  long sp_itopk, sp_width;  /* values currently programmed into sp */
} sk_cuvs_index;

/* Grow a device buffer only when the current capacity is too small. On
 * failure the previous buffer stays valid so the caller can keep serving. */
static int grow_buf(cuvsResources_t res, void** ptr, long* cap, long need,
                    size_t elem_size)
{
  void* p = 0;
  if (need <= *cap) return 0;
  if (cuvsRMMAlloc(res, &p, elem_size * (size_t)need) != CUVS_SUCCESS) return 1;
  if (*ptr) cuvsRMMFree(res, *ptr, elem_size * (size_t)*cap);
  *ptr = p;
  *cap = need;
  return 0;
}

static void release_scratch(sk_cuvs_index* h)
{
  if (h->sp) { cuvsCagraSearchParamsDestroy(h->sp); h->sp = 0; }
  if (h->dd) { cuvsRMMFree(h->res, h->dd, sizeof(float) * (size_t)h->dd_cap); h->dd = 0; h->dd_cap = 0; }
  if (h->nd) { cuvsRMMFree(h->res, h->nd, sizeof(unsigned) * (size_t)h->nd_cap); h->nd = 0; h->nd_cap = 0; }
  if (h->qd) { cuvsRMMFree(h->res, h->qd, sizeof(float) * (size_t)h->qd_cap); h->qd = 0; h->qd_cap = 0; }
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  long graph_degree = 0, intermediate = 0, niter = 0;
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  graph_degree = env_long("SEEKDB_CUVS_GRAPH_DEGREE", 128);
  intermediate = env_long("SEEKDB_CUVS_INTERMEDIATE_DEGREE", 128);
  niter        = env_long("SEEKDB_CUVS_NITER", 80);

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->graph_degree = graph_degree;
  h->ip->intermediate_graph_degree = intermediate;
  h->ip->nn_descent_niter = niter;
  {
    const char* algo = env_str("SEEKDB_CUVS_BUILD_ALGO", "nn_descent");
    double refine = env_dbl("SEEKDB_CUVS_REFINEMENT", 0.0);
    if (strcmp(algo, "ivf_pq") == 0 || strcmp(algo, "auto") == 0) {
      struct cuvsIvfPqParams* gb =
          (struct cuvsIvfPqParams*)calloc(1, sizeof(*gb));
      if (gb == NULL) goto fail_ip;
      if (cuvsIvfPqIndexParamsCreate(&gb->ivf_pq_build_params) != CUVS_SUCCESS) {
        free(gb); goto fail_ip;
      }
      if (cuvsIvfPqSearchParamsCreate(&gb->ivf_pq_search_params) != CUVS_SUCCESS) {
        cuvsIvfPqIndexParamsDestroy(gb->ivf_pq_build_params);
        free(gb); goto fail_ip;
      }
      gb->ivf_pq_build_params->metric = L2Expanded;
      gb->refinement_rate = (float)refine;
      h->ip->graph_build_params = gb;
      h->ip->build_algo = (strcmp(algo, "auto") == 0) ? AUTO_SELECT : IVF_PQ;
    } else if (strcmp(algo, "iterative") == 0) {
      h->ip->build_algo = ITERATIVE_CAGRA_SEARCH;
    } else {
      h->ip->build_algo = NN_DESCENT;
    }
    fprintf(stderr, "[seekdb_cuvs] build n=%ld dim=%ld algo=%s refine=%.2f graph_degree=%ld "
                    "intermediate_graph_degree=%ld nn_descent_niter=%ld\n",
            n, dim, algo, refine, graph_degree, intermediate, niter);
  }
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  long itopk = 0, width = 0;
  DLManagedTensor qt, nt, dt;
  long qs[2], nsh[2], dsh[2];
  cuvsFilter f;

  if (!h || !out_ids) return 1;
  if (nq <= 0 || topk <= 0) return 2;

  itopk = env_long("SEEKDB_CUVS_ITOPK", topk * 4);
  if (itopk < 512) itopk = 512;
  if (itopk < topk) itopk = topk;
  width = env_long("SEEKDB_CUVS_SEARCH_WIDTH", 4);

  if (!h->sp) {
    if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) {
      h->sp = 0;
      fprintf(stderr, "[seekdb_cuvs] search param create failed: %s\n", cuvsGetLastErrorText());
      return 14;
    }
    h->sp_itopk = 0;
    h->sp_width = 0;
  }
  if (h->sp_itopk != itopk || h->sp_width != width) {
    h->sp->itopk_size = itopk;
    h->sp->search_width = width;
    h->sp_itopk = itopk;
    h->sp_width = width;
    fprintf(stderr, "[seekdb_cuvs] search params topk=%ld itopk=%ld search_width=%ld\n",
            topk, itopk, width);
  }

  if (grow_buf(h->res, (void**)&h->qd, &h->qd_cap, nq * h->dim, sizeof(float))) return 10;
  if (grow_buf(h->res, (void**)&h->nd, &h->nd_cap, nq * topk, sizeof(unsigned))) return 11;
  if (grow_buf(h->res, (void**)&h->dd, &h->dd_cap, nq * topk, sizeof(float))) return 12;
  if (cudaMemcpy(h->qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) return 13;

  qs[0] = nq; qs[1] = h->dim;
  nsh[0] = nq; nsh[1] = topk;
  dsh[0] = nq; dsh[1] = topk;
  qt.dl_tensor.data = h->qd;
  qt.dl_tensor.device.device_type = kDLCUDA; qt.dl_tensor.device.device_id = 0;
  qt.dl_tensor.ndim = 2;
  qt.dl_tensor.dtype.code = kDLFloat; qt.dl_tensor.dtype.bits = 32; qt.dl_tensor.dtype.lanes = 1;
  qt.dl_tensor.shape = qs; qt.dl_tensor.strides = 0; qt.dl_tensor.byte_offset = 0;
  nt = qt; nt.dl_tensor.data = h->nd; nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = nsh;
  dt = qt; dt.dl_tensor.data = h->dd; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.shape = dsh;

  f.type = NO_FILTER; f.addr = (uintptr_t)0;
  if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] search failed: %s\n", cuvsGetLastErrorText());
    return 15;
  }
  if (cudaMemcpy(out_ids, h->nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) return 16;
  if (out_dist && cudaMemcpy(out_dist, h->dd, sizeof(float)*nq*topk, cudaMemcpyDefault) != cudaSuccess)
    return 17;
  return 0;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  release_scratch(h);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  int rc = 0;
  if (!h) return 10;
  rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
