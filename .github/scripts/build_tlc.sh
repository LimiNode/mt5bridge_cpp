#!/usr/bin/env bash
set -euo pipefail

# The v1.8.0 release asset is rebuilt and replaced by the upstream release
# workflow. Pin the source commit instead of consuming that mutable asset.
readonly tla_tools_repository='https://github.com/tlaplus/tlaplus.git'
readonly tla_tools_commit='210df3af8aff4f6032bf9be459d5a911d079da6d'
readonly tla_tools_directory="$(mktemp -d "${RUNNER_TEMP}/tlaplus.XXXXXX")"

git init --quiet "${tla_tools_directory}"
git -C "${tla_tools_directory}" remote add origin "${tla_tools_repository}"
git -C "${tla_tools_directory}" fetch --quiet --depth 1 origin "${tla_tools_commit}"
git -C "${tla_tools_directory}" checkout --quiet --detach FETCH_HEAD

test "$(git -C "${tla_tools_directory}" rev-parse HEAD)" = "${tla_tools_commit}"
echo "Building TLC from upstream commit ${tla_tools_commit}"

pushd "${tla_tools_directory}/tlatools/org.lamport.tlatools" >/dev/null
ant -f customBuild.xml \
  -Dgit.branch=detached \
  -Dgit.tag=none \
  -Dgit.revision="${tla_tools_commit}" \
  -Dgit.shortRevision="${tla_tools_commit:0:7}" \
  -Dgit.commitsCount=1 \
  -DBuild-Rev="${tla_tools_commit}" \
  compile compile-test dist
popd >/dev/null

cp "${tla_tools_directory}/tlatools/org.lamport.tlatools/dist/tla2tools.jar" \
  "${GITHUB_WORKSPACE}/tla2tools.jar"
sha256sum "${GITHUB_WORKSPACE}/tla2tools.jar"
