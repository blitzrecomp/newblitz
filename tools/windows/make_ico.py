#!/usr/bin/env python3
"""The Windows app icon (platform/windows/windows_stage.cmake): a PNG as a multi-size .ico.
    make_ico.py <icon.png> <out.ico>
"""
import sys

from PIL import Image

Image.open(sys.argv[1]).convert("RGBA").save(
    sys.argv[2], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
