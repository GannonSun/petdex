# Petdex Windows 桌面宠物产品化计划

## 目标

Petdex 要先成为一个普通人也能直接使用的 Windows 桌面宠物应用：用户下载、安装、双击，就能在桌面上陪着自己的猫猫狗狗。

非开发人员不需要 Codex、Claude、Gemini 或任何 agent，也不需要会开发。朋友只要下载安装 Windows 应用，就能在桌面上看到自己的宠物。宠物形象可以由维护者根据朋友家的宠物照片生成，上传到 Petdex，然后让朋友直接下载安装或导入使用。

Agent 联动是增强功能，不是核心依赖。

## 当前 Windows v0 状态

已完成：

- Windows 可以构建 `petdex-desktop.exe`。
- 桌面上可以显示透明背景、置顶的小宠物。
- 支持拖动。
- 支持跨屏拖动和跨 DPI 显示。
- 宠物本体已改为原生 Win32 渲染，不再依赖 WebView2 主窗口。
- 使用双窗口结构：
  - 渲染窗口负责透明宠物绘制。
  - 输入窗口负责拖动和右键。
- 宠物素材从本机目录读取：
  - `%USERPROFILE%\.petdex\pets\<宠物名>\`
  - `%USERPROFILE%\.codex\pets\<宠物名>\`
- 没有 agent 也能单独运行。

## 当前 Windows 后端方向

Windows 版主宠物窗口采用：

- `Win32`
- `WIC`
- `UpdateLayeredWindow`
- 原生透明分层窗口

不建议再把 WebView2 当作主宠物窗口。

WebView2 后续可以只用于设置页、宠物商店、登录上传等辅助 UI。原因是 WebView2 透明窗口在 Windows 多屏、多 DPI、右键菜单场景下容易出现黑底问题。

## 下一阶段优先级

### 1. 窗口位置保存和恢复

目标：

- 用户把宠物拖到哪里，下次启动仍然在那里。
- 支持多屏幕。
- 如果屏幕变化导致位置不可见，自动把宠物拉回当前主屏幕。

要做：

- 拖动结束后保存窗口位置。
- 存到 `%USERPROFILE%\.petdex\desktop-state.json`。
- 启动时读取位置。
- 检测位置是否还在任一显示器工作区内。
- 如果不在，回退到主屏幕右下角或默认位置。

验收：

- 拖动宠物后关闭。
- 再打开，宠物位置不变。
- 拔掉副屏后启动，宠物不会跑到屏幕外。

### 2. 补完整宠物动画状态

目标：

- Windows 版不要只播放 `idle`。
- 支持 spritesheet 里的完整状态：
  - `idle`
  - `running-right`
  - `running-left`
  - `waving`
  - `jumping`
  - `failed`
  - `waiting`
  - `running`
  - `review`

要做：

- 抽象动画状态机。
- 当前原生渲染已经能按 frame 画 `idle`，要扩展为按 row 和 frame 播放。
- 拖动时可以播放 `running` 或 grabbed 状态。
- 右键切换宠物后重新加载 spritesheet 和状态。

验收：

- `idle` 正常慢速播放。
- 拖动时动画变化。
- 松手后可以 `waving` 或回到 `idle`。
- 切换宠物后动画正常。

### 3. 原生宠物选择面板

目标：

- 替代现在简陋的 Win32 右键菜单。
- 给非开发用户一个更友好的宠物选择体验。

建议先不要恢复 WebView2 菜单。先做原生 Win32 小面板，更稳。

要做：

- 右键宠物弹出一个小窗口。
- 显示宠物缩略图和名称。
- 支持点击切换宠物。
- 支持 Quit。
- 后续再加搜索。

验收：

- 右键出现选择面板。
- 可以看到宠物图，而不只是名字。
- 点击后桌宠切换。
- 不出现黑底。

### 4. 本地宠物安装体验

目标：

普通用户拿到一个宠物包后，可以简单安装。

宠物目录结构建议：

```text
.petdex-pet/
  pet.json
  spritesheet.webp
  preview.png
