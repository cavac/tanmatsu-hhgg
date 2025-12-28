#!/bin/bash
# Convert a video file for Tanmatsu ESP32-P4 video player (MJPEG version)
# Uses hardware JPEG decoder for faster playback at native 600x480 resolution
#
# Usage: ./convert_video_mjpeg.sh <input.mp4> [display_name]
#
# Examples:
#   ./convert_video_mjpeg.sh myvideo.mp4
#   ./convert_video_mjpeg.sh myvideo.mp4 "My Cool Video"
#
# Output files are created in sdcard/at.cavac.hhgg/

set -e

OUTDIR="sdcard/at.cavac.hhgg"
PLAYLIST="$OUTDIR/playlist.json"

# Video settings
WIDTH=600
HEIGHT=480
FPS=30
JPEG_QUALITY=5   # FFmpeg MJPEG quality (2-31, lower is better)

# Check arguments
if [ $# -lt 1 ]; then
    echo "Usage: $0 <input.mp4> [display_name]"
    echo "  input.mp4    - Source video file"
    echo "  display_name - Optional display name (defaults to filename)"
    echo ""
    echo "Output: MJPEG video (${WIDTH}x${HEIGHT}@${FPS}fps) + AAC audio"
    exit 1
fi

INPUT="$1"
BASE=$(basename "$INPUT" .mp4)
DISPLAY_NAME="${2:-$BASE}"

# Check input exists
if [ ! -f "$INPUT" ]; then
    echo "Error: Input file '$INPUT' not found"
    exit 1
fi

# Create output directory
mkdir -p "$OUTDIR"

echo "Converting: $INPUT"
echo "Display name: $DISPLAY_NAME"
echo "Output: $OUTDIR/${BASE}.avi + ${BASE}.aac"
echo "Resolution: ${WIDTH}x${HEIGHT} @ ${FPS}fps"

# Get duration for playlist
DURATION=$(ffprobe -v error -show_entries format=duration \
    -of default=noprint_wrappers=1:nokey=1 "$INPUT" | cut -d. -f1)
echo "Duration: ${DURATION}s"

# Extract video: MJPEG in AVI container, native 600x480 resolution
# Hardware JPEG decoder can easily handle this at 30fps
echo "Extracting MJPEG video stream..."
ffmpeg -y -i "$INPUT" \
    -c:v mjpeg \
    -q:v $JPEG_QUALITY \
    -vf "scale=${WIDTH}:${HEIGHT}:force_original_aspect_ratio=decrease,pad=${WIDTH}:${HEIGHT}:(ow-iw)/2:(oh-ih)/2,format=yuvj420p" \
    -r $FPS \
    -an \
    -f avi "$OUTDIR/${BASE}.avi"

# Extract audio: AAC at 44.1kHz (same as H.264 version for audio player compatibility)
echo "Extracting audio stream..."
ffmpeg -y -i "$INPUT" \
    -vn \
    -c:a aac -b:a 128k -ar 44100 -ac 2 \
    -f adts "$OUTDIR/${BASE}.aac"

# Show file sizes
VIDEO_SIZE=$(stat -c%s "$OUTDIR/${BASE}.avi" 2>/dev/null || stat -f%z "$OUTDIR/${BASE}.avi")
AUDIO_SIZE=$(stat -c%s "$OUTDIR/${BASE}.aac" 2>/dev/null || stat -f%z "$OUTDIR/${BASE}.aac")
TOTAL_SIZE=$((VIDEO_SIZE + AUDIO_SIZE))
echo "File sizes: Video=${VIDEO_SIZE} bytes, Audio=${AUDIO_SIZE} bytes, Total=$((TOTAL_SIZE / 1024 / 1024))MB"

# Update or create playlist.json
echo "Updating playlist..."
if [ -f "$PLAYLIST" ]; then
    # Add to existing playlist (requires jq)
    if command -v jq &> /dev/null; then
        # Check if entry already exists
        EXISTS=$(jq --arg id "$BASE" '.videos | map(.id == $id) | any' "$PLAYLIST")
        if [ "$EXISTS" = "true" ]; then
            # Update existing entry
            jq --arg id "$BASE" \
               --arg name "$DISPLAY_NAME" \
               --arg vfile "${BASE}.avi" \
               --arg afile "${BASE}.aac" \
               --argjson dur "$DURATION" \
               '(.videos[] | select(.id == $id)) |= {id: $id, display_name: $name, video_file: $vfile, audio_file: $afile, duration_sec: $dur}' \
               "$PLAYLIST" > "${PLAYLIST}.tmp" && mv "${PLAYLIST}.tmp" "$PLAYLIST"
            echo "Updated existing entry: $BASE"
        else
            # Add new entry
            jq --arg id "$BASE" \
               --arg name "$DISPLAY_NAME" \
               --arg vfile "${BASE}.avi" \
               --arg afile "${BASE}.aac" \
               --argjson dur "$DURATION" \
               '.videos += [{id: $id, display_name: $name, video_file: $vfile, audio_file: $afile, duration_sec: $dur}]' \
               "$PLAYLIST" > "${PLAYLIST}.tmp" && mv "${PLAYLIST}.tmp" "$PLAYLIST"
            echo "Added new entry: $BASE"
        fi
    else
        echo "Warning: jq not installed. Please manually add entry to playlist.json"
    fi
else
    # Create new playlist
    cat > "$PLAYLIST" << EOF
{
  "title": "Video Player",
  "videos": [
    {
      "id": "$BASE",
      "display_name": "$DISPLAY_NAME",
      "video_file": "${BASE}.avi",
      "audio_file": "${BASE}.aac",
      "duration_sec": $DURATION
    }
  ]
}
EOF
    echo "Created new playlist with: $BASE"
fi

echo ""
echo "Done! Files created:"
echo "  $OUTDIR/${BASE}.avi (MJPEG ${WIDTH}x${HEIGHT}@${FPS}fps)"
echo "  $OUTDIR/${BASE}.aac"
echo "  $PLAYLIST (updated)"
