# Nskry 静态网站

视觉改版交接见 [手绘风 × 白板风开发边界](../docs/Proposal/website_design_handoff.md)。接手前先核对未提交基线，并在隔离环境开发。

独立 Astro 工程，不参与桌面 CMake 构建。Node.js 24+，使用锁文件安装：

```powershell
cd website
npm ci
npm test
npm run build
npm run dev
```

`dist/` 可部署到静态托管；`npm run preview` 预览生产构建。无服务端、数据库或启动网络请求。
本地预览地址以终端输出为准。默认根路径，项目 Pages 使用构建环境变量：

```powershell
$env:WEBSITE_BASE = '/Nskry/'
npm run build
npm run preview
```

根域名部署恢复 `WEBSITE_BASE=/` 后重新构建。不要把子路径构建当根路径部署。
未配置真实域名前不生成虚构 canonical/sitemap。后续部署时明确域名与路径，再加入这些元数据。

## 内容维护

- `src/data/plugins/*.json`：插件版本、身份、兼容性、包变体、隐私与限制。
- `src/content/plugins/*.md`：对应 ID 的详情；缺失或孤立文章会阻止构建。
- `src/data/releases/*.json`、`src/content/releases/<version>.md`：发行记录和说明；当前为空，因为没有核实正式资产。
- `src/content/guides/*.md`：教程，含 `title`、`description`、`order`。
- `src/layouts/`、`src/styles/`：模板替换入口，数据和 URL 尽量保持稳定。

插件记录字段见 `src/lib/data.mjs` 的 strict schema。`date` 是页面资料核对日期；开发版本来自 manifest，不是正式发布时间。
第三方条目填写 `origin: "third-party"` 并提供自己的来源、许可、隐私和支持信息。
不得将本地 `build/` 包写成线上下载地址，也不把未通过发行检查的版本标为 stable。

`assets` 每项必须有 `name/url/bytes/sha256/includes/setup`；大小和 hash 来自实际文件。
固定版本 HTTPS 资产先上传并校验，再更新网站数据。每个发布记录关联准确源码/构建材料，不能只指向主分支。
外部资产 URL 的可达性、重定向、hash 及 Nskry 实际安装需发行时验证；离线构建不会联网代测。

## 官方 catalog 门禁

`npm run catalog:preview` 仅在忽略目录 `.astro/catalog-preview/` 生成兼容 schema 1 的 JSON。
**不会输出到 dist**，没有线上 catalog 路由，避免空目录被误读为已检查到最新。
`catalog: true` 只允许稳定、官方、单包条目；多变体不能自动挑选。
上线前仍需完成官方来源安装链、认证/签名/信任根、撤销和客户端联调。
当前两个官网设置保持原值，本工程不修改桌面用户配置。

## 发布

当前仅本地完成。部署到 GitHub Pages 时，先在仓库设置确认 Pages、域名与路径。
审核分支构建后上传 `dist/`，部署任务才授予 Pages 写入权限；不要给投稿 PR 发布凭证。
先用预览验证页面和下载，再发布；保留上一份 dist 可回滚。
网站和包为不同资产，页面回滚不会使客户端降级。撤回软件应公告并发布修复版本。
未创建部署 workflow，避免在未确认部署目标时意外上线。

纯逻辑测试覆盖 schema、重复 ID、兼容范围和 catalog 门禁；构建后检查所有本地页面引用和子路径。
不声称这些校验已检查外部二进制、签名、所有地区网络或客户端更新。
方案见 `../docs/Proposal/static_website_plan.md`。
