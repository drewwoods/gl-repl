#compdef docs-assets.sh
# zsh completion for scripts/docs-assets.sh
#
# Works either way round:
#   fpath=(.../scripts/completions $fpath)   # before compinit — autoloaded
#   source .../scripts/completions/_docs-assets.sh   # any time after compinit
#
# Candidates come from the script's own --list, so the GIF_ASSETS/PNG_ASSETS
# arrays in docs-assets.sh stay the single source of truth and this file cannot
# drift. --list exits before the magick/ffmpeg probe, so completion works on a
# machine without the capture tools installed.

_docs_assets() {
    local context state state_descr line
    typeset -A opt_args

    # A category flag already on the line narrows the candidate list.
    local -a cats
    cats=( ${(M)words[2,-1]:#(--gifs|--pngs)} )

    _arguments -s -S \
        '--gifs[regenerate all GIF assets]' \
        '--pngs[regenerate all PNG assets]' \
        '--list[list selected asset names and exit]' \
        '(-j --jobs)'{-j+,--jobs}'[regenerate up to N assets in parallel]:count:' \
        '(-h --help)'{-h,--help}'[show help and exit]' \
        '*:asset:->assets'

    case $state in
        assets)
            local -a assets
            assets=( ${(f)"$(${words[1]} --list $cats 2>/dev/null)"} )
            # Drop names already on the line.
            assets=( ${assets:|words} )
            _describe -t assets asset assets
            ;;
    esac
}

# Autoloaded via fpath + the #compdef line above: compinit binds the file to the
# command and expects running it to *do* the completion.
# Sourced from a shell rc instead: nothing has bound anything, so register.
if [[ ${zsh_eval_context[-1]} == loadautofunc ]]; then
    _docs_assets "$@"
elif (( $+functions[compdef] )); then
    compdef _docs_assets docs-assets.sh
else
    print -u2 "_docs-assets.sh: run compinit before sourcing this file"
    return 1
fi
