#!/bin/bash
# Downloads the hardware test programs this emulator is checked against.
#
# They live in games/, which the repository does not track: they are
# third-party binaries of a few megabytes, and both collections are
# freely available from the addresses below. Run this once on a new
# machine and every test in the issues can be reproduced.
#
#   ps1-tests   github.com/JaCzekanski/ps1-tests, prebuilt on its
#               releases page. One PS-EXE per test, no menus, plain
#               text verdicts, and reference output captured from a
#               real console next to each binary.
#
#   psxtest_cpu psx.amidog.se. The one test of his that runs
#               unattended — his GTE and GPU tests are menu-driven and
#               report in coloured glyphs on screen.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
games="$here/games"
mkdir -p "$games"
cd "$games"

echo "ps1-tests..."
gh release download --repo JaCzekanski/ps1-tests --pattern "tests.zip" \
    --clobber
mkdir -p ps1-tests
unzip -q -o tests.zip -d ps1-tests

echo "psxtest_cpu..."
curl -sSL -o psxtest_cpu.zip \
    "https://psx.amidog.se/lib/exe/fetch.php?media=psx:download:psxtest_cpu.zip"
unzip -q -o psxtest_cpu.zip

# Amidog ships no checksums, so this is the one his 1.3 release has.
expected=023aec8c92aaaf4d3b07956e26dd6c77ff397456
actual=$(sha1sum psxtest_cpu.exe | cut -d' ' -f1)
if [ "$actual" != "$expected" ]; then
    echo "psxtest_cpu.exe is not the expected build: $actual" >&2
    exit 1
fi

echo
echo "ready. try:"
echo "  ./build-rel/wobble-dbg SCPH1001.BIN \\"
echo "    -c 'exe games/psxtest_cpu.exe' -c 'frames 4000' -c tty"
