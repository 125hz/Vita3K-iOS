#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 --build-dir DIR --configuration CONFIG --output FILE.ipa" >&2
}

build_dir=""
configuration="Release"
output=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            build_dir="$2"
            shift 2
            ;;
        --configuration)
            configuration="$2"
            shift 2
            ;;
        --output)
            output="$2"
            shift 2
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done

if [[ -z "$build_dir" || -z "$output" ]]; then
    usage
    exit 2
fi

expected_app="$build_dir/ios/$configuration-iphoneos/Vita3K-iOS.app"
if [[ ! -d "$expected_app" ]]; then
    candidates="$(find "$build_dir" -type d -name 'Vita3K-iOS.app' -path '*-iphoneos/*')"
    candidate_count="$(printf '%s\n' "$candidates" | sed '/^$/d' | wc -l | tr -d ' ')"
    if [[ "$candidate_count" -ne 1 ]]; then
        echo "Expected exactly one device .app; found $candidate_count." >&2
        printf '%s\n' "$candidates" >&2
        exit 1
    fi
    expected_app="$candidates"
fi

executable="$expected_app/Vita3K-iOS"
plist="$expected_app/Info.plist"

[[ -f "$executable" ]] || { echo "Missing app executable: $executable" >&2; exit 1; }
[[ -f "$plist" ]] || { echo "Missing Info.plist: $plist" >&2; exit 1; }

plutil -lint "$plist"
file "$executable" | grep -q 'arm64' || {
    echo "The app executable is not an arm64 device binary." >&2
    file "$executable" >&2
    exit 1
}

if codesign -d "$expected_app" >/dev/null 2>&1; then
    echo "Refusing to package a signed application bundle." >&2
    exit 1
fi

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/Payload" "$(dirname "$output")"
ditto "$expected_app" "$stage/Payload/Vita3K-iOS.app"

output="$(cd "$(dirname "$output")" && pwd)/$(basename "$output")"
(cd "$stage" && /usr/bin/zip -qry "$output" Payload)

/usr/bin/unzip -tq "$output"
echo "Packaged unsigned IPA: $output"
