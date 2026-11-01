# seekdb × AMD hipVS：GPU CAGRA 建图与查询性能报告（第二轮）

日期：2026-11-01 / 02（Asia/Shanghai）
场景：OceanBase 2026 数据库大赛 AMD 赛道（seekdb + hipVS/cuVS CAGRA，gfx1100 / Radeon PRO W7900 级 48GiB）
基线：commit 634888d9b037153fa143d2c913ba4b25503e1d62（amd-oceanbase-official-contest-base-v5）

## 1. 结论

| 指标（本地实测，512 条查询，精确真值） | 基线 V5 | 候选（本分支） | 提升 |
|---|---|---|---|
| SIFT250K 128d 建图（DDL → 首条合法查询） | 44.98 s | 5.58 s | 8.06x |
| SIFT250K 查询 QPS（Top-10 / Top-100 几何平均） | 140.9 | 248.7 | 1.76x |
| SIFT Recall@10 / @100 | 1.0000 / 1.0000 | 1.0000 / 0.9990 | +0.0000 / -0.0010 |
| 256d 250K 建图 | 46.61 s | 25.13 s | 1.85x |
| 256d 查询 QPS | 85.8 | 129.0 | 1.50x |
| 256d Recall@10 / @100 | 0.9973 / 0.9378 | 0.9947 / 0.9709 | -0.0025 / +0.0331 |
| 显存峰值（256d） | 822 MB | 883 MB | 1.07x（门限 1.25x） |

综合分数模型（官方）：score = 4 + 14*min(1, ln B / ln 3.0) + 2*min(1, ln Q / ln 1.5)，
其中 B = Bsift^0.4 * Bhidden^0.6，Q = Qsift^0.4 * Qhidden^0.6。

本地 B = 8.06^0.4 * 1.85^0.6 = 3.33，本地 Q = 1.76^0.4 * 1.50^0.6 = 1.61。
上一轮官方 9.628 那次：本地 B = 1.48、官方 B = 1.329（折算 0.896）；本地 Q = 1.66、官方 Q = 1.644（无折算）。
按同一折算预估：官方 B ≈ 2.98、Q ≈ 1.61 → 约 19.9 分。

## 2. 环境与数据

- GPU 实例：AMD Radeon Cloud（seekdb-hipvs 模板），gfx1100，48 GiB 显存，128 vCPU / 503 GiB 内存，
  ROCm 7.2.4（hipcc 7.2.53211，clang 22.0.0），cuVS 头文件与库在 /opt/hipvs。
- 构建命令（与赛题一致，未做任何改动）：
  - bridge：tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
  - seekdb：./build.sh release -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF
    -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so --make -j64
- 数据：
  - SIFT250K：ann-benchmarks 的 sift-128-euclidean.hdf5 前 250,000 条做底库，4096 条真实 test 查询；
    本地 bench 使用前 512 条查询（真值按子集精确重算，float32 分块 L2）。
  - 256d 代理：250,000 x 256 固定种子（seed 7）高斯合成，4096 条查询，用于逼近 Hidden250K 的维度。
    赛题文档明确 256 维自测数据只能用于开发验证，因此 256d 结论一律以“相对同轮基线”的形式给出。
- 计时口径：冷启动 observer，LOAD DATA 导入，CREATE VECTOR INDEX 开始计时，到第一条合法查询返回为止；
  查询用单客户端进程跑完，逐条计时并汇总 QPS；每轮清空数据目录与索引。

## 3. 改动内容（全部在 amd-cagra-submit 分支，按提交顺序）

1. d1ff25adc test(vector): add reproducible GPU contest bench（prepare/run/compare/report 四子命令，
   固定真值、固定 SQL、固定计时口径、显存与 RSS 采样、门限与分数模型）。
2. 62900d110 perf(hipvs): reuse per-index search scratch buffers and CAGRA search params
   - 句柄内保存 query / 邻居 id / 距离三块设备缓冲与 cuvsCagraSearchParams，按 nq、topk 扩容复用；
   - 容量足够时完全不再分配，扩容失败返回错误并保持原状态；
   - 保留 C ABI 与既有快照格式，不缓存查询结果。
3. e9b07a667 perf(cuvs): dispatch GPU index searches to one persistent worker thread per index
   - 每个 GPU 索引一个常驻工作线程（32 MiB 栈），用条件变量交接任务，替代“每次查询新建线程再 join”；
   - 工作线程不反向获取注册表锁；销毁时先停线程再释放。
