# FlexGravity

> 给 Ballance 加上"地图作者自定义重力场"的 BML+ 模组。

FlexGravity 让关卡作者可以在 Virtools 里摆几个区域、几个源点，玩家滚进去就会被
吸住、被弹开、被托起来，或者沿着一条隐形的"重力线"绕圈。

模组本身没有任何游戏内界面，装上即用；具体某个关卡有没有重力场、是什么效果，
完全由那张地图决定。

---

## 安装

### 前置

- Ballance（原版即可）
- **BML+ 0.3.13**（Ballance Mod Loader Plus）

### 步骤

1. 关闭游戏。
2. 把 `FlexGravity.bmodp` 复制到：

   ```text
   D:\Ballance\ModLoader\Mods\
   ```

3. 启动游戏。加载成功时左下角会出现一条提示；`EnableLog = 1` 时日志 `ModLoader.log` 里还能看到：

   ```text
   FlexGravity 0.1.0 loading (BML+ 0.3.13)
   ```

4. 进入带重力场的地图，滚进去就能感受到。

### 卸载

删掉 `D:\Ballance\ModLoader\Mods\FlexGravity.bmodp` 即可，不会在存档或关卡里留下任何东西。

---

## 配置

配置文件：`D:\Ballance\ModLoader\Configs\FlexGravity.cfg`（首次启动自动生成）

```ini
Debug {
  B DebugMode 0
  B EnableLog 0
  B ShowGVector 0
  B EnableFieldLines 0
  B EnableEquipotentials 0
  F FieldLineDensity 10
  F EquipotentialDensity 10
  F DebugViewDistance 100
}
```

| 选项 | 默认 | 说明 |
|---|---|---|
| `DebugMode` | `0` | **总开关**：调试覆盖层、`fg debug` 命令、下面所有诊断项的前提。正常游玩请保持关闭——不画任何东西，帧率不受影响 |
| `EnableLog` | `0` | 写详细日志。**关闭时模组一行日志都不写**（你主动敲的控制台命令除外） |
| `ShowGVector` | `0` | 在球上画一根蓝色箭头，指向当前**总重力**（引擎重力 + 所有生效的场），并在旁边标出大小 |
| `EnableFieldLines` | `0` | 红色场线（诊断用，见下） |
| `EnableEquipotentials` | `0` | 半透明黄色等势面：只画球附近的几层（诊断用，不含自定义场） |
| `FieldLineDensity` | `10` | 场线密度：100 = 在重力大小 20 的地方每 1 单位一条线 |
| `EquipotentialDensity` | `10` | 等势面层密度，同一套刻度 |
| `DebugViewDistance` | `100` | 覆盖层的绘制半径（以相机为球心的世界单位） |

`DebugMode` 与 `EnableLog` 都关闭时，模组在日志里完全静默；排查问题时把 `EnableLog` 打开即可，
改配置立即生效。

---

## 游戏内命令（仅调试）

命令分三级。命令名就是 `fg`；`/` 只用来唤出命令行，**不要**把命令写成 `/fg`
（BML 不会剥掉前导 `/`，`/fg` 会提示 Unknown Command）：

```text
fg                    显示用法
fg debug              列出可用的调试子命令
fg debug <command>    执行
```

> `fg debug ...` 只有在 `DebugMode = 1` 时才放行。关闭时命令还在，但会提示
> `debug commands are disabled`。配置改动**立即生效**，不需要重启。

