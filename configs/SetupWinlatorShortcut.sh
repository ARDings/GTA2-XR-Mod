#!/system/bin/sh
# Run in WinlatorXR's Debug Terminal for the selected container:
# sh /sdcard/Download/GTA2Ceiling/gta2/SetupWinlatorShortcut.sh
# No game/registry files are changed. No container number is hardcoded.
set -eu
umask 077
fail() { printf '%s\n' "GTA2Ceiling setup: $*" >&2; exit 1; }
[ "$#" -eq 0 ] || fail 'This script takes no arguments.'

imagefs=$(pwd -P) || fail 'Cannot resolve the current directory.'
[ "${imagefs##*/}" = imagefs ] || fail 'Open the selected container Debug Terminal (working directory must be imagefs).'
[ -d "$imagefs/home" ] || fail 'Missing imagefs/home directory.'
[ "$(cd "$imagefs/home" && pwd -P)" = "$imagefs/home" ] || fail 'imagefs/home must not redirect elsewhere.'
[ -L "$imagefs/home/xuser" ] || fail 'No active container link home/xuser; select and initialize your container first.'
active_home=$(readlink -f "$imagefs/home/xuser") || fail 'Cannot resolve the selected container.'
[ -d "$active_home" ] || fail 'Selected container home does not exist.'
[ "${active_home%/*}" = "$imagefs/home" ] || fail 'Selected home is outside imagefs/home.'
case "${active_home##*/}" in
    xuser-*) container_number=${active_home##*/xuser-} ;;
    *) fail 'Unexpected selected container home name.' ;;
esac
case "$container_number" in ''|*[!0-9]*) fail 'Invalid selected container identifier.' ;; esac

script_parent=$(dirname "$0")
game_dir=$(cd "$script_parent" && pwd -P) || fail 'Cannot resolve the script directory.'
case "$game_dir" in
    */Download/GTA2Ceiling/gta2) ;;
    *) fail 'Place the script and mod files in Download/GTA2Ceiling/gta2 beside your own gta2.exe.' ;;
esac
template="$game_dir/GTA2Ceiling.desktop"
[ -d "$game_dir/data" ] && [ ! -L "$game_dir/data" ] || fail 'The private game data directory is missing or is a symbolic link.'
for file in gta2.exe gta2ceiling.dll gta2ceiling_vid.dll gta2ceiling.ini install.reg StartGTA2Ceiling.cmd GTA2Ceiling.desktop; do
    [ -f "$game_dir/$file" ] && [ -r "$game_dir/$file" ] || fail "Required file missing/unreadable: $file"
    [ ! -L "$game_dir/$file" ] || fail "Required file must not be a symbolic link: $file"
done

# Validate the complete template as data. Never source/eval desktop contents.
# Exact LF lines also catch mixed versions of this script and its template.
line_number=0
while IFS= read -r line || [ -n "$line" ]; do
    line_number=$((line_number + 1))
    case "$line_number:$line" in
        '1:[Desktop Entry]'|'2:Name=GTA2Ceiling'|'3:Type=Application'|\
        '4:Exec=wine C:/windows/system32/cmd.exe'|'5:'|'6:[Extra Data]'|\
        '7:autoclose=0'|'8:execArgs=/d /c D:\GTA2Ceiling\gta2\StartGTA2Ceiling.cmd'|\
        '9:screenSize=3072x1536'|'10:useReshade=3'|'11:use_pt=1'|\
        '12:xr_colour_key=3'|'13:xr_colour_key_tolerance=10'|'14:gcc_exact_colour_key=1') ;;
        *) fail "Unexpected shortcut template content on line $line_number; use matching files from the mod package." ;;
    esac
done < "$template"
[ "$line_number" -eq 14 ] || fail 'Incomplete shortcut template.'

