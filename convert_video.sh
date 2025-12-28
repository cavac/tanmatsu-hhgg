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

echo ""
echo "Done! Created: $OUTDIR/${BASE}.avi (MJPEG ${WIDTH}x${HEIGHT} + MP3 audio)"
