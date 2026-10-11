# METIS 源码复刻记录

2026-10-12。用户选择先复刻原厂行为、记录缺陷，再决定是否修复。本记录覆盖源码；编译和设备验证仍暂停。

## 来源和版本

- 原件：OS3.0.305 的 `metis.ko`，Linux 6.6.118。原文件路径已不存在；继续使用现存 Ghidra `/metis.ko-3b1f65` 和 `/scheduler.ko-5f307f` 中的原始指令、数据及 relocation 证据，未重新提取镜像。
- 算法复刻起点：`5fec212ba964bd7bb074a9bc2d727e7c0ee6f418`，`dash-w-oss`；本轮 provider 接线起点：`1198930313195de32843217cb01025104496c3da`。
- Native 起点：`1590799c59d00423858c95d8683a9cab84e86326`，Linux 6.6.144。
- 用户批准的共享类型改动：core `dff32a01801d`。原 `rwsem_waiter_type` / `rwsem_waiter` 定义原样移入内部 `kernel/locking/rwsem.h`，rwsem 和 METIS 消费同一份定义。
- 本轮源码版本以包含本记录的提交为准；提交号同时记录在 Android workspace 的 `docs/driver-bringup/dash-missing-stock-modules.md`。

## 覆盖口径

原件 412 个函数的源码计划已有 **412 个对应映射**。此前的四个 ARM64 机器入口桥现在由共享声明的原生 C adapter 取代，连接已恢复的算法；普通 helper/adapter 不重复计入算法数量。原入口 canonical C 类型仍未知；原生源码接线不代表 stock MODVERSIONS、可加载性或运行验证已经确认。

`119893031319` 在原 261 个映射上增加 151 个。本轮接线保持算法映射数量不变。映射数量是源码交接记录，不代表当前 ABI、可加载性或运行验证通过。

- 原生 API 对应：`list_add`、`get_task_struct`、三个原件 `put_task_struct` 副本、`_copy_from_user`；使用当前 native API，不复制旧版编译器指令。
- Trace：保留原 7 个真实事件，补 23 个，合计 30 个 `TRACE_EVENT` 及真实后端；原先只有声明的事件已接入这些 schema。两个计入原函数计划的生成包装是 `trace_mi_get_futex_owner_pid` 和 `trace_mi_wakeup_link`。
- 恢复真实本地 defaults/BSS/perCPU owners、七项 important-task 字符串 relocation、模块/FAS init/exit、misc/fops、proc/syscore/sysctl 表。
- 106 个参数、17 组自定义参数 ops。两个频率数组保留共用的 `n_freq`；105 项权限 0644，`clear_persist_vip_task` 为 0200。
- 原件 148 个 GPL export 名称已保存，最后三个名称采用共享的 native-adapted C 契约，需与 scheduler 一同重编。原 canonical C/MODVERSIONS 仍未知；TP fastpath 原本不是导出，保持本地调用。

## 原生共享接口与原 C 类型缺口

| 原入口 / Ghidra 地址 | 已恢复算法 / 原生入口 | C 类型与接入边界 |
|---|---|---|
| `mi_check_preempt_wakeup_hook` / `001173c0` | 五参 C adapter 调用 `metis_final_preempt_wakeup_policy` | rq/task/三个 bool 输出采用当前类型；原额外输出实际是第 11 参数、入口 SP+0x10，用于跳过后续 MTK VIP；原 unused formals 未知，类型字 `9f5b6ed3` 不用于新 CFI |
| `metis_coldstart_rt_find_lowest_rq` / `00125d2c` | 三参 C adapter 调用 `metis_final_coldstart_lowest_policy` | 使用真实 mask/int found/int*lowest；原第五个 NULL 的 C 类型未知；原类型字 `70b666d2` 不用于新 CFI |
| `metis_choose_cpu_fastpath` / `001194bc` | 七参 C adapter 调用共享 choose policy | 当前 provider 显式传入 task-util、skipped/excluded mask、cluster/end/skip；不复制原 context，也不猜其 C tag；原类型字 `c621b909` 不用于新 CFI |
| `metis_tp_choose_cpu_fastpath` / `00121cd8` | 五参 C adapter 调用共享 TP policy | 使用实际 util/table provider 与 allowed mask；原类型字 `2cc2dd8f` 不用于新 CFI；原函数不导出 |

四个原生入口位于 `metis-scheduler-interface.c`，算法仍位于 `metis-final-scheduler-monitor.c` / `metis-final-scheduler-placement.c`。`include/linux/xiaomi_metis.h` 是 scheduler、METIS 和 MIGT 使用的真实共享声明。原 `metis-stock-entries/context/util-call` 汇编桥及 opaque helper 已移除；观察到的原始指令、类型字、字段读取次序保留在证据报告与此前提交中。