desktop="$active_home/.wine/drive_c/users/xuser/Desktop"
[ -d "$desktop" ] || fail 'Wine Desktop is missing; start this container once, then retry.'
resolved_desktop=$(cd "$desktop" && pwd -P) || fail 'Cannot resolve Wine Desktop.'
[ "$resolved_desktop" = "$desktop" ] || fail 'Desktop path redirects outside the selected container or through an unexpected link.'
target="$desktop/GTA2Ceiling.desktop"
lock="$desktop/.GTA2Ceiling.shortcut.lock"
mkdir "$lock" 2>/dev/null || fail 'Another setup is active (or its lock remains); close it before retrying.'
temporary=''
cleanup() {
    if [ -n "$temporary" ]; then rm -f "$temporary"; fi
    rm -f "$lock/original"
    rmdir "$lock" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

[ ! -L "$target" ] || fail 'Existing shortcut is a symbolic link; nothing was replaced.'
if [ -e "$target" ]; then
    [ -f "$target" ] || fail 'Existing shortcut is not a regular file.'
    cp "$target" "$lock/original" || fail 'Cannot read the existing shortcut.'
    [ ! -L "$target" ] && cmp -s "$target" "$lock/original" || fail 'Existing shortcut changed while reading it.'
    if cmp -s "$template" "$lock/original"; then
        printf '%s\n' "Already configured: $target"
        exit 0
    fi
    # Only update our own cmd launcher. The same cmd.exe with different args is
    # a different application too, so protect both fields against replacement.
    section=''
    exec_count=0
    args_count=0
    name_count=0
    existing_exec=''
    existing_args=''
    existing_name=''
    carriage_return=$(printf '\r')
    while IFS= read -r line || [ -n "$line" ]; do
        line=${line%"$carriage_return"}
        # Treat spaced duplicate target fields as ambiguous too; do not let
        # alternate INI whitespace hide a different launch target.
        line=${line#"${line%%[![:space:]]*}"}
        line=${line%"${line##*[![:space:]]}"}
        key=${line%%=*}
        key=${key%"${key##*[![:space:]]}"}
        case "$line" in
            '[Desktop Entry]') section=desktop ;;
            '[Extra Data]') section=extra ;;
            '['*) section=other ;;
            *=*)
                case "$section:$key" in
                    desktop:Exec) exec_count=$((exec_count+1)); existing_exec=${line#*=} ;;
                    desktop:Name) name_count=$((name_count+1)); existing_name=${line#*=} ;;
                    extra:execArgs) args_count=$((args_count+1)); existing_args=${line#*=} ;;
                esac ;;
        esac
    done < "$lock/original"
    [ "$exec_count" -eq 1 ] && [ "$args_count" -eq 1 ] && [ "$name_count" -eq 1 ] &&
        [ "$existing_name" = GTA2Ceiling ] &&
        [ "$existing_exec" = 'wine C:/windows/system32/cmd.exe' ] &&
        [ "$existing_args" = '/d /c D:\GTA2Ceiling\gta2\StartGTA2Ceiling.cmd' ] ||
        fail 'Existing GTA2Ceiling.desktop launches something else or is ambiguous; nothing was replaced. Save/rename it yourself if intentional.'
fi

temporary=$(mktemp "$desktop/.GTA2Ceiling.desktop.XXXXXX") || fail 'Cannot stage shortcut.'
cp "$template" "$temporary" || fail 'Cannot copy shortcut template.'
chmod 644 "$temporary" || fail 'Cannot set shortcut permissions.'
cmp -s "$template" "$temporary" || fail 'Staged shortcut differs from the template.'
if [ -f "$lock/original" ]; then
    [ -f "$target" ] && [ ! -L "$target" ] && cmp -s "$target" "$lock/original" ||
        fail 'Existing shortcut changed during setup; nothing was replaced.'
    backup="$target.backup.$$"
    [ ! -e "$backup" ] && [ ! -L "$backup" ] || fail 'Backup path already exists; nothing was replaced.'
    cp "$lock/original" "$backup" || fail 'Cannot preserve the existing shortcut.'
    cmp -s "$target" "$backup" || fail 'Existing shortcut changed while preparing its backup.'
    printf '%s\n' "Previous matching launcher saved: $backup"
else
    [ ! -e "$target" ] && [ ! -L "$target" ] || fail 'A shortcut appeared during setup; nothing was replaced.'
fi
[ ! -L "$target" ] || fail 'Shortcut changed to a link during setup; nothing was replaced.'
mv -f "$temporary" "$target" || fail 'Cannot publish shortcut.'
temporary=''
printf '%s\n' "Installed only: $target" \
    'Refresh the WinlatorXR shortcut list, then launch GTA2Ceiling.' \
    'This container must map D: to the Quest Download directory.' \
    'Passthrough/pink chromakey are provisional; this setup is not a gameplay or MR acceptance.'
