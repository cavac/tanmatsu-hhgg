#!/bin/bash
# Convert a video file for Tanmatsu ESP32-P4 video player
# Usage: ./convert_video.sh <input.mp4> [display_name]
#
# Examples:
#   ./convert_video.sh myvideo.mp4
#   ./convert_video.sh myvideo.mp4 "My Cool Video"
#
# Output files are created in sdcard/at.cavac.hhgg/

set -e

OUTDIR="sdcard/at.cavac.hhgg"
PLAYLIST="$OUTDIR/playlist.json"

# Check arguments
if [ $# -lt 1 ]; then
    echo "Usage: $0 <input.mp4> [display_name]"
    echo "  input.mp4    - Source video file"
    echo "  display_name - Optional display name (defaults to filename)"
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
echo "Output: $OUTDIR/${BASE}.h264 + ${BASE}.aac"

# Get duration for playlist
DURATION=$(ffprobe -v error -show_entries format=duration \
    -of default=noprint_wrappers=1:nokey=1 "$INPUT" | cut -d. -f1)
echo "Duration: ${DURATION}s"

# Extract video: H.264 Baseline Profile, 300x240 (will be 2x upscaled to 600x480), 10fps
# Half resolution for faster decode, upscaled on device
echo "Extracting video stream..."
ffmpeg -y -i "$INPUT" \
    -c:v libx264 -profile:v baseline -level 3.0 \
    -preset veryslow -tune fastdecode \
    -vf "scale=300:240:force_original_aspect_ratio=decrease,pad=300:240:(ow-iw)/2:(oh-ih)/2,format=yuv420p" \
    -x264opts "slices=1:no-deblock" \
    -g 15 -keyint_min 15 \
    -b:v 300k -maxrate 400k -bufsize 300k \
    -r 10 \
    -an \
    -f h264 "$OUTDIR/${BASE}.h264"

# Extract audio: AAC at 44.1kHz (BSP I2S actual rate)
echo "Extracting audio stream..."
ffmpeg -y -i "$INPUT" \
    -vn \
    -c:a aac -b:a 128k -ar 44100 -ac 2 \
    -f adts "$OUTDIR/${BASE}.aac"

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
               --arg vfile "${BASE}.h264" \
               --arg afile "${BASE}.aac" \
               --argjson dur "$DURATION" \
               '(.videos[] | select(.id == $id)) |= {id: $id, display_name: $name, video_file: $vfile, audio_file: $afile, duration_sec: $dur}' \
               "$PLAYLIST" > "${PLAYLIST}.tmp" && mv "${PLAYLIST}.tmp" "$PLAYLIST"
            echo "Updated existing entry: $BASE"
        else
            # Add new entry
            jq --arg id "$BASE" \
               --arg name "$DISPLAY_NAME" \
               --arg vfile "${BASE}.h264" \
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
      "video_file": "${BASE}.h264",
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
echo "  $OUTDIR/${BASE}.h264"
echo "  $OUTDIR/${BASE}.aac"
echo "  $PLAYLIST (updated)"
