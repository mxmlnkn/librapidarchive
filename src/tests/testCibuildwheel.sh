#!/usr/bin/env bash
#
# Build wheels locally with the cibuildwheel.
#
# Prerequisites:
#  - Linux x86_64 host for the i686 and x86_64 manylinux images.
#  - podman or docker with Linux containers. Set CIBW_CONTAINER_ENGINE to choose.
#  - In a restricted container sandbox, rootless podman may fail on user namespaces, netlink, or
#    read-only cgroups. A wrapper named `podman` in PATH can run it as root with
#    `run --network=host --cgroups=disabled`; the rest of the invocation stays the same.
#
# Examples:
#   src/tests/testCibuildwheel.sh rapidgzip i686
#   src/tests/testCibuildwheel.sh indexed_bzip2 x86_64
#   CIBW_BUILD='cp39-* cp310-* cp311-*' RAPIDGZIP_ABI3=disable \
#       src/tests/testCibuildwheel.sh rapidgzip x86_64
#   CIBW_PARALLEL=8 src/tests/testCibuildwheel.sh rapidgzip x86_64
#
# The script creates a virtualenv in <repo>/venvs/cibuildwheel and puts wheels in <repo>/wheelhouse/<project>-<arch>.
# It builds each identifier in parallel and runs abi3audit on abi3 wheels.

set -euo pipefail

project="${1:-rapidgzip}"
arch="${2:-x86_64}"

case "${project}" in
    rapidgzip)
        package_dir="python/rapidgzip"
        abi3_var="RAPIDGZIP_ABI3"
        ;;
    indexed_bzip2)
        package_dir="python/indexed_bzip2"
        abi3_var="INDEXED_BZIP2_ABI3"
        ;;
    *)
        echo "Unknown project: ${project}. Use rapidgzip or indexed_bzip2." >&2
        exit 2
        ;;
esac

repo_root="$(git rev-parse --show-toplevel)"
cd "${repo_root}"
venv="${repo_root}/venvs/cibuildwheel"
wheelhouse="${repo_root}/wheelhouse/${project}-${arch}"

if [[ ! -x "${venv}/bin/python" ]]; then
    python3 -m venv "${venv}"
fi
"${venv}/bin/python" -m pip install --quiet --upgrade pip cibuildwheel build abi3audit

engine="${CIBW_CONTAINER_ENGINE:-}"
if [[ -z "${engine}" ]]; then
    if command -v podman >/dev/null 2>&1; then
        engine=podman
    elif command -v docker >/dev/null 2>&1; then
        engine=docker
    else
        echo "cibuildwheel needs podman or docker. Install one or set CIBW_CONTAINER_ENGINE." >&2
        echo "For identifier-only checks, use: CIBW_ARCHS_LINUX=${arch} CIBW_BUILD='cp314-*' \\" >&2
        echo "  ${venv}/bin/cibuildwheel --platform linux --print-build-identifiers ${package_dir}" >&2
        exit 1
    fi
fi

build="${CIBW_BUILD:-cp314-*}"
# The abi3 builds are opt-in; per-version builds should set the variable to disable.
if [[ "${build}" == *cp314* && "${abi3_var}" != "disable" ]]; then
    export "${abi3_var}=enable"
elif [[ "${abi3_var}" != "enable" ]]; then
    export "${abi3_var}=disable"
fi

if [[ "${project}" == "rapidgzip" ]]; then
    # ISA-L needs nasm in both the manylinux (yum) and musllinux (apk) images.
    export CIBW_BEFORE_ALL_LINUX="${CIBW_BEFORE_ALL_LINUX:-uname -a; if command -v yum >/dev/null; then yum install -y nasm; elif command -v apk >/dev/null; then apk add nasm; fi}"
fi

export CIBW_ARCHS_LINUX="${arch}"
export CIBW_BUILD="${build}"
export CIBW_CONTAINER_ENGINE="${engine}"
export CIBW_BUILD_VERBOSITY="${CIBW_BUILD_VERBOSITY:-1}"
mkdir -p "${wheelhouse}"

mapfile -t identifiers < <(
    "${venv}/bin/python" -m cibuildwheel \
        --platform linux \
        --archs "${arch}" \
        --print-build-identifiers \
        "${package_dir}"
)
if [[ "${#identifiers[@]}" -eq 0 ]]; then
    echo "No cibuildwheel identifiers matched CIBW_BUILD='${build}'." >&2
    exit 1
fi

printf 'Building %d identifier(s):\n' "${#identifiers[@]}"
printf '  %s\n' "${identifiers[@]}"

jobs="${CIBW_PARALLEL:-$( nproc )}"
printf '%s\n' "${identifiers[@]}" | xargs -P "${jobs}" -I{} \
    "${venv}/bin/python" -m cibuildwheel \
        --only {} \
        --output-dir "${wheelhouse}" \
        "${package_dir}"

mapfile -t abi3_wheels < <(find "${wheelhouse}" -maxdepth 1 -name '*-abi3-*.whl' -print)
if [[ "${#abi3_wheels[@]}" -gt 0 ]]; then
    for wheel in "${abi3_wheels[@]}"; do
        "${venv}/bin/abi3audit" --verbose "${wheel}"
    done
else
    echo "No abi3 wheels to audit."
fi

echo "Wheels written to ${wheelhouse}"
