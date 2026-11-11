#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# QML gates: ES5-only JavaScript (PLT-05), at most 5 pulley items (PLT-06), and a
# component load of every file (needs Qt5Qml + Silica, i.e. the SDK/native container).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
qml="$root/Clients/Sailfish/qml"
status=0
# ES5: no let/const declarations, arrow functions, template strings or for-of.
# Comments ("// ..." and translator "//: ...") are stripped first.
es5=$(find "$qml" \( -name '*.qml' -o -name '*.js' \) -exec awk '
        { line=$0; sub(/(^|[[:space:]])\/\/([:[:space:]].*)?$/, "", line)
          if (line ~ /(^|[^A-Za-z_.])(let|const)[[:space:]]+[A-Za-z_]/ || line ~ /=>/ || line ~ /`/ ||
              line ~ /for[[:space:]]*\([^)]*[[:space:]]of[[:space:]]/) print FILENAME ":" FNR ": " $0 }' {} \;)
if [ -n "$es5" ]; then
    echo "$es5"; echo "qml check: non-ES5 syntax found (PLT-05)"; status=1
fi
# Pulley menus: count MenuItem lines between PullDownMenu/PushUpMenu and the closing brace at the same indent.
for f in $(find "$qml" -name '*.qml'); do
    awk -v file="$f" '
        /(PullDownMenu|PushUpMenu)[[:space:]]*\{/ { inmenu=1; depth=0; items=0 }
        inmenu { n=gsub(/\{/,"{"); m=gsub(/\}/,"}"); depth+=n-m; if ($0 ~ /MenuItem[[:space:]]*\{/) items++;
                 if (depth<=0) { if (items>5) { printf "qml check: %s has a pulley menu with %d items (PLT-06)\n", file, items; bad=1 } inmenu=0 } }
        END { exit bad }' "$f" || status=1
done
if pkg-config --exists Qt5Quick 2>/dev/null && [ -d /usr/lib64/qt5/qml/Sailfish/Silica ]; then
    out=${TMPDIR:-/tmp}/sailtube-qml-check.$$
    mkdir -p "$out"
    cp "$root/tests/qml/QmlCheck.cpp" "$out/"
    (cd "$out" && moc QmlCheck.cpp -o QmlCheck.moc && \
        "${CXX:-c++}" -std=c++11 -fPIC -I. $(pkg-config --cflags Qt5Quick Qt5Qml Qt5Gui) -o check QmlCheck.cpp $(pkg-config --libs Qt5Quick Qt5Qml Qt5Gui))
    QT_QPA_PLATFORM=minimal "$out/check" "$qml" || status=1
    rm -rf "$out"
else
    echo "qml check: component load SKIPPED (no Qt5Qml/Silica)"
fi
exit $status
