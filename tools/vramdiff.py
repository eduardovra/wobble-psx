"""Compares a VRAM dump against a test's reference image.

    vramdiff.py <ours.ppm> <reference.png> [left top right bottom]

Write the dump with the debugger's `vram <file>` and take the reference
from the test's own directory. Both sides are reduced to the five-bit
channels VRAM really holds, so the answer does not depend on how either
side widened them to eight — ours repeats the top bits into the bottom
ones, ps1-tests shifts and leaves zeroes, and those are the same pixel.

Give a rectangle to compare only the part of VRAM a test draws in. A
percentage over the whole megabyte can be dominated by background that
disagrees for reasons that are not the emulator's: gpu/transparency
reads 85% wrong whole and is exact inside the region it draws.
"""

import subprocess
import sys

Pixel = tuple[int, int, int]


def read_image(path: str) -> tuple[int, int, bytes]:
    """Width, height and raw RGB bytes of any image ImageMagick reads."""
    size = subprocess.run(
        ["identify", "-format", "%w %h", path],
        capture_output=True,
        text=True,
        check=True,
    )
    width, height = (int(number) for number in size.stdout.split())
    body = subprocess.run(
        ["convert", path, "-depth", "8", "rgb:-"],
        capture_output=True,
        check=True,
    )
    return width, height, body.stdout


def to_five_bit(pixels: bytes) -> bytes:
    return bytes(channel >> 3 for channel in pixels)


def pixel_at(pixels: bytes, width: int, x: int, y: int) -> Pixel:
    at = (y * width + x) * 3
    return (pixels[at], pixels[at + 1], pixels[at + 2])


def compare(
    ours: bytes,
    reference: bytes,
    width: int,
    region: tuple[int, int, int, int],
) -> list[tuple[int, int]]:
    left, top, right, bottom = region
    differing = []
    for y in range(top, bottom):
        for x in range(left, right):
            if pixel_at(ours, width, x, y) != pixel_at(reference, width, x, y):
                differing.append((x, y))
    return differing


def main() -> int:
    ours_path, reference_path = sys.argv[1], sys.argv[2]
    ours_width, ours_height, ours = read_image(ours_path)
    reference_width, reference_height, reference = read_image(reference_path)

    if (ours_width, ours_height) != (reference_width, reference_height):
        print(f"size differs: {ours_width}x{ours_height} vs "
              f"{reference_width}x{reference_height}")
        return 2

    if len(sys.argv) > 6:
        region = tuple(int(value) for value in sys.argv[3:7])
    else:
        region = (0, 0, ours_width, ours_height)

    differing = compare(
        ours=to_five_bit(ours),
        reference=to_five_bit(reference),
        width=ours_width,
        region=region,
    )

    left, top, right, bottom = region
    total = (right - left) * (bottom - top)
    if not differing:
        print(f"identical ({total} pixels)")
        return 0

    percent = 100.0 * len(differing) / total
    print(f"{len(differing)} of {total} pixels differ ({percent:.3f}%)")

    xs = [x for x, _ in differing]
    ys = [y for _, y in differing]
    print(f"bounding box: x {min(xs)}..{max(xs)}  y {min(ys)}..{max(ys)}")

    ours_five = to_five_bit(ours)
    reference_five = to_five_bit(reference)
    for x, y in differing[:8]:
        got = pixel_at(ours_five, ours_width, x, y)
        wanted = pixel_at(reference_five, ours_width, x, y)
        print(f"  ({x:4d},{y:3d}) got {got} want {wanted}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