```

`pet.json` 示例：

```json
{
  "slug": "bao-shiwu",
  "displayName": "包师傅",
  "author": "Petdex",
  "version": "1.0.0"
}
```

要做：

- 支持把宠物包复制到 `%USERPROFILE%\.petdex\pets\<slug>\`。
- 可以先做命令行安装：

```powershell
petdex-desktop.exe install C:\path\to\bao-shiwu
```

- 后续做拖拽安装或设置页安装。

验收：

- 用户下载宠物包。
- 双击或命令安装。
- 重新打开桌宠后能看到新宠物。

### 5. Windows 安装包

目标：

让非开发人员可以下载安装，不需要 Zig、不需要源码。

要做：

- 打包 `petdex-desktop.exe`。
- 附带默认宠物。
- 安装到 `%LOCALAPPDATA%\Petdex\`。
- 创建桌面快捷方式。
- 可选创建开始菜单快捷方式。

推荐方案：

- 第一版可以用 Inno Setup 或 NSIS。
- 先不做自动更新。
- 先不做 Microsoft Store。

验收：

- 用户下载 `.exe` 安装包。
- 一路下一步。
- 桌面出现 Petdex 快捷方式。
- 双击后出现宠物。

### 6. 上传和下载宠物

目标：

维护者可以帮朋友生成自家宠物，然后上传到 Petdex。朋友可以在网页上下载自己的宠物或下载完整安装包。

要做：

- Web 端支持宠物详情页。
- 每个宠物有：
  - 名称
  - 预览图
  - spritesheet
  - 下载按钮
- 下载内容可以是：
  - 单独宠物包
  - 或带该宠物的 Windows 安装包

短期建议：

- 先做“下载宠物包”。
- 安装包只提供通用 Petdex。
- 用户安装 Petdex 后，再导入宠物包。

长期可以做：

- 每个宠物生成一个定制安装包。
- 用户下载后自带对应宠物。

### 7. 宠物生成工作流

目标：

可以根据朋友家的猫狗照片生成 Petdex 宠物。

当前推荐流程：

1. 用户提供宠物照片。
2. 用 Codex/hatch-pet 或图像生成流程做宠物形象。
3. 生成 8x9 spritesheet。
4. 生成 `pet.json`。
5. 本地 QA：
   - 看透明背景。
   - 看每个状态动作。
   - 看缩略图。
6. 上传到 Petdex。
7. 用户下载安装或导入。

需要补的工具：

- 宠物包校验器。
- spritesheet 预览页。
- 一键打包 `.petdex-pet.zip`。
- 上传前检查：
  - 是否有 `spritesheet.webp`。
  - 是否有 `pet.json`。
  - 图片尺寸是否符合规范。
  - 文件大小是否合理。

## 暂时不优先做

这些可以晚点做：

- agent 联动。
- Codex/Claude/Gemini hooks。
- 自动更新。
- 系统托盘。
- 开机自启动。
- `petdex://` 协议。
- 多 agent 气泡头像。
- 商店支付。
- Microsoft Store。
- 云同步。
- 用户账户体系深度集成。

## 推荐开发顺序

1. Windows 桌宠体验补齐：
   - 保存窗口位置。
   - 完整动画状态。
   - 原生宠物选择面板。

2. 普通用户可安装：
   - Windows 安装包。
   - 默认宠物。
   - 桌面快捷方式。

3. 宠物包导入：
   - 本地宠物包格式。
   - 安装或导入宠物。
   - 校验坏包。

4. Web 下载链路：
   - 宠物详情页。
   - 下载宠物包。
   - 下载 Windows 安装包。

5. 宠物生成和上传：
   - 生成 spritesheet。
   - QA 预览。
   - 打包上传。

