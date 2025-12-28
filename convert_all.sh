#!/bin/bash
# Convert all videos in videos/ folder
# Usage: ./convert_all.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Check if videos directory exists
if [ ! -d "$SCRIPT_DIR/videos" ]; then
    echo "Error: videos/ directory not found"
    exit 1
fi

# Count MP4 files
COUNT=$(find "$SCRIPT_DIR/videos" -maxdepth 1 -name "*.mp4" | wc -l)
if [ "$COUNT" -eq 0 ]; then
    echo "No MP4 files found in videos/ folder"
    exit 1
fi

echo "Found $COUNT video(s) to convert"
echo "================================"

CONVERTED=0
for input in "$SCRIPT_DIR/videos"/*.mp4; do
    if [ -f "$input" ]; then
        echo ""
        echo "[$((CONVERTED + 1))/$COUNT] Converting: $(basename "$input")"
        echo "----------------------------------------"
        "$SCRIPT_DIR/convert_video.sh" "$input"
        CONVERTED=$((CONVERTED + 1))
    fi
done

echo ""
echo "================================"
echo "All $CONVERTED videos converted!"
echo ""
echo "Output files are in: sdcard/at.cavac.hhgg/"
echo "Copy to SD card with: make copy-to-sd"
