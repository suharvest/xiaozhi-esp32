#!/usr/bin/env python3
"""
ESP32 Assets Partition Extractor

Extract assets from ESP32 assets partition binary file.
Usage:
    1. Read the assets partition from ESP32:
       esptool.py -p /dev/ttyUSB0 read_flash 0x800000 0x800000 assets.bin

    2. Run this script:
       python extract_assets.py assets.bin -o raspberry_pi_display/assets
"""

import argparse
import struct
import json
import os
from pathlib import Path
from typing import Dict, Any, Optional


class AssetEntry:
    """Asset index entry"""
    ENTRY_SIZE = 44  # 32 + 4 + 4 + 2 + 2 (no padding)

    def __init__(self, data: bytes):
        # Parse: name(32) + size(4) + offset(4) + width(2) + height(2)
        self.name = data[:32].rstrip(b'\x00').decode('utf-8', errors='replace')
        self.size = struct.unpack('<I', data[32:36])[0]
        self.offset = struct.unpack('<I', data[36:40])[0]
        self.width = struct.unpack('<H', data[40:42])[0]
        self.height = struct.unpack('<H', data[42:44])[0]

    def __repr__(self):
        return f"AssetEntry(name='{self.name}', size={self.size}, offset={self.offset}, {self.width}x{self.height})"


def calculate_checksum(data: bytes) -> int:
    """Calculate checksum (matches ESP32 implementation)"""
    checksum = 0
    for byte in data:
        checksum += byte
    return checksum & 0xFFFF


def extract_assets(bin_path: str, output_dir: str, verbose: bool = False) -> bool:
    """Extract assets from binary file"""

    if not os.path.exists(bin_path):
        print(f"Error: File not found: {bin_path}")
        return False

    with open(bin_path, 'rb') as f:
        data = f.read()

    print(f"Read {len(data)} bytes from {bin_path}")

    # Parse header (12 bytes)
    if len(data) < 12:
        print("Error: File too small")
        return False

    stored_files = struct.unpack('<I', data[0:4])[0]
    stored_chksum = struct.unpack('<I', data[4:8])[0]
    stored_len = struct.unpack('<I', data[8:12])[0]

    print(f"Header: files={stored_files}, checksum=0x{stored_chksum:04x}, length={stored_len}")

    # Sanity check
    if stored_files == 0 or stored_files > 1000:
        print(f"Error: Invalid number of files: {stored_files}")
        return False

    if stored_len > len(data) - 12:
        print(f"Error: stored_len ({stored_len}) exceeds file size ({len(data) - 12})")
        return False

    # Verify checksum
    calculated_checksum = calculate_checksum(data[12:12+stored_len])
    if calculated_checksum != stored_chksum:
        print(f"Warning: Checksum mismatch (expected 0x{stored_chksum:04x}, got 0x{calculated_checksum:04x})")
        # Continue anyway
    else:
        print("Checksum verified OK")

    # Parse asset index table
    index_offset = 12
    assets: Dict[str, AssetEntry] = {}

    for i in range(stored_files):
        entry_data = data[index_offset + i * AssetEntry.ENTRY_SIZE :
                          index_offset + (i + 1) * AssetEntry.ENTRY_SIZE]
        if len(entry_data) < AssetEntry.ENTRY_SIZE:
            print(f"Error: Incomplete entry at index {i}")
            break

        entry = AssetEntry(entry_data)
        assets[entry.name] = entry
        if verbose:
            print(f"  [{i}] {entry}")

    print(f"Found {len(assets)} assets")

    # Create output directory
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    (output_path / "emojis").mkdir(exist_ok=True)
    (output_path / "backgrounds").mkdir(exist_ok=True)
    (output_path / "icons").mkdir(exist_ok=True)
    (output_path / "raw").mkdir(exist_ok=True)

    # Calculate data offset (after index table)
    data_offset = 12 + stored_files * AssetEntry.ENTRY_SIZE

    # Extract each asset
    extracted_count = 0
    index_json = None

    for name, entry in assets.items():
        # Calculate absolute offset
        abs_offset = data_offset + entry.offset

        if abs_offset + entry.size > len(data):
            print(f"  Warning: Asset '{name}' offset out of bounds, skipping")
            continue

        asset_data = data[abs_offset:abs_offset + entry.size + 2]  # +2 for magic bytes

        # Check magic bytes "ZZ"
        if len(asset_data) < 2 or asset_data[0:2] != b'ZZ':
            print(f"  Warning: Asset '{name}' has invalid magic bytes, skipping")
            continue

        # Extract actual data (skip "ZZ" prefix)
        actual_data = asset_data[2:2+entry.size]

        # Determine output path based on file type/name
        if name == "index.json":
            out_file = output_path / "raw" / name
            try:
                index_json = json.loads(actual_data.decode('utf-8'))
            except:
                pass
        elif name.endswith('.bin') and 'font' in name.lower():
            out_file = output_path / "raw" / name
        elif name.endswith('.bin'):
            out_file = output_path / "raw" / name
        else:
            out_file = output_path / "raw" / name

        out_file.parent.mkdir(parents=True, exist_ok=True)
        with open(out_file, 'wb') as f:
            f.write(actual_data)

        extracted_count += 1
        if verbose:
            print(f"  Extracted: {name} ({entry.size} bytes)")

    print(f"Extracted {extracted_count} assets to {output_dir}/raw/")

    # Process index.json to organize assets
    if index_json:
        print("\nProcessing index.json...")
        organize_assets(output_path, index_json, assets, verbose)

    # Create Pygame-compatible index
    create_pygame_index(output_path, index_json, assets)

    print(f"\nDone! Assets extracted to: {output_dir}")
    return True


