#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  scripts/verify-packages.sh --deb <path.deb> --rpm <path.rpm> [--rpm-spec <path.spec>]

Checks:
  - Package payload contains only:
      /usr/bin/solakon-one-fritz-powerregulator
      /usr/lib/systemd/system/solakon-one-fritz-powerregulator.service
      /etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf
    plus their parent directories.
  - DEB control metadata contains conffiles entry for /etc config file.
  - Optional RPM spec contains %config(noreplace) marker for /etc config file.
EOF
}

require_cmd() {
    local cmd="$1"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "ERROR: required command not found: $cmd" >&2
        exit 1
    fi
}

extract_tar_listing_from_deb_member() {
    local deb="$1"
    local member="$2"

    case "$member" in
        *.tar.gz) ar p "$deb" "$member" | tar -tzf - ;;
        *.tar.xz) ar p "$deb" "$member" | tar -tJf - ;;
        *.tar.zst) ar p "$deb" "$member" | tar --zstd -tf - ;;
        *.tar) ar p "$deb" "$member" | tar -tf - ;;
        *)
            echo "ERROR: unsupported deb archive member format: $member" >&2
            exit 1
            ;;
    esac
}

check_deb() {
    local deb="$1"
    local expected_conf="/etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf"

    [[ -f "$deb" ]] || { echo "ERROR: deb package not found: $deb" >&2; exit 1; }

    local data_member
    data_member="$(ar t "$deb" | awk '/^data\.tar(\..+)?$/ { print; exit }')"
    [[ -n "$data_member" ]] || { echo "ERROR: no data.tar* member found in $deb" >&2; exit 1; }

    local control_member
    control_member="$(ar t "$deb" | awk '/^control\.tar(\..+)?$/ { print; exit }')"
    [[ -n "$control_member" ]] || { echo "ERROR: no control.tar* member found in $deb" >&2; exit 1; }

    local payload
    payload="$(extract_tar_listing_from_deb_member "$deb" "$data_member")"

    while IFS= read -r entry; do
        [[ -z "$entry" ]] && continue
        case "$entry" in
            ./etc/|./etc/solakon-one-fritz-powerregulator/|./usr/|./usr/bin/|./usr/lib/|./usr/lib/systemd/|./usr/lib/systemd/system/)
                ;;
            ./etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf)
                ;;
            ./usr/bin/solakon-one-fritz-powerregulator)
                ;;
            ./usr/lib/systemd/system/solakon-one-fritz-powerregulator.service)
                ;;
            *)
                echo "ERROR: unexpected DEB payload entry: $entry" >&2
                exit 1
                ;;
        esac
    done <<< "$payload"

    local control_list
    control_list="$(extract_tar_listing_from_deb_member "$deb" "$control_member")"
    if ! grep -qx './conffiles' <<< "$control_list"; then
        echo "ERROR: DEB control archive does not contain ./conffiles" >&2
        exit 1
    fi

    local conffiles
    case "$control_member" in
        *.tar.gz) conffiles="$(ar p "$deb" "$control_member" | tar -xOzf - ./conffiles)" ;;
        *.tar.xz) conffiles="$(ar p "$deb" "$control_member" | tar -xOJf - ./conffiles)" ;;
        *.tar.zst) conffiles="$(ar p "$deb" "$control_member" | tar --zstd -xOf - ./conffiles)" ;;
        *.tar) conffiles="$(ar p "$deb" "$control_member" | tar -xOf - ./conffiles)" ;;
        *)
            echo "ERROR: unsupported control member format: $control_member" >&2
            exit 1
            ;;
    esac

    if ! grep -qx "$expected_conf" <<< "$conffiles"; then
        echo "ERROR: DEB conffiles does not include $expected_conf" >&2
        exit 1
    fi

    echo "OK: DEB payload and conffiles metadata verified: $deb"
}

check_rpm() {
    local rpm_file="$1"
    local rpm_spec="${2:-}"

    [[ -f "$rpm_file" ]] || { echo "ERROR: rpm package not found: $rpm_file" >&2; exit 1; }

    local payload
    payload="$(rpm -qpl "$rpm_file")"

    while IFS= read -r entry; do
        [[ -z "$entry" ]] && continue
        case "$entry" in
            /etc/solakon-one-fritz-powerregulator|/etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf)
                ;;
            /usr/bin/solakon-one-fritz-powerregulator)
                ;;
            /usr/lib/systemd|/usr/lib/systemd/system|/usr/lib/systemd/system/solakon-one-fritz-powerregulator.service)
                ;;
            *)
                echo "ERROR: unexpected RPM payload entry: $entry" >&2
                exit 1
                ;;
        esac
    done <<< "$payload"

    if [[ -n "$rpm_spec" ]]; then
        [[ -f "$rpm_spec" ]] || { echo "ERROR: rpm spec not found: $rpm_spec" >&2; exit 1; }
        if ! grep -q '^%config(noreplace) /etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf$' "$rpm_spec"; then
            echo "ERROR: RPM spec missing %config(noreplace) marker for /etc config" >&2
            exit 1
        fi
    fi

    echo "OK: RPM payload verified: $rpm_file"
}

main() {
    require_cmd ar
    require_cmd tar
    require_cmd rpm

    local deb=""
    local rpm_file=""
    local rpm_spec=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --deb)
                deb="$2"
                shift 2
                ;;
            --rpm)
                rpm_file="$2"
                shift 2
                ;;
            --rpm-spec)
                rpm_spec="$2"
                shift 2
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                echo "ERROR: unknown argument: $1" >&2
                usage
                exit 1
                ;;
        esac
    done

    if [[ -z "$deb" || -z "$rpm_file" ]]; then
        echo "ERROR: --deb and --rpm are required" >&2
        usage
        exit 1
    fi

    check_deb "$deb"
    check_rpm "$rpm_file" "$rpm_spec"

    echo "All package checks passed."
}

main "$@"