| 命令 | 作用 |
|---|---|
| `fg debug status` | 扫描摘要：当前关卡读到了几个场、几个源、几行自定义场，以及 `ignored` / `warnings` 计数 |
| `fg debug list` | 把完整解析结果写进日志（U 场、M/L/S 源、`FG_R` 行、`FG_RG`、每行 `FG_Custom` 的原文） |
| `fg debug fstatus` | **最有用**：球位置处每个源、每个自定义场的贡献，总加速度，通道数与受影响物体数 |
| `fg debug custom [at x y z]` | 自定义场逐行：`Domain` 是否通过、该行加速度、总和 |
| `fg debug expr [at x y z] <表达式>` | 即时编译并求值一个表达式（`x y z r theta phi` 都能用），语法错误会给出字节位置 |
| `fg debug domain [at x y z] <关系式>` | 即时编译作用域，逐条打印两边的取值和结果 |
| `fg debug viz` | 调试覆盖层的计数器（帧数、箭头投影、场线与等势面统计） |
| `fg debug reload` | 重新扫描关卡数据（改完地图或换关卡不必重启） |
| `fg debug help` | 命令列表（`?` 等价） |

说明：

- `at x y z` 是可选的求值点，不给就用当前球的位置；球不在附近时也能查任意坐标。
- 命令的输出照常写进日志，**即使 `EnableLog = 0`**——它们是你主动要求的，`list` 也只靠日志。
- `expr` / `domain` 用的是游戏里同一套解析器，所以"编辑器能过、游戏里认不认"可以直接当场验证。

示例：

```text
> fg debug fstatus
sources=3 R rows=3 global=0 custom=0 total=(130.2568, -129.1846, 0.0017) channels=1 affected=12
```

---

## 调试可视化

| 开关 | 你会看到 |
|---|---|
| `ShowGVector` | 蓝色箭头指向总重力方向，旁边标 `g=` 数值；被球挡住的部分不会画出来（重叠层没有深度测试，这是用射线—球求交自己算的） |
| `EnableFieldLines` | 红色场线：沿重力方向积分出来的曲线，能看出"东西会往哪走"。顶点数有上限（最近的那些线优先），相机不动时直接复用上一帧的投影 |
| `EnableEquipotentials` | 半透明黄色等势面：等势位的"地形"。**只画球所在势位附近的 ±3 层**，层间距仍由 `EquipotentialDensity` 决定（默认 200 势位单位 ≈ 10 米）；它**不包含自定义场**——任意表达式场一般不是保守场，没有一致的势 |

两个图层共用一份顶点预算（场线 6000 点、等势面 6000 个三角形），合计固定在 4.2 万顶点以内，
所以不会再出现顶点超限导致的整屏错乱。`fg debug viz` 里的 `vertices=` 是实测值，
`batchProjection maxDeltaPx=` 是"批量投影与单点投影一致"的现场自检（应为 0.0000）。

球在移动时等势面是**按整步成批更换**的（不是逐帧抖动）：势位跨过一层边界才会换一批面。
覆盖层在游戏暂停时自动隐藏，不会盖住 ESC 菜单。

---

## 你会遇到什么

关卡里可能出现的五种场：

| 类型 | 手感 |
|---|---|
| **匀强场（U）** | 滚进某个区域，整体变轻 / 变重 / 被往某个方向推，离开立刻恢复 |
| **单极子（M）** | 像有个看不见的星球，越近吸得越狠；负值则是排斥 |
| **线场（L）** | 被吸向一条隐形的线，沿线方向力差不多，靠近两头会减弱；穿过线时会猛地被拽一下 |
| **面场（S）** | 被垂直于一块平面吸住；穿过平面后会被往回拉（双面）；靠近平面边缘会有"往里吸"的拉扯感 |
| **自定义场（C）** | 作者用表达式写的任意向量场：可以做"离心环""沿某方向越来越强""只在某个空间范围内"这类效果；每行的 `Domain` 决定它在哪生效，形状完全由公式决定 |

这些都不是 bug，是场本身的形状。

### 注意事项

- 场只作用于**可移动的动态物体**（球、箱子、木板……），固定的地形不受影响。
- 场是**叠加**的：多个场重叠时加速度相加，不会互相覆盖。
- 场不设上限：数值给得太大，物体会被弹飞甚至物理爆炸，这是地图作者的责任。
- 相机、死亡、复活、检查点逻辑完全保持原版。

---

## 兼容性

