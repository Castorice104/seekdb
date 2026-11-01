# seekdb × AMD hipVS 竞赛冲刺性能报告

本报告记录 2026 OceanBase × AMD 竞赛冲刺中，对 seekdb 的 GPU 向量索引路径（hipVS + cuVS CAGRA）所做的三处优化，以及在同一台 Radeon Cloud GPU 实例上、用官方编译命令与真实 SIFT250K 数据得到的配对测量结果。所有数字均由本仓库自带工具 `contest_bench.py` 现场测得，原始 JSON 见 `evidence/` 目录。

## 1. 结论摘要

| 指标 | 基线（V5 + 原 bridge） | 候选（本分支） | 变化 |
| --- | --- | --- | --- |
| SIFT250K 冷构建（DDL → 首条合法查询） | 47.16 s | 30.15 s | **1.56×** |
| SIFT250K Top-10 QPS | 142.2 | 302.1 | **2.12×** |
| SIFT250K Top-100 QPS | 137.5 | 279.4 | **2.03×** |
| SIFT250K Recall`10 / Recall`100 | 0.999951 / 0.999971 | 0.999976 / 0.999968 | +2.5e-05 / -3e-06 |
| 256d 代理冷构建 | 46.02 s | 32.17 s | **1.43×** |
| 256d 代理 Top-10 / Top-100 QPS | 88.8 / 86.4 | 132.3 / 120.7 | **1.49× / 1.40×** |
| 256d 代理 Recall`10 / Recall`100 | 0.995508 / 0.937607 | 0.994751 / 0.936509 | -7.6e-04 / -1.1e-03 |
| 显存峰值（两个数据集） | 699 / 821 MB | 699 / 821 MB | 1.00× |
| 进程内存峰值 VmHWM | 2820 / 3245 MB | 2279 / 3246 MB | 不增 |

按赛题评分公式（`B = B_sift^0.4 * B_hidden^0.6`，`Q = Q_sift^0.4 * Q_hidden^0.6`，总分 `4 + 14*min(1, ln B/ln 3) + 2*min(1, ln Q/ln 1.5)`），以本机 256d 代理数据集代替 Hidden250K 估算：

- 构建 `B = 1.564^0.4 * 1.430^0.6 = 1.4825` → 构建分 `14 * ln(1.4825)/ln(3) = 7.02`
- 查询 `Q = 2.077^0.4 * 1.443^0.6 = 1.6691` → 查询分 `2 * min(1, ln(1.6691)/ln(1.5)) = 2.00`（已被 1.5× 上限封顶）
- 预估总分 **11.0**（门槛 4 + 7.02 + 2.00）；同一轮基线按公式只有门槛分 4.0。

## 2. 实验环境

| 项 | 值 |
| --- | --- |
| 平台 | AMD Radeon Cloud，模板 seekdb-hipvs（镜像 seekdb-hipvs-nb-demo_v1.4.0） |
| GPU | gfx1100（RDNA3，48 GiB VRAM，`/sys/class/drm/card1/device`） |
| CPU | AMD EPYC，nproc = 128 |
| 内存 | 503 GiB |
| ROCm / 编译器 | HIP 7.2.53211-97f5574fe2，AMD clang 22.0.0（ROCm 7.2.4，`/opt/rocm-7.2.4`） |
| 依赖缓存 | `/opt/seekdb-deps-cache`（软链为 `deps/3rd`） |
| 基线提交 | `634888d9b037153fa143d2c913ba4b25503e1d62`（V5，分支 amd-oceanbase-official-contest-base-v5） |
| 数据集 | SIFT250K×128d（ann-benchmarks sift-128-euclidean 的前 250000 行；nq=4096 真实查询）；256d 合成代理 n=250000 nq=4096 seed=7 |

编译命令严格使用官方命令，只把 bridge 路径换成我们统一替换的文件：

``````
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
./build.sh release -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
    -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so --make -j24
``````

