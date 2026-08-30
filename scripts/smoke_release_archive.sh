#!/usr/bin/env bash

set -euo pipefail

usage() {
  echo "usage: $0 ARCHIVE VERSION TARGET PREVIOUS_ARCHIVE" >&2
  exit 64
}

[[ $# -eq 4 ]] || usage

archive=$1
expected_version=$2
expected_target=$3
previous_archive=$4

if [[ ! "$expected_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "invalid release version: $expected_version" >&2
  exit 64
fi

case "$expected_target" in
  linux-x86_64|macos-x86_64|macos-arm64) ;;
  *)
    echo "unsupported release target: $expected_target" >&2
    exit 64
    ;;
esac

if [[ ! -f "$archive" ]]; then
  echo "release archive does not exist: $archive" >&2
  exit 66
fi
if [[ ! -f "$previous_archive" ]]; then
  echo "previous release archive does not exist: $previous_archive" >&2
  exit 66
fi

archive_dir=$(cd "$(dirname "$archive")" && pwd -P)
archive="$archive_dir/$(basename "$archive")"
previous_archive_dir=$(cd "$(dirname "$previous_archive")" && pwd -P)
previous_archive="$previous_archive_dir/$(basename "$previous_archive")"
archive_root="pipeshellx-${expected_version}-${expected_target}"
expected_name="${archive_root}.tar.gz"
previous_root="pipeshellx-0.5.0-${expected_target}"
previous_name="${previous_root}.tar.gz"

if [[ "$(basename "$archive")" != "$expected_name" ]]; then
  echo "expected archive '$expected_name', got '$(basename "$archive")'" >&2
  exit 65
fi
if [[ "$(basename "$previous_archive")" != "$previous_name" ]]; then
  echo "expected previous archive '$previous_name', got '$(basename "$previous_archive")'" >&2
  exit 65
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
source_dir=$(cd "$script_dir/.." && pwd -P)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/pipeshellx-release-smoke.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

tar -tzf "$archive" > "$work_dir/archive.list"
if ! awk -v root="$archive_root" '
  /^\// || /(^|\/)\.\.($|\/)/ { exit 1 }
  $0 != root && $0 != root "/" && index($0, root "/") != 1 { exit 1 }
  END { if (NR == 0) exit 1 }
' "$work_dir/archive.list"; then
  echo "archive contains an empty, absolute, escaping, or unexpected path" >&2
  exit 65
fi
tar -tzf "$previous_archive" > "$work_dir/previous-archive.list"
if ! awk -v root="$previous_root" '
  /^\// || /(^|\/)\.\.($|\/)/ { exit 1 }
  $0 != root && $0 != root "/" && index($0, root "/") != 1 { exit 1 }
  END { if (NR == 0) exit 1 }
' "$work_dir/previous-archive.list"; then
  echo "previous archive contains an empty, absolute, escaping, or unexpected path" >&2
  exit 65
fi

mkdir "$work_dir/clean"
tar -xzf "$archive" -C "$work_dir/clean"
prefix="$work_dir/clean/$archive_root"

required_files=(
  "bin/pipeshellx"
  "include/psx/pipeline/planner.hpp"
  "share/doc/pipeshellx/LICENSE"
  "share/doc/pipeshellx/NOTICE"
  "share/doc/pipeshellx/THIRD_PARTY_NOTICES.md"
  "share/doc/pipeshellx/README.md"
  "share/doc/pipeshellx/CHANGELOG.md"
)
for required in "${required_files[@]}"; do
  if [[ ! -f "$prefix/$required" ]]; then
    echo "release archive is missing $required" >&2
    exit 65
  fi
done

if [[ ! -x "$prefix/bin/pipeshellx" ]]; then
  echo "installed CLI is not executable" >&2
  exit 65
fi

for internal in \
  include/cli_options.hpp \
  include/client_config.hpp \
  include/psx/cli \
  include/psx/os/backend.hpp; do
  if [[ -e "$prefix/$internal" ]]; then
    echo "release archive exposes private path: $internal" >&2
    exit 65
  fi
done

version_output=$("$prefix/bin/pipeshellx" --version)
if [[ "$version_output" != *"$expected_version"* ]]; then
  echo "installed CLI reports the wrong version: $version_output" >&2
  exit 65
fi
"$prefix/bin/pipeshellx" --help >/dev/null

set +e
"$prefix/bin/pipeshellx" --definitely-invalid-release-smoke-option \
  >"$work_dir/invalid.stdout" 2>"$work_dir/invalid.stderr"
invalid_status=$?
set -e
if [[ $invalid_status -ne 2 ]]; then
  echo "invalid CLI option returned $invalid_status instead of 2" >&2
  exit 65
fi

package_dir=$(find "$prefix" -type d -path '*/cmake/pipeshellx' -print -quit)
if [[ -z "$package_dir" ]]; then
  echo "release archive has no pipeshellx CMake package" >&2
  exit 65
fi
for package_file in pipeshellxConfig.cmake pipeshellxConfigVersion.cmake pipeshellxTargets.cmake; do
  if [[ ! -f "$package_dir/$package_file" ]]; then
    echo "release archive is missing CMake package file: $package_file" >&2
    exit 65
  fi
done
if grep -R -F -e "$source_dir" -e 'pipeshellx_warnings' "$package_dir" >/dev/null; then
  echo "release CMake package leaks a source path or private warnings target" >&2
  exit 65
fi

cmake -S "$source_dir/tests/packaging/downstream" \
  -B "$work_dir/clean-consumer" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$prefix" >/dev/null
cmake --build "$work_dir/clean-consumer" --parallel 2 >/dev/null
"$work_dir/clean-consumer/bin/pipeshellx_package_consumer"

# Exercise an actual v0.5-to-current upgrade. Releases live in immutable
# versioned directories; switching a relative `current` symlink atomically
# avoids stale files from the old install (notably the former uppercase CLI)
# while keeping operator-managed configuration outside release directories.
mkdir -p "$work_dir/upgrade/releases" "$work_dir/upgrade/etc/pipeshellx"
tar -xzf "$previous_archive" -C "$work_dir/upgrade/releases"
previous_prefix="$work_dir/upgrade/releases/$previous_root"
if [[ ! -x "$previous_prefix/bin/PipeShellX" ]]; then
  echo "v0.5 archive is missing its uppercase CLI" >&2
  exit 65
fi
previous_version=$("$previous_prefix/bin/PipeShellX" --version)
if [[ "$previous_version" != *'0.5.0'* ]]; then
  echo "previous archive reports the wrong version: $previous_version" >&2
  exit 65
fi

ln -s "releases/$previous_root" "$work_dir/upgrade/current"
printf '%s\n' 'user-managed=true' > "$work_dir/upgrade/etc/pipeshellx/user.conf"

tar -xzf "$archive" -C "$work_dir/upgrade/releases"
ln -s "releases/$archive_root" "$work_dir/upgrade/current.next"
python3 -c 'import os, sys; os.replace(sys.argv[1], sys.argv[2])' \
  "$work_dir/upgrade/current.next" "$work_dir/upgrade/current"
upgrade_prefix="$work_dir/upgrade/current"

upgrade_version=$("$upgrade_prefix/bin/pipeshellx" --version)
if [[ "$upgrade_version" != *"$expected_version"* ]]; then
  echo "atomic archive upgrade selected the wrong CLI: $upgrade_version" >&2
  exit 65
fi
if python3 -c \
  'import os, sys; sys.exit(0 if "PipeShellX" in os.listdir(sys.argv[1]) else 1)' \
  "$upgrade_prefix/bin"; then
  echo "atomic archive upgrade retained the obsolete uppercase CLI" >&2
  exit 65
fi
if [[ "$(cat "$work_dir/upgrade/etc/pipeshellx/user.conf")" != 'user-managed=true' ]]; then
  echo "archive upgrade overwrote user-managed configuration" >&2
  exit 65
fi

cmake -S "$source_dir/tests/packaging/downstream" \
  -B "$work_dir/upgrade-consumer" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$upgrade_prefix" >/dev/null
cmake --build "$work_dir/upgrade-consumer" --parallel 2 >/dev/null
"$work_dir/upgrade-consumer/bin/pipeshellx_package_consumer"

echo "release archive smoke: OK ($expected_name)"
