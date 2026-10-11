#!/usr/bin/env bash
set -u

ROOT="${GITHUB_WORKSPACE:?GITHUB_WORKSPACE is required}"
BASE_SOURCE="$ROOT/base-source"
CANDIDATE_SOURCE="$ROOT/candidate-source"
TARGETS=(
  agc_driver_image_plane_address_on_line_implicit_lod_tests
  agc_driver_depth_fast_clear_submit_tests
  agc_driver_flat_apertures_tests
  agc_driver_image_plane_address_on_line_tests
  agc_masked_select_tests
  agc_driver_subvector_loop_tests
)
TEST_REGEX='^(agc_driver_depth_fast_clear_submit|agc_driver_flat_apertures_(0|1|2|3|4|5|6|7|8|9|10|11|12|13)|agc_driver_image_plane_address_on_line_implicit_lod|agc_driver_image_plane_address_on_line|agc_masked_select|agc_driver_subvector_loop)$'
EXPECTED_TESTS=(agc_driver_depth_fast_clear_submit agc_driver_flat_apertures_0 agc_driver_flat_apertures_1 agc_driver_flat_apertures_2 agc_driver_flat_apertures_3 agc_driver_flat_apertures_4 agc_driver_flat_apertures_5 agc_driver_flat_apertures_6 agc_driver_flat_apertures_7 agc_driver_flat_apertures_8 agc_driver_flat_apertures_9 agc_driver_flat_apertures_10 agc_driver_flat_apertures_11 agc_driver_flat_apertures_12 agc_driver_flat_apertures_13 agc_driver_image_plane_address_on_line_implicit_lod agc_driver_image_plane_address_on_line agc_masked_select agc_driver_subvector_loop)
overall=0
mkdir -p "$ROOT/evidence"
printf 'run_id=%s\nattempt=%s\nbase_sha=%s\ncandidate_sha=%s\nrunner=%s\n' \
  "${GITHUB_RUN_ID:-unknown}" "${GITHUB_RUN_ATTEMPT:-unknown}" "${BASE_SHA:?}" "${CANDIDATE_SHA:?}" "$(uname -a)" \
  >> "$ROOT/evidence/identity.txt"

run_tree() {
  local revision="$1" source="$2" expected="$3"
  local build="$ROOT/build/$revision" out="$ROOT/evidence/$revision"
  local configure_rc=125 registration_rc=125 build_rc=125 ctest_rc=125
  mkdir -p "$out"
  printf 'expected_sha=%s\n' "$expected" > "$out/status.txt"
  {
    uname -a
    gcc --version | head -n 1
    cmake --version | head -n 1
    ninja --version
    ccache --version | head -n 1
    python3 --version
  } > "$out/toolchain.txt" 2>&1

  actual="$(git -C "$source" rev-parse HEAD 2>&1)"
  printf 'actual_sha=%s\n' "$actual" >> "$out/status.txt"
  if [[ "$actual" != "$expected" ]]; then
    printf 'error: expected %s but checked out %s\n' "$expected" "$actual" > "$out/source-check.log"
    overall=1
    return
  fi
  if [[ -n "$(git -C "$source" status --porcelain)" ]]; then
    git -C "$source" status --short > "$out/source-check.log"
    echo 'error: source checkout is dirty' >> "$out/source-check.log"
    overall=1
    return
  fi
  git -C "$source" submodule status --recursive > "$out/submodules.txt" 2>&1
  if grep -Eq '^[+-U]' "$out/submodules.txt"; then
    echo 'error: one or more source submodules are not at the pinned gitlink' >> "$out/submodules.txt"
    overall=1
    return
  fi

  timeout --signal=TERM --kill-after=10s 30m cmake -S "$source" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DANYPS5_ENABLE_SPIRV_TOOLS=ON > "$out/configure.log" 2>&1
  configure_rc=$?
  printf 'configure_exit=%s\n' "$configure_rc" >> "$out/status.txt"
  if [[ "$configure_rc" -ne 0 ]]; then overall=1; return; fi

  python3 - "$build" "${EXPECTED_TESTS[@]}" > "$out/registration.log" 2>&1 <<'PY'
import json
import subprocess
import sys
from pathlib import Path
build = Path(sys.argv[1])
expected = set(sys.argv[2:])
result = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                        check=True, capture_output=True, text=True)
registered = {item["name"] for item in json.loads(result.stdout).get("tests", [])}
missing = sorted(expected - registered)
if missing:
    print("required diagnostic tests are not registered: " + ", ".join(missing))
    raise SystemExit(1)
print("all required diagnostic tests are registered: " + ", ".join(sorted(expected)))
PY
  registration_rc=$?
  printf 'registration_exit=%s\n' "$registration_rc" >> "$out/status.txt"
  if [[ "$registration_rc" -ne 0 ]]; then overall=1; return; fi

  timeout --signal=TERM --kill-after=10s 45m cmake --build "$build" --target "${TARGETS[@]}" --parallel 2 \
    > "$out/build.log" 2>&1
  build_rc=$?
  printf 'build_exit=%s\n' "$build_rc" >> "$out/status.txt"
  if [[ "$build_rc" -ne 0 ]]; then overall=1; return; fi

  timeout --signal=TERM --kill-after=10s 5m python3 "$ROOT/driver/tools/check_test_registration.py" "$build" \
    > "$out/registration-pairing.log" 2>&1
  registration_pairing_rc=$?
  printf 'registration_pairing_exit=%s\n' "$registration_pairing_rc" >> "$out/status.txt"
  if [[ "$registration_pairing_rc" -ne 0 ]]; then overall=1; return; fi

  timeout --signal=TERM --kill-after=10s 45m ctest --test-dir "$build" -R "$TEST_REGEX" --no-tests=error --output-on-failure \
    --timeout 120 --repeat until-fail:3 --parallel 1 \
    --output-log "$out/ctest.log" --output-junit "$out/ctest.xml" > "$out/ctest-console.log" 2>&1
  ctest_rc=$?
  printf 'ctest_exit=%s\n' "$ctest_rc" >> "$out/status.txt"
  if [[ "$ctest_rc" -ne 0 ]]; then overall=1; fi

}

run_tree base "$BASE_SOURCE" "$BASE_SHA"
run_tree candidate "$CANDIDATE_SOURCE" "$CANDIDATE_SHA"
printf 'overall_exit=%s\n' "$overall" >> "$ROOT/evidence/identity.txt"
exit "$overall"
