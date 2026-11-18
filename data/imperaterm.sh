# imperaterm.sh: at each prompt the shell tells the terminal who is at it and
# where, and the tab says so. A terminal cannot find that out: it has to
# be told. It is told in two strings that other terminals read as well:
# OSC 7 with file://host/directory, and OSC 1337 with RemoteHost=user@host.
#
# For shells of the Bourne kind: sh, ash, dash, ksh, bash and zsh.
#
# Imperaterm has the shell it starts read this, where it can: bash by
# PROMPT_COMMAND in the environment and the others by ENV, unless those
# are set already. A shell that does not get that environment has to read
# it some other way, and such are the shells of sudo -i, su - and ssh:
#
#   every login shell reads it if it is in /etc/profile.d;
#   a line in a shell's own file reads it there:  . /path/to/imperaterm.sh
#
# On another host it does the same for a terminal that is reached by ssh.
case "$-:${TERM-}" in
*i*:xterm*)
    [ -n "${_imperaterm_host-}" ] || {
        _imperaterm_host=$(uname -n 2>/dev/null)
        _imperaterm_user=$(id -un 2>/dev/null)
    }
    _imperaterm_report() {
        # A % in the directory is written %25, as a URI has it.
        _imperaterm_dir=
        _imperaterm_left=$PWD
        while :; do
            case $_imperaterm_left in
            *%*)
                _imperaterm_dir=$_imperaterm_dir${_imperaterm_left%%\%*}%25
                _imperaterm_left=${_imperaterm_left#*\%}
                ;;
            *)
                break
                ;;
            esac
        done
        printf '\033]7;file://%s%s\033\\\033]1337;RemoteHost=%s@%s\033\\' \
            "$_imperaterm_host" "$_imperaterm_dir$_imperaterm_left" "$_imperaterm_user" "$_imperaterm_host" 2>/dev/null >/dev/tty
    }
    if [ -n "${BASH_VERSION-}" ]; then
        case ${PROMPT_COMMAND-} in
        *imperaterm.sh*)
            # This is being read at each prompt, by the environment's
            # PROMPT_COMMAND: now is when to say it.
            _imperaterm_report
            ;;
        *_imperaterm_report*) ;;
        *)
            PROMPT_COMMAND="_imperaterm_report${PROMPT_COMMAND:+;$PROMPT_COMMAND}"
            ;;
        esac
    elif [ -n "${ZSH_VERSION-}" ]; then
        eval 'case " ${precmd_functions[*]-} " in
              *" _imperaterm_report "*) ;;
              *) precmd_functions+=(_imperaterm_report) ;;
              esac' 2>/dev/null
    else
        # The prompt is expanded each time it is shown: what is in it
        # says its piece to the terminal itself and adds nothing to the
        # prompt.
        case ${PS1-} in
        *_imperaterm_report*) ;;
        *)
            PS1='$(_imperaterm_report)'"${PS1-\$ }"
            ;;
        esac
    fi
    ;;
esac
