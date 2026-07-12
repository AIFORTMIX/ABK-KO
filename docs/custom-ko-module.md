# 自定义 `.c` 编译为 `.ko` 模块

ABK 现在提供独立的外部内核模块构建入口，可把同一份自定义 `.c` 按指定 Android/KMI/GKI 分支编译为对应版本的 `.ko`。该流程不会打包 boot 镜像，也不会修改原有内核构建产物。

## 使用入口

在 GitHub Actions 手动运行：

```text
编译自定义 KO 模块 / build-ko-module.yml
```

默认示例源码是：

```text
modules/custom/abk_hello.c
```

运行完成后，每个目标版本会上传一个 artifact，里面包含：

```text
<module_name>.ko
metadata.txt
target.txt
```

`metadata.txt` 会记录 `sha256`、`vermagic`、依赖模块、许可证和构建模式。

## 单个 `.c` 文件

把你的源码放到仓库中，例如：

```text
modules/custom/my_driver.c
```

然后运行 `编译自定义 KO 模块` 工作流，填写：

```text
source_path = modules/custom/my_driver.c
module_name = my_driver
android_version = android14
kernel_version = 6.1
sub_level = 172
os_patch_level = 2025-07
```

单个 `.c` 文件会自动生成 Kbuild：

```makefile
obj-m += my_driver.o
```

所以源文件名不必和模块名一致，工作流会复制为 `<module_name>.c` 后再构建。

## 模块目录

如果模块不止一个源文件，可以提交一个目录：

```text
modules/custom/my_driver/
├── Kbuild
├── my_driver.c
└── helper.c
```

示例 `Kbuild`：

```makefile
obj-m += my_driver.o
my_driver-y := my_driver.o helper.o
```

工作流填写：

```text
source_path = modules/custom/my_driver
module_name = my_driver
```

如果目录中没有 `Kbuild` 或 `Makefile`，脚本会尝试用 `<module_name>.c` 自动生成最小 Kbuild。

## 批量构建

如果希望同一份 `.c` 对某个 Android/KMI 的全部已知版本生成 `.ko`，启用：

```text
build_all_known = true
```

工作流会读取：

```text
data/<android_version>/<kernel_version>.json
```

并为其中的每个 `date + kernel sublevel` 生成矩阵任务。例如选择：

```text
android_version = android14
kernel_version = 6.1
build_all_known = true
```

会按 `data/android14/6.1.json` 中记录的版本逐个编译。

如果还要包含 LTS 分支，启用：

```text
include_lts = true
```

## `insmod` 兼容规则

`.ko` 能否正常 `insmod`，核心取决于目标设备运行中的内核是否与构建所用内核匹配。请至少确认：

```sh
uname -r
modinfo -F vermagic my_driver.ko
```

两者的主版本、SMP/PREEMPT、modversions、架构和本地版本后缀需要匹配。若目标内核启用了 `CONFIG_MODVERSIONS`，还需要符号 CRC 与构建内核一致。

如果设备 `uname -r` 带有额外后缀，可以在工作流中填写：

```text
kernel_localversion = -android14-11-gxxxxxxxxxxxxx-ab123456-4k
```

该值会写入 `CONFIG_LOCALVERSION`，用于尽量匹配目标设备的 `vermagic`。如果只是给 ABK 自己编译出的同一内核加载模块，建议保持与内核构建时的 localversion 一致。

## 加载示例

在目标 Android 设备上：

```sh
su
insmod /data/local/tmp/my_driver.ko
lsmod | grep my_driver
dmesg | tail -n 50
```

卸载：

```sh
rmmod my_driver
```

如果 `insmod` 报错：

```text
Exec format error
```

通常是 `vermagic`、架构或符号版本不匹配。请优先对比 `uname -r` 和 `modinfo -F vermagic`。

如果报错：

```text
Unknown symbol ...
```

说明模块引用了目标内核未导出或 CRC 不匹配的符号。需要改用目标内核导出的符号，或在同一份内核源码和同一配置下重新构建。

## 本地脚本

核心脚本位于：

```text
scripts/build_external_ko.sh
```

用法：

```sh
scripts/build_external_ko.sh \
  --kernel-root /path/to/android-kernel-tree \
  --source modules/custom/my_driver.c \
  --module-name my_driver \
  --output-dir out/my_driver-ko
```

脚本会自动判断内核树构建系统：

| 内核树类型 | 构建方式 |
|---|---|
| 存在 `build/build.sh` | 使用 `EXT_MODULES=<module_dir> build/build.sh` |
| Kleaf/Bazel 内核树 | 生成 `kernel_module` 目标并执行 `tools/bazel build` |

可选环境变量：

```sh
ABK_KO_EXTRA_CFLAGS="-DDEBUG"
ABK_KO_LTO=none
ABK_KO_BAZEL_ARGS="--verbose_failures"
```
