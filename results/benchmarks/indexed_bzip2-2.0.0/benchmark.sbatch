#!/usr/bin/env bash
#SBATCH --job-name=ibzip2-bench
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=256
#SBATCH --mem=480G
#SBATCH --time=08:00:00
# --exclusive is intentionally not set here; pass it on the sbatch command line for full-node runs.
#SBATCH --hint=multithread
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.err

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=${REPO:-}
if [[ -z "$repo" ]]; then
    # Slurm copies the batch script to a spool directory, so BASH_SOURCE is not inside the checkout.
    candidate=$script_dir
    while [[ "$candidate" != "/" && ! -d "$candidate/.git" ]]; do
        candidate=$(dirname -- "$candidate")
    done
    if [[ -d "$candidate/.git" ]]; then
        repo=$candidate
    elif [[ -n "${SLURM_SUBMIT_DIR:-}" ]] &&
         git -C "$SLURM_SUBMIT_DIR" rev-parse --show-toplevel >/dev/null 2>&1; then
        repo=$(git -C "$SLURM_SUBMIT_DIR" rev-parse --show-toplevel)
    elif git rev-parse --show-toplevel >/dev/null 2>&1; then
        repo=$(git rev-parse --show-toplevel)
    else
        repo=$PWD
    fi
fi
if ! git -C "$repo" rev-parse --show-toplevel >/dev/null 2>&1; then
    echo "error: REPO is not a git checkout: $repo" >&2
    echo "Set REPO to the checkout path or submit with sbatch from the checkout directory." >&2
    exit 1
fi
bench_ws=${BENCH_WS:-$(dirname -- "$repo")}
venv=${VENV:-$bench_ws/venv}
data_dir=${DATA_DIR:-$bench_ws/data}

echo "repo=$repo" >&2
echo "bench_ws=$bench_ws venv=$venv data_dir=$data_dir" >&2

cluster_name=${CLUSTER_NAME:-${SLURM_CLUSTER_NAME:-}}
if [[ -z "$cluster_name" ]] && command -v scontrol >/dev/null 2>&1; then
    cluster_name=$(scontrol show config 2>/dev/null | sed -n 's/^ClusterName *= *//p' | head -1)
fi
if [[ -z "$cluster_name" ]]; then
    fqdn=$(hostname -f 2>/dev/null || hostname)
    case "$fqdn" in
        *barnard*) cluster_name=barnard ;;
        *romeo*) cluster_name=romeo ;;
    esac
fi
cpu_model=$( ( lscpu 2>/dev/null | sed -n 's/^Model name:[[:space:]]*//p' | head -1 ) || true )
if [[ -z "$cluster_name" || "$cluster_name" == "$(hostname -s)" ]]; then
    case "$cpu_model" in
        *8470*) cluster_name=barnard ;;
        *7702*) cluster_name=romeo ;;
    esac
fi
cluster_name=${cluster_name:-$(hostname -s)}
out_dir=${OUT_DIR:-$repo/results/benchmarks/indexed_bzip2-2.0.0/$cluster_name}

# 7-Zip's SHA-512/FP16 code needs an assembler newer than the Barnard binutils.
if [[ -z "${SEVEN_ZIP_MARCH:-}" ]]; then
    if [[ "$cluster_name" == barnard* || "$cpu_model" == *8470* ]]; then
        SEVEN_ZIP_MARCH=icelake-server
    fi
fi
if [[ -n "${SEVEN_ZIP_MARCH:-}" ]]; then
    export SEVEN_ZIP_MARCH
fi
build_tools=${BUILD_TOOLS:-1}
if [[ "${SKIP_BUILD:-0}" == 1 ]]; then
    build_tools=0
fi
build_jobs=${BUILD_JOBS:-16}
prepare_data=${PREPARE_DATA:-1}
prepare_data_only=${PREPARE_DATA_ONLY:-0}
create_gzip=${CREATE_GZIP:-1}
create_gzip_ctu=${CREATE_GZIP_CTU:-1}
stage_data=${STAGE_DATA:-1}
stage_tools=${STAGE_TOOLS:-1}
stage_libs=${STAGE_LIBS:-1}
stage_root=${STAGE_ROOT:-}
keep_staged_data=${KEEP_STAGED_DATA:-0}
json_interval=${JSON_INTERVAL:-300}

