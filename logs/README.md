# logs/ — AI Coding 日志

本目录存放本项目开发过程中与 AI 编程工具的对话记录，随作品源码一并提交。

## 目录结构

```text
logs/
└── Issaclfl/                          # GitHub 用户名
    ├── manifest.json                  # 会话清单
    └── <YYYY-MM-DD>/
        └── zcode__<session_id>.jsonl  # 一个会话一个文件
```

每行一个事件：

```json
{"schema_version":"1.0","session_id":"...","team_id":"contest2026_434_TinyMind",
 "github_login":"Issaclfl","tool":"zcode","seq":0,"ts":"2026-08-21T...",
 "role":"user","text":"..."}
```

`role=assistant` 的事件另带 `model` / `tokens_in` / `tokens_out`。

## 覆盖范围

从 **2026-08-21（选题调研）到 2026-09-17（提交前最后一天）**，共 19 个会话、
1055 条事件，覆盖本项目全过程：

| 阶段 | 日期 | 会话内容 |
|---|---|---|
| 选题调研 | 08-21 ~ 08-25 | 大赛规则研究、SF32LB52 与 SiFli-SDK 资料检索、NuttX 音频框架调研 |
| 方案论证 | 08-29 ~ 09-03 | openvela 工作区结构勘察、SiFli HAL 音频代码现状、从官方 SDK 提取初始化时序 |
| 驱动实现与调试 | 09-05 | 主体开发（735 条事件），含 11 项适配难点的定位过程 |
| 收尾与扩展 | 09-16 ~ 09-17 | AI Agent 引擎移植调研、IP 承载方案论证、交付物整理 |

## 关于 tool 字段

`tool` 如实填写为 `zcode`。

官方手册列举的工具是 `claude-code` / `opencode` / `codex` / `kiro`，本机实际使用的是
**ZCode**（Claude Code 兼容的编码 agent）。没有改写成官方列表里的名字——日志的意义
在于可核验，换个名字就失去意义了。

## 数据来源与处理

- 逐条取自本机 ZCode 会话库（`~/.zcode/cli/db/db.sqlite` 的 `message` / `part` 表），
  **未做内容加工、未润色、未删改技术结论**。
- 角色按平台自身记录的 `semantics.kind` 过滤：只保留 `user_prompt`（人真正输入的）
  与 `assistant_response`（模型回复），运行期插入的合成事件（`timeline_event` 等）
  与工具通知不计入。
- **凭据已脱敏**：导出时对 API Key、Bearer Token 等做替换，避免随公开仓库泄露。
- 单条消息文本上限 20000 字符（截断仅影响超长的整篇文件粘贴，不影响对话本身）。

## 复现导出

导出脚本不在本仓内（它读的是本机私有会话库）。如需核对，可对照
`docs/技术报告.docx` 与 `board/` 下的源码——日志里每一条技术结论都能在代码或文档里
找到对应产物。
