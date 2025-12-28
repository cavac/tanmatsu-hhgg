#!/bin/bash
# Convert a video file for Tanmatsu ESP32-P4 video player
# Creates interleaved AVI with MJPEG video + PCM audio
#
# Usage: ./convert_video.sh <input.mp4> [display_name]
#
# Examples:
#   ./convert_video.sh myvideo.mp4
#   ./convert_video.sh myvideo.mp4 "My Cool Video"
#
# Output: Single .avi file with interleaved video and audio

set -e

OUTDIR="sdcard/at.cavac.hhgg"
PLAYLIST="$OUTDIR/playlist.json"

# Video settings
WIDTH=600
HEIGHT=480
JPEG_QUALITY=5   # FFmpeg MJPEG quality (2-31, lower is better)

# Audio settings (MP3 for good compression + AVI compatibility)
AUDIO_RATE=44100
AUDIO_CHANNELS=2
AUDIO_BITRATE=128k

# Check arguments
if [ $# -lt 1 ]; then
    echo "Usage: $0 <input.mp4> [display_name]"
    echo "  input.mp4    - Source video file"
    echo "  display_name - Optional display name (defaults to filename)"
    echo ""
    echo "Output: Interleaved AVI (MJPEG ${WIDTH}x${HEIGHT} + PCM ${AUDIO_RATE}Hz stereo)"
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
echo "Output: $OUTDIR/${BASE}.avi"

# Get duration and FPS for display
DURATION=$(ffprobe -v error -show_entries format=duration \
    -of default=noprint_wrappers=1:nokey=1 "$INPUT" | cut -d. -f1)
SRC_FPS=$(ffprobe -v error -select_streams v -of default=noprint_wrappers=1:nokey=1 \
    -show_entries stream=r_frame_rate "$INPUT" | head -1)
echo "Source: ${SRC_FPS} fps, ${DURATION}s duration"
echo "Output: ${WIDTH}x${HEIGHT} MJPEG + MP3 ${AUDIO_BITRATE} stereo"

# Create interleaved AVI with MJPEG video and MP3 audio
# - Video: MJPEG at native framerate, scaled to 600x480 with letterboxing
# - Audio: MP3 at 128kbps stereo 44.1kHz
echo "Creating interleaved AVI..."
ffmpeg -y -i "$INPUT" \
    -c:v mjpeg \
    -q:v $JPEG_QUALITY \
    -vf "scale=${WIDTH}:${HEIGHT}:force_original_aspect_ratio=decrease,pad=${WIDTH}:${HEIGHT}:(ow-iw)/2:(oh-ih)/2,format=yuvj420p" \
    -c:a libmp3lame \
    -b:a $AUDIO_BITRATE \
    -ar $AUDIO_RATE \
    -ac $AUDIO_CHANNELS \
    -f avi "$OUTDIR/${BASE}.avi"

# Show file size
FILE_SIZE=$(stat -c%s "$OUTDIR/${BASE}.avi" 2>/dev/null || stat -f%z "$OUTDIR/${BASE}.avi")
echo "File size: $((FILE_SIZE / 1024 / 1024))MB"

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
               --argjson dur "$DURATION" \
               '(.videos[] | select(.id == $id)) |= {id: $id, display_name: $name, video_file: $vfile, duration_sec: $dur}' \
               "$PLAYLIST" > "${PLAYLIST}.tmp" && mv "${PLAYLIST}.tmp" "$PLAYLIST"
            echo "Updated existing entry: $BASE"
        else
            # Add new entry
            jq --arg id "$BASE" \
               --arg name "$DISPLAY_NAME" \
               --arg vfile "${BASE}.avi" \
               --argjson dur "$DURATION" \
               '.videos += [{id: $id, display_name: $name, video_file: $vfile, duration_sec: $dur}]' \
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
      "duration_sec": $DURATION
    }
  ]
}
EOF
    echo "Created new playlist with: $BASE"
fi

echo ""
echo "Done! File created:"
echo "  $OUTDIR/${BASE}.avi (MJPEG ${WIDTH}x${HEIGHT} + MP3 audio)"
echo "  $PLAYLIST (updated)"
