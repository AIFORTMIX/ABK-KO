#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  build_external_ko.sh --kernel-root <dir> --source <file-or-dir> --module-name <name> --output-dir <dir>

Environment:
  ABK_KO_BUILD_CONFIG      build.sh 使用的 BUILD_CONFIG，默认 common/build.config.gki.aarch64
  ABK_KO_LTO               Kleaf/Bazel LTO 模式，默认 thin；6.12 建议 none
  ABK_KO_EXTRA_CFLAGS      追加到 ccflags-y 的自定义 CFLAGS
  ABK_KO_BAZEL_ARGS        追加传给 tools/bazel build 的参数

说明:
  - source 可以是单个 .c 文件，也可以是包含 Kbuild/Makefile 的模块目录。
  - 单个 .c 文件会被复制为 <module-name>.c，并自动生成 Kbuild。
  - 输出目录会包含 .ko 和 metadata.txt。
EOF
}

kernel_root=""
source_path=""
module_name=""
output_dir=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --kernel-root)
      kernel_root="${2:-}"
      shift 2
      ;;
    --source)
      source_path="${2:-}"
      shift 2
      ;;
    --module-name)
      module_name="${2:-}"
      shift 2
      ;;
    --output-dir)
      output_dir="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "::error::未知参数: $1"
      usage
      exit 2
      ;;
  esac
done

if [ -z "$kernel_root" ] || [ -z "$source_path" ] || [ -z "$module_name" ] || [ -z "$output_dir" ]; then
  echo "::error::缺少必要参数"
  usage
  exit 2
fi

if ! [[ "$module_name" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]]; then
  echo "::error::module-name 必须是合法 C/Kbuild 标识符，只能包含字母、数字、下划线，且不能以数字开头: $module_name"
  exit 1
fi

kernel_root="$(realpath "$kernel_root")"
source_path="$(realpath "$source_path")"
mkdir -p "$output_dir"
output_dir="$(realpath "$output_dir")"

if [ ! -d "$kernel_root/common" ]; then
  echo "::error::未找到内核 common 目录: $kernel_root/common"
  exit 1
fi

work_base="${RUNNER_TEMP:-/tmp}/abk-ko-build"
module_dir="$work_base/$module_name"
rm -rf "$module_dir"
mkdir -p "$module_dir"

if [ -f "$source_path" ]; then
  case "$source_path" in
    *.c) ;;
    *)
      echo "::error::单文件输入必须是 .c 文件: $source_path"
      exit 1
      ;;
  esac
  cp "$source_path" "$module_dir/$module_name.c"
  {
    [ -n "${ABK_KO_EXTRA_CFLAGS:-}" ] && echo "ccflags-y += ${ABK_KO_EXTRA_CFLAGS}"
    echo "obj-m += ${module_name}.o"
  } > "$module_dir/Kbuild"
elif [ -d "$source_path" ]; then
  cp -a "$source_path/." "$module_dir/"
  if [ ! -f "$module_dir/Kbuild" ] && [ ! -f "$module_dir/Makefile" ]; then
    if [ ! -f "$module_dir/$module_name.c" ]; then
      echo "::error::目录输入缺少 Kbuild/Makefile，且未找到 $module_name.c"
      exit 1
    fi
    {
      [ -n "${ABK_KO_EXTRA_CFLAGS:-}" ] && echo "ccflags-y += ${ABK_KO_EXTRA_CFLAGS}"
      echo "obj-m += ${module_name}.o"
    } > "$module_dir/Kbuild"
  fi
else
  echo "::error::source 不存在: $source_path"
  exit 1
fi

echo "模块源码目录: $module_dir"
find "$module_dir" -maxdepth 2 -type f | sort

if ! grep -R "MODULE_LICENSE" "$module_dir" >/dev/null 2>&1; then
  echo "::warning::源码中未发现 MODULE_LICENSE()，内核可能会将模块标记为 proprietary 或拒绝部分 GPL-only 符号。"
fi

build_with_build_sh() {
  local build_config="${ABK_KO_BUILD_CONFIG:-common/build.config.gki.aarch64}"
  local dist_dir="$output_dir/dist"
  mkdir -p "$dist_dir"

  echo "检测到 build/build.sh，使用 Android 经典 build.sh 外部模块流程。"
  (
    cd "$kernel_root"
    LTO=thin \
      BUILD_CONFIG="$build_config" \
      EXT_MODULES="$module_dir" \
      DIST_DIR="$dist_dir" \
      build/build.sh CC="/usr/bin/ccache clang"
  )
}

build_with_kleaf() {
  local pkg="$kernel_root/abk_external_module"
  local target="${module_name}_module"
  local lto="${ABK_KO_LTO:-thin}"
  rm -rf "$pkg"
  mkdir -p "$pkg"
  cp -a "$module_dir/." "$pkg/"

  cat > "$pkg/BUILD.bazel" <<EOF
load("//build/kernel/kleaf:kernel.bzl", "kernel_module")

kernel_module(
    name = "$target",
    srcs = glob(["**"]),
    outs = ["$module_name.ko"],
    kernel_build = "//common:kernel_aarch64",
)
EOF

  echo "检测到 Kleaf/Bazel 内核树，生成目标: //abk_external_module:$target"
  (
    cd "$kernel_root"
    tools/bazel build \
      --disk_cache="${HOME}/.cache/bazel" \
      --config=fast \
      "--lto=$lto" \
      ${ABK_KO_BAZEL_ARGS:-} \
      "//abk_external_module:$target"
  )
}

if [ -f "$kernel_root/build/build.sh" ]; then
  build_with_build_sh
else
  build_with_kleaf
fi

mapfile -t built_modules < <(
  {
    find "$module_dir" "$output_dir" "$kernel_root" \
      -type f -name "$module_name.ko" 2>/dev/null || true
  } | awk '!seen[$0]++'
)

if [ "${#built_modules[@]}" -eq 0 ]; then
  echo "::error::未找到构建产物 $module_name.ko"
  exit 1
fi

selected_ko="${built_modules[0]}"
for candidate in "${built_modules[@]}"; do
  case "$candidate" in
    "$kernel_root/bazel-bin/"*|"$output_dir/"*|"$module_dir/"*)
      selected_ko="$candidate"
      break
      ;;
  esac
done

cp "$selected_ko" "$output_dir/$module_name.ko"

metadata="$output_dir/metadata.txt"
{
  echo "module_name=$module_name"
  echo "ko_path=$module_name.ko"
  echo "source=$source_path"
  echo "kernel_root=$kernel_root"
  echo "build_mode=$([ -f "$kernel_root/build/build.sh" ] && echo build.sh || echo kleaf)"
  echo "sha256=$(sha256sum "$output_dir/$module_name.ko" | awk '{print $1}')"
  if command -v modinfo >/dev/null 2>&1; then
    echo "vermagic=$(modinfo -F vermagic "$output_dir/$module_name.ko" 2>/dev/null || true)"
    echo "depends=$(modinfo -F depends "$output_dir/$module_name.ko" 2>/dev/null || true)"
    echo "license=$(modinfo -F license "$output_dir/$module_name.ko" 2>/dev/null || true)"
  fi
  if command -v file >/dev/null 2>&1; then
    echo "file=$(file "$output_dir/$module_name.ko")"
  fi
} > "$metadata"

echo "构建完成: $output_dir/$module_name.ko"
cat "$metadata"
