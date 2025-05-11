#!/usr/bin/env python3
"""
spiral_generator.py

Generate a bitmap (.bmp) image of a spiral that starts in the top-left corner and winds inward toward the center.
Each pixel along the spiral path transitions smoothly through color by varying hue from start to end.
Size of the image is dynamic via command-line arguments.
"""
import argparse
from PIL import Image
import colorsys

def draw_pixel(pixels, x, y, index, total):
    """
    Color a single pixel based on its position in the spiral sequence.
    We map the index [0..total-1] to a hue in [0..1], then convert to RGB.
    """
    ratio = index / (total - 1) if total > 1 else 0
    r, g, b = colorsys.hsv_to_rgb(ratio, 1.0, 1.0)
    pixels[x, y] = (int(r * 255), int(g * 255), int(b * 255))


def generate_spiral(width, height, output_path):
    """
    Create a spiral fill from the top-left corner inwards, coloring each pixel along the path.
    Save the result as a BMP file.
    """
    # Create a new image and load its pixel map
    img = Image.new("RGB", (width, height), (0, 0, 0))
    pixels = img.load()

    total_pixels = width * height
    index = 0

    # Spiral boundary markers
    top, bottom = 0, height - 1
    left, right = 0, width - 1

    # Spiral fill loop
    while left <= right and top <= bottom:
        # Left to right along the top row
        for x in range(left, right + 1):
            draw_pixel(pixels, x, top, index, total_pixels)
            index += 1
        top += 1

        # Top to bottom along the right column
        for y in range(top, bottom + 1):
            draw_pixel(pixels, right, y, index, total_pixels)
            index += 1
        right -= 1

        # Right to left along the bottom row, if still valid
        if top <= bottom:
            for x in range(right, left - 1, -1):
                draw_pixel(pixels, x, bottom, index, total_pixels)
                index += 1
            bottom -= 1

        # Bottom to top along the left column, if still valid
        if left <= right:
            for y in range(bottom, top - 1, -1):
                draw_pixel(pixels, left, y, index, total_pixels)
                index += 1
            left += 1

    # Save as BMP
    img.save(output_path, "BMP")
    print(f"Spiral image saved to {output_path} ({width}x{height})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Generate a spiral bitmap with smooth color transitions."
    )
    parser.add_argument(
        "--width", type=int, default=500,
        help="Width of the output image in pixels (default: 500)"
    )
    parser.add_argument(
        "--height", type=int, default=500,
        help="Height of the output image in pixels (default: 500)"
    )
    parser.add_argument(
        "--output", type=str, default="spiral.bmp",
        help="Output BMP filename (default: spiral.bmp)"
    )
    args = parser.parse_args()

    generate_spiral(args.width, args.height, args.output)
