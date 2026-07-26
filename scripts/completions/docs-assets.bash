# bash completion for scripts/docs-assets.sh
#
#   source scripts/completions/docs-assets.bash
#
# Candidates come from the script's own --list, so the GIF_ASSETS/PNG_ASSETS
# arrays in docs-assets.sh stay the single source of truth and this file cannot
# drift. --list exits before the magick/ffmpeg probe, so completion works on a
# machine without the capture tools installed.
#
# Kept to bash 3.2 syntax (the /bin/bash macOS ships).

_docs_assets_complete() {
    local cur prev w cats="" typed="" avail="" names name
    local i=1 skip=0

    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}

    # --jobs takes a count; offer nothing rather than stray asset names.
    case "$prev" in
        -j|--jobs) return ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W '--gifs --pngs --list -j --jobs -h --help' -- "$cur") )
        return 0
    fi

    # Replay the words already on the line: a category flag narrows the
    # candidate list, a name already typed drops out of it.
    while [[ $i -lt $COMP_CWORD ]]; do
        w=${COMP_WORDS[i]}
        if [[ $skip -eq 1 ]]; then
            skip=0
        else
            case "$w" in
                --gifs|--pngs) cats="$cats $w" ;;
                -j|--jobs)     skip=1 ;;
                -*|"")         ;;
                *)             typed="$typed$w " ;;
            esac
        fi
        i=$((i + 1))
    done

    names=$("${COMP_WORDS[0]}" --list $cats 2>/dev/null) || return

    for name in $names; do
        case " $typed" in
            *" $name "*) continue ;;
        esac
        avail="$avail $name"
    done

    # compgen exits nonzero on no match; don't leak that as the hook's status.
    COMPREPLY=( $(compgen -W "$avail" -- "$cur") )
    return 0
}

# Registering the basename also covers scripts/docs-assets.sh and
# ./scripts/docs-assets.sh: bash retries the last path component when the full
# command word has no compspec of its own.
complete -F _docs_assets_complete docs-assets.sh
