#!/usr/bin/env python3
import sys
import os
import json
import hashlib
import base64
import signal
import subprocess
import time
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
import boto3
from botocore.exceptions import BotoCoreError, ClientError
import argparse

BLOCK_SIZE = 524288  # 512 KiB
MAX_WORKERS = 64

def aws_login() -> None:
    if subprocess.run(
        ["aws", "sts", "get-caller-identity", "--query", "Arn", "--output", "text"],
        stderr=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
    ).returncode != 0:
        subprocess.run(["aws", "login", "--remote"])

def get_instance_arch(instance_type: str):
    response = boto3.client('ec2').describe_instance_types(InstanceTypes=[instance_type])
    return response['InstanceTypes'][0]['ProcessorInfo']['SupportedArchitectures'][0]

def convert_to_raw(src: str) -> tuple[str, bool]:
    ext = src.rsplit(".", 1)[-1].lower()
    if ext == "raw":
        return src, False
    dst = f"aws/{os.path.basename(src).rsplit('.', 1)[0]}.raw"
    print(f"Converting {ext} → raw...")
    subprocess.run(["qemu-img", "convert", "-O", "raw", src, dst], check=True)
    return dst, True

def get_virtual_size(src: str) -> int:
    info = json.loads(subprocess.check_output(
        ["qemu-img", "info", "--output=json", src]
    ))
    return info["virtual-size"]

def upload_block_worker(ebs_client, snapshot_id, block_index, data, counters, counter_lock, total_blocks):
    """Worker function executed by the thread pool to upload a single block."""
    checksum = base64.b64encode(hashlib.sha256(data).digest()).decode()

    ebs_client.put_snapshot_block(
        SnapshotId=snapshot_id,
        BlockIndex=block_index,
        BlockData=data,
        DataLength=BLOCK_SIZE,
        Checksum=checksum,
        ChecksumAlgorithm="SHA256",
    )

    # Thread-safe update of counters and progress logging
    with counter_lock:
        counters["changed"] += 1
        counters["processed_blocks"] += 1
        current_processed = counters["processed_blocks"]
        current_changed = counters["changed"]

        if current_processed % 50 == 0 or current_processed == total_blocks:
            pct = current_processed / total_blocks * 100
            print(f"  {current_processed}/{total_blocks} blocks ({pct:.1f}%), {current_changed} uploaded")

