#!/usr/bin/env bash
# Verify the generated archive in the same invocation that creates it.
set -euo pipefail
archive=${1:?Usage: package-test.sh ARCHIVE}
project=$(cd -- "$(dirname -- "$0")/.." && pwd)
version=$(< "$project/VERSION")
root=r1-controller-$version
expected=(
    "$root/r1-controller" "$root/r1-launcher"
    "$root/README.md" "$root/CHANGELOG.md" "$root/VERSION"
    "$root/docs/KEY_MAP.md" "$root/docs/INSTALL.md"
    "$root/scripts/" "$root/scripts/deploy.sh" "$root/scripts/install-device.sh"
    "$root/packaging/" "$root/packaging/applications/"
    "$root/packaging/applications/R1 Controller.app"
    "$root/packaging/applications/R1 Stop.app"
)
names=$(tar -tzf "$archive")
details=$(tar -tvzf "$archive")
actual_sorted=$(printf '%s\n' "$names" | LC_ALL=C sort)
expected_sorted=$(printf '%s\n' "${expected[@]}" | LC_ALL=C sort)
[[ $actual_sorted == "$expected_sorted" ]] || {
    echo 'Unexpected release archive membership' >&2
    diff -u <(printf '%s\n' "$expected_sorted") <(printf '%s\n' "$actual_sorted")
    exit 1
}
mapfile -t member_names <<< "$names"
mapfile -t member_details <<< "$details"
[[ ${#member_names[@]} == "${#member_details[@]}" ]]
for i in "${!member_names[@]}"; do
    expected_type=-
    case ${member_names[i]} in
        "$root/scripts/"|"$root/packaging/"|"$root/packaging/applications/") expected_type=d ;;
    esac
    [[ ${member_details[i]:0:1} == "$expected_type" ]] || {
        echo "Unexpected archive member type: ${member_names[i]}" >&2
        exit 1
    }
done
tar -xOf "$archive" "$root/VERSION" | cmp - "$project/VERSION"
printf '%s\n' "$details"
echo 'PASS: exact archive membership, member types and VERSION; no private artifacts'
