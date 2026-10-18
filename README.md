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

3. 启动游戏。加载成功时左下角会出现一条提示，日志 `ModLoader.log` 里能看到：

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
}
```

| 选项 | 默认 | 说明 |
|---|---|---|
| `DebugMode` | `0` | 打开后：① 把每一次施力的加速度写进 `ModLoader.log`；② **解锁 `fg debug` 下的调试命令**。正常游玩请保持关闭——日志不会刷屏，帧率不受影响 |

除这一项外没有任何其它设置。

---

## 游戏内命令（仅调试）

命令分三级，`fg` 本身始终存在（写成 `/fg` 也可以，两者等价）：

```text
fg                    显示用法
fg debug              列出可用的调试子命令
fg debug <command>    执行
```

> `fg debug ...` 只有在 `DebugMode = 1` 时才放行。关闭时命令还在，但会提示
> `debug commands are disabled`。配置改动**立即生效**，不需要重启。

| 命令 | 作用 |
|---|---|
| `fg debug status` | 显示当前关卡读到了几个场、几个源 |
| `fg debug list` | 把解析结果写进日志（曲线端点、平面尺寸等） |
| `fg debug ball` | 当前球、质量、是否已物理化 |
| `fg debug ustatus` | 匀强场：球是否在区域里、当前受影响的物体数 |
| `fg debug fstatus` | **最有用**：每个源对球的贡献、距离、总加速度 |
| `fg debug force x y z` | 给球加一个手动加速度（会暂停所有自动场） |
| `fg debug clear` | 取消手动加速度，恢复自动场 |
| `fg debug reload` | 重新扫描关卡数据 |
| `fg debug help` | 命令帮助 |

示例：

```text
> fg debug fstatus
sources=3 R rows=3 global=0 total=(130.2568, -129.1846, 0.0017) channels=1
```

---

## 你会遇到什么

关卡里可能出现的四种场：

| 类型 | 手感 |
|---|---|
| **匀强场（U）** | 滚进某个区域，整体变轻 / 变重 / 被往某个方向推，离开立刻恢复 |
| **单极子（M）** | 像有个看不见的星球，越近吸得越狠；负值则是排斥 |
| **线场（L）** | 被吸向一条隐形的线，沿线方向力差不多，靠近两头会减弱；穿过线时会猛地被拽一下 |
| **面场（S）** | 被垂直于一块平面吸住；穿过平面后会被往回拉（双面）；靠近平面边缘会有"往里吸"的拉扯感 |

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
- 没有重力场数据的普通关卡：模组只写一行 `No FlexGravity data in this level`，
  不做任何事，零开销。

---

## 制图

**关卡作者请看《自定义重力场制图教程.pdf》。**

里面写清了：五个 Group（`FG_U` / `FG_M` / `FG_L` / `FG_S` / `FG_R`）、六个 DataArray、
对象命名、四种场的参数怎么填、作用域怎么划，以及性能约束（区域网格别用高模、面场别放太多）。

简单说：所有数据都在 Virtools 的 `Global/Groups` 和 `Global/Arrays` 里，
按固定名字建好、填好，进游戏就生效，不需要写任何脚本。

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

---

## 致谢

- [BallanceModLoaderPlus](https://github.com/doyaGu/BallanceModLoaderPlus)
- [physics_RT](https://github.com/doyaGu/physics_RT)
- [IPion / IVP](https://github.com/doyaGu/ivp)（1/66 物理步长的来源）
- [BallanceModding](https://github.com/BearKidsTeam/BallanceModding)
