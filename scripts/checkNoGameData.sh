#!/bin/sh
# Keeps retail game data out of the repository.
#
# The rule is not "no binary data files" — that was too blunt, and it cost this
# project a set of perfectly clean, purpose-built test fixtures. The rule is
# that data of the kind a retail install produces may only live in testAssets/,
# and every file there is pinned by hash. So:
#
#   1. No game-data-shaped file tracked anywhere outside testAssets/.
#   2. Every manifest entry is tracked by git and matches its recorded hash.
#   3. Nothing is tracked under testAssets/ that the manifest does not list.
#
# Rules 2 and 3 both consult git rather than the filesystem, deliberately. An
# earlier version hashed whatever was on disk while listing what git knew
# about, and so cheerfully validated four fixtures that .gitignore had silently
# excluded from the commit.

set -eu

DATA_EXTENSIONS='\.(package|dds|tga|xa|far|iff|bmp|jpg|jpeg|mp3|wav|iso|img|bin|cue|nrg|mdf)$'
FIXTURE_DIRECTORY="testAssets"
MANIFEST="$FIXTURE_DIRECTORY/manifest.sha256"

failureCount=0
scratchDirectory=$(mktemp -d)
trap 'rm -rf "$scratchDirectory"' EXIT

echo "checking for game data outside $FIXTURE_DIRECTORY/..."
git ls-files | grep -iE "$DATA_EXTENSIONS" | grep -v "^$FIXTURE_DIRECTORY/" \
    > "$scratchDirectory/strays" || true
if [ -s "$scratchDirectory/strays" ]; then
    cat "$scratchDirectory/strays"
    echo "error: game data must live under $FIXTURE_DIRECTORY/ and be listed in the manifest" >&2
    failureCount=$((failureCount + 1))
else
    echo "  none"
fi

if [ ! -f "$MANIFEST" ]; then
    echo "error: missing $MANIFEST" >&2
    exit 1
fi

awk '{ print $2 }' "$MANIFEST" | sed "s|^|$FIXTURE_DIRECTORY/|" | sort > "$scratchDirectory/listed"
git ls-files "$FIXTURE_DIRECTORY" \
    | grep -vE "^$FIXTURE_DIRECTORY/(manifest\.sha256|README\.md)$" \
    | sort > "$scratchDirectory/tracked"

echo "checking every manifest entry is committed..."
if comm -23 "$scratchDirectory/listed" "$scratchDirectory/tracked" > "$scratchDirectory/missing" &&
   [ ! -s "$scratchDirectory/missing" ]; then
    echo "  all listed fixtures are tracked"
else
    cat "$scratchDirectory/missing"
    echo "error: manifest lists a file git is not tracking (is it caught by .gitignore?)" >&2
    failureCount=$((failureCount + 1))
fi

echo "checking for unlisted files in $FIXTURE_DIRECTORY/..."
if comm -13 "$scratchDirectory/listed" "$scratchDirectory/tracked" > "$scratchDirectory/unlisted" &&
   [ ! -s "$scratchDirectory/unlisted" ]; then
    echo "  none"
else
    cat "$scratchDirectory/unlisted"
    echo "error: file tracked under $FIXTURE_DIRECTORY/ but absent from the manifest" >&2
    failureCount=$((failureCount + 1))
fi

echo "verifying fixture contents against $MANIFEST..."
if ( cd "$FIXTURE_DIRECTORY" && sha256sum --quiet --check manifest.sha256 ); then
    echo "  all fixtures match"
else
    echo "error: a fixture does not match its recorded hash" >&2
    failureCount=$((failureCount + 1))
fi

if [ "$failureCount" -ne 0 ]; then
    echo "game data check FAILED" >&2
    exit 1
fi

echo "game data check passed"