`src/oblib/lib/CMakeLists.txt:92-94` 把 bridge 的绝对路径直接链进 `oblib_lib`，二进制 `ldd` 可见 `libseekdb_cuvs_bridge.so => /work/bridge/libseekdb_cuvs_bridge.so`。因此**同一只 observer 二进制**只需替换 `.so` 文件内容即可对比基线/候选，消除了两次构建带来的编译器与布局差异；最后一轮对比正是在同一只二进制上完成的。

## 3. 测量方法

`contest_bench.py` 的 `run` 子命令每次执行都：

1. 冷启动 observer（`--nodaemon`、独立 data-dir、`cpu_count=24`、`memory_budget=64G`），等待端口可用；
2. `LOAD DATA LOCAL INFILE` 导入 base.tsv（10 万～25 万行，导入不计时）；
3. 起计时器，执行 `CREATE VECTOR INDEX ... WITH (distance=l2, type=hnsw, lib=cuvs)`，随后立即用**同一个长连接**执行第一条合法查询，两者之差即构建时间（与赛题口径一致：从 DDL 起到第一条固定查询完成）；
4. 每条查询单独一个 mysql 客户端进程并逐条计时，共 nq=4096 条（另含 32 条预热不计）；
5. 用精确 Top-100 真值计算 Recall`10 / Recall`100，并校验每条查询恰好返回 k 个合法、不重复 ID；
6. 采样 `/sys/class/drm/card1/device/mem_info_vram_used` 峰值与 `/proc/<pid>/status` 的 `VmHWM`。

精确真值在旁路单独计算（numpy 分块暴力 L2，SIFT250K 约 12 s），不计入任何测量时间；真值与数据哈希写入 `manifest.json`（SIFT base sha256 `fc9bf0dd…`，truth sha256 `0425b7e6…`）。每个结果 JSON 同时记录 observer 与 bridge 的 sha256、nproc、hostname 与完整环境变量，可复核。

## 4. 代码改动

三处改动各自独立成提交，可按需回退；全部保持 FP32、L2、既有 SQL 形态、既有快照格式与 ID 映射。

### 4.1 查询侧缓冲区与参数对象复用（`tools/hipvs_bridge/seekdb_cuvs_bridge.c`）

原实现每次 `seekdb_cuvs_search` 都做 3 次 `cuvsRMMAlloc` + 3 次 `cuvsRMMFree` + 1 次 `cudaMemcpy` + `cuvsCagraSearchParamsCreate/Destroy`。对 nq=1 的逐条查询，这些固定开销与一次真实图搜索同量级。改动后句柄内保存查询缓冲、邻居 ID 缓冲、距离缓冲以及 `cuvsCagraSearchParams_t`，按容量惰性增长；扩容失败返回错误并保留原状态；`free` 统一释放；构建与反序列化得到的句柄走同一初始化路径。C ABI 不变，不缓存查询结果。

同时把建图/查询常量改为可被环境变量覆盖（默认值与 V5 逐字一致，仅为消融服务）：`SEEKDB_CUVS_GRAPH_DEGREE`(128)、`SEEKDB_CUVS_INTERMEDIATE_DEGREE`、`SEEKDB_CUVS_NITER`、`SEEKDB_CUVS_ITOPK`、`SEEKDB_CUVS_SEARCH_WIDTH`(4)。

### 4.2 查询工作线程复用（`src/oblib/lib/vector/ob_vsag_adaptor.cpp`）

原实现每条查询都 `pthread_create` 一个 32 MiB 栈的线程再 `pthread_join`，即一次 4096 条的正式评测会在索引生命周期内创建 4096 个线程。改为每个 GPU 索引按需创建一个持久工作线程（32 MiB 栈），用互斥量 + 条件变量交接任务；工作线程不反向获取注册表锁；索引析构时先停线程再释放资源；任务交接失败回退到原有临时线程路径，行为等价。

### 4.3 建图参数（同 `seekdb_cuvs_bridge.c`）