# The Romeo GCC 14.2 crtbegin.o is not PIE-capable, so leave PIE off unless requested.
export LIBRAPIDARCHIVE_ENABLE_PIE=${LIBRAPIDARCHIVE_ENABLE_PIE:-OFF}
host_logical=$(lscpu 2>/dev/null | sed -n 's/^CPU(s):[[:space:]]*//p' | head -1)
host_logical=${host_logical:-$(nproc)}
host_physical=$(lscpu 2>/dev/null | awk -F: '
    /^Socket\(s\)/ { gsub(/ /, "", $2); sockets = $2 }
    /^Core\(s\) per socket/ { gsub(/ /, "", $2); cores = $2 }
    END { if (sockets && cores) print sockets * cores }')
host_physical=${host_physical:-$(( host_logical / 2 ))}
(( host_physical < 1 )) && host_physical=1
smt=$(( host_logical / host_physical ))
(( smt < 1 )) && smt=1

# Slurm counts physical cores for allocation on these partitions, while nproc and the affinity
# mask include the SMT siblings. For full-node jobs, use the full logical CPU count.
slurm_cores=${SLURM_CPUS_PER_TASK:-${SLURM_CPUS_ON_NODE:-0}}
if [[ "$slurm_cores" =~ ^[0-9]+$ ]] && (( slurm_cores > 0 )); then
    if (( slurm_cores >= host_physical )); then
        logical_cores=$host_logical
    else
        logical_cores=$(( slurm_cores * smt ))
    fi
else
    logical_cores=$host_logical
fi
affinity_cores=$(nproc)
if (( affinity_cores < logical_cores )); then
    logical_cores=$affinity_cores
fi
(( logical_cores > host_logical )) && logical_cores=$host_logical
physical_cores=$(( logical_cores / smt ))
(( physical_cores < 1 )) && physical_cores=1

if [[ -n "${THREADS:-}" ]]; then
    threads=$THREADS
else
    thread_candidates=()
    thread=1
    while (( thread <= logical_cores )); do
        thread_candidates+=("$thread")
        (( thread *= 2 ))
    done
    thread_candidates+=("$physical_cores" "$logical_cores")
    threads=$(printf '%s\n' "${thread_candidates[@]}" | sort -n -u | tr '\n' ' ')
fi
echo "cluster=$cluster_name out_dir=$out_dir threads=$threads stage_data=$stage_data" \
     "stage_tools=$stage_tools stage_libs=$stage_libs stage_root=${STAGE_ROOT:-auto}" >&2

parallel_reference_tools=${PARALLEL_REFERENCE_TOOLS:-0}
skip_reference_tools=${SKIP_REFERENCE_TOOLS:-0}
reference_tool_jobs=${REFERENCE_TOOL_JOBS:-0}
main_parallel_tools=(indexed_bzip2 ibzip2 rapidgzip ibzip2-1.7.0 pbz2 lbzip2-rs mibz2 fbz crabz2 lbzip2 pbzip2)
if [[ -n "${PARALLEL_TOOLS:-}" ]]; then
    read -r -a main_parallel_tools <<< "$PARALLEL_TOOLS"
fi
reference_tools=("CPython.bz2" "GNU bzip2" "libarchive" "bzip2-rs-cli" "seek-bzip2" "7z" "GNU gzip" "igzip")
reference_cpus=()
reference_group_tools=()

prepare_parallel_reference_tools() {
    reference_cpus=()
    reference_group_tools=()
    if [[ "$parallel_reference_tools" != 1 ]]; then
        return 0
    fi
    if ! command -v taskset >/dev/null 2>&1; then
        echo "taskset unavailable; running reference tools sequentially" >&2
        parallel_reference_tools=0
        return 0
    fi
    if [[ -z "${host_logical:-}" || -z "${logical_cores:-}" ]]; then
        host_logical=$(lscpu 2>/dev/null | sed -n 's/^CPU(s):[[:space:]]*//p' | head -1)
        host_logical=${host_logical:-$(nproc)}
        logical_cores=$host_logical
    fi
    if (( logical_cores < host_logical )); then
        echo "Parallel reference tools need a full-node allocation; running sequentially" >&2
        parallel_reference_tools=0
        return 0
    fi
    local topology=${REFERENCE_TOOL_TOPOLOGY:-spread}
    case "$topology" in
        spread|l3|socket) ;;
        *)
            echo "Unknown REFERENCE_TOOL_TOPOLOGY=$topology; using spread" >&2
            topology=spread
            ;;
    esac

    local topology_tool=
    for tool in lstopo-no-graphics lstopo; do
        if command -v "$tool" >/dev/null 2>&1; then
            topology_tool=$tool
            break
        fi
    done
    if [[ -n "$topology_tool" ]]; then
        local topology_script
        topology_script=$(mktemp)
        cat > "$topology_script" <<'PY'