def _next_console_chunk(prev: str, current: str) -> str:
    """Return the portion of `current` that comes after `prev` ended.

    Handles the case where AWS truncates the front of the 64 KB console
    buffer once boot output grows past it — a naive line-count offset would
    silently skip content.
    """
    if not prev:
        return current
    if current.startswith(prev):
        return current[len(prev):]
    # Front-truncated: search for a shrinking suffix of prev inside current.
    # Start with a decent chunk to avoid spurious partial-line matches, but
    # scale the floor to `prev`'s actual length so short prevs still match.
    max_suffix = min(len(prev), 4096)
    min_suffix = min(16, max_suffix)
    step = max(1, (max_suffix - min_suffix) // 32) or 1
    for suffix_len in range(max_suffix, min_suffix - 1, -step):
        suffix = prev[-suffix_len:]
        idx = current.rfind(suffix)
        if idx != -1:
            return current[idx + suffix_len:]
    # No overlap at all: log rotated past everything we had. Show all of
    # current with a marker so the user can tell where they lost context.
    return "\n[--- console buffer rotated past last seen output ---]\n" + current


def cleanup_aws_resources(ec2_client, instance_id=None, ami_id=None,
                          snapshot_id=None):
    """Best-effort teardown of anything that would keep costing money.

    Each step is guarded so a failure in one still lets the others run.
    Called from both KeyboardInterrupt and SIGTERM paths.
    """
    if instance_id:
        try:
            print(f"\nTerminating instance {instance_id}...", flush=True)
            ec2_client.terminate_instances(InstanceIds=[instance_id])
            # AWS billing stops as soon as the instance reaches
            # `shutting-down`; the full `terminated` state can take another
            # 1-2 minutes and blocks the caller for no operational benefit.
            for _ in range(30):
                try:
                    desc = ec2_client.describe_instances(
                        InstanceIds=[instance_id])
                    state = desc["Reservations"][0]["Instances"][0]["State"][
                        "Name"]
                except (BotoCoreError, ClientError):
                    state = None
                if state in ("shutting-down", "terminated", "stopped"):
                    print(f"Instance {instance_id} is {state}.", flush=True)
                    break
                time.sleep(2)
        except (BotoCoreError, ClientError) as e:
            print(f"WARN: terminate {instance_id} failed: {e}",
                  file=sys.stderr, flush=True)

    if ami_id:
        try:
            print(f"Deregistering AMI {ami_id}...", flush=True)
            ec2_client.deregister_image(ImageId=ami_id)
        except (BotoCoreError, ClientError) as e:
            print(f"WARN: deregister {ami_id} failed: {e}",
                  file=sys.stderr, flush=True)

    if snapshot_id:
        try:
            print(f"Deleting snapshot {snapshot_id}...", flush=True)
            ec2_client.delete_snapshot(SnapshotId=snapshot_id)
        except (BotoCoreError, ClientError) as e:
            print(f"WARN: delete snapshot {snapshot_id} failed: {e}",
                  file=sys.stderr, flush=True)


def stream_console(ec2_client, instance_id, poll_interval=5):
    """Tail the serial console until KeyboardInterrupt.

    Uses full-string overlap detection so that once the console buffer
    (~64 KB) rolls, we keep printing new content instead of silently
    freezing on a stale line offset.
    """
    print("Waiting for console output (usually 30-120s after launch)...",
          flush=True)
    printed = ""
    saw_any = False
    while True:
        try:
            resp = ec2_client.get_console_output(
                InstanceId=instance_id, Latest=True)
        except (BotoCoreError, ClientError) as e:
            print(f"[console poll error, retrying: {e}]",
                  file=sys.stderr, flush=True)
            time.sleep(poll_interval)
            continue

        output = resp.get("Output", "") or ""
        if output and output != printed:
            if not saw_any:
                saw_any = True
            new_chunk = _next_console_chunk(printed, output)
            if new_chunk:
                sys.stdout.write(new_chunk)
                if not new_chunk.endswith("\n"):
                    sys.stdout.write("\n")
                sys.stdout.flush()
            printed = output

        time.sleep(poll_interval)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("region", help="AWS region to deploy to (e.g. us-east-1)")
    parser.add_argument("instance", help="EC2 instance type to launch (e.g. t3.micro)")
    parser.add_argument("--image", default="build/last/loader.img",
                        help="Path to the source image file to upload (default: build/last/loader.img)")
    parser.add_argument("--attach", action="store_true", help="Stream system log and terminate instance on Ctrl+C")
    args = parser.parse_args()

    aws_login()

    src, region, instance = args.image, args.region, args.instance
    instance_arch = get_instance_arch(instance)

    if src.endswith(".raw"):
        virtual_size = os.path.getsize(src)
    else:
        virtual_size = get_virtual_size(src)
    volume_gib = (virtual_size + (1024**3) - 1) // (1024**3)
    print(f"Virtual size: {virtual_size} bytes → volume: {volume_gib} GiB")

    os.makedirs("aws", exist_ok=True)
    raw_image, cleanup = convert_to_raw(src)

    ebs_client = boto3.client("ebs", region_name=region)
    ec2_client = boto3.client("ec2", region_name=region)

    resp = ebs_client.start_snapshot(VolumeSize=volume_gib, Description="miniosv direct upload")
    snapshot_id = resp["SnapshotId"]
    print(f"Started snapshot: {snapshot_id}")

    with open("aws/.snapshot-id", "w") as f:
        f.write(snapshot_id)

    image_bytes = os.path.getsize(raw_image)
    total_blocks = (image_bytes + BLOCK_SIZE - 1) // BLOCK_SIZE
    print(f"Uploading {total_blocks} blocks using {MAX_WORKERS} parallel threads...")

    # Threading management tools
    counter_lock = threading.Lock()
    counters = {"changed": 0, "processed_blocks": 0}

    futures = set()

    with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
        with open(raw_image, "rb") as f:
            block_index = 0
            while True:
                data = f.read(BLOCK_SIZE)
                if not data:
                    break

                if len(data) < BLOCK_SIZE:
                    data = data.ljust(BLOCK_SIZE, b'\x00')

                # If block is completely empty, skip uploading entirely (sparse optimization)
                if not any(data):
                    with counter_lock:
                        counters["processed_blocks"] += 1
                    block_index += 1
                    continue

                # Memory-throttling link: If our queue is full, wait for an upload to finish
                # before reading more data from disk into memory.
                if len(futures) >= MAX_WORKERS * 2:
                    completed, futures = next(iter(as_completed(futures))), futures
                    futures.remove(completed)
                    completed.result() # Raise exceptions if any thread failed

                # Dispatch upload job to the thread pool
                future = executor.submit(
                    upload_block_worker,
                    ebs_client, snapshot_id, block_index, data,
                    counters, counter_lock, total_blocks
                )
                futures.add(future)
                block_index += 1

        # Wait for any remaining out-standing blocks to wrap up
        for future in as_completed(futures):
            future.result()

    final_changed = counters["changed"]
    ebs_client.complete_snapshot(SnapshotId=snapshot_id, ChangedBlocksCount=final_changed)
    print(f"Snapshot complete: {snapshot_id} ({final_changed} blocks written)")

    if cleanup:
        os.remove(raw_image)

    # --- AMI Registration ---
    print("Waiting for AWS to finalize snapshot state...")
    waiter = ec2_client.get_waiter('snapshot_completed')
    waiter.wait(SnapshotIds=[snapshot_id])
    print("Snapshot status is now 'completed'.")

    base_name = os.path.basename(src).rsplit('.', 1)[0]
    ami_name = f"miniosv-{base_name}-{int(time.time())}"

    print(f"Registering AMI: {ami_name}...")
    ami_response = ec2_client.register_image(
        Name=ami_name,
        Description=f"MiniOSv image created from {src}",
        Architecture=instance_arch,
        RootDeviceName="/dev/xvda",
        BlockDeviceMappings=[
            {
                "DeviceName": "/dev/xvda",
                "Ebs": {
                    "SnapshotId": snapshot_id,
                    "VolumeSize": volume_gib,
                    "VolumeType": "gp3",
                    "DeleteOnTermination": True
                }
            }
        ],
        VirtualizationType="hvm",
        EnaSupport=True,
        BootMode="uefi-preferred"
    )

    ami_id = ami_response["ImageId"]
    print(f"Successfully registered AMI: {ami_id}")

    with open("aws/.ami-id", "w") as f:
        f.write(ami_id)

    # --- Launch an instance from the AMI ---
    print("Waiting for AMI to become 'available'...")
    ec2_client.get_waiter('image_available').wait(ImageIds=[ami_id])

    print(f"Launching {instance} instance from {ami_id}...")
    run_response = ec2_client.run_instances(
        ImageId=ami_id,
        InstanceType=instance,
        MinCount=1,
        MaxCount=1,
        TagSpecifications=[
            {
                "ResourceType": "instance",
                "Tags": [{"Key": "Name", "Value": ami_name}],
            }
        ],
    )

    instance_id = run_response["Instances"][0]["InstanceId"]
    print(f"Launched instance: {instance_id}")

    with open("aws/.instance-id", "w") as f:
        f.write(instance_id)

    print("Waiting for instance to enter 'running' state...")
    ec2_client.get_waiter('instance_running').wait(InstanceIds=[instance_id])

    desc = ec2_client.describe_instances(InstanceIds=[instance_id])
    inst = desc["Reservations"][0]["Instances"][0]
    public_dns = inst.get("PublicDnsName") or "(none)"
    public_ip = inst.get("PublicIpAddress") or "(none)"
    print(f"Instance running: {instance_id} ({instance})")
    print(f"  Public DNS: {public_dns}")
    print(f"  Public IP:  {public_ip}")

    if args.attach:
        print("\nStreaming system log "
              "(Ctrl+C to terminate instance and delete AMI/snapshot)...")

        # Route SIGTERM to the same teardown path as SIGINT so `kill <pid>`
        # from another shell also cleans up.
        def _sigterm_handler(signum, frame):
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, _sigterm_handler)

        try:
            stream_console(ec2_client, instance_id)
        except KeyboardInterrupt:
            cleanup_aws_resources(
                ec2_client,
                instance_id=instance_id,
                ami_id=ami_id,
                snapshot_id=snapshot_id,
            )

if __name__ == "__main__":
    main()
