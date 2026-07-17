#!/usr/bin/env python3
"""Create the manifest file required by the RK Motorcycle OTA mini program.

Example:
    python tools/package_firmware.py firmware.bin --product-id 0x1001 --hw-id 1 --fw-version 1.0.2
"""

import argparse
import json
import re
import sys
import zlib
from pathlib import Path
from typing import List, Optional, Tuple


DEFAULT_APP_BASE = 0x08008000
DEFAULT_APP_MAX_SIZE = 128 * 1024
VERSION_PATTERN = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def parse_u32(value: str) -> int:
    try:
        number = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"无效的整数：{value}") from error
    if not 0 <= number <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError(f"数值必须在 0 到 0xFFFFFFFF 之间：{value}")
    return number


def parse_version(value: str) -> Tuple[str, int]:
    match = VERSION_PATTERN.fullmatch(value)
    if not match:
        raise argparse.ArgumentTypeError("版本号格式必须为 主版本.次版本.修订版本，例如 1.0.2")
    major, minor, patch = (int(part) for part in match.groups())
    if any(part > 0xFF for part in (major, minor, patch)):
        raise argparse.ArgumentTypeError("版本号每一段必须在 0 到 255 之间")
    return value, (major << 16) | (minor << 8) | patch


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="为微信小程序 OTA 生成 .manifest.json 文件（CRC32/大小自动计算）。"
    )
    parser.add_argument("firmware", type=Path, help="App 固件 .bin 文件路径")
    parser.add_argument("--product-id", required=True, type=parse_u32, help="产品型号，例如 0x1001")
    parser.add_argument("--hw-id", required=True, type=parse_u32, help="硬件版本，例如 1")
    parser.add_argument("--fw-version", required=True, type=parse_version, help="目标版本，例如 1.0.2")
    parser.add_argument("--app-base", type=parse_u32, default=DEFAULT_APP_BASE, help="App 起始地址，默认 0x08008000")
    parser.add_argument("--app-max-size", type=parse_u32, default=DEFAULT_APP_MAX_SIZE, help="App 最大字节数，默认 131072")
    parser.add_argument("--output", type=Path, help="输出 manifest 路径；默认与 bin 同目录、同名 .manifest.json")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    firmware_path = args.firmware.expanduser().resolve()
    if firmware_path.suffix.lower() != ".bin":
        print("错误：固件文件必须是 .bin", file=sys.stderr)
        return 2
    if not firmware_path.is_file():
        print(f"错误：找不到固件文件：{firmware_path}", file=sys.stderr)
        return 2

    image = firmware_path.read_bytes()
    image_size = len(image)
    if image_size == 0:
        print("错误：固件文件为空", file=sys.stderr)
        return 2
    if image_size > args.app_max_size:
        print(
            f"错误：固件大小 {image_size} bytes 超过 App 最大容量 {args.app_max_size} bytes",
            file=sys.stderr,
        )
        return 2

    version_string, version_u32 = args.fw_version
    image_crc32 = zlib.crc32(image) & 0xFFFFFFFF
    output_path = args.output.expanduser() if args.output else firmware_path.with_suffix(".manifest.json")
    output_path = output_path.resolve()

    manifest = {
        "product_id": args.product_id,
        "hw_id": args.hw_id,
        "fw_version": version_string,
        "image_size": image_size,
        "image_crc32": f"0x{image_crc32:08X}",
        "app_base": f"0x{args.app_base:08X}",
        "app_max_size": args.app_max_size,
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("OTA manifest 已生成")
    print(f"固件:     {firmware_path}")
    print(f"输出:     {output_path}")
    print(f"大小:     {image_size} bytes")
    print(f"CRC32:    0x{image_crc32:08X}")
    print(f"版本编码: 0x{version_u32:08X} ({version_string})")
    return 0


def interactive_main() -> int:
    """Run when the file is double-clicked in Windows Explorer."""
    print("=" * 56)
    print("RK Motorcycle OTA 升级包 Manifest 生成工具")
    print("请按提示输入信息；可直接把文件拖到此窗口后按回车。")
    print("=" * 56)
    firmware = input("固件 .bin 文件路径: ").strip().strip('"')
    product_id = input("产品型号 product_id（例如 0x1001）: ").strip()
    hw_id = input("硬件版本 hw_id（例如 1）: ").strip()
    fw_version = input("目标固件版本（例如 1.0.2）: ").strip()
    try:
        return main([
            firmware,
            "--product-id", product_id,
            "--hw-id", hw_id,
            "--fw-version", fw_version,
        ])
    except SystemExit as error:
        return int(error.code) if isinstance(error.code, int) else 2
    finally:
        input("\n按回车键关闭窗口…")


if __name__ == "__main__":
    if len(sys.argv) == 1:
        raise SystemExit(interactive_main())
    raise SystemExit(main())