`intermediate_graph_degree` 默认 256 → 128，`nn_descent_niter` 默认 100 → 80。依据见第 6 节：256d 数据上 Recall 对这两个参数极其敏感，而 128 维 SIFT 几乎不敏感；两者都要照顾时，只有 `i128 + n80` 同时满足“256d 相对下降 ≤ 0.005”与“构建明显变快”。

### 4.4 基准工具与 A/B 开关

新增 `contest_bench.py`（prepare/run/compare/report 四子命令）、`contest_gpu.sh`（GPU 窗口一键脚本）、`CONTEST_RUNBOOK.md`。另加一个**只用于 A/B** 的运行时开关 `SEEKDB_CUVS_SYNC_SEARCH=1`，它让候选二进制退化为“每次新建线程 + 每次分配缓冲”的原始行为，从而在同一只二进制内复现基线。

## 5. 正式对比结果

配对条件：同一 observer 二进制、同一 bridge 路径、同一数据集、同一参数（`cpu_count=24`、`memory_budget=64G`、`datafile_size=96G`）；基线加 `SEEKDB_CUVS_SYNC_SEARCH=1` 并使用基线 bridge，候选不加任何环境变量。

### 5.1 SIFT250K（128d，n=250000，nq=4096）

| 轮次 | 构建 | Top-10 QPS | Top-100 QPS | 几何平均 | R`10 | R`100 | VRAM | VmHWM |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| base-final（SYNC + v1 bridge） | 47.16 s | 142.2 | 137.5 | 139.8 | 0.999951 | 0.999971 | 699 MB | 2820 MB |
| cand-final（无 env + final bridge） | 30.15 s | 302.1 | 279.4 | 290.5 | 0.999976 | 0.999968 | 699 MB | 2279 MB |

- 构建加速 **1.564×**；查询加速 Top-10 **2.125×**、Top-100 **2.031×**、几何平均 **2.077×**
- 单条查询延迟：Top-10 7.03 ms → 3.31 ms，Top-100 7.27 ms → 3.58 ms
- Recall 变化 +2.5e-05 / -3.0e-06，远小于 0.005 门槛
- 基线自身复测漂移：45.01 s / 137.4 QPS（相对首测 -1.0% / -0.9%），说明 1.56× 与 2.08× 不是噪声

### 5.2 256d 代理（n=250000，nq=4096，seed=7）

| 轮次 | 构建 | Top-10 QPS | Top-100 QPS | 几何平均 | R`10 | R`100 | 显存 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| base-final | 46.02 s | 88.8 | 86.4 | 87.6 | 0.995508 | 0.937607 | 821 MB |
| cand-final | 32.17 s | 132.3 | 120.7 | 126.4 | 0.994751 | 0.936509 | 821 MB |

- 构建加速 **1.430×**；查询 Top-10 **1.490×**、Top-100 **1.397×**、几何平均 **1.443×**
- 单条查询延迟：Top-10 11.26 ms → 7.56 ms，Top-100 11.57 ms → 8.28 ms
- Recall 变化 -7.6e-04 / -1.1e-03，仍在 0.005 门槛内，但余量只剩 0.0036（第 6 节解释了为什么不再激进）

### 5.3 分项归因

| 轮次（SIFT250K） | 构建 | 几何平均 QPS | 说明 |
| --- | --- | --- | --- |
| base = v1 bridge + SYNC | 45.47 s | 138.5 | 原始行为 |
| v2 bridge + worker | 44.90 s | 290.1 | **查询 2.09×，全部来自线程复用** |
| final bridge（含 i128/n80） | 30.15 s | 290.5 | **构建 1.56×，全部来自建图参数** |

结论：查询加速来自“不再为每条查询新建线程 + 不再每次分配/释放 GPU 缓冲”，构建加速来自“降低 CAGRA 中间图规模与 NN-descent 迭代次数”。两者互不干扰，可分别回退。

### 5.4 重启持久化与 ID 稳定性