placement 直接传递当前 provider 的 scalar 和 mask；不搬出或复制 `fair.c` 私有 FBC 结构。传参替代原 opaque context 的延后字段读取，C adapter 与原件的栈帧、寄存器、CFI、故障栈和自动 sanitizer 胶水不宣称相同。三个原 GPL 名称的 ABI 已明确改变，不能混用旧 stock scheduler.ko。

`get_cpu_util_without` 保留真实零初始化 owner，改为共享的 `unsigned long(int, struct task_struct *)` native callback。`fair.c` 的真实 adapter 调用 `mtk_cpu_util_next(cpu, task, -1, 0)`，由现存 cpufreq_sugov_ext provider 实现；METIS 不反向直接导入 scheduler。每次调用仍重读 slot，保留 entry guard 后 callback 消失的竞态。原 callable typedef（family `86f86ddd`）仍 UNKNOWN；新 CFI/CRC 应由实际编译生成，本轮未生成或验证。

`set_foreground_pid` / `00116704` 是原件 literal RET。源码保留空实现；unused `unsigned int` 形参根据 `019c0cac` setter 家族推断，原 canonical typedef 仍 UNKNOWN，不能宣称原 CFI 已确认。

## 保留的原件缺陷

以下为原指令、数据或生命周期证据中的行为，尚未在设备触发，也未修复。

- 初始化只以最终 `misc_register` 状态决定返回值；之前的错误忽略、失败无 unwind，部分回调先于 FAS 状态初始化发布。卸载漏撤 hook、线程、notifier、timer/work，缺完整同步；comm-monitor exit 是实际 RET。
- Binder/futex/rwsem owner 在无引用或离开 RCU 后继续使用；usage 检查后再加引用存在竞态。Futex 仅改 `node_list`；rwsem corruption reporter 阻止插入时，already-listed 输出仍可能为 true；timeout/debug 路径可解引用 NULL task。
- Boost 只处理 cluster 1/2；必需 perflock callback 和返回数组未检查 NULL，频率稳定循环没有上限。执行/删除后的 plain clear 可丢并发请求。
- QoS 首次失败移除已激活请求，再注册 policy notifier；注册标志先置位、错误忽略，失败 visit mask 不恢复。更新 policy 时先 put，再读取 `policy->cpu` 并调用 update；原 `policy+0x28` 对应 `cpuinfo.max_freq`。
- ioctl 按低 8 位分派，部分 copy/未知命令返回成功；mmap 返回 0 而不建立映射。参数 getter 返回数值/长度却不写 buffer；VIP 计数可无条件递减。
- signed UID 比较允许 wrapped-negative `u32` 越界索引；`uid_type` 允许 200000..200063 越过 bitmap。PID/频率 parser 部分更新、泄漏、旧槽和排序假设照旧。
- 私有 task `+0xd0` 既被当执行时间，也作为 affinity mask；`+0xd8` 写入重叠。使用真实 driver storage 保留这项冲突，native 布局与后果未验证。
- runtime-history 接受 bucket 10，覆盖私有 priority 字段，随后 duration 写入可超出原 0x280 对象；未扩大对象来隐藏原越界。历史发布、计数和索引无锁/可 wrap。
- LB 先释放 source 锁再取得 destination 锁，不更新 rq clock；fallback 可在未从 source 移除/未 `set_task_cpu` 时激活；reverse walk 实际允许六项。
- stock fair placement 将 skipped mask 和 skip flag 交给 METIS，却没有在后续 MTK 选核中消费；本轮按原指令保留丢弃输出，没有补上 mask 过滤。实际生效的另一个 MIGT skip-mask consumer 属于不同接口。
- affinity 直接改 native mask/count，CPU32/low32 差异、空 mask 和恢复旧 mask 后 count 不一致照旧；线程列表遍历不加锁/引用。
- `metis_schlat_clear` 原参数 `get=param_get_int` 且 `arg=NULL`，读参数可能解引用 NULL；两个频率数组共用 count。
- 部分 trace 固定拷贝 16/32/64 字节，可能越界读取/无终止；字段名 `ppid` 实际保存 TGID，部分打印使用 live jiffies。实际使用 `strscpy` 的事件保留原安全拷贝，不把所有事件都归作 overread。

此前分类、VIP、冷启动、deadline、IO、延迟统计等片段的原异常和竞态仍保留；没有顺手修复。

## 接入与验证状态

用户已明确批准 upstream/provider 修改并授权共享文件选择。本轮实际接回以下源码调用：

