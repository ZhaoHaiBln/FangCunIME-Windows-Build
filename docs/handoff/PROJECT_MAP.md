# Windows 源码地图

## 目录

```text
FangCunIME-Windows/
├── ANTIGRAVITY.md                  交接入口
├── CMakeLists.txt                  版本、依赖、核心库、TSF DLL、测试
├── core/include/fangcun/           引擎、词库、排序、用户数据、主题模型接口
├── core/src/                      跨平台输入核心实现
├── tsf/include/                   Windows TSF、窗口、设置接口与 GUID
├── tsf/src/                       Windows COM/TSF 与 UI 实现
├── assets/                        自带词库和元数据
├── tests/                         核心与 KeyHandler 回归
├── installer/                     PowerShell、BAT、Inno Setup
├── .github/workflows/build.yml     公开仓库 Windows x64 构建
├── THIRD_PARTY_NOTICES.md          第三方声明
├── licenses/                      许可文本
└── docs/handoff/                  交接说明、提示词、校验清单
```

## 按修改目标定位

| 想修改什么 | 优先查看 | 关联与注意点 |
| --- | --- | --- |
| 候选窗布局、皮肤、字体、颜色、光标跟随 | `tsf/src/CandidateWindow.cpp`、`tsf/include/CandidateWindow.hpp` | Direct2D/DirectWrite；主题值还涉及 `core/include/fangcun/ThemeConfig.hpp` |
| 设置窗口、控件和保存反馈 | `tsf/src/SettingsWindow.cpp`、`tsf/src/SettingsStore.cpp` | 保存/广播设置；`settings.ini` 属于用户数据 |
| 空格、数字、退格、Esc、Enter 的行为 | `tsf/src/KeyHandler.cpp`、`tests/test_keyhandler.cpp` | 区分需要输入法处理的按键与应交还宿主的按键 |
| Windows 输入法激活、焦点、文档与 composition | `tsf/src/TextService.cpp`、`tsf/src/EditSession.cpp` | 宿主线程、COM 生命周期、异步回调和会话代次不能混用 |
| 小鹤双拼、全拼、九宫格和混输 | `core/src/InputScheme.cpp`、`FlypyConverter.cpp`、`MixedInput.cpp` | 引擎组合在 `FangCunEngine.cpp`；用真实词库回归 |
| 查询性能、取消、旧结果丢弃 | `core/src/FangCunEngine.cpp` | 查询线程和 generation 与 `TextService`、`KeyHandler` 联动 |
| 候选排序、学习与个人词典 | `CandidateRanker.cpp`、`UserStore.cpp` | `user.db` 是用户数据，不属于 `assets/` 的基础词库 |
| 词库打开、缓存与首次解压 | `core/src/DictionaryRepository.cpp` | `pinyin-v1.dbz` 使用 zlib；保留原始基础词库 |
| COM、TSF 注册及显示名称 | `tsf/src/Register.cpp`、`tsf/include/Guid.hpp`、`FangCunTSF.def` | 更改 GUID、导出函数或注册范围会影响升级和已有注册 |
| 安装包、升级与卸载 | `installer/FangCunIME.iss`、`install.ps1`、`upgrade.ps1`、`uninstall.ps1` | DLL 可能被宿主加载；用户数据需要保留 |
| CI 与产物内容 | `.github/workflows/build.yml` | MSVC x64 + Ninja；Setup 和 portable 为两份 artifact |

## 必须一起交接的资源

| 文件 | 用途 |
| --- | --- |
| `assets/dict.db` | 随项目提供的基础词库 |
| `assets/pinyin-v1.dbz` | 压缩拼音数据库 |
| `assets/pinyin-v1.json` | 拼音数据库元数据 |
| `THIRD_PARTY_NOTICES.md`、`licenses/rime-ice-LICENSE.txt` | 词库第三方声明与许可 |

三份资源已经包含在源码包中。不要把本机的 `user.db`、`settings.ini`、构建缓存、安装目录或 Android 工程打进源码交接包。