- 与 PhysicsEditor、ReducedGravity 这类改全局重力的模组**可以共存**：FlexGravity 加的是
  增量，不覆盖全局重力。
- 和其它修改同一批物体的物理模组可能互相干扰（谁后执行谁生效）。
- 没有重力场数据的普通关卡：模组不做任何事，零开销。

---

## 制图

**关卡作者请看《自定义重力场制图教程.pdf》。**

里面写清了：五个 Group（`FG_U` / `FG_M` / `FG_L` / `FG_S` / `FG_R`）、七个 DataArray
（`FG_U` / `FG_M` / `FG_L` / `FG_S` / `FG_R` / `FG_RG` / `FG_Custom`）、对象命名、
五种场的参数怎么填、作用域怎么划，以及性能约束（区域网格别用高模、面场别放太多）。

简单说：所有数据都在 Virtools 的 `Global/Groups` 和 `Global/Arrays` 里，
按固定名字建好、填好，进游戏就生效，不需要写任何脚本。

`FG_Custom` 是个例外：它没有 Group、也没有 Object 列，每行就是三格表达式
（直角 `X/Y/Z` 或球坐标 `R/Theta/Phi`）加一格 `Domain`；`Domain` 满足时整行生效，
不满足就跳过。表达式语法与示例见编辑器里的「函数填写说明」窗口。

### Group 是硬性条件

数组里的每一条记录，其对象必须**同时**在对应的 Group 里：

- `FG_U` 行 → 对象 ∈ `FG_U` group
- `FG_M` / `FG_L` / `FG_S` 行 → 对象 ∈ 同名 group
- `FG_R` 行 → **区域对象** ∈ `FG_R` group
- `FG_RG` 没有 Group，但它列出的源必须已经通过自己那一组的校验

Group 里没有、数组里有 → 这条记录**不生效**，日志里留一条带 `(ignored)` 的 warning，
`fg debug status` 的 `ignored=N` 会 +1。

`FG_R` 行的区域不在组里时**整行作废**：区域就是作用域，作用域为空，行内列出的源也不再作用。

---

## 从源码构建（开发者）

需求：

- Windows + Visual Studio 2022（x86 工具链）
- CMake 3.20+
- BML+ 0.3.13 SDK
- Virtools SDK 2.1

```powershell
cmake -S . -B ..\build -G "Visual Studio 17 2022" -A Win32
cmake --build ..\build --config Release --parallel 4
```

产物：`..\build\bin\FlexGravity.bmodp`。

> 必须是 **Win32 / x86**：Ballance 和 BML+ 都是 32 位。

### 测试与基准

两个 exe 都只依赖 Virtools 的数学库，不需要游戏、不需要 BML 运行时：

```powershell
..\build\bin\flexgravity_tests.exe   # core 单测（636 项断言）
..\build\bin\flexgravity_bench.exe   # 区域判定 / 场求值基准
```

不想构建它们时：`-DFLEXGRAVITY_BUILD_TESTS=OFF`。

### 源码结构

```text
src/core/         纯数学与数据，零引擎依赖（可脱离游戏测试）
src/core/expr/    FG_Custom 表达式内核：词法 / 语法 / 后缀式 VM
src/vt/           引擎边界：对象/数组访问、SEH 曲线探针、几何提取、施力通道
src/scan/         Group / DataArray 解析与校验
src/runtime/      每帧场求值
src/viz/          调试覆盖层：重力箭头、场线、等势面
src/diagnostics/  控制台报告、表驱动命令
src/mod/          IMod 实现、生命周期、配置
```

---

## 致谢

- [BallanceModLoaderPlus](https://github.com/doyaGu/BallanceModLoaderPlus)
- [physics_RT](https://github.com/doyaGu/physics_RT)
- [IPion / IVP](https://github.com/doyaGu/ivp)（1/66 物理步长的来源）
- [BallanceModding](https://github.com/BearKidsTeam/BallanceModding)
