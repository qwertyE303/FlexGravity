# FlexGravity

> 给 Ballance 地图加上自定义重力场！

**当前支持BML+ 0.3.12+ / BML 0.3.40+！**

*相关制图教程：[自定义重力场制图教程](docs/自定义重力场制图教程.pdf)*

## 你可以创造/游玩到什么

| 类型 | 描述 |
|---|---|
| **匀强场** | 均匀强度的重力场 |
| **单极子** | 类似点电荷，受力是平方反比 |
| **线场** | 在线上均匀分布重力源，离得近时受力是一次方反比，离得远/在线两端受力会不同 |
| **面场** | 在面上均匀分布重力源，离得近时是匀强场，离得远/在面边缘受力会不同 |
| **任意自定义场** | 用函数表达式写的任意重力场，可以能量不守恒 |
| **自定义作用域（制图软件绘制/函数绘制）** | 任意划定某个场起作用的范围 |

## 配置

### BML+

| 选项 | 默认 | 说明 |
|---|---|---|
| `DebugMode` | `0` | 下列调试功能、`fg debug` 命令的**总开关** |
| `EnableLog` | `0` | 输出调试日志。 |
| `ShowGVector` | `0` | 在球心生成蓝色箭头，显示玩家球所在点的实际重力方向，旁边标大小。 |
| `EnableFieldLines` **（不完善，勿用）** | `0` | 显示红色重力场线 |
| `EnableEquipotentials` **（不完善，勿用）** | `0` | 显示半透明黄色等势面 |
| `FieldLineDensity` | `10` | 重力场线密度 |
| `EquipotentialDensity` | `10` | 等势面密度 |
| `DebugViewDistance` | `50` | 覆盖层显示的视距 |

### BML

| 选项 | 默认 | 说明 |
|---|---|---|
| `DebugMode` | `0` | 下列调试功能、`fg debug` 命令的**总开关** |
| `EnableLog` | `0` | 输出调试日志。 |
| `ShowGVector` | `0` | 在球心生成蓝色箭头，显示玩家球所在点的实际重力方向，旁边标大小。 |

---

## 命令

### /fg debug

**需要打开DebugMode**

```text
fg debug freemode 0|1|true|false   取消 / 恢复玩家球受到的全部重力
fg debug status                    扫描摘要与计数
fg debug list                      完整解析结果（写日志）
fg debug fstatus                   球位置处每个源/CField 的贡献、总加速度
fg debug custom [at x y z]         CField / CRegion 逐行视图
fg debug expr [at x y z] <表达式>  即时编译并求值
fg debug domain [at x y z] <表达式> 即时编译并求值作用域
fg debug viz                       覆盖层计数器与投影自检
fg debug reload                    重新扫描
fg debug help
```

**注：除了freemode，其他基本没啥用可以不管。**

### /fg help

帮助

