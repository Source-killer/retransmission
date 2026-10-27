"""Identify the work filesystem and benchmark only private temporary files."""

import json
from pathlib import Path
import subprocess
import tempfile


def filesystem_info(path):
    try:
        result = subprocess.run(
            ["findmnt", "--json", "--target", str(path), "--output", "TARGET,SOURCE,FSTYPE,OPTIONS"],
            check=True, capture_output=True, text=True, timeout=10)
        return json.loads(result.stdout)["filesystems"][0]
    except (OSError, subprocess.SubprocessError, ValueError, KeyError, IndexError) as error:
        return {"fstype": "unknown", "error": str(error)}


def disk_target_eligible(filesystem, storage):
    return storage != "tmpfs" and (filesystem.get("fstype") or "unknown") not in {"tmpfs", "ramfs", "unknown"}


def summarize_fio(result, direction):
    rate = 0.0
    iops = 0.0
    for job in result["jobs"]:
        if job.get("error", 0):
            raise RuntimeError(f"fio job {job.get('jobname')} failed: {job['error']}")
        counters = job[direction]
        runtime_ms = counters["runtime"]
        if runtime_ms > 0:
            rate += counters["io_bytes"] * 1000 / runtime_ms
        iops += counters["iops"]
    return {"gbit_per_second": rate * 8 / 1e9, "mib_per_second": rate / (1024 * 1024), "iops": iops}


def run_fio(root, output, size_mib):
    summary = {}
    cases = [("sequential_write", "write", "1m", 1), ("sequential_read", "read", "1m", 1),
             ("random_write_16k", "randwrite", "16k", 4)]
    with tempfile.TemporaryDirectory(prefix="fio-", dir=root) as directory:
        data = Path(directory) / "data.bin"
        for name, operation, block_size, jobs in cases:
            result_path = output / f"fio-{name}.json"
            command = ["fio", f"--name={name}", f"--filename={data}",
                       f"--size={min(size_mib, 1024) * 1024 * 1024}", f"--rw={operation}", f"--bs={block_size}",
                       "--ioengine=libaio", "--direct=1", "--iodepth=16", f"--numjobs={jobs}",
                       "--runtime=5", "--ramp_time=1", "--time_based=1", "--end_fsync=1",
                       "--refill_buffers=1", "--group_reporting=1", "--output-format=json", f"--output={result_path}"]
            with (output / f"fio-{name}.log").open("w") as log:
                subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, timeout=120)
            summary[name] = summarize_fio(json.loads(result_path.read_text()), "read" if operation == "read" else "write")
    return summary
