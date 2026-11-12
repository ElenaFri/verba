# bash completion for verba
#
# Completes verba's own options up to the point where the traced <command>
# begins; from the command word onward, defers to ordinary command/filename
# completion so `verba [opts] mycmd <TAB>` behaves as if completing `mycmd`.

_verba()
{
	local cur prev words cword
	_init_completion 2>/dev/null || {
		cur=${COMP_WORDS[COMP_CWORD]}
		prev=${COMP_WORDS[COMP_CWORD-1]}
		words=("${COMP_WORDS[@]}")
		cword=$COMP_CWORD
	}

	local opts='-h --help -V --version --json -o --output --allow --tty --heuristic --strict --patterns'

	# Options that take an argument.
	case "$prev" in
	-o|--output|--patterns)
		_filedir
		return
		;;
	--allow)
		_filedir -d
		return
		;;
	esac

	# Find the command word: the first non-option argument that is not itself
	# the value of an argument-taking option. Once it exists before the cursor,
	# we are completing the traced command's own arguments, not verba's.
	local i cmd_index=0
	for ((i = 1; i < cword; i++)); do
		local w=${words[i]}
		case "$w" in
		-o|--output|--allow|--patterns)
			((i++))	# skip the option's value
			continue
			;;
		--)
			cmd_index=$((i + 1))
			break
			;;
		-*)
			continue
			;;
		*)
			cmd_index=$i
			break
			;;
		esac
	done

	if ((cmd_index > 0 && cmd_index <= cword)); then
		# We are at or past the command: complete commands/files normally.
		if ((cmd_index == cword)); then
			COMPREPLY=($(compgen -c -- "$cur"))
		else
			_filedir
		fi
		return
	fi

	if [[ $cur == -* ]]; then
		COMPREPLY=($(compgen -W "$opts" -- "$cur"))
		return
	fi

	# No command yet and current word is not an option: offer commands.
	COMPREPLY=($(compgen -c -- "$cur"))
}

complete -F _verba verba