4. 5db2f8914 / a051d232c perf(hipvs): 128d 缩短 NN-descent 计划（先 20 轮、后 10 轮）。
5. 052c1378e perf(hipvs): dense 256d graphs, fp16 NN-descent for 128d, tunable width（本轮提交）
   - dim <= 128：graph_degree = intermediate_graph_degree = 64，nn_descent_niter = 2，search_width = 4；
   - dim > 128：graph_degree = intermediate_graph_degree = 192，nn_descent_niter = 45，search_width = 8；
   - 128d 走 fp16 建图：先用 float16 副本跑 NN-descent（更便宜），再用 cuvsCagraIndexGetGraph +
     cuvsCagraIndexFromArgs 把图提升（promote）到 float32 数据集上用于搜索；
     本 cuVS 构建无法直接搜索 float16 数据集（cagra_search.cuh:174 Unrecognized dataset format），
     因此必须提升；
   - 所有参数可用环境变量覆盖，默认值与基线行为在未设置时逐字一致：
     SEEKDB_CUVS_GRAPH_DEGREE / INTERMEDIATE_DEGREE / NITER / FP16 / FP16Q / ITOPK / SEARCH_WIDTH /
     BUILD_ALGO / REFINEMENT；SEEKDB_CUVS_SYNC_SEARCH 用于在同一个二进制里复现基线的线程模型。

## 4. 参数扫描证据（bridge 级自测，250K，nq=200，精确暴力真值）

256d，graph_degree = intermediate_graph_degree = 192，fp32，width 8：

| niter | 建图 s | Recall@10 | Recall@100 |
|---|---|---|---|
| 20 | 13.2 | 0.9845 | 0.9508 |
| 30 | 16.0 | 0.9890 | — |
| 40 | 18.8–19.5 | 0.9920–0.9935 | 0.9687–0.9696 |
| 45 | 20.3 | 0.9935 | 0.9706 |
| 50 | 22.3 | 0.9975 | 0.9712 |
| 60 | 25.0 | 0.9970 | 0.9760 |

对照：基线配置（fp32，graph_degree 128，intermediate 256，niter 100）建图 47.9 s，
Recall@100 = 0.9332；官方 Hidden 的同轮基线为 0.9142。

结构性参数（256d，fp32，niter 40，width 8）：degree 96/128/160/192/256 的 Recall@100 分别为
0.865 / 0.868 / 0.956 / 0.969 / —；把 intermediate_graph_degree 降到 96 会掉到 0.868。
结论：降低图度数会显著掉召回，提升图度数可以在更短迭代下保住召回，这是本轮提速的主要来源。

被否决的路线：

- IVF_PQ 建图（build_algo = ivf_pq）：256d 建图 16.15 s（2.8x）但 Recall@100 仅 0.6609，
  refinement_rate = 0 / 0.5 / 1.0 几乎无差别（0.660 / 0.660 / 0.661），质量不可用。
- ITERATIVE_CAGRA_SEARCH 建图：建图 64.78 s 且 Recall@100 0.9271，比基线更慢更差。
- search itopk 放大到 1024 / 1536：cuvsCagraSearch 直接失败（rc=15，打印 cuVS 栈回溯），
  可用上限为 512；width 6/8 有效。
- 128d 用 ivf_pq / 降 degree 到 32：召回掉得比收益快。

128d（SIFT250K，fp16，width 4）：

| 配置 | 建图 s | Recall@10 | Recall@100 |
|---|---|---|---|
| degree 128, niter 10 | 6.45 | 1.0000 | 0.99996 |
| degree 128, niter 2 | 4.20 | 0.9998 | 0.99915 |
| degree 96, niter 2 | 3.16 | — | 0.99945 |
| degree 64, niter 2 | 2.91 | 1.0000 | 0.99940 |
| degree 64, niter 5 | 3.54 | — | 0.99995 |

## 5. 复现

- tools/hipvs_bridge 与 observer 的编译命令见第 2 节，与赛题 README 完全一致。
- docs/gpu-vector-index-hipvs-cuvs/validation/contest_bench.py：
  - prepare：从 fvecs/hdf5 切子集、写 base/query/TSV、按子集重算精确 Top-100 真值、记录 sha256；
  - run：冷启动 observer、导入、DDL → 首条查询计时、逐条查询计时、算召回、校验 ID 完整性、
    采样显存与 RSS；
  - compare：给出 build/qps 加速比、召回差、显存比、门限判定与分数模型；
  - report：把多轮结果汇总成 Markdown 表。
- docs/gpu-vector-index-hipvs-cuvs/validation/contest_gpu.sh：GPU 窗口内的一键流程
  （probe / bridges / obs / round / sweep / report），本轮所有数字均由它产生。

## 6. 风险与后续

- 256d Recall@10 相对同轮基线为 -0.0025 ~ -0.0039（门限 -0.005），余量偏小；若官方判定超限，
  把 dim > 128 的 niter 从 45 提到 55（建图约 27 s，1.70x）可把差值收回到 -0.001 以内。
- 查询侧的 1.5x 封顶已满足（本地 1.50–1.76x），进一步提速不再加分，后续优化应全部投向建图。
- 构建时间的理论上限受 SQL 侧固定开销约束：128d 建图 5.6 s 中约 3 s 与 NN-descent 无关
  （快照落盘、首条查询启动），256d 类似，因此单纯压缩 niter 的收益已接近饱和。