def organize_assets(output_path: Path, index_json: dict, assets: Dict[str, AssetEntry], verbose: bool = False):
    """Organize assets based on index.json"""

    raw_dir = output_path / "raw"

    # Process emoji collection
    emoji_collection = index_json.get("emoji_collection", [])
    for emoji in emoji_collection:
        if isinstance(emoji, dict):
            name = emoji.get("name", "")
            file = emoji.get("file", "")
            if file and (raw_dir / file).exists():
                # Copy to emojis directory with name-based filename
                src = raw_dir / file
                # Keep original extension or use .bin for raw
                ext = Path(file).suffix or ".bin"
                dst = output_path / "emojis" / f"{name}{ext}"
                if verbose:
                    print(f"  Emoji: {name} -> {dst.name}")
                dst.write_bytes(src.read_bytes())

    # Process skin/background images
    skin = index_json.get("skin", {})
    for theme_name in ["light", "dark"]:
        theme = skin.get(theme_name, {})
        bg_file = theme.get("background_image", "")
        if bg_file and (raw_dir / bg_file).exists():
            src = raw_dir / bg_file
            ext = Path(bg_file).suffix or ".bin"
            dst = output_path / "backgrounds" / f"bg_{theme_name}{ext}"
            if verbose:
                print(f"  Background: {theme_name} -> {dst.name}")
            dst.write_bytes(src.read_bytes())

    # Process icon collection
    icon_collection = index_json.get("icon_collection", [])
    for icon in icon_collection:
        if isinstance(icon, dict):
            name = icon.get("name", "")
            file = icon.get("file", "")
            if file and (raw_dir / file).exists():
                src = raw_dir / file
                ext = Path(file).suffix or ".bin"
                dst = output_path / "icons" / f"{name}{ext}"
                if verbose:
                    print(f"  Icon: {name} -> {dst.name}")
                dst.write_bytes(src.read_bytes())


def create_pygame_index(output_path: Path, index_json: Optional[dict], assets: Dict[str, AssetEntry]):
    """Create Pygame-compatible index.json"""

    pygame_index = {
        "version": 1,
        "source": "extracted_from_esp32",
        "emojis": {},
        "backgrounds": {},
        "icons": {},
        "theme": {
            "light": {},
            "dark": {}
        }
    }

    if index_json:
        # Process emoji collection
        for emoji in index_json.get("emoji_collection", []):
            if isinstance(emoji, dict):
                name = emoji.get("name", "")
                file = emoji.get("file", "")
                eaf = emoji.get("eaf", {})

                if name:
                    pygame_index["emojis"][name] = {
                        "file": f"emojis/{name}{Path(file).suffix or '.bin'}",
                        "fps": eaf.get("fps", 0) if eaf else 0,
                        "loop": eaf.get("loop", True) if eaf else True
                    }

        # Process skin
        skin = index_json.get("skin", {})
        for theme_name in ["light", "dark"]:
            theme = skin.get(theme_name, {})
            pygame_index["theme"][theme_name] = {
                "text_color": theme.get("text_color", "#000000" if theme_name == "light" else "#FFFFFF"),
                "background_color": theme.get("background_color", "#FFFFFF" if theme_name == "light" else "#1E1E1E"),
                "background_image": f"backgrounds/bg_{theme_name}.bin" if theme.get("background_image") else None
            }

        # Process icon collection
        for icon in index_json.get("icon_collection", []):
            if isinstance(icon, dict):
                name = icon.get("name", "")
                file = icon.get("file", "")
                if name:
                    pygame_index["icons"][name] = {
                        "file": f"icons/{name}{Path(file).suffix or '.bin'}"
                    }

    # Write index
    index_file = output_path / "index.json"
    with open(index_file, 'w', encoding='utf-8') as f:
        json.dump(pygame_index, f, indent=2, ensure_ascii=False)

    print(f"Created Pygame index: {index_file}")


def main():
    parser = argparse.ArgumentParser(
        description='Extract assets from ESP32 assets partition binary',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Extract from binary file
  %(prog)s assets.bin -o raspberry_pi_display/assets

  # Extract with verbose output
  %(prog)s assets.bin -o assets -v

To read assets partition from ESP32:
  esptool.py -p /dev/ttyUSB0 read_flash 0x800000 0x800000 assets.bin
        """
    )
    parser.add_argument('input', help='Path to assets.bin file')
    parser.add_argument('-o', '--output', default='assets',
                        help='Output directory (default: assets)')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Verbose output')

    args = parser.parse_args()

    success = extract_assets(args.input, args.output, args.verbose)
    return 0 if success else 1


if __name__ == '__main__':
    exit(main())
