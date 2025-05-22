#!/bin/env bash

set -eo pipefail

cd src

. evergreen/prelude_venv.sh
activate_venv
python buildscripts/install_bazel.py
bazel_bin="$HOME/.local/bin/bazelisk"
# number of parallel jobs to use for build.
# Even with scale=0 (the default), bc command adds decimal digits in case of multiplication. Division by 1 gives us a whole number with scale=0
bazel_jobs=$(bc <<< "$(grep -c '^processor' /proc/cpuinfo) * .85 / 1")
build_config="--config=local --jobs=$bazel_jobs --compiler_type=gcc --opt=off --dbg=False --allocator=system"
bazel_query='mnemonic("CppCompile|LinkCompile", filter(//src/mongo, deps(//:install-core)) except //src/mongo/db/modules/enterprise/src/streams/third_party/...)'
bazel_cache="--output_user_root=$workdir/bazel_cache"

python bazel/coverity/generate_coverity_command.py --bazel_executable=$bazel_bin --bazel_cache=$bazel_cache --bazel_query="$bazel_query" $build_config --noinclude_artifacts
$bazel_bin $bazel_cache build $build_config --build_tag_filters=gen_source //src/...
bazelBuildCommand="$bazel_bin $bazel_cache build $build_config //src/mongo/db/modules/enterprise/coverity:enterprise_coverity_build"
echo "Bazel Build Command: $bazelBuildCommand"
covIdir="$workdir/covIdir"
if [ -d "$covIdir" ]; then
  echo "covIdir already exists, meaning idir extracted after download from S3"
else
  mkdir $workdir/covIdir
fi
$workdir/coverity/bin/cov-build --dir "$covIdir" --verbose 0 -j $bazel_jobs --return-emit-failures --parse-error-threshold=99 --bazel $bazelBuildCommand
ret=$?
if [ $ret -ne 0 ]; then
  echo "cov-build faild with exit code $ret"
else
  echo "cov-build was successful"
fi