1. `eas/vip.c`：在现有 `vip_enable` 前调用五参抢占入口，以 `*preempt || skip_mtk_vip` 提前返回；保持现有 RVH 注册。
2. `eas/rt.c`：真实输入 mask 在过滤前经过 cold-start UI 排除；MTK 选核后、清输入 mask 前，将私有 `avail_lowest_mask` 交给冷启动入口。结果为 -1 且 reason 不是 `LB_RT_FAIL` 时，从修改后的 mask 选替代 CPU。原 bytes `001237e4` 比较 0x1000，旧报告写作“排除 no-target reason”错误；stock !ret 使用 0x80000。METIS 可达时将私有 mask 初始化为空，与 stock `001236e0` 一致，防止 !ret 后的新兜底读取当前未初始化 mask。
3. `fair.c`：在完成 `build_cpu_array` 后发布真实 CPU 表；candidate stage 发布真实 util callback，以 `metis_vip_task(p) || (migt_sched_enable && migt_sched_enable())` 进入 choose。同步回接 stock 的 overutil/NULL-pd VIP/controller bypass 与 sync 非 METIS-VIP guard。使用真实 task-util、pause mask、cluster/end；返回 CPU 进入既有 energy selection，skipped/skip 输出按原厂丢弃。保持 `vip_loom_select_task_rq_fair_hook` owner 与私有 FBC 结构。恢复 scheduler 真正拥有的零初始化 `migt_sched_enable` slot，由现有 MIGT init/exit 发布/清理。
4. `fair.c` 的现有 `android_rvh_sched_fork_init`：在 MTK soft-affinity 初始化前调用实际 `mi_task_fork(NULL, task)`，采用已有 mi_schedule header，受 `IS_REACHABLE(CONFIG_XIAOMI_MI_SCHEDULE)` 控制。stock 调用 `0012b190` 见 `mi_schedule/stock-scheduler-fork-callsite.json`；不新增第二个 fork probe，不对已存在任务重复分配。现有 mi_schedule init 仍负责其原先的任务初始化与缺陷。

provider 引用由 `IS_REACHABLE(CONFIG_XIAOMI_METIS)` 控制；scheduler=m、METIS=m 可形成直接依赖，builtin scheduler 不能调用 modular METIS，配置该组合时这些接线不生效。源码依赖为 scheduler → METIS → mi_schedule / xr-qi → perf-actuator，现有真实 utility callback 不制造反向模块导入。METIS Kconfig 增加其实际硬导入依赖 `XIAOMI_MI_SCHEDULE && XIAOMI_XR_QI`。

Parent Kconfig/Makefile 已加入 METIS traversal，默认仍为 n；没有修改 dash.config、Kleaf、package 或 load maps。现有调度 hook/卸载和 table/provider lifetime 缺陷仍保留，清 publication 不代表完整同步或安全卸载。其它已恢复算法所需的 runtime、scheduler accounting、LB capacity/overutil/halt 与 perflock/corectl providers 仍存在缺口；stock wake/DSU、独立 MIGT selection/skip producers 也未在本轮补齐，不能把这三条调用链和 fork 状态生产者宣称为全部 METIS runtime 接入完成。

编译、native API/KMI/CFI、对象布局、链接、加载、运行和卸载全部 **UNTESTED**。本轮只读取源码、现存原 bytes 和报告来决定接线；没有执行构建、测试、生成 CFI、hash/CRC/artifact 对比或设备操作；没有新 build stamp。旧产物记录不能用于当前提交。

## 原始证据索引

源码注释保留原函数地址。详细分工映射和原 bytes 位于 `/home/kokutoumikan/android-17/tmp/dash-missing-ko-re/metis/`：

- `symbols.txt`、`relocations.txt`、`disassembly.txt`、`stock-decompiled*.json`。
- `retained-data-20261011.json` 和 `retained-data-tail-20261011.json`；原 `.data` 从 `0016b6d8` 起，大小 0x402c。
- `final-scheduler-report-20261011.txt`、`final-link-report-20261011.txt`、`final-boost-report-20261011.txt`、`final-control-report-20261011.txt`。
- `final-owners-report-20261011.txt`、`final-trace-report-20261011.txt`、`final-root-report-20261011.txt`。
- `abi-preempt-report-20261012.txt`、`abi-rtlowest-report-20261012.txt`、`abi-placement-report-20261012.txt`；前者更正旧报告误记的第九参数。
- `stock-entries-report-20261012.txt`、`stock-util-report-20261012.txt`；root 在前者的两个简单入口交接后追加两个 placement 原入口及延后字段读取。
- `native-vip-rt-report-20261012.txt` 更正 RT fallback 的旧推断；该代理停在证据报告，VIP/RT 由 root 据此修改。`native-fair-report-20261012.txt` 记录真实 placement gate、丢弃输出和当前 provider lifetime；`native-dependencies-report-20261012.txt` 记录真实符号依赖与未接的其它 producer。

这些临时报告给出逐函数证据；本文件是随源码保存的覆盖、缺口、缺陷和验证状态入口。