import sys
import xml.etree.ElementTree as ElementTree

topology = sys.argv[1] if len(sys.argv) > 1 else "spread"
root = ElementTree.fromstring( sys.stdin.read() )

if topology == "spread":
    cpus = []
    for obj in root.iter( "object" ):
        if obj.get( "type" ) == "Core":
            first = next( ( child.get( "os_index" ) for child in obj.iter( "object" )
                            if child.get( "type" ) == "PU" ), None )
            if first is not None:
                cpus.append( int( first ) )
    cpus = sorted( set( cpus ) )
    if cpus:
        bits = max( 1, ( len( cpus ) - 1 ).bit_length() )
        seen = set()
        result = []
        for value in range( 1 << bits ):
            index = 0
            remaining = value
            for _ in range( bits ):
                index = ( index << 1 ) | ( remaining & 1 )
                remaining >>= 1
            index = index * len( cpus ) // ( 1 << bits )
            if index >= len( cpus ):
                index = len( cpus ) - 1
            if index in seen:
                continue
            seen.add( index )
            result.append( cpus[index] )
        print( "\n".join( str( cpu ) for cpu in result ) )
    sys.exit( 0 )

type_name = {"l3": "L3Cache", "socket": "Package"}[topology]
entries = []


def walk( obj, package ):
    if obj.get( "type" ) == "Package":
        package = obj.get( "os_index" )
    if obj.get( "type" ) == type_name:
        first = next( ( child.get( "os_index" ) for child in obj.iter( "object" )
                        if child.get( "type" ) == "PU" ), None )
        if first is not None:
            entries.append( ( package, first ) )
    for child in obj.findall( "object" ):
        walk( child, package )


walk( root, None )
packages = {}
for package, cpu in entries:
    packages.setdefault( package, [] ).append( cpu )
result = []
for index in range( max( ( len( cpus ) for cpus in packages.values() ), default=0 ) ):
    for package in sorted( packages, key=lambda value: ( value is None, value ) ):
        if index < len( packages[package] ):
            result.append( packages[package][index] )
