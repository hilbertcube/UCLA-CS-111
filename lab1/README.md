## UID: 706752260

## Pipe Up

Executes a chain of programs connected by pipes, mimicking the shell `|` operator.

## Building

```
make
```

This compiles `pipe.c` into the `pipe` executable.

## Running

Pass the programs we want to pipe together as arguments:

```
./pipe ls cat wc
```

This is equivalent to running `ls | cat | wc` in the shell. A single program is also valid:

```
./pipe ls
```

The first program receives the parent's standard input, the last program writes to the parent's standard output, and all standard error output goes to the parent's standard error.

## Cleaning up

```
make clean
```

This removes the compiled object file and the `pipe` executable.