用候选 bridge 在 10k 小数据集上做了一次快照往返验证：建索引 → 200 条查询（结果集 content hash）→ 停 observer → 重启 → 同一批查询得到**完全相同的 ID 集合**（sha256 `25f5cc22b26e3752…`）；随后 `drop table` 并重建索引，结果集哈希不变。说明工作线程复用与缓冲复用没有破坏快照加载路径与 ID 映射。

同一次运行的 observer 日志还记录了 bridge 实际生效的建图参数：`[seekdb_cuvs] build n=10000 dim=128 algo=nn_descent refine=0.00 graph_degree=128 intermediate_graph_degree=128 nn_descent_niter=80`（见 `evidence/persist/build_log_excerpt.txt`）。另外，在 50 次 Top-100 查询前后采样 GPU 显存，从 26.8 MB 上升到 473.5 MB（见 `evidence/final/gpu_proof.txt`，内含 `rocm-smi` 输出），证明建图与查询确实在 GPU 上执行。

## 6. 建图参数筛选（关键决策依据）

128d SIFT 上 Recall 对 `nn_descent_niter` 极不敏感（n10 仍有 0.9996），但 256d 上非常敏感：

| 配置（256d 代理） | 构建 | R`100 | 相对基线 |
| --- | --- | --- | --- |
| baseline（g128 i256 n100） | 46.02 s | 0.937607 | — |
| i128 n100 | 37.67 s | 0.938018 | +4.1e-04 |
| i128 n80（**选中**） | 31.39 s | 0.936365 | -1.2e-03 |
| i128 n70 | 29.10 s | 0.934565 | -3.0e-03 |
| i128 n60 | 25.64 s | 0.932246 | -5.4e-03（**超门槛**） |
| i128 n40 | 19.70 s | 0.922976 | -1.5e-02 |
| i256 n40 | 25.67 s | 0.921538 | -1.6e-02 |
| i256 n20 | 18.27 s | 0.890203 | -4.7e-02 |
| g96 i192 n100 | 39.14 s | 0.890154 | -4.7e-02 |
| g64 i128 n100 | 36.42 s | 0.781021 | -1.6e-01 |

另有两个方向被否掉：

- **IVF_PQ 建图**（`SEEKDB_CUVS_BUILD_ALGO=ivf_pq`）：256d 构建 16.15 s（2.8×），但 R`100 仅 0.6609；`refinement_rate` 取 0 / 0.5 / 1.0 分别为 0.660 / 0.660 / 0.661，refinement 不生效，质量不可用。
- **ITERATIVE_CAGRA_SEARCH**：构建 64.78 s、R`100 0.9271，既慢又差。

最终取 `i128 + n80`：256d 上仍有 0.0036 的 Recall 余量，同时拿到 1.43×～1.56× 构建加速。n60/n70 虽然更快，但 n60 直接越过 0.005 门槛、n70 只剩 0.0018 余量，遇到分布不同的 Hidden250K 有零分风险，故不采用。

## 7. 门槛核对

| 门槛 | 要求 | 实测 | 结论 |
| --- | --- | --- | --- |
| SIFT Recall`10/`100 | ≥ 0.95 且 ≥ baseline - 0.005 | 0.999976 / 0.999968 | 通过 |
| 256d 代理 Recall`10/`100 | ≥ 0.90 / 0.85 且 ≥ baseline - 0.005 | 0.994751 / 0.936509 | 通过（余量 0.0036） |
| ID 完整性 | 每条查询恰好 k 个合法、不重复 ID | integrity_errors = 0，empty_results = 0 | 通过 |
| 显存 | ≤ 45 GB 且 ≤ baseline 1.25× | 699 / 821 MB，比值 1.00 | 通过 |
| 进程内存 | ≤ 96 GB | VmHWM 2279 / 3246 MB | 通过 |
| QPS 比 | 两数据集 Top-10/Top-100 几何平均 ≥ baseline 0.90 | 2.077 / 1.443 | 通过 |
| 官方编译命令 | 原样可用、正式构建关 TRACE | 是（`-DOB_BUILD_CUVS_TRACE=OFF`） | 通过 |
| 基线祖先 | `git merge-base --is-ancestor 634888d9 HEAD` | 通过 | 通过 |

## 8. 风险与后续

1. **Hidden250K 的 Recall 余量**：全部 256d 结论来自合成代理数据（其基线 R`100 0.9377，官方公布 hidden 基线 0.9142，难度接近但分布不同）。若正式评测出现相对下降超 0.005，回退顺序：先把 `SEEKDB_CUVS_NITER` 调到 100（构建仍有 1.2×～1.3×），再考虑 `intermediate_graph_degree` 回到 256。两者都是纯环境变量，不需要重新编译。
2. **128d 上建图参数偏保守**：SIFT 上 n10 都有 0.9996，说明 128d 的 Recall 余量被 256d 需求绑架了。若能按维度分派参数（bridge 已能拿到 dim，可在 `seekdb_cuvs_build` 内按 dim 选择默认值），128d 还能再取更激进的参数换约 1.2× 额外构建加速——留作后续工作。
3. **批量接口**：本次只测了逐条查询（nq=1，赛题正式形态），`dbms_vector.batch_knn` 复用同一 worker，收益方向一致但未单独量化。
4. **未做**：自定义建图算法（一天窗口内风险过高）、FP16、快照格式重构。

