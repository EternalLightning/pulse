# 发布流程

正式发布使用 GitHub Actions，同时生成 Windows 10 / 11 和 Windows 8.1 两种安装包。

本地验证使用 `build_release_ci.ps1`。`package_release.ps1` 为可选的 Authenticode 签名流程，需要自行配置代码签名证书。
