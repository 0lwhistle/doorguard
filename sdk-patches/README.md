# sdk-patches

对官方 SDK 的全部修改以 patch 形式存放于此。当前为空(尚未开始修改 SDK)。

生成(在 VM 的 SDK 仓库,分支 k7-door-guard-dev):
    git format-patch f06400239 -o ~/Linux/rk3576/k7_rk3576/sdk-patches/

复原(新环境):
    git am sdk-patches/*.patch
