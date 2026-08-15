# bash completion for gl-repl
#
#   source scripts/completions/gl-repl.bash
#
# Example, tutorial, and tour names come from the binary's own --list-examples,
# --list-tutorials, and --list-tours, so the catalogs stay the single source
# of truth. All list paths print and exit before any GL, window, or audio init,
# so completion is instant and works headless.
#
# Kept to bash 3.2 syntax (the /bin/bash macOS ships).

_gl_repl_opts='-h --help --accum --no-accum --no-audio --dump-code --dump-flat
--flat-histogram --dump-state-layout --detailed-prof --export-c --export-glr
--export-ply --export-ply-srgb --assets --example --examples-dir --time
--window --watch --tutorial --tour --list-examples --list-tutorials --list-tours --list-config --lint-scenes'

# Catalog names carry spaces, parens and '&', so candidates have to come back
# shell-escaped (or bare inside an open quote) to survive as one argument.
_gl_repl_put() {
    local quoted=$1 i
    shift
    COMPREPLY=( "$@" )
    # Readline is inside a quote: spaces are already literal there.
    [[ -n "$quoted" ]] && return 0
    for i in "${!COMPREPLY[@]}"; do
        COMPREPLY[$i]=$(printf '%q' "${COMPREPLY[$i]}")
    done
    return 0
}

_gl_repl_names() {
    local cur=$1 listopt=$2 quote="" names
    case "$cur" in
        \"*) quote='"'; cur=${cur#\"} ;;
        \'*) quote="'"; cur=${cur#\'} ;;
    esac
    # Match against the unescaped form so re-completing mid-name works.
    cur=${cur//\\/}

    # Catalog lines are "%4d  <name>"; the numbered-line match drops the header.
    names=$("${COMP_WORDS[0]}" "$listopt" 2>/dev/null |
                sed -n 's/^ *[0-9][0-9]*  //p')
    [[ -n "$names" ]] || return 0

    local IFS=$'\n'
    local matches=( $(compgen -W "$names" -- "$cur") )
    unset IFS
    _gl_repl_put "$quote" "${matches[@]}"
}

_gl_repl_paths() {
    local cur=$1 quote="" d pat
    shift
    case "$cur" in
        \"*) quote='"'; cur=${cur#\"} ;;
        \'*) quote="'"; cur=${cur#\'} ;;
    esac
    cur=${cur//\\/}

    local IFS=$'\n'
    local matches=()
    for pat in "$@"; do
        [[ -n "$pat" ]] || continue
        matches+=( $(compgen -f -X "!$pat" -- "$cur") )
    done
    # Directories are always offered: to descend, and as a workspace argument.
    for d in $(compgen -d -- "$cur"); do
        matches[${#matches[@]}]="$d/"
    done
    unset IFS
    _gl_repl_put "$quote" "${matches[@]}"
}

_gl_repl_complete() {
    local cur prev
    cur=${COMP_WORDS[COMP_CWORD]}
    prev=${COMP_WORDS[COMP_CWORD-1]}

    case "$prev" in
        --example)      _gl_repl_names "$cur" --list-examples ; return 0 ;;
        --tutorial)     _gl_repl_names "$cur" --list-tutorials ; return 0 ;;
        --tour)         _gl_repl_names "$cur" --list-tours    ; return 0 ;;
        --assets|--examples-dir|--lint-scenes)
                        _gl_repl_paths "$cur" ""              ; return 0 ;;
        --export-c)     _gl_repl_paths "$cur" '*.c'           ; return 0 ;;
        --export-glr)   _gl_repl_paths "$cur" '*.glr'         ; return 0 ;;
        --export-ply)   _gl_repl_paths "$cur" '*.ply'         ; return 0 ;;
        --window)
            COMPREPLY=( $(compgen -W '1200x800 1600x1000 2400x1600' -- "$cur") )
            return 0
            ;;
        --time)         return 0 ;;   # free-form seconds
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "$_gl_repl_opts" -- "$cur") )
        return 0
    fi

    # Positional: a saved scene (*.c or *.glr) or a workspace directory.
    _gl_repl_paths "$cur" '*.c' '*.glr'
    return 0
}

# Registering the basename also covers ./gl-repl and build/release/gl-repl:
# bash retries the last path component when the full command word has no
# compspec of its own.
complete -F _gl_repl_complete gl-repl