print( "\n".join( result ) )
PY
        mapfile -t reference_cpus < <("$topology_tool" --of xml 2>/dev/null | python3 "$topology_script" "$topology")
        rm -f "$topology_script"
    fi

    if (( ${#reference_cpus[@]} == 0 )); then
        echo "hwloc topology unavailable or empty; falling back to NUMA CPUs" >&2
        local node_cpu_lists=()
        while IFS= read -r line; do
            [[ -n "$line" ]] && node_cpu_lists+=("$line")
        done < <(numactl --hardware 2>/dev/null | sed -n 's/^node [0-9]* cpus: //p')
        if (( ${#node_cpu_lists[@]} == 0 )); then
            echo "Could not query NUMA CPUs; running reference tools sequentially" >&2
            parallel_reference_tools=0
            return 0
        fi
        local fallback_jobs=${reference_tool_jobs:-0}
        if (( fallback_jobs <= 0 )); then
            fallback_jobs=${#node_cpu_lists[@]}
        fi
        (( fallback_jobs > ${#reference_tools[@]} )) && fallback_jobs=${#reference_tools[@]}
        local cpus cpu_array index
        for ((index=0; index<${#reference_tools[@]}; index++)); do
            for cpus in "${node_cpu_lists[@]}"; do
                read -r -a cpu_array <<< "$cpus"
                if (( index < ${#cpu_array[@]} )); then
                    reference_cpus+=("${cpu_array[$index]}")
                fi
                (( ${#reference_cpus[@]} >= fallback_jobs )) && break
            done
            (( ${#reference_cpus[@]} >= fallback_jobs )) && break
        done
    fi

    local jobs=${reference_tool_jobs:-0}
    if (( jobs <= 0 )); then
        jobs=${#reference_cpus[@]}
    fi
    (( jobs > ${#reference_cpus[@]} )) && jobs=${#reference_cpus[@]}
    (( jobs > ${#reference_tools[@]} )) && jobs=${#reference_tools[@]}
    (( jobs < 1 )) && jobs=1
    reference_cpus=("${reference_cpus[@]:0:jobs}")
    jobs=${#reference_cpus[@]}
    if (( jobs < 1 )); then
        echo "No reference CPUs available; running reference tools sequentially" >&2
        parallel_reference_tools=0
        return 0
    fi
    for ((index=0; index<jobs; index++)); do
        reference_group_tools[index]=""
    done
    for index in "${!reference_tools[@]}"; do
        local group_index=$(( index % jobs ))
        if [[ -n "${reference_group_tools[$group_index]}" ]]; then
            reference_group_tools[$group_index]+=$'\n'"${reference_tools[$index]}"
        else
            reference_group_tools[$group_index]="${reference_tools[$index]}"
        fi
    done
    echo "Reference tools: $jobs parallel jobs on CPUs ${reference_cpus[*]}" >&2
}

if ! command -v module >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    source /etc/profile.d/modules.sh
fi

module load release/2026 Anaconda3/2025.06-1
if [[ "$prepare_data_only" != 1 ]]; then
    module load GCC/14.2.0 CMake/3.31.3 Ninja/1.12.1
    # ISA-L needs NASM. Fall back to an ISA-L-off build when no module is available.
    module load NASM/2.16.03 2>/dev/null ||
        module load NASM/2.16.01 2>/dev/null ||
        module load NASM 2>/dev/null || true
    for hardware_module in numactl/2.0.19 hwloc/2.11.2; do
        module load "$hardware_module" 2>/dev/null ||
            echo "Module $hardware_module unavailable; hardware metadata will use /proc and sysfs" >&2
    done
    if module load Rust/1.91.1 2>/dev/null; then
        echo "Loaded Rust/1.91.1 for bzip2-rs-cli and the Rust decoders" >&2
    else
        echo "Rust module unavailable; the Rust decoders will be skipped" >&2
    fi
fi

if [[ "$prepare_data_only" != 1 ]]; then
    if [[ ! -x "$venv/bin/python" ]]; then
        python3 -m venv "$venv"
    fi
    # shellcheck disable=SC1091
    source "$venv/bin/activate"
    # The extension is built with GCC 14.2 and _GLIBCXX_ASSERTIONS, but the Anaconda venv may
    # otherwise load its older libstdc++.so.6 at import time. Preloading also overrides an RPATH
    # in the Anaconda Python binary, which LD_LIBRARY_PATH alone does not.
    gcc_libstdcxx=
    if command -v g++ >/dev/null 2>&1; then
        gcc_libstdcxx=$(g++ -print-file-name=libstdc++.so.6)
    fi
    if [[ -f "$gcc_libstdcxx" ]]; then
        gcc_libdir=$(dirname -- "$gcc_libstdcxx")
        export LD_LIBRARY_PATH="$gcc_libdir:${LD_LIBRARY_PATH:-}"
        export LD_PRELOAD="$gcc_libstdcxx${LD_PRELOAD:+:$LD_PRELOAD}"
        echo "Using GCC libstdc++: $gcc_libstdcxx" >&2
    else
        echo "Could not locate the GCC libstdc++.so.6; the Python extension may fail to import" >&2
    fi
    if [[ ! -f "$venv/.rapidarchive-ready" ]]; then
        python -m pip install --upgrade pip setuptools wheel cython matplotlib
        touch "$venv/.rapidarchive-ready"
    fi

    cd "$repo"
    git submodule update --init --recursive

    cpu_model=$( ( lscpu 2>/dev/null | sed -n 's/^Model name:[[:space:]]*//p' | head -1 ) || true )
    if [[ -z "$cpu_model" ]]; then
        cpu_model=$(uname -m)
    fi
    cpu_suffix=$(printf '%s' "$cpu_model" | tr -cs 'A-Za-z0-9' '-' | sed 's/^-//;s/-$//')
    cpu_suffix=${cpu_suffix%-Core-Processor}
    cpu_suffix=${cpu_suffix%-Processor}
    bindir=${BUILD_OTHER_TOOLS_DIR:-$repo/build-other-tools${cpu_suffix:+-$cpu_suffix}}/bin
    if [[ "$build_tools" == 1 ]]; then
        # Rerunning is cheap and also picks up newly added tools such as STREAM.
        JOBS="$build_jobs" "$repo/results/benchmarks/indexed_bzip2-2.0.0/build-other-tools.sh"
        if ! python3 -c "import indexed_bzip2" >/dev/null 2>&1; then
            echo "Warning: indexed_bzip2 import failed even with ${gcc_libstdcxx:-the GCC runtime} preloaded:" >&2
            python3 -c "import indexed_bzip2" >&2 || true
        fi
    fi
    export PATH="$bindir:$PATH"
fi

download() {
    curl -L --fail --retry 3 --retry-delay 5 -C - -o "$2" "$1"
}

silesia_zip_url='https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip'
silesia_tar_url='https://mirror.circlestorm.org/silesia.tar'
silesia_zip_md5='c240c17d6805fb8a0bde763f1b94cd99'
silesia_raw_size=211957760
ctu_url='https://mcfp.felk.cvut.cz/publicDatasets/CTU-13-Dataset/CTU-13-Dataset.tar.bz2'

prepare_silesia() {
    local tar=$data_dir/silesia.tar
    local zip=$data_dir/silesia.zip
    if [[ ! -f "$tar" || "$(stat -c%s "$tar")" -ne "$silesia_raw_size" ]]; then
        local zip_ok=0
        if [[ -f "$zip" ]] && [[ "$(md5sum "$zip" | cut -d' ' -f1)" == "$silesia_zip_md5" ]]; then
            zip_ok=1
        fi
        if [[ "$zip_ok" == 0 ]]; then
            rm -f "$zip.tmp"
            if download "$silesia_zip_url" "$zip.tmp" &&
               [[ "$(md5sum "$zip.tmp" | cut -d' ' -f1)" == "$silesia_zip_md5" ]]; then
                mv "$zip.tmp" "$zip"
                zip_ok=1
            else
                rm -f "$zip.tmp"
            fi
        fi
        if [[ "$zip_ok" == 1 ]]; then
            rm -rf "$data_dir/silesia" "$data_dir/silesia-extract"
            mkdir -p "$data_dir/silesia-extract"
            python3 -m zipfile -e "$zip" "$data_dir/silesia-extract"
            if [[ -d "$data_dir/silesia-extract/silesia" ]]; then
                mv "$data_dir/silesia-extract/silesia" "$data_dir/silesia"
                rm -rf "$data_dir/silesia-extract"
            else
                mv "$data_dir/silesia-extract" "$data_dir/silesia"
            fi
            tar -cf "$tar" -C "$data_dir" silesia
            if [[ "$(stat -c%s "$tar")" -ne "$silesia_raw_size" ]]; then
                echo "Silesia zip produced an unexpected tar size" >&2
                rm -f "$tar"
                return 1
            fi
        else
            echo "Silesia zip unavailable, trying $silesia_tar_url" >&2
            rm -f "$tar.tmp"
            download "$silesia_tar_url" "$tar.tmp"
            if [[ "$(stat -c%s "$tar.tmp")" -ne "$silesia_raw_size" ]]; then
                echo "Downloaded Silesia tar has an unexpected size" >&2
                return 1
            fi
            mv "$tar.tmp" "$tar"
        fi
    fi

    if [[ ! -f "$data_dir/silesia.tar.bz2" ]]; then
        bzip2 -9 -c "$tar" > "$data_dir/silesia.tar.bz2.tmp"
        mv "$data_dir/silesia.tar.bz2.tmp" "$data_dir/silesia.tar.bz2"
    fi
    if [[ "$create_gzip" == 1 && ! -f "$data_dir/silesia.tar.gz" ]]; then
        gzip -9 -c "$tar" > "$data_dir/silesia.tar.gz.tmp"
        mv "$data_dir/silesia.tar.gz.tmp" "$data_dir/silesia.tar.gz"
    fi
}

prepare_silesia20x() {
    local tar=$data_dir/silesia.tar
    if [[ ! -f "$data_dir/silesia-20x.tar.bz2" ]]; then
        ( for _ in $(seq 20); do cat "$tar"; done ) |
            bzip2 -9 -c > "$data_dir/silesia-20x.tar.bz2.tmp"
        mv "$data_dir/silesia-20x.tar.bz2.tmp" "$data_dir/silesia-20x.tar.bz2"
    fi
    if [[ "$create_gzip" == 1 && ! -f "$data_dir/silesia-20x.tar.gz" ]]; then
        ( for _ in $(seq 20); do cat "$tar"; done ) |
            gzip -9 -c > "$data_dir/silesia-20x.tar.gz.tmp"
        mv "$data_dir/silesia-20x.tar.gz.tmp" "$data_dir/silesia-20x.tar.gz"
    fi
}

prepare_ctu() {
    local bz2=$data_dir/CTU-13-Dataset.tar.bz2
    if [[ ! -f "$bz2" || "$(stat -c%s "$bz2")" -lt 1900000000 ]]; then
        download "$ctu_url" "$bz2.tmp"
        if [[ "$(stat -c%s "$bz2.tmp")" -lt 1900000000 ]]; then
            echo "Downloaded CTU-13 archive has an unexpected size" >&2
            return 1
        fi
        mv "$bz2.tmp" "$bz2"
    fi
    if [[ "$create_gzip_ctu" == 1 && ! -f "$data_dir/CTU-13-Dataset.tar.gz" ]]; then
        if command -v lbzip2 >/dev/null 2>&1; then
            lbzip2 -dc "$bz2" | gzip -9 -c > "$data_dir/CTU-13-Dataset.tar.gz.tmp"
        else
            bzip2 -dc "$bz2" | gzip -9 -c > "$data_dir/CTU-13-Dataset.tar.gz.tmp"
        fi
        mv "$data_dir/CTU-13-Dataset.tar.gz.tmp" "$data_dir/CTU-13-Dataset.tar.gz"
    fi
}

prepare_datasets() {
    local specs=${DATASETS:-"silesia.tar.bz2:7 silesia-20x.tar.bz2:3 CTU-13-Dataset.tar.bz2:1"}
    mkdir -p "$data_dir"
    if [[ "$specs" == *silesia* ]]; then
        prepare_silesia
    fi
    if [[ "$specs" == *silesia-20x* ]]; then
        prepare_silesia20x
    fi
    if [[ "$specs" == *CTU* || "${PREPARE_CTU:-0}" == 1 ]]; then
        prepare_ctu
    fi
}

find_dataset() {
    local name=$1
    local candidate
    for candidate in "$data_dir/$name" "$data_dir/silesia/$name" "$data_dir/CTU/$name"; do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    find "$data_dir" -maxdepth 3 -type f -name "$name" -print -quit 2>/dev/null || true
}

run_dataset_parallel_reference() {
    local path=$1
    local source_path=$2
    local iterations=$3
    local benchmark_script="$repo/results/benchmarks/indexed_bzip2-2.0.0/benchmarkIndexedBzip2.py"
    local dataset_tag
    dataset_tag=$(basename -- "$path")
    dataset_tag=${dataset_tag%%.*}

    echo "  Parallel scaling tools: ${main_parallel_tools[*]}" >&2
    python3 "$benchmark_script" "$path" --iterations "$iterations" --threads $threads \
        --json-interval "$json_interval" \
        --source-file "$source_path" --tools "${main_parallel_tools[@]}" || return $?

    if [[ "$skip_reference_tools" == 1 ]]; then
        return 0
    fi

    local pids=()
    local group_index group_tools cpu ref_output pid
    for group_index in "${!reference_group_tools[@]}"; do
        [[ -n "${reference_group_tools[$group_index]}" ]] || continue
        mapfile -t group_tools <<< "${reference_group_tools[$group_index]}"
        cpu=${reference_cpus[$group_index]}
        ref_output="$out_dir/reference-${dataset_tag}-job${SLURM_JOB_ID:-$$}-part$(( group_index + 1 )).json"
        echo "  Reference part $(( group_index + 1 )) on CPU $cpu: ${group_tools[*]}" >&2
        taskset -c "$cpu" python3 "$benchmark_script" "$path" --iterations "$iterations" --threads 1 \
            --json-interval "$json_interval" \
            --source-file "$source_path" --tools "${group_tools[@]}" --no-stream \
            --output "$ref_output" &
        pids+=("$!")
    done

    local status=0
    for pid in "${pids[@]}"; do
        wait "$pid" || status=1
    done
    return "$status"
}

run_dataset() {
    local name=$1
    local iterations=$2
    local path source_path staged
    if [[ "$name" = /* ]]; then
        path=$name
        if [[ -n "${stage_dir:-}" ]]; then
            staged=$stage_dir/$(basename -- "$name")
            if [[ -f "$staged" ]]; then
                path=$staged
            fi
        fi
    else
        path=$(find_dataset "$name")
    fi
    if [[ -z "$path" ]]; then
        echo "Skipping $name: not found under $data_dir" >&2
        return
    fi
    source_path=${dataset_source_paths["$name"]:-$path}
    echo "Benchmarking $path with $iterations iterations" >&2
    if [[ "${parallel_reference_tools:-0}" == 1 ]]; then
        run_dataset_parallel_reference "$path" "$source_path" "$iterations"
    else
        local tool_args=()
        if [[ "$skip_reference_tools" == 1 ]]; then
            tool_args=(--tools "${main_parallel_tools[@]}")
        fi
        python3 "$repo/results/benchmarks/indexed_bzip2-2.0.0/benchmarkIndexedBzip2.py" \
            "$path" --iterations "$iterations" --threads $threads \
            --json-interval "$json_interval" --source-file "$source_path" "${tool_args[@]}"
    fi
}

if [[ -n "${DATASETS:-}" ]]; then
    read -r -a dataset_specs <<< "$DATASETS"
else
    dataset_specs=(silesia.tar.bz2:7 silesia-20x.tar.bz2:3 CTU-13-Dataset.tar.bz2:1)
fi

if [[ "$prepare_data_only" == 1 ]]; then
    prepare_datasets
    exit 0
fi

if [[ "$prepare_data" == 1 ]]; then
    prepare_datasets
fi

if [[ "${PREPARE_ONLY:-0}" == 1 ]]; then
    echo "Preparation complete; not running the benchmark"
    exit 0
fi

# Horse is latency-bound, so copy the compressed inputs to node-local storage for the timed runs.
stage_dir=
cleanup_stage() {
    if [[ -n "${stage_dir:-}" && "$keep_staged_data" != 1 ]]; then
        rm -rf -- "$stage_dir"
    fi
}
trap cleanup_stage EXIT
declare -A dataset_source_paths=()
if [[ "$stage_data" == 1 ]]; then
    required_bytes=0
    for spec in "${dataset_specs[@]}"; do
        name=${spec%:*}
        if [[ "$name" = /* ]]; then
            path=$name
        else
            path=$(find_dataset "$name")
        fi
        [[ -n "$path" && -f "$path" ]] || continue
        dataset_source_paths["$name"]=$path
        required_bytes=$(( required_bytes + $(stat -c%s -- "$path") ))
        if [[ "$path" == *.bz2 && -f "${path%.bz2}.gz" ]]; then
            required_bytes=$(( required_bytes + $(stat -c%s -- "${path%.bz2}.gz") ))
        fi
    done
    if (( ${#dataset_source_paths[@]} > 0 )); then
        if [[ "$stage_tools" == 1 && -n "${bindir:-}" && -d "$bindir" ]]; then
            required_bytes=$(( required_bytes + $(du -sb -- "$bindir" | cut -f1) ))
        fi
        if [[ -n "${STAGE_ROOT:-}" ]]; then
            stage_candidates=("$STAGE_ROOT")
        else
            # Short benchmark inputs fit into the node's shared memory, which removes the
            # node-local disk from the timed runs. Fall back to TMPDIR and /tmp.
            stage_candidates=("/dev/shm" "${TMPDIR:-/tmp}" "/tmp")
        fi
        selected_stage_root=
        seen_stage_roots=' '
        for candidate in "${stage_candidates[@]}"; do
            [[ -d "$candidate" ]] || continue
            if [[ "$seen_stage_roots" == *" $candidate "* ]]; then
                continue
            fi
            seen_stage_roots+="$candidate "
            candidate_available=$(df -B1 --output=avail "$candidate" 2>/dev/null | tail -1 | tr -d ' ' || true)
            if [[ ! "$candidate_available" =~ ^[0-9]+$ ]]; then
                continue
            fi
            if (( required_bytes > candidate_available )); then
                echo "Not enough space in $candidate: need $required_bytes B, have $candidate_available B" >&2
                continue
            fi
            if [[ "$candidate" == "/dev/shm" ]]; then
                mem_available=$(awk '/^MemAvailable:/ { print $2 * 1024 }' /proc/meminfo 2>/dev/null || true)
                if [[ ! "$mem_available" =~ ^[0-9]+$ ]] || (( mem_available < required_bytes * 4 )); then
                    echo "Skipping /dev/shm: only $mem_available B MemAvailable for $required_bytes B of data" >&2
                    continue
                fi
            fi
            selected_stage_root=$candidate
            break
        done
        if [[ -n "$selected_stage_root" ]]; then
            stage_root=$selected_stage_root
            echo "Using $stage_root for staging" >&2
        else
            echo "Not staging datasets: no candidate has enough space for $required_bytes B" >&2
            stage_data=0
        fi
    fi
fi
if [[ "$stage_data" == 1 && ${#dataset_source_paths[@]} -gt 0 ]]; then
    stage_tag=$(printf '%s-%s' "${SLURM_JOB_ID:-$$}" "${SLURM_JOB_NAME:-bench}" | tr -cs 'A-Za-z0-9._-' '-')
    stage_dir=$(mktemp -d -p "$stage_root" "ibzip2-${stage_tag}-data-XXXXXX")
    echo "Staging datasets from $data_dir to $stage_dir" >&2
    for name in "${!dataset_source_paths[@]}"; do
        path=${dataset_source_paths[$name]}
        cp -f --reflink=auto -- "$path" "$stage_dir/" 2>/dev/null ||
            cp -f -- "$path" "$stage_dir/"
        if [[ "$path" == *.bz2 && -f "${path%.bz2}.gz" ]]; then
            cp -f --reflink=auto -- "${path%.bz2}.gz" "$stage_dir/" 2>/dev/null ||
                cp -f -- "${path%.bz2}.gz" "$stage_dir/"
        fi
    done
    data_dir=$stage_dir
    if [[ "$stage_tools" == 1 && -n "${bindir:-}" && -d "$bindir" ]]; then
        # Exec'ing the tool binaries from Horse is the remaining launch-stall source. The
        # benchmark binaries are small, so keep them on the same node-local filesystem as the data.
        tool_stage_dir=$stage_dir/bin
        mkdir -p "$tool_stage_dir"
        cp -a --reflink=auto -- "$bindir/." "$tool_stage_dir/" 2>/dev/null ||
            cp -a -- "$bindir/." "$tool_stage_dir/"
        bindir=$tool_stage_dir
        export PATH="$bindir:$PATH"
        echo "Staged tools in $bindir" >&2
    fi
    if [[ "$stage_libs" == 1 ]]; then
        # The C++ tools preload the GCC libstdc++ from the shared software tree. Stage the whole
        # GCC runtime directory and point the loader at the node-local copy, otherwise every exec
        # can still stall on /software even when the binary itself is in /dev/shm.
        runtime_lib_dir=$stage_dir/lib
        mkdir -p "$runtime_lib_dir"
        gcc_libstdcxx=$(command -v g++ >/dev/null 2>&1 && g++ -print-file-name=libstdc++.so.6 || true)
        if [[ -n "$gcc_libstdcxx" && -f "$gcc_libstdcxx" ]]; then
            gcc_libdir=$(dirname -- "$gcc_libstdcxx")
            # The GCC libdir can contain large static libraries. Copy only the shared runtime
            # libraries that the staged tools and the Python extension need.
            for libPattern in 'libstdc++.so.6*' 'libgcc_s.so.1*' 'libatomic.so.1*'; do
                for lib in "$gcc_libdir"/$libPattern; do
                    [[ -e "$lib" || -L "$lib" ]] || continue
                    cp -a --reflink=auto -- "$lib" "$runtime_lib_dir/" 2>/dev/null ||
                        cp -a -- "$lib" "$runtime_lib_dir/"
                done
            done
            other_preloads=
            if [[ -n "${LD_PRELOAD:-}" ]]; then
                IFS=: read -r -a preload_array <<< "$LD_PRELOAD"
                for preload in "${preload_array[@]}"; do
                    if [[ -n "$preload" && "$preload" != "$gcc_libstdcxx" ]]; then
                        other_preloads+="${other_preloads:+:}$preload"
                    fi
                done
            fi
            export LD_LIBRARY_PATH="$runtime_lib_dir:${LD_LIBRARY_PATH:-}"
            export LD_PRELOAD="$runtime_lib_dir/$(basename -- "$gcc_libstdcxx")${other_preloads:+:$other_preloads}"
            echo "Staged runtime libraries in $runtime_lib_dir" >&2
        else
            echo "Could not locate the GCC libstdc++.so.6; runtime libraries are not staged" >&2
        fi
        if [[ -x "$bindir/pbzip2" ]]; then
            libbz2=$(timeout 10 ldd "$bindir/pbzip2" 2>/dev/null | awk '/libbz2/ {print $3; exit}')
            if [[ -n "$libbz2" && -f "$libbz2" ]]; then
                cp -a --reflink=auto -- "$libbz2" "$runtime_lib_dir/" 2>/dev/null ||
                    cp -a -- "$libbz2" "$runtime_lib_dir/"
                echo "Staged $(basename -- "$libbz2") in $runtime_lib_dir" >&2
            fi
        fi
    fi
fi

mkdir -p "$out_dir"
out_dir=$(cd -- "$out_dir" && pwd)
cd "$out_dir"

prepare_parallel_reference_tools

for spec in "${dataset_specs[@]}"; do
    run_dataset "${spec%:*}" "${spec##*:}"
done
