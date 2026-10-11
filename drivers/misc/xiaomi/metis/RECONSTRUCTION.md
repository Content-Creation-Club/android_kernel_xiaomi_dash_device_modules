# METIS 源码复刻记录

2026-10-12。用户选择先复刻原厂行为、记录缺陷，再决定是否修复。本记录覆盖源码；编译和设备验证仍暂停。

## 来源和版本

- 原件：OS3.0.305 的 `metis.ko`，Linux 6.6.118。原文件路径已不存在；继续使用现存 Ghidra `/metis.ko-3b1f65` 和 `/scheduler.ko-5f307f` 中的原始指令、数据及 relocation 证据，未重新提取镜像。
- Device-module 起点：`5fec212ba964bd7bb074a9bc2d727e7c0ee6f418`，`dash-w-oss`。
- Native 起点：`1590799c59d00423858c95d8683a9cab84e86326`，Linux 6.6.144。
- 用户批准的共享类型改动：core `dff32a01801d`。原 `rwsem_waiter_type` / `rwsem_waiter` 定义原样移入内部 `kernel/locking/rwsem.h`，rwsem 和 METIS 消费同一份定义。
- 本轮源码版本以包含本记录的提交为准；提交号同时记录在 Android workspace 的 `docs/driver-bringup/dash-missing-stock-modules.md`。

## 覆盖口径

原件 412 个函数的源码计划已有 **412 个对应映射**：400 个显式 C 实现、4 个 ARM64 原始机器入口桥、6 个原生 API 编译器副本、2 个真实 trace 宏生成的调用包装。四个桥接入口的原 canonical C 类型仍未知；机器输入与观察到的类型字不等于 C typedef、MODVERSIONS 或可加载性已经确认。

本轮在原 261 个映射上增加 151 个。新增 helper、拆分函数和 trace/perf 生成胶水不额外计数。映射数量是源码交接记录，不代表当前 ABI、可加载性或运行验证通过。

- 原生 API 对应：`list_add`、`get_task_struct`、三个原件 `put_task_struct` 副本、`_copy_from_user`；使用当前 native API，不复制旧版编译器指令。
- Trace：保留原 7 个真实事件，补 23 个，合计 30 个 `TRACE_EVENT` 及真实后端；原先只有声明的事件已接入这些 schema。两个计入原函数计划的生成包装是 `trace_mi_get_futex_owner_pid` 和 `trace_mi_wakeup_link`。
- 恢复真实本地 defaults/BSS/perCPU owners、七项 important-task 字符串 relocation、模块/FAS init/exit、misc/fops、proc/syscore/sysctl 表。
- 106 个参数、17 组自定义参数 ops。两个频率数组保留共用的 `n_freq`；105 项权限 0644，`clear_persist_vip_task` 为 0200。
- 原件 148 个 GPL exports 保存 145 个；三个机器入口虽已保存，canonical C/MODVERSIONS 尚未确认，其导出留缺。TP fastpath 原本不是导出。

## 原始机器入口与 C 类型缺口

| 原入口 / Ghidra 地址 | 已保存算法和入口 | C 类型与接入边界 |
|---|---|---|
| `mi_check_preempt_wakeup_hook` / `001173c0` | 汇编将 rq/task/三个 bool 输出送入 `metis_final_preempt_wakeup_policy` | 额外输出实际是第 11 参数、入口 SP+0x10，用于跳过 provider 后续 MTK VIP；未使用 C 形参未知；原类型字 `9f5b6ed3` |
| `metis_coldstart_rt_find_lowest_rq` / `00125d2c` | 汇编将 mask/found/lowest 送入 `metis_final_coldstart_lowest_policy` | 第五个 NULL 的 C 类型未知；原类型字 `70b666d2`；真实调用在 MTK 内部选核之后、清 mask 之前 |
| `metis_choose_cpu_fastpath` / `001194bc` | 原始 context 汇编入口、延后字段读取、共享 choose policy | 原 context C tag/完整字段类型未知；原类型字 `c621b909`；当前 fair.c 不产生这份 context，也未调用 METIS |
| `metis_tp_choose_cpu_fastpath` / `00121cd8` | 原始 context 汇编入口、真实 slot util bridge、共享 TP policy | 同上；原类型字 `2cc2dd8f`；当前 provider 缺 CPU 表/slot 发布 |

四个原入口位于 `metis-stock-entries.S`，算法位于 `metis-final-scheduler-monitor.c` / `metis-final-scheduler-placement.c`。汇编通过原生 `SYM_TYPED_FUNC_START` 保存真实函数边界和观察到的类型字，未猜 C 原型、修改 C 函数生成的 CFI、生成假 CRC 或注册不匹配的 hook。

原 provider 拥有的 context 保持 opaque，不复制当前 fair.c/native core 结构。`metis-stock-context.S` 按原字段契约读取，placement 共享算法按原路径延后取值：choose excluded-pointer 在 enable 回调前读取（001194f8），skipped-pointer 在 allowed-mask 构造后读取（001195c4）；TP util 在分类/guard 前读取（00121cfc），cluster 在初始 idle 早退之后读取（00121d94），end 每次 row-end 重读（00121fa0）。普通 scalar 接口仍存在，不另复制一份算法。

