# myshell

A custom shell implementation in C for Ubuntu/WSL/Linux environments. Supports built-in commands, external programs, piping, redirection, job control, command history, and signal handling.

## Features
- Built-in commands: `cd`, `pwd`, `mkdir`, `touch`, `history`, `jobs`, `fg`, `bg`, `exit`
- External program execution
- Piping (`|`) and redirection (`>`, `<`, `>>`)
- Handles multiple arguments, quoted strings, and escape characters
- Background/foreground job control
- Command history
- Signal handling (`Ctrl+C`, `Ctrl+Z`)

## Build & Run
```sh
make
./myshell
```

## Usage
Type commands at the shell prompt, e.g.:
```
ls -l
mkdir testdir
echo "Hello World" > out.txt
cat < out.txt
ls | grep src
sleep 10 &
jobs
fg 1
exit
```

## Manual Testing
Test all features using commands above. See project source for details.