## 9. 复现步骤

``````
# 1. 编译 bridge（基线 v1 与候选 v2 都可，两条 git archive 分支）
bash tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

# 2. 编译 observer（官方命令）
./build.sh release -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
    -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so --make -j24

# 3. 准备数据（SIFT 走 fvecs；没有 SIFT 时自动退化为合成代理数据）
python3 docs/gpu-vector-index-hipvs-cuvs/validation/contest_bench.py prepare \
    --dataset sift250k --sift-base sift_base.fvecs --sift-queries sift_query.fvecs \
    --nq 4096 --out /amd-work/ds

# 4. 单轮测量
python3 docs/gpu-vector-index-hipvs-cuvs/validation/contest_bench.py run \
    --dataset-dir /amd-work/ds \
    --observer build_release/src/observer/seekdb \
    --bridge /work/bridge/libseekdb_cuvs_bridge.so --tag base --out /work/res \
    --work-dir /work/w-base --cpu-count 24 --memory-budget 64G --datafile-size 96G \
    --vram-sysfs /sys/class/drm/card1/device

# 5. 配对比较与汇总
python3 docs/gpu-vector-index-hipvs-cuvs/validation/contest_bench.py compare \
    --baseline /work/res/base.json --candidate /work/res/cand.json \
    --combine --out /work/res/paired.json
python3 docs/gpu-vector-index-hipvs-cuvs/validation/contest_bench.py report \
    --results /work/res
``````

一键版：`bash docs/gpu-vector-index-hipvs-cuvs/validation/contest_gpu.sh sweep /work contest DS`（详见 `CONTEST_RUNBOOK.md`）。

## 10. 原始证据

| 文件 | 内容 |
| --- | --- |
| `evidence/final/sift-base-final.json`、`sift-cand-final.json` | 第 5.1 节原始结果（含每条查询耗时数组、observer/bridge sha256） |
| `evidence/final/d256-base-final.json`、`d256-cand-final.json` | 第 5.2 节原始结果 |
| `evidence/final/summary.json` | 合并 B/Q 与预估分数 |
| `evidence/sweep/*.json` | 第 6 节全部参数消融轮次 |
| `evidence/sweep/*.log` | 每轮 observer 与 bench 日志 |
| `evidence/persist/persist.log` | 第 5.4 节重启持久化全程日志 |
| `evidence/persist/build_log_excerpt.txt` | 运行时建图/查询参数日志摘录 |
| `evidence/final/gpu_proof.txt` | observer 的 GPU 显存采样与 `rocm-smi` 输出 |