抢占桥较原函数提前读取第 11 参数的栈槽；观察到的真实调用者始终提供该槽，没有提前解引用其中的指针，输出仍仅在原优先级分支写入。桥接、C helper 和 util tail-call 的栈帧/寄存器分配不同于原件；精确指令、故障栈和自动 sanitizer 胶水不宣称相同。

`get_cpu_util_without` 的零初始化 8 字节 METIS owner 已恢复为 opaque `void *`；原 callable typedef（family `86f86ddd`）仍 UNKNOWN。`metis-stock-util.c` 提供当前四参数类型的真实适配函数，私有汇编 helper 将 CPU/task 送到实际 slot，保留原 family 检查和 `BRK #0x8228`，不把 opaque slot 强转成猜测的 C callback、不填造 provider。实际 stock provider `0012d8b8` 包装 `mtk_cpu_util_next(cpu, task, -1, 0)`，适配器只服务现有 policy 的这组请求；它每次重读 slot，保留 entry guard 后 slot 变 NULL 的竞态。

原 C 类型未知时，不能从保留的类型字反推准确的 genksyms 类型/CRC；因此三个原 GPL 导出仍缺。将来的 source-backed native provider 可以使用现有真实 scalar C 接口并一同重编；这与直接兼容旧 stock scheduler.ko 是不同的交付，尚未实施。

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
- affinity 直接改 native mask/count，CPU32/low32 差异、空 mask 和恢复旧 mask 后 count 不一致照旧；线程列表遍历不加锁/引用。
- `metis_schlat_clear` 原参数 `get=param_get_int` 且 `arg=NULL`，读参数可能解引用 NULL；两个频率数组共用 count。
- 部分 trace 固定拷贝 16/32/64 字节，可能越界读取/无终止；字段名 `ppid` 实际保存 TGID，部分打印使用 live jiffies。实际使用 `strscpy` 的事件保留原安全拷贝，不把所有事件都归作 overread。

此前分类、VIP、冷启动、deadline、IO、延迟统计等片段的原异常和竞态仍保留；没有顺手修复。

## 接入与验证状态

Leaf `Makefile` 已列出实际源码对象，`Kconfig` 默认 n。本轮没有启用 parent/dash.config/Kleaf/package/load wiring，也没有补造 scheduler/FPSGO/sugov/perflock providers。叶级表和入口存在不代表依赖链闭环。

下一步需要在真实 MTK provider 中接回调用链，超出本轮 rwsem 共享类型的批准范围：

1. `drivers/misc/mediatek/sched/eas/vip.c` 的 `vip_check_preempt_wakeup`：在现有 `vip_enable` gate 前调用 METIS 抢占算法，再以 `*preempt || skip_mtk_vip` 决定是否跳过后续 MTK VIP。沿用现有 probe 注册。
2. `drivers/misc/mediatek/sched/eas/rt.c` 的 `mtk_find_lowest_rq` / out：在清空输入 mask 前，将实际 `avail_lowest_mask` 和选择结果交给冷启动算法；结果被置为 -1 时按原件从修改后的 mask 选择替代 CPU。独立 RVH probe 会拿到不同顺序/不同 mask。
3. `drivers/misc/mediatek/sched/fair.c` 的 `build_cpu_array` / `mtk_find_energy_efficient_cpu`：发布真实 CPU 表，并在已有选核阶段以真实 task-util、pause/excluded mask、跳过 mask、cluster/end 调用 placement policy；后续候选搜索需消费跳过结果。保留已有 task-turbo callback 的 owner。

这些调用需要配套的真实共享 C 声明/导出和模块依赖。当前 scalar interfaces 已有定义，原调用地址和当前源码插入位置分别见 ABI 报告；provider 修改未应用。其它已恢复算法所依赖的真实 scheduler/QoS producers 也没有因入口桥而自动补齐。

编译、native API/KMI/CFI、对象布局、链接、加载、运行和卸载全部 **UNTESTED**。没有执行构建、测试、生成 CFI、额外审计、hash/CRC/artifact 对比或设备操作；没有新 build stamp。旧产物记录不能用于当前提交。

## 原始证据索引

源码注释保留原函数地址。详细分工映射和原 bytes 位于 `/home/kokutoumikan/android-17/tmp/dash-missing-ko-re/metis/`：

- `symbols.txt`、`relocations.txt`、`disassembly.txt`、`stock-decompiled*.json`。
- `retained-data-20261011.json` 和 `retained-data-tail-20261011.json`；原 `.data` 从 `0016b6d8` 起，大小 0x402c。
- `final-scheduler-report-20261011.txt`、`final-link-report-20261011.txt`、`final-boost-report-20261011.txt`、`final-control-report-20261011.txt`。
- `final-owners-report-20261011.txt`、`final-trace-report-20261011.txt`、`final-root-report-20261011.txt`。
- `abi-preempt-report-20261012.txt`、`abi-rtlowest-report-20261012.txt`、`abi-placement-report-20261012.txt`；前者更正旧报告误记的第九参数。
- `stock-entries-report-20261012.txt`、`stock-util-report-20261012.txt`；root 在前者的两个简单入口交接后追加两个 placement 原入口及延后字段读取。

这些临时报告给出逐函数证据；本文件是随源码保存的覆盖、缺口、缺陷和验证状态入口。
